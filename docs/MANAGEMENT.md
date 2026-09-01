# Managing loomtabulator: a spec for external orchestration

This document is a contract, not code. loomtabulator itself does not
watch other processes, does not detect its own siblings, and does not
decide when to restart anything beyond itself - all of that is owned by
whatever external supervisor launches and monitors it (a separate
project, out of scope for this repo). This spec exists so that
supervisor can be built against a stable interface: what to launch,
what to poll, what "healthy" means, and - for the multi-instance case -
what invariants it must enforce that loomtabulator cannot enforce for
itself.

Part 1 describes the real, working, single-instance contract that
exists in the code today. Part 2 is a forward-looking design for
chaining multiple instances together via DPDK rings - the `ring_output`
stage it depends on does not exist yet (see "Open items" at the end);
this section specifies the interface it needs to honor once built, so
supervisor development doesn't have to block on it.

## Part 1: Single-instance contract (exists today)

### Identity

A supervisor launching an instance controls its identity through EAL
and app-level flags, all on the command line - there is no config file,
no service discovery, no default identity beyond what's passed in:

- `--file-prefix=NAME` - names the DPDK shared-memory namespace this
  instance's rings/memzones live in. If omitted, `main.c`'s
  `build_eal_argv()` injects `--file-prefix=loomtabulator` by default -
  **a supervisor managing more than one instance must pass this
  explicitly and distinctly per instance** (or per shared-namespace
  group - see Part 2), since two unrelated instances sharing a prefix
  by accident collide in the same memory namespace.
- `--huge-unlink=existing` - also injected by default (see
  `build_eal_argv()`) unless already present or `--no-huge` was given.
  Purges whatever a *previous, already-exited* run under the same
  `--file-prefix` left behind, the moment a *new* run under that same
  prefix starts. Safe for a lone instance restarting itself. **Not**
  safe if another still-running process is attached to that same
  namespace as a DPDK secondary - see Part 2's restart-ordering
  section.
- `-l CORELIST` (EAL) / `--workers=N` (app) - the lcore set this
  instance runs on. DPDK lcores are physical-core-bound within a shared
  `--file-prefix` namespace; two instances in the same namespace (or
  really, on the same host at all) **must** be given disjoint `-l`
  sets. loomtabulator does not and cannot validate another process's
  core usage - this is the supervisor's responsibility entirely.
- `--graph=PATH` - the pipeline this instance runs. `--graphs-dir=PATH`
  is a separate library of named graphs the *web UI* can swap
  `--graph=PATH`'s file from (see README.md) - a supervisor orchestrating
  restarts programmatically would more likely write `--graph=PATH`
  directly, or drive the same `POST /api/graphs/load` +
  `POST /api/reload` HTTP sequence the UI uses (see below).
- `--web-port=N` - each instance's status/API server. Nothing today
  auto-assigns or discovers this; a supervisor managing N instances
  must allocate and track N distinct ports itself (and, if instances
  run in containers, map each to a distinct host port).

### Liveness and health

- `GET /status.json` - the cheapest possible liveness probe: if this
  answers at all, the process is up, EAL init succeeded, and the
  worker pool is running. Reports `records_in`/`records_dropped`/
  `records_forwarded` (cumulative) and `uptime_sec`. A supervisor
  wanting "is it making progress" rather than just "is it up" should
  sample this on an interval and watch the deltas, the same way this
  project's own README.md benchmarking procedure already does.
- `GET /api/stage-status` - per-node counters from each stage's own
  `struct stage.get_status()` (ABI v6), keyed by graph node id. Useful
  for a supervisor that wants finer-grained health than "the process
  answers HTTP" - e.g. a `gfp_extract`-style stage reporting
  `checksum_failures`/`malformed_frames` climbing without the process
  itself being unhealthy in any way `/status.json` alone would show.
- **There is no process-level heartbeat or crash callback.** A
  supervisor that launched the process directly (holds its PID) should
  just watch for the child exiting. One attached as a DPDK secondary to
  a *different* process's namespace (Part 2's case) has to use
  `rte_eal_primary_proc_alive()` instead - see Part 2.

### Restart

Two distinct mechanisms exist, and they are **not** interchangeable:

- `POST /api/reload` - the *graceful* path. Drains the input ring,
  joins every worker, tears down every stage, calls
  `rte_eal_cleanup()`, then `execv()`'s a fresh instance of itself
  (same PID) that re-reads whatever `--graph=PATH` now points to. This
  is the only supported way to pick up a graph change without
  restarting from outside. A supervisor driving a graph swap
  programmatically should call this (optionally preceded by
  `POST /api/graphs/load?name=X` to select which graph) rather than
  killing and relaunching the process itself - it's cheaper and it's
  the path this project actually tests.
- `SIGINT`/`SIGTERM` - triggers the *same* graceful shutdown sequence
  (drain, join, teardown, `rte_eal_cleanup()`) but does **not** re-exec
  - the process exits for good. This is what an external supervisor
  should send if it intends to relaunch the process itself with new
  arguments (a different `--graph=PATH`, a different `--workers=N`,
  etc.) rather than letting the process pick up a graph change on its
  own via `/api/reload`.
- **Do not `SIGKILL` a healthy instance if avoidable.** Skipping the
  graceful sequence skips `rte_eal_cleanup()`, which means the
  hugepage-backed segment and `/var/run/dpdk/<prefix>/` directory are
  left behind uncleanly - not fatal (the next launch's
  `--huge-unlink=existing` still purges it), but it means mid-shutdown
  state (a stage's own open file/socket, e.g. `forward_udp`'s UDP
  socket or `dump_binary`'s open file) isn't torn down cleanly either.
  Reserve `SIGKILL` for a genuinely hung process.

### Startup failure modes a supervisor should distinguish

loomtabulator refuses to start (non-zero exit, no partial startup) on:
a missing/invalid `--graph=PATH` graph (bad JSON, unknown stage type,
port-type mismatch - see `graph_config_load()`), a `--web-port` already
in use, or too few worker lcores for the requested `--workers=N`. These
are all **configuration** failures, not transient ones - a supervisor
that retries a crash-looping instance without changing its arguments
first will just loop forever. `--plugins-dir`/`--graphs-dir` being
missing/empty are **not** fatal (zero plugins loaded / directory
auto-created respectively) - don't treat those as startup failures.

## Part 2: Multi-instance daisy-chain design (proposed)

The motivating use case: several independent loomtabulator graphs,
each a genuinely separate pipeline (not just "more workers on one
graph" - `--workers=N` already covers that within a single process),
chained so one instance's output feeds the next instance's input,
across process and core boundaries.

### Topology

All instances in one chain share a **single DPDK multi-process
namespace** - the same `--file-prefix` - because DPDK offers no
supported way to hand a named `rte_ring` from one namespace to another;
sharing a ring means sharing a namespace. Within that namespace:

- Instance 1 is the DPDK **primary** for the chain (no `--proc-type`
  flag needed - a process auto-becomes primary if none already exists
  under that `--file-prefix`, and secondary if one does).
- Instances 2..N are DPDK **secondaries**: `--proc-type=secondary
  --file-prefix=<the chain's shared prefix>`.
- Every instance still needs its own disjoint `-l` core set, same rule
  as Part 1 - now doubly important since a naming collision or core
  overlap inside one shared namespace is a same-namespace collision,
  not just a same-host one.

### The `ring_output` stage (not yet built)

Symmetric to how `ring_input.c` already makes loomtabulator a ring
*consumer* today: a new stage type whose `process()` calls
`rte_ring_enqueue()` on a named output ring instead of `sendto()`
(`forward_udp`) or `fwrite()` (`dump_binary`). No new thread or lcore
needed for the stage itself - enqueueing is inline in whichever worker
is already handling that record, the same posture every existing
terminal stage already has.

**Naming contract**: a `ring_output` node's configured ring name must
match the *next* instance's own `input.ring_name` in *that* instance's
graph JSON - the same "producer and consumer must agree on a name"
contract `ring_input.h` already documents for the existing
single-consumer input ring, just applied one more hop down the chain.
loomtabulator does not and will not cross-validate two separate graph
files against each other; a mismatched name is a silent "consumer
never sees any records," not a startup error - a supervisor wiring up
a chain should treat ring-name agreement between adjacent graph files
as part of what it validates before launch, the same way it already
has to keep `-l` sets disjoint.

**Allocator correctness**: this introduces a second cross-process
allocator boundary of the same kind `df0accb` ("Fix cross-process ring
allocator mismatch") already had to fix once for the input ring - every
producer and consumer of a given ring must agree on the same
`rte_malloc()`/`rte_mempool` allocation scheme. Whoever implements
`ring_output` needs to re-verify this explicitly, not assume the
existing input-ring convention just extends for free.

### Health signal for the chain

Prefer `GET /status.json` polling per instance (Part 1's mechanism,
already works, no DPDK dependency) as the **primary** signal - a
supervisor that launched every instance already has each one's
`--web-port`. Layer `rte_eal_primary_proc_alive(config_file_path)` on
top as a defense-in-depth check specifically for the primary (instance
1): it's a `flock()`-based check on the primary's runtime config file,
true only while the primary is actually alive - useful because a
secondary's own HTTP server can stay transiently reachable for a brief
window after its upstream primary has already died, which
`/status.json` alone wouldn't catch. DPDK gives no push/callback
version of this - both signals are poll-only.

### The restart-ordering invariant (the one that matters most)

**If instance K restarts, every instance K+1..N downstream of it must
also restart. Instances 1..K-1 upstream must not.**

Why: instances 2..N hold DPDK secondary attachments into instance K's
memory namespace (if K=1) or depend on K still being alive to feed
their input ring (any K). A downstream instance's existing attachment
doesn't get invalidated by an *upstream* restart in the DPDK-memory
sense unless K=1 (the actual primary) - but it does stop receiving any
records either way, so from a "is this chain doing useful work"
standpoint, restarting K without cascading to K+1..N just leaves them
alive and idle, silently. Cascade regardless of which instance in the
chain triggered the restart.

**The sharpest version of this** is instance 1 specifically, because
of `--huge-unlink=existing` (on by default - see Part 1): the moment a
*new* instance 1 starts under the shared `--file-prefix`, DPDK purges
the *previous* instance 1's hugepage segments. If instances 2..N are
still running and attached at that moment, their mappings into those
now-unlinked-and-reallocated segments are invalidated out from under
them - substantially worse than Part 1's single-instance restart story,
where nothing else was ever attached to begin with. **Confirmed, not
just predicted**: calling `POST /api/reload` on instance 1 while a
secondary was still attached produced a real EAL panic mid-shutdown
(`EAL: PANIC in eal_thread_wait_command(): cannot read on configuration
pipe`), and the re-exec'd fresh instance then failed EAL init outright
(`Cannot allocate memzone list`) - instance 1 was left completely down,
not degraded. Graph state itself was unaffected (`POST /api/graph`
already wrote it to disk beforehand, independent of the reload), but
the process has to be relaunched by hand. **The supervisor
must fully stop instances 2..N before restarting instance 1**, not
after, and not concurrently. Restarting some instance K > 1 instead
only requires stopping K+1..N first (K itself isn't the primary, so
K's own restart doesn't touch the shared hugepage segments) - but the
same "downstream first, then the restarting instance" ordering still
applies for the "don't leave idle orphans" reason above.

### Explicitly out of scope for loomtabulator itself

loomtabulator will not grow: awareness of sibling instances, its own
`rte_eal_primary_proc_alive()` polling loop, or any self-triggered
cascading restart logic. This mirrors the project's own already-settled
"restart is the resolved answer, not hot-swap" position (see
`CLAUDE.md`'s Phase 3 design sketch) - keeping each instance simple and
pushing multi-instance orchestration entirely to the separate
supervisor project this spec is written for.

## Open items (need deciding when `ring_output` is actually built)

- Exact `ring_output` config schema (ring name, ring size - probably
  mirroring `graph_config.h`'s existing `input.ring_name`/`ring_size`
  shape) and which `stage_port_type`(s) it accepts as input (likely
  the same `raw_record`-or-broader set `forward_udp` already accepts,
  since a ring record is just bytes regardless of which upstream stage
  produced it).
- Whether an instance should expose its own `ring_output` target(s) in
  `GET /status.json` (e.g. an `"outputs": [{"ring_name": "...",
  "file_prefix": "..."}]` field) so a supervisor can verify chain
  topology against reality at startup, rather than trusting graph-file
  naming agreement blindly.
- Whether `graph_config_load()` should gain an optional, purely
  informational validation pass for `ring_output` (e.g. warn if the
  configured ring name looks malformed) even though it can never see
  the downstream instance's own graph file to fully cross-check it.
