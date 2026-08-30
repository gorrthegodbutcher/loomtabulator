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
real, loadable example. Every path through the tree still ends in a
UDP-forwarding stage. As of Phase 2, the chain runs across multiple worker lcores
(`--workers=N`) pulling competitively off the input ring, with an
epoch/watermark barrier (`src/epoch_barrier.c`) ensuring all of one
epoch's records finish before the next epoch's are considered started
- see `CLAUDE.md` for the mechanism.

```
                              +-> [worker 1: validate -> extract -> convert -> forward_udp] -+
[testgen thread] --rte_ring-> +-> [worker 2: ...........................................] -+--> UDP (kernel socket)
                              +-> [worker N: ...........................................] -+
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

- `--graph=PATH` (required) - the JSON pipeline definition to load. See
  `testdata/example_graph.json` for the v1 shape: `input` (the ring to
  create), `nodes` (stage instances, each a `type` matching a compiled-in
  stage and a `data.config` block), `edges` (must form a single linear
  chain in v1 - see `src/graph_config.c`'s validation).
- `--web-port=N` - `GET /status.json` (records in/dropped/forwarded,
  uptime), `GET /api/stage-types` (Phase 3 web UI's palette data),
  `GET /api/graph` / `POST /api/graph` (load/save the graph file - a
  save validates but does not affect the running pipeline; restart to
  apply it), and static files from `--web-root`. Default 8080, `0`
  disables it.
- `--web-root=PATH` - directory holding the built Phase 3 web UI
  (`web/dist/`) to serve as static files. Default `../web/dist`
  (matches running `./build/loomtabulator` from within `src/`); empty
  string disables static-file serving.
- `--plugins-dir=PATH` - directory scanned for stage-type `.so` plugins
  at startup (see "Stage types" below). Default `../plugins` (matches
  running `./build/loomtabulator` from within `src/`); a missing or
  empty directory loads zero plugins, not an error.
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
implements. The four built-ins below are rebuilt as plugins too
(`make plugins` in `src/`, producing `plugins/{validate,extract,
convert,forward_udp}.so`), loaded through the exact same mechanism a
third-party plugin uses - no special-casing between "built-in" and
"external":

- **`validate`** - confirms `chrono_record_hdr.magic`/`len` are sane.
  Config: `require_magic` (bool, default true).
- **`extract`** - pulls one fixed-offset, fixed-width big-endian field
  out of the payload. Config: `field_offset_bytes`, `field_width_bytes`
  (2, 4, or 8).
- **`convert`** - linear raw-to-engineering calibration:
  `engineering = raw * scale + offset`. Config: `scale`, `offset`.
- **`forward_udp`** - sends the engineering value as an 8-byte UDP
  payload over a plain kernel socket (one per stage instance, created
  at graph-load time). Config: `dst_ip`, `dst_port` (required),
  `src_ip`, `src_port` (optional - only needed to bind to a specific
  local address/port, e.g. on a multi-homed host; otherwise the kernel
  picks the route and an ephemeral port itself, like any ordinary UDP
  client).

New stage types are built entirely outside this repo: implement
`struct stage`'s init/process/teardown contract (plus an optional
`out_port_count(state)` if your stage routes records to more than one
destination - see `plugin-sdk/README.md`'s "Output ports" and
`testdata/example_branching_graph.json` for a worked example), export
the two `stage_abi.h` functions, and build against `plugin-sdk/` (a
small, frozen ABI - `stage.h`, `stage_abi.h`, `json.h`/`.c` - with zero
DPDK dependency; see `plugin-sdk/README.md` for the full build rules
and two worked examples). Drop the resulting `.so` into `--plugins-dir`
and
it's picked up on the next startup - no rebuild of loomtabulator
itself, no source-tree changes. `dlopen()` is a real code-execution
trust boundary; loomtabulator does no sandboxing or vetting of plugins
beyond an ABI-version check, an accepted tradeoff given it already
runs in a container.

## Verifying it end-to-end

No NIC, vdev, or even a second machine needed - `forward_udp` sends
over a plain kernel UDP socket, so any ordinary UDP listener on the
same host works. `testdata/example_graph.json` ships pointed at
`127.0.0.1:12345` by default. Simplest local check:
```
nc -ul 12345
```
```
./build/loomtabulator -l 0-3 --no-huge --no-pci -- \
  --graph=../testdata/example_graph.json --testgen-count=1000
```
You should see 1000 lines of binary garbage (raw 8-byte big-endian
doubles) arrive on the `nc` side - if you want a real decode, a couple
of lines of Python (`struct.unpack('>d', sock.recv(8))`) will do it.
For cross-host or real-NIC verification instead, point `dst_ip`/
`dst_port` at another machine and use `dpdk-app-example --receiver`
there (it already does UDP dst-port filtering and a clean summary) -
nothing about `loomtabulator` itself requires that, it's just a more
capable receiver than `nc` if you want packet-loss/ordering stats.

`testdata/example_branching_graph.json` demonstrates real multi-port
routing (see "Stage types" above) - it needs
`plugin-sdk/example_router_stage.c` built and dropped into
`--plugins-dir` first (it's not one of the four built-ins `make
plugins` produces). Point two listeners at `127.0.0.1:12345` and
`:12346`; running it with `--testgen-count=300` sends 298 records to
the first and 2 (the records whose counter value's low byte is `42`)
to the second - proof the routing decision, not just the graph
validation, actually executes.

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
