# loomtabulator

A pluggable data-processing pipeline, developed inside a VS Code Dev
Container built on top of the `spdk-dpdk-ubuntu` image. Plain DPDK, no
SPDK - it reads from a `rte_ring` and writes UDP over a plain kernel
socket, no block device and no dedicated NIC/DPDK-vdev involved. This
means no physical or virtual hardware port is required to build, run,
or test it - the only real hardware consideration is CPU core
isolation for the worker lcores (see `--workers=N` below), a host-level
setup step independent of anything in this repo.

**The goal**: consume chrontabulator's eventual sorted-replay output
(records read back off NVMe, sorted by capture time) through a chain of
small, composable processing modules - validate, extract a field,
convert raw counts to engineering units, forward as UDP - and
eventually let that chain be visually built and edited from a web UI
(React Flow, Phase 3 - see `CLAUDE.md`'s "Phase 3 design sketch" for
the full plan). Chrontabulator doesn't have a replay feature yet, so
the pipeline **stands on its own**: a built-in synthetic record
generator (`testgen`) feeds it instead, so the whole mechanism can be
built and verified before that integration exists.

Named in the same spirit as `chrontabulator` - this is its sibling,
"weaving" processing modules together into a finished, forwarded
product.

## Scope so far (Phase 1 + 2 done, Phase 3 scaffolded)

A pipeline is a tree rooted at the input ring: most nodes have exactly
one output, but a stage can declare more than one (see "Stage types"
below and `plugin-sdk/README.md`'s "Output ports") and route each
record to one of them - `testdata/example_branching_graph.json` is a
real, loadable example. Every path through the tree ends in a leaf -
either a UDP-forwarding stage or a file-dumping one (see "Stage types"
below for both). As of Phase 2, the chain runs across multiple worker lcores
(`--workers=N`) pulling competitively off the input ring, with an
epoch/watermark barrier (`src/epoch_barrier.c`) ensuring all of one
epoch's records finish before the next epoch's are considered started
- see `CLAUDE.md` for the mechanism.

```
                              +-> [worker 1: validate -> extract -> convert -> dump_text] -+
[testgen thread] --rte_ring-> +-> [worker 2: ......................................... ] -+--> a file, or UDP
                              +-> [worker N: ......................................... ] -+
```

## Prerequisites

1. Build and tag the base image, from the sibling `spdk-dpdk-ubuntu` repo:
   ```
   cd ../spdk-dpdk-ubuntu
   docker build -f Dockerfile-single -t spdk-dpdk-ubuntu:26.05-local .
   ```
2. Allocate hugepages on the host - only needed if running without
   `--no-huge` (see "Usage" below; no NIC/vdev is ever required, with
   or without hugepages):
   ```
   echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
   ```
3. Install VS Code's "Dev Containers" extension (`ms-vscode-remote.remote-containers`).

## Getting started

Open this folder in VS Code, "Dev Containers: Reopen in Container".
First launch builds standalone DPDK (a few minutes) and runs the test
suite; subsequent launches skip the DPDK build since it's already
configured. `make` (in `src/`) builds both the `loomtabulator` binary
and the built-in stage plugins (`make shared` + `make plugins`,
producing `plugins/*.so`); `loomtabulator` won't have any stage types
available unless `plugins/` exists and is populated, or
`--plugins-dir` points somewhere that is.

## Usage

```
./loomtabulator <EAL args> -- --graph=PATH [options]
```

`main.c` injects `--file-prefix=loomtabulator --huge-unlink=existing`
into the EAL args automatically unless you've already passed your own
(an explicit override always wins) - this namespaces this process's own
DPDK runtime/hugepage state away from any other DPDK app on the same
host (this repo's own test suite included) and, on every startup,
has DPDK purge whatever a *previous, already-exited* loomtabulator run
left behind (hugepage-backed segment files and the
`/var/run/dpdk/<prefix>/` runtime directory are NOT removed by a clean
shutdown by default, and otherwise accumulate indefinitely across
restarts). Skipped automatically if you pass `--no-huge` yourself
(hugepages aren't in play at all then).

- `--graph=PATH` (required) - the JSON pipeline definition to load. See
  `testdata/example_graph.json` for the basic shape: `input` (the ring
  to create), `nodes` (stage instances, each a `type` matching a loaded
  plugin and a `data.config` block), `edges` (`source`/`target` node
  ids, plus an optional `source_port` for a multi-output stage - must
  form a tree with exactly one root and no fan-in - see
  `src/graph_config.c`'s validation, and `testdata/example_branching_graph.json`
  for a worked multi-port example).
- `--web-port=N` - `GET /status.json` (records in/dropped/forwarded,
  uptime), `GET /api/stage-types` (Phase 3 web UI's palette data),
  `GET /api/graph` / `POST /api/graph` (load/save the *active* graph
  file - a save validates but does not affect the running pipeline),
  `GET/POST /api/graphs`, `POST /api/graphs/load`, `DELETE /api/graphs`
  (the graph *library* in `--graphs-dir` - see below),
  `POST /api/reload` (gracefully restarts the process in place -
  same PID, via `execv()` - applying whatever graph is currently
  saved; the web UI's Save panel has a matching Reload button), and
  static files from `--web-root`. Default 8080, `0` disables it.
- `--web-root=PATH` - directory holding the built Phase 3 web UI
  (`web/dist/`) to serve as static files. Default `../web/dist`
  (matches running `./build/loomtabulator` from within `src/`); empty
  string disables static-file serving.
- `--plugins-dir=PATH` - directory scanned for stage-type `.so` plugins
  at startup (see "Stage types" below). Default `../plugins` (matches
  running `./build/loomtabulator` from within `src/`); a missing or
  empty directory loads zero plugins, not an error.
- `--graphs-dir=PATH` - a library of named `.json` graphs the web UI's
  "Manage graphs..." dialog can list, upload into, load from (making
  one the *active* graph - same "writes to `--graph=PATH`, restart
  required" effect Save has - see `POST /api/graphs/load` above), and
  delete from. Distinct from `--graph=PATH` itself, which always names
  the one currently-active graph, not this directory. Default `../graphs`
  (matches running `./build/loomtabulator` from within `src/`); created
  automatically if missing. Gitignored (`/graphs/`) - it's local,
  per-deployment state, not checked-in example content (same posture as
  `testdata/gfp_router_graph.json` above).
- `--workers=N` - worker lcores to run the pipeline on. Default: EAL
  lcore count minus 1 (the main lcore is orchestration-only - status
  ticks and shutdown, never blocked inside a barrier drain). Must be
  `<=` that.
- `--testgen-rate=N` / `--testgen-count=N` / `--testgen-payload=N` -
  the synthetic input generator standing in for chrontabulator's replay
  (see "Scope so far" above). Payload is an 8-byte big-endian
  incrementing counter at offset 0, zero-padded after that - matches
  `testdata/example_graph.json`'s `extract` stage config out of the box.
- `--testgen-barrier-every=N` - insert an epoch barrier record every N
  data records (0 = never, the default). Manual smoke-test aid for the
  epoch/watermark barrier - watch stderr for "drained barrier" lines to
  confirm epochs are advancing at a sane cadence.

Example - no NIC/vdev needed, runs anywhere (see "Verifying it end to
end" below for what to point at the receiving UDP port):
```
./build/loomtabulator -l 0-3 --no-huge --no-pci -- \
  --graph=../testdata/example_graph.json --testgen-rate=1000
```

## Stage types (v1)

Loaded as `.so` plugins at startup (`dlopen()`, scanned from
`--plugins-dir`) - see `src/plugin_loader.c` for the loading mechanism
and `src/stage.h`/`src/stage_abi.h` for the interface every stage type
implements. The seven built-ins below are rebuilt as plugins too
(`make plugins` in `src/`, producing `plugins/{validate,extract,
convert,forward_udp,dump_binary,dump_text,null_sink}.so`), loaded
through the exact same mechanism a third-party plugin uses - no
special-casing between "built-in" and "external":

- **`validate`** - confirms `chrono_record_hdr.magic`/`len` are sane.
  Config: `require_magic` (bool, default true). A `len` mismatch (or a
  record shorter than the header) is always a hard drop - the record
  isn't even parseable. A bad magic is different: the record's shape is
  still trustworthy, so it's flagged (`STAGE_RECORD_FLAG_INTEGRITY_
  FAILED`, see "Invalid-record routing" below) and passed through
  rather than dropped outright - what actually happens to it from there
  is the graph's call, not this stage's.
- **`extract`** - two modes, via `mode` (`"numeric"`, the default, or
  `"bytes"`). `"numeric"` pulls one fixed-offset, fixed-width big-endian
  field out of the payload and re-encodes it as a canonical 8-byte
  value (config: `field_offset_bytes`, `field_width_bytes` - 2, 4, or
  8) - the only mode `convert` (below) can consume, though the type
  system doesn't enforce that anymore (see "Type simplification"
  below); its own runtime length check does. `"bytes"` copies a byte
  range verbatim, any offset/length, with no reinterpretation (config:
  `field_offset_bytes`, `field_length_bytes`) - for slicing off a
  prefix (e.g. `chrono_record_hdr`) before handing the rest to another
  stage.
- **`convert`** - linear raw-to-engineering calibration:
  `engineering = raw * scale + offset`. Config: `scale`, `offset`.
- **`forward_udp`** - a leaf. Sends a record's bytes verbatim as a UDP
  payload over a plain kernel socket (one per stage instance, created
  at graph-load time). Accepts `raw_record` (an opaque byte blob -
  shipped as-is, whatever length it happens to be) but deliberately not
  `engineering` (a double needs an explicit encoding step first - see
  `dump_text` below, or `forward_udp_stage.c`'s own header comment for
  why). Config: `dst_ip`, `dst_port` (required), `src_ip`, `src_port`
  (optional - only needed to bind to a specific local address/port,
  e.g. on a multi-homed host; otherwise the kernel picks the route and
  an ephemeral port itself, like any ordinary UDP client).
- **`dump_binary`** - a leaf. Writes every record's bytes verbatim to a
  file, back to back, no framing added - accepts `raw_record`/
  `wire_frame`, a generic "capture whatever's flowing at this point"
  debugging tool. Config: `path` (required; truncated on open).
- **`dump_text`** - a leaf, and the built-in consumer of `convert`'s
  output. Formats each `engineering` value as ASCII text (`%.17g`)
  followed by a literal carriage return (`\r`, not `\n` - see
  `dump_text_stage.c`'s own comment), one value per record. Config:
  `path` (required; truncated on open).
- **`null_sink`** - a leaf. The data-sink stage: accepts literally
  anything (`raw_record`/`engineering`/`wire_frame` alike - every port
  type that exists), counts records and bytes received (see "Per-stage
  status reporting" below), and discards the data - no file, no socket,
  no side effect beyond those two counters. Useful as a graph
  terminator when you want to measure what's flowing through a branch
  (or drop what a `validate`/`extract` stage flags as invalid - see
  "Invalid-record routing" below) without actually persisting or
  transmitting it anywhere. No config.

### Type simplification (version 5)

`enum stage_port_type` has three values now - `raw_record`,
`engineering`, `wire_frame`. `validated` and `extracted` are gone:
both only ever described the same wire shape as `raw_record` (an
opaque byte blob), differing just in "has this been checked" or
"has this been narrowed to a sub-slice" - not real shape differences,
and `extract`'s own `"bytes"` mode proved the model actively harmful
(it made `extracted` describe two incompatible things depending on
config). "Has this record passed a check" moved to `struct
stage_record.flags` (`STAGE_RECORD_FLAG_INTEGRITY_FAILED`) instead - an
attribute of the data, not a type of it, and one that stacks (a future
range-check stage can flag independently of whatever `validate` already
flagged). Chain position still tells you what's been checked, exactly
as before: nothing reaches `convert` without going through `extract`
first, enforced by `in_types` membership, not a dedicated type.

### Invalid-record routing (version 5)

Any stage - built-in or third-party - that sets
`STAGE_RECORD_FLAG_INTEGRITY_FAILED` on its output gets flagged-record
routing for free, decided by the *graph*, not the stage. Per node, in
the graph JSON:

- `"data": {"on_invalid": "drop"}` (the default, omit to get this) -
  a flagged record is dropped, same as an ordinary `ok=false` result.
- `"data": {"on_invalid": "pass"}` - a flagged record continues down
  the node's normal edge unchanged, flag still set.
- An edge with `"invalid_target": true` (instead of `"source_port"`) -
  a flagged record goes down this dedicated edge instead, to a
  completely different downstream chain (e.g. a `dump_binary`
  quarantine file). Mutually exclusive with `"source_port"` on the same
  edge; at most one per source node; overrides `"on_invalid"` if both
  are present.

`validate` is the first (and so far only) built-in to set this bit -
see its own description above. `testdata/example_invalid_routing_graph.json`
is a worked example (`validate` wired to both `forward_udp`, its normal
path, and a `dump_binary` quarantine file via `invalid_target`). The web
UI (Phase 3) surfaces both knobs too: each stage node grows a second,
red/dashed source handle for its invalid-record edge, and a right-click
"On invalid: drop/pass" toggle in its context menu.

### Per-stage status reporting (version 6)

Any stage can optionally implement `struct stage.get_status(state,
out)`, filling a small fixed-size struct with named `uint64_t` counters
(e.g. `records_checked`, `bytes_written`) - `NULL` (the default) means
"nothing to report." Polled periodically - `--status-poll-interval=N`
seconds (default 2; deliberately slow, not a hot-path concern) - from a
new `GET /api/stage-status`, which reports every node's current
counters keyed by the graph's own node ids:

```json
[
  { "node_id": "n1", "type": "validate",
    "fields": [ { "name": "records_checked", "value": 1042 },
                { "name": "records_flagged", "value": 3 } ] }
]
```

`validate` (`records_checked`/`records_flagged`) and `dump_binary`
(`records_written`/`bytes_written`) are the two built-in reference
implementations. Since a node instance's state is already concurrently
written by every worker lcore that routes a record to it, any stage
exposing a counter this way needs atomics, same as those two use. The
web UI polls this too: hovering a node with status shows it in a native
tooltip, and a docked, collapsible **Status** panel on the right keeps
a live-refreshing card per reporting node - large values are scaled
with byte (`B`/`KB`/`MB`/`GB`/`TB`) or engineering (`K`/`M`/`B`/`T`)
unit suffixes, since some counters run into the trillions.

New stage types are built entirely outside this repo: implement
`struct stage`'s init/process/teardown contract - a stage can accept
more than one input type (`in_types` is a bitmask, see
`plugin-sdk/README.md`'s "Input types") and route to more than one
output (an optional `out_port_count(state)` - see "Output ports" and
`testdata/example_branching_graph.json` for a worked example) - export
the two `stage_abi.h` functions, and build against `plugin-sdk/` (a
small, frozen ABI - `stage.h`, `stage_abi.h`, `json.h`/`.c` - with zero
DPDK dependency; see `plugin-sdk/README.md` for the full build rules
and two worked examples). Drop the resulting `.so` into `--plugins-dir`
and it's picked up on the next startup - no rebuild of loomtabulator
itself, no source-tree changes. `dlopen()` is a real code-execution
trust boundary; loomtabulator does no sandboxing or vetting of plugins
beyond an ABI-version check, an accepted tradeoff given it already
runs in a container.

A `.so` doesn't have to be exactly one stage type. A **loomlet** - a
plugin bundling a family of related stage types together into one
build (shared helper code, one file to drop into `--plugins-dir`
instead of one per stage type) - exports `loom_stage_entry_at(index)`
instead of the ordinary single-stage `loom_stage_entry()`; loomtabulator
calls it with `index = 0, 1, 2, ...` until it returns `NULL`, registering
each stage it hands back exactly like a single-stage plugin's one stage
would be. This is an addition to the *loading protocol*, not a
`STAGE_ABI_VERSION` bump - it doesn't touch `struct stage`'s own layout,
so every existing single-stage plugin (every built-in included) needs no
changes at all. See `plugin-sdk/README.md`'s "Loomlets" section for the
full contract and a worked example.

## Verifying it end-to-end

No NIC, vdev, or even a second machine needed. `testdata/example_graph.json`
(`validate -> extract -> convert -> dump_text`) needs nothing but a
file to inspect afterward:
```
./build/loomtabulator -l 0-3 --no-huge --no-pci -- \
  --graph=../testdata/example_graph.json --testgen-count=1000
cat build/example_output.txt   # or wherever its "path" config points
```
You should see 1000 calibrated values, one per line-ish (each
`\r`-terminated, not `\n` - see `dump_text`'s own entry above), each
the running counter times `0.001`.

For a UDP-forwarding example instead, `forward_udp` sends over a plain
kernel UDP socket, so any ordinary UDP listener works:
```
nc -ul 12345
```
Wire a `forward_udp` node into a graph pointed at `127.0.0.1:12345`
(e.g. swap `example_graph.json`'s `dump_text` node for one, since
`extract`'s raw byte-blob output is one of `forward_udp`'s accepted
types directly - no `convert` step needed) and run the same way. For
cross-host or real-NIC verification instead, point `dst_ip`/`dst_port`
at another machine and use `dpdk-app-example --receiver` there (it
already does UDP dst-port filtering and a clean summary) - nothing
about `loomtabulator` itself requires that, it's just a more capable
receiver than `nc` if you want packet-loss/ordering stats.

`testdata/example_branching_graph.json` demonstrates real multi-port
routing (see "Stage types" above) - it needs
`plugin-sdk/example_router_stage.c` built and dropped into
`--plugins-dir` first (it's not one of the six built-ins `make
plugins` produces). Point two listeners at `127.0.0.1:12345` and
`:12346`; running it with `--testgen-count=300` sends 298 records to
the first and 2 (the records whose counter value's low byte is `42`)
to the second - proof the routing decision, not just the graph
validation, actually executes.

## Sizing the input ring, and benchmarking sustained throughput

`--graph=PATH`'s `input.ring_size` (a `rte_ring`, so must be a power of
2) trades off two genuinely separate things: whether the pipeline can
*sustain* a given record rate at all (a function of per-record
processing cost and worker count, not ring size), and how long the
ring can absorb a *stall* in the consumer (a worker blocked in
`epoch_barrier.c`'s drain, a scheduling hiccup, etc.) before the
producer's `rte_ring_enqueue()` starts failing and records are
dropped. Ring slots are just pointers (8 bytes each), so sizing
generously costs next to nothing in memory - the real question is how
many milliseconds of stall you want to tolerate at your target rate.

**Worked example**: 6.75 Gbps aggregate GFP ingestion, 1024-byte
client payload, spread across multiple CIDs. Per-record size on
`LOOM_INPUT_RING` is 32 bytes (`chrono_record_hdr`) + 12 bytes (GFP
core header: PLI/cHEC/Type/tHEC/CID/Spare/eHEC, no optional pFCS) +
1024 bytes payload = **1068 bytes/record**. 6.75 Gbps ÷ 8 ÷ 1068 ≈
**790,225 records/sec**, i.e. one record roughly every 1.27
microseconds, sustained. At that rate, `ring_size: 4096` only buffers
≈5.2ms before a stall causes drops; `ring_size: 8192` gives ≈10.4ms of
margin - bump further (16384, ≈20.8ms) if your deployment's worst-case
stall is longer than that. (`testdata/gfp_router_graph.json` is a
local, gitignored graph tuned this way for one specific deployment -
not a checked-in example; use `testdata/example_branching_graph.json`
as the template for a graph of your own.)

**Benchmarking whether a target rate is actually sustainable** (the
question ring size alone can't answer) needs a real load generator
feeding the ring, not just a bigger buffer:
```
# terminal 1 - the pipeline itself (substitute your own graph)
./build/loomtabulator -l 1,2 --no-pci -- \
  --graph=../testdata/your_graph.json --plugins-dir=../plugins \
  --web-port=8080 --workers=1

# terminal 2 - a real DPDK secondary-process producer (a separate,
# unpublished internal tool - any producer attaching to the same named
# ring works, testgen.c included, as long as it can hit line rate)
./gfp_test_driver -l 3 --proc-type=secondary --no-pci \
  --file-prefix=loomtabulator -- --rate=0 --count=0 \
  --cids=1,2,3,4 --payload-size=1024

# terminal 3 - sample records_forwarded twice a few seconds apart
curl -s localhost:8080/status.json
```
`(records_forwarded delta) / (time delta)` is the sustained rate;
`records_dropped` climbing means the ring is filling faster than
workers drain it - the actual signal to raise `--workers=N` or
investigate per-record overhead in `src/pipeline_worker.c`'s hot loop,
not just the ring size. Measured on this codebase as of the
`gfp_extract` plugin's addition: **3.13M rec/s at `--workers=1`**,
**4.16M rec/s at `--workers=2`** (sub-linear scaling - `epoch_barrier.c`'s
shared `in_flight` atomic is contended across cores, a known, deliberately
deferred cost - see that file's own header comment) - both comfortably
over the 790,225 rec/s target with zero drops, so no code changes were
needed to hit this particular rate. Redo this measurement rather than
trusting last session's numbers if you change `pipeline_worker.c`,
change stage plugins on the hot path, or target a meaningfully higher
rate.

## Roadmap

Working now: the stage-tree mechanism (including multi-port routing -
a stage can send different records to different downstream stages, see
"Stage types" above), JSON graph loading/validation, a synthetic input
generator, real UDP forwarding over a plain kernel socket (no
NIC/DPDK-vdev needed - see "Verifying it end-to-end" above), (Phase 2)
a multi-core worker pool with epoch/watermark barriers for ordering
across cores, and (Phase 3, scaffolded) a React Flow web UI -
a real Vite+React+`@xyflow/react` canvas served airgapped as static
files from `web/dist/`, that loads the current graph
(`GET /api/graph`), lets you edit it, and saves it back
(`POST /api/graph`, validated the same way `--graph=PATH` is at
startup). Saving does not affect the already-running pipeline - a
restart picks up the saved file. See `CLAUDE.md`'s "Phase 3 design
sketch" for why a live hot-swap was tried and deliberately reverted
(it surfaced a real, pre-existing concurrency bug in the Phase 2
worker pool that's still open, independent of the web UI). Also
planned: real integration with chrontabulator's replay feature via a
DPDK multi-process shared ring, once that feature exists (Phase 4).
