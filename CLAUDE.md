# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working
with code in this repository.

## What this repo is

A pluggable data-processing pipeline: chrontabulator will eventually
feed it sorted, replayed capture records over a shared `rte_ring`
(DPDK multi-process); this project runs them through a chain of small
modules (validate, extract, convert raw-to-engineering, forward) and
transmits the result as UDP. See `README.md` for the user-facing
overview. The original planning doc for this project lived at
`~/.claude/plans/noble-kindling-lemon.md` (a Claude Code plan-mode
artifact, local to the machine/session that wrote it, not part of this
repo or git) - everything from it that still matters is folded into
this file and `README.md`'s Roadmap section, so that path is no longer
load-bearing for understanding the project.

**Status**: Phase 1 (single-core pipeline engine) and Phase 2
(multi-core worker pool with epoch/watermark barriers) are done. A
pipeline is a strictly linear chain (input ring -> N stages -> one
UDP-forwarding terminal stage), described by a hand-written JSON graph
file loaded at startup (`src/graph_config.c`), and now runs across
`--workers=N` worker lcores pulled competitively off a multi-consumer
input ring (`src/pipeline_worker.c`), gated by `src/epoch_barrier.c` at
barrier records so all of epoch N finishes before epoch N+1 starts.
Chrontabulator's replay feature doesn't exist yet, so a built-in
synthetic generator (`src/testgen.c`) feeds the input ring in its
place. Phase 3 (React Flow web UI) is scaffolded but not feature-complete
- see "Phase 3 design sketch" below for what exists vs. what's still open.

## Build

VS Code Dev Container (`.devcontainer/`), same shape as the sibling
`dpdk-app-example` repo - open in "Dev Containers: Reopen in Container",
or manually:
```
cd src && bear -- make && make test
```
Requires the `spdk-dpdk-ubuntu:26.05-local` base image built first (see
that repo's own Dockerfile) and standalone DPDK built inside the
container (`.devcontainer/setup.sh` does this automatically on first
launch).

The base image ships SPDK's source tree (which brings in DPDK as a
submodule) and a pre-built `isa-l`, but deliberately does **not**
compile DPDK itself - `setup.sh` does that standalone (`meson setup` +
`ninja ... install` into `/usr/local`, bypassing SPDK's own partial
internal DPDK build, which only covers the subset SPDK itself needs).
That compiled build lives in the container's own filesystem layer, not
a host bind-mount: a `docker start` on an existing container preserves
it (the `meson setup` guard in `setup.sh` just skips reconfiguring), but
recreating the container (`docker run`, or VS Code's "Rebuild Container")
wipes it back to the base image's pristine, DPDK-not-yet-built state. If
a session looks like it has a working project but DPDK suddenly seems
unbuilt, check that first, and confirm with
`pkg-config --exists libdpdk && pkg-config --libs libdpdk` /
`ldconfig -p | grep librte_eal` before assuming something else broke.

## Testing

Five tiers (`make test` in `src/`), same convention as
`dpdk-app-example`'s `tests/test_common.c`:
- `tests/test_stage_chain.c` - `validate`/`extract`/`convert` are
  deliberately mbuf-free (see `src/stage.h`'s header comment), so this
  runs as a plain host binary with zero DPDK involvement.
- `tests/test_graph_config.c` - exercises `graph_config_load()`'s
  schema/chain validation, linked against `tests/stub_stage_registry.c`
  (a frozen copy of the pre-plugin-conversion static 4-entry table)
  rather than the real `src/plugin_loader.c` - this tier is about
  `graph_config_load()`'s chain-building logic against a known-good
  registry, not the `dlopen()` machinery itself (that's
  `test_plugin_loader`'s job, below). `forward_udp_stage.c` transmits
  over a plain kernel UDP socket, not a DPDK-bound NIC (see that file's
  own header comment), so this entire tier links with zero DPDK
  involvement - a plain host binary, same as `test_stage_chain`.
- `tests/test_epoch_barrier.c` - `src/epoch_barrier.c`'s state machine
  is deliberately DPDK-free too (see its own header comment), so this
  runs real pthreads against a fake in-memory FIFO with zero EAL
  involvement, checking every data record lands in its correct epoch
  and that epoch boundaries never interleave.
- `tests/test_plugin_loader.c` - exercises the real
  `dlopen()`/`dlsym()`/ABI-version-check/name-collision logic in
  `src/plugin_loader.c` against small fixture plugins
  (`tests/plugin_fixtures/`) built into throwaway directories, never
  the real `plugins/`. Zero DPDK involvement, same posture as
  `test_stage_chain`.
- `tests/test_pipeline_workers.c` - the epoch/watermark guarantee, but
  end to end through a real multi-consumer `rte_ring` and
  `pipeline_worker.c`. Needs EAL init (`--no-huge --no-pci`) but no
  hardware - no port or mbuf pool involved at all, and the only tier
  that still needs DPDK.

Full end-to-end verification (no NIC needed at all - see "What's NOT
built yet" below for why - a plain UDP listener, or optionally
`dpdk-app-example --receiver` over a real NIC) is described in
`README.md`.

To run just one tier instead of all five, run its recipe line directly
from `src/` (each tier is a compile-then-run pair in the `test` target
of `src/Makefile`) - e.g. for `test_epoch_barrier` alone:
```
cd src && $(CC) -Wall -Wextra -I. -pthread -o build/test_epoch_barrier ../tests/test_epoch_barrier.c epoch_barrier.c && ./build/test_epoch_barrier
```
or, once already built once via `make test`, just re-run the binary
directly: `./build/test_epoch_barrier`.

## Conventions carried over from the sibling projects

- **Vendor, don't share a library.** `src/common.c`/`.h` (used today
  only for `app_parse_ipv4()` - see below) and `src/record.h` are
  copied from `dpdk-app-example`/`chrontabulator`, not pulled in as a
  submodule or shared library - each has a header comment naming where
  it came from. Keep them in sync by hand if the originals change;
  there's no automated mechanism for this, on purpose (matches how
  `chrontabulator` already vendors `dpdk-app-example`'s `common.c`).
  `src/port_init.c`/`.h` was vendored the same way for NIC bring-up,
  but was removed entirely once `forward_udp_stage.c` stopped needing a
  DPDK-bound NIC port at all (see "What's NOT built yet" below) - no
  vendored file should be kept around once nothing calls into it.
  `plugin-sdk/`'s four host-derived files (`stage.h`, `stage_abi.h`,
  `json.h`, `json.c` - what a third-party plugin author vendors into
  their own repo) are the one exception to "keep in sync by hand":
  they're **build-generated** copies of this repo's own `src/`
  originals (`make plugin-sdk` in `src/`), not hand-synced, because
  they come from this same repo at the same commit rather than a
  different repo on its own release cadence - generating them removes
  an entire "shipped a stale SDK" failure class for free. Don't hand-edit
  the copies in `plugin-sdk/`; regenerate instead.
- **Dynamic plugin loading via `dlopen()`.** Every stage type - built-in
  and third-party alike, no special-casing between them - is a `.so`
  file loaded at startup by `src/plugin_loader.c`, which scans
  `--plugins-dir` (default `../plugins`) and registers whatever it
  finds. This reverses the project's earlier "static composition, no
  dynamic loading" stance (the original reasoning mirrored
  `chrontabulator` statically linking its one NIC driver rather than
  using DPDK's `-d` plugin mechanism) - reversed deliberately so
  independent developers can build and drop in new stage types without
  touching this repo's source tree, and so an AI session writing a new
  stage only needs the small, frozen ABI in `src/stage_abi.h` +
  `src/stage.h`, not the whole codebase. The ABI contract (two exported
  functions per `.so`, `STAGE_ABI_VERSION` hard-equality-checked before
  anything else is touched, one stage per `.so`) is documented in
  `src/stage_abi.h`'s header comment and `plugin-sdk/README.md`.
  `dlopen()` is a real code-execution trust boundary - loomtabulator
  does no sandboxing or vetting of plugins beyond the ABI-version
  check, an accepted tradeoff given it already runs in a container,
  not something the loader tries to mitigate. `STAGE_ABI_VERSION` is
  currently `2` (added `struct stage.max_out_ports`,
  `struct stage_result.out_port`, `struct stage_record.flags` -
  groundwork requested by `docs/RECOMMENDATIONS.md` for a future
  multi-output-routing phase; `graph_config.c` rejects any stage
  declaring `max_out_ports != 1` today, since the pipeline engine only
  executes single-output chains so far) - see `plugin-sdk/README.md`
  for what changed and why multi-port stages aren't executable yet.
- **Startup-time validation over hot-path error handling.** A bad graph
  config is a refuse-to-run startup failure (`graph_config_load()`
  returning false with a clear message), never something discovered
  mid-run. Stage `process()` functions have exactly two outcomes -
  succeed, or a clean `ok=false` drop - no third "error" case; if a
  stage's `init()` accepted its config, `process()` is expected to
  always resolve one way or the other.
- **Real verification before calling something done.** Don't just
  confirm code compiles - build in the dev container, actually load a
  graph, actually send/receive real traffic, same standard the sibling
  projects hold themselves to. This no longer requires a NIC or a DPDK
  vdev at all (see "What's NOT built yet" below) - the input ring still
  needs a real EAL init (`--no-huge --no-pci` works fine, no hardware),
  but a plain UDP listener on the receiving end is enough to verify the
  output side.

## What's NOT built yet (don't assume otherwise)

- Web UI is scaffolded, not feature-complete: `web/` is a real
  Vite+React+`@xyflow/react` project with a palette (from
  `GET /api/stage-types`), a canvas that loads the currently-saved graph
  (`GET /api/graph`) and enforces port-type connection rules
  client-side, and a working Save button (`POST /api/graph`) that
  validates and writes the graph file to disk. Saving does **not**
  affect the already-running pipeline - see "Phase 3 design sketch"
  below for why hot-swap was deliberately not built, and what a restart
  needs to look like instead. No per-stage config editor yet either -
  a loaded node's `data.config` round-trips unedited through the UI.
- No real chrontabulator integration - `testgen.c`'s synthetic
  generator is the only input source that exists today. Phase 4.
- No `rte_reorder` - deliberately not used; the epoch/watermark barrier
  in `src/epoch_barrier.c` is the cheaper mechanism chosen instead (see
  that file's own header comment). Revisit only if testing ever shows
  it isn't enough.
- **Resolved**: `forward_udp_stage.c` used to build and transmit a raw
  Ethernet/IPv4/UDP frame via `rte_eth_tx_burst()` on a DPDK-bound NIC
  port (`src/port_init.c`, now deleted) - hardcoded to TX queue 0 with
  no locking, a real (not hypothetical) race once Phase 2 let multiple
  workers call it concurrently. The output/forward mechanism was always
  a genuinely open decision (DPDK TX vs. a kernel socket vs. writing
  timestamped files) - resolved in favor of a plain kernel UDP socket,
  one per stage instance, since this project's output was never
  actually tied to owning dedicated NIC hardware (the input side's
  `rte_ring` is the real external interface DPDK is needed for - see
  "What this repo is" above). This also resolves the race: POSIX
  guarantees `sendto()` on a shared socket fd from multiple threads is
  safe, with each call atomic with respect to the datagram's own
  contents - no locking needed, unlike the DPDK TX path it replaced.
  One practical effect: the binary no longer requires a NIC or a DPDK
  vdev to run at all - `--no-huge --no-pci` EAL args are enough (the
  input ring and worker-pool lcore launch still need a real EAL init,
  just no hardware).

Don't build ahead into Phase 3/4 without being asked - the schema
needed a chance to stabilize against real Phase 1/2 use before the web
UI locks it in, and that's now the case.

## Phase 3 design sketch: React Flow web UI

Scaffolded, not feature-complete - the mechanism below is real and
verified end to end (grep for these exact pieces, don't take this
section on faith):

- **`web/` directory**: a real Vite + React + `@xyflow/react` project
  (`@xyflow/react` is MIT, ~183KB min / ~58KB gzip per Bundlephobia at
  the time this was chosen). `web/src/App.tsx` renders a palette (from
  `GET /api/stage-types`), loads the currently-saved graph on open
  (`GET /api/graph`, via `web/src/graphApi.ts`), and shows a
  `<ReactFlow>` canvas that can add stage nodes and wire edges,
  enforcing the same `source.out_type == target.in_type` rule
  `graph_config.c` enforces at load time. Its Save button POSTs the
  current graph (`POST /api/graph`); the graph JSON schema
  (`testdata/example_graph.json`, validated by `src/graph_config.c`)
  echoes React Flow's own `nodes`/`edges` shape on purpose, so this
  serializes directly - no schema version bump needed for this phase.
  No per-stage config editor yet - a loaded node's `data.config` is
  carried through save unedited.
- **Backend**: `GET /api/stage-types` (`src/web_status.c`'s
  `handle_stage_types()`) serializes `src/plugin_loader.c`'s
  dynamically-populated table (one entry per successfully-loaded `.so`
  plugin - see "Dynamic plugin loading via `dlopen()`" above) via
  `plugin_loader.c:stage_port_type_name()` - the UI's palette and
  edge-validation both derive from this table alone, never from a
  stage's internal behavior. `GET /api/graph`
  serves the raw text of the last graph successfully saved (initially
  the `--graph=PATH` file's own contents at startup) - see
  `struct web_graph_ctx` in `web_status.h`. `POST /api/graph` validates
  the uploaded graph by calling the exact same `graph_config_load()`
  startup uses (same errors, same rules), then, on success, writes it
  to `--graph=PATH`'s file and updates what `GET /api/graph` serves -
  see "Live-reload was tried and reverted" below for why it stops
  there instead of also swapping the running pipeline. `web_status.c`
  also serves `web/dist/` as static files (`serve_static_file()`),
  directory controlled by `--web-root=PATH` (default `../web/dist`,
  matching the convention of running `./build/loomtabulator` from
  within `src/`; empty string disables static serving).
- **Live-reload was tried and reverted - restart is the resolved
  answer.** The open question CLAUDE.md previously flagged (hot-swap
  the running `pipeline_chain` vs. require a restart) was worked
  through in full: a hot-swap implementation (a `graph_swap.c` module,
  double-buffered `pipeline_chain_set`, and a `swap_pending` flag added
  to `epoch_barrier.c` alongside its existing `barrier_pending`) was
  built and initially verified working. But stress-testing it (per
  this project's own "validate hard with `-fsanitize=thread`" standard
  for `epoch_barrier.c`) surfaced a **pre-existing, already-shipped**
  Phase 2 race in `epoch_barrier.c`'s barrier-drain logic - its own
  header comment had flagged this exact class of race as a documented,
  accepted risk ("vanishingly unlikely... if stress testing ever
  surfaces it, the standard fix is..."). It was surfaced: about 1% of
  plain runs and ~12% of `-fsanitize=thread` runs hit it. Three
  successive fix attempts (a generation-counter grace period, an
  announce-before-check reordering, producer-side epoch tagging on
  `chrono_record_hdr.reserved`) each closed the specific bug just
  found and then stress-testing revealed a *different* one in the same
  area - including one variant of the race that predates all of this
  session's changes and is not specific to hot-swap at all (see
  git history/session notes for the full trace if this needs
  revisiting). Given three non-composable races found across three
  attempts, hot-swap was reverted entirely rather than ship a
  known-incomplete fix: `graph_swap.c`/`.h` were deleted,
  `epoch_barrier.c`/`.h` and `pipeline_worker.c`/`.h` are back to their
  original Phase 2 shape, and `POST /api/graph` only validates and
  saves to disk - the response includes `"restart_required": true`,
  and the web UI surfaces that to the user rather than claiming the
  change is live. **The underlying Phase 2 race is real and still
  unfixed** (present since Phase 2 shipped, independent of anything in
  this section) - it's a pre-existing, documented, narrow risk, not a
  regression from this work; revisit it deliberately, on its own,
  rather than as a side effect of a web UI feature.
- **Serving the built UI**: Vite's built `web/dist/` is multi-file and
  content-hashed, so it's served as plain static files
  (`serve_static_file()` in `src/web_status.c`) rather than embedded as
  this project family's usual "one big C string" status page.
- **Airgap requirement**: `.devcontainer/Dockerfile` installs Node 20 LTS
  via NodeSource's setup script (build-time only; `curl` added to the
  base apt install to support it) and `.devcontainer/setup.sh` runs
  `npm ci && npm run build` in `web/` after `make test`. Verified: `grep
  -rEo 'https?://[^"'"'"' )]*' web/dist/` is **not** empty, but every hit
  is either an inert XML/SVG namespace URI (`w3.org/2000/svg` etc. -
  identifiers passed to `createElementNS`, never fetched) or a
  react.dev/reactflow.dev doc-link baked into a library's dev-mode error
  message string - re-verify this distinction (inert string vs. an
  actual fetch/CDN `<script src>`) on any dependency bump rather than
  assuming a bare zero-hits grep. `web/package-lock.json` is committed
  so `npm ci` is reproducible.
- **Dev container port**: unchanged - the status/API server listens on
  8080 in-container, mapped to host port 8092 in
  `.devcontainer/devcontainer.json`. `web/vite.config.ts` also proxies
  `/api` and `/status.json` to `localhost:8080` under `npm run dev`, so
  the UI can be iterated on live against a running binary without
  rebuilding `web/dist/` each time.
