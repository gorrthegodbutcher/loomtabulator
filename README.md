# loomtabulator

A pluggable data-processing pipeline, developed inside a VS Code Dev
Container built on top of the `spdk-dpdk-ubuntu` image. Plain DPDK, no
SPDK - like `dpdk-app-example`, it reads from a ring and writes UDP, no
block device involved.

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

## Scope so far (Phase 1 + 2 done)

CLI + a hand-written JSON graph file only, no web UI yet (Phase 3). A
pipeline is a strictly linear chain - one path from the input ring to
one UDP-forwarding stage at the end; branching pipelines are a later
phase. As of Phase 2, the chain runs across multiple worker lcores
(`--workers=N`) pulling competitively off the input ring, with an
epoch/watermark barrier (`src/epoch_barrier.c`) ensuring all of one
epoch's records finish before the next epoch's are considered started
- see `CLAUDE.md` for the mechanism.

```
                              +-> [worker 1: validate -> extract -> convert -> forward_udp] -+
[testgen thread] --rte_ring-> +-> [worker 2: ...........................................] -+--> NIC
                              +-> [worker N: ...........................................] -+
```

## Prerequisites

1. Build and tag the base image, from the sibling `spdk-dpdk-ubuntu` repo:
   ```
   cd ../spdk-dpdk-ubuntu
   docker build -f Dockerfile-single -t spdk-dpdk-ubuntu:26.05-local .
   ```
2. Allocate hugepages on the host:
   ```
   echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
   ```
3. Install VS Code's "Dev Containers" extension (`ms-vscode-remote.remote-containers`).

## Getting started

Open this folder in VS Code, "Dev Containers: Reopen in Container".
First launch builds standalone DPDK (a few minutes) and runs the test
suite; subsequent launches skip the DPDK build since it's already
configured.

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
  uptime). Default 8080, `0` disables it.
- `--mtu=BYTES` / `--force-10g` - same meaning as `dpdk-app-example`'s
  equivalent flags.
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

Example, against a real NIC bound to `vfio-pci`:
```
./build/loomtabulator -l 0-1 --no-shconf -a <pci-addr> -- \
  --graph=../testdata/example_graph.json --testgen-rate=1000
```

## Stage types (v1)

Compiled-in, see `src/stage_registry.c` for the full table and
`src/stage.h` for the interface every stage type implements:

- **`validate`** - confirms `chrono_record_hdr.magic`/`len` are sane.
  Config: `require_magic` (bool, default true).
- **`extract`** - pulls one fixed-offset, fixed-width big-endian field
  out of the payload. Config: `field_offset_bytes`, `field_width_bytes`
  (2, 4, or 8).
- **`convert`** - linear raw-to-engineering calibration:
  `engineering = raw * scale + offset`. Config: `scale`, `offset`.
- **`forward_udp`** - builds and transmits a UDP frame carrying the
  engineering value. Config: `dst_mac`, `src_ip`, `dst_ip`, `src_port`,
  `dst_port`, `hw_checksum` (bool, default true - same meaning as
  `dpdk-app-example`'s hardware checksum offload).

New stage types are added by implementing `struct stage`'s
init/process/teardown contract (see any file in `src/stages/`) and
registering it in `src/stage_registry.c` - no dynamic plugin loading
(`.so`, `dlopen`), matching how `chrontabulator` statically links its
one NIC driver rather than loading it at runtime.

## Verifying it end-to-end

With `dpdk-app-example --receiver` on another port/host as the
consumer (it already does UDP dst-port filtering and a clean summary):
```
./build/loomtabulator -l 0-1 -a <pci-addr> -- \
  --graph=../testdata/example_graph.json --testgen-count=1000
```
```
./dpdk-app-example -l 0-1 -a <other-pci-addr> -- \
  --receiver --port=12345
```

## Roadmap

Working now: the stage-chain mechanism, JSON graph loading/validation,
a synthetic input generator, real UDP forwarding with optional hardware
checksum offload, and (Phase 2) a multi-core worker pool with
epoch/watermark barriers for ordering across cores. Planned: a React
Flow web UI for visually building pipelines, served airgapped from a
Vite-built static bundle baked into the image (Phase 3 - see
`CLAUDE.md`'s "Phase 3 design sketch" for the concrete plan: schema,
new `GET /api/stage-types` + save endpoints, dev container/Node
changes, the airgap build contract); real integration with
chrontabulator's replay feature via a DPDK multi-process shared ring,
once that feature exists (Phase 4).

One known, documented gap from Phase 2 worth flagging here too:
`src/stages/forward_udp_stage.c` still transmits on a single hardcoded
TX queue with no locking, so it's not safe for a multi-worker graph
that ends in it as-is - see `CLAUDE.md`'s "What's NOT built yet" for
the full note. The real output/forward mechanism (DPDK TX vs. a kernel
socket vs. writing timestamped files) is still an open decision.
