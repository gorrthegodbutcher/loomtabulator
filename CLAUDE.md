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
place. No web UI yet - see "Phase 3" below.

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

## Testing

Four tiers (`make test` in `src/`), same convention as
`dpdk-app-example`'s `tests/test_common.c`:
- `tests/test_stage_chain.c` - `validate`/`extract`/`convert` are
  deliberately mbuf-free (see `src/stage.h`'s header comment), so this
  runs as a plain host binary with zero DPDK involvement.
- `tests/test_graph_config.c` - exercises `graph_config_load()`'s
  schema/chain validation. Links DPDK (via `stage_registry.c`'s
  `forward_udp` table entry) but never calls into EAL-dependent code,
  so it still runs as a plain process, no special EAL flags needed.
- `tests/test_epoch_barrier.c` - `src/epoch_barrier.c`'s state machine
  is deliberately DPDK-free too (see its own header comment), so this
  runs real pthreads against a fake in-memory FIFO with zero EAL
  involvement, checking every data record lands in its correct epoch
  and that epoch boundaries never interleave.
- `tests/test_pipeline_workers.c` - the same guarantee, but end to end
  through a real multi-consumer `rte_ring` and `pipeline_worker.c`.
  Needs EAL init (`--no-huge --no-pci`) but no hardware - no port or
  mbuf pool involved at all.

Full end-to-end verification (real NIC, `dpdk-app-example --receiver`
as the consumer) is described in `README.md`.

## Conventions carried over from the sibling projects

- **Vendor, don't share a library.** `src/common.c`/`.h`,
  `src/port_init.c`/`.h`, and `src/record.h` are copied from
  `dpdk-app-example`/`chrontabulator`, not pulled in as a submodule or
  shared library - each has a header comment naming where it came from.
  Keep them in sync by hand if the originals change; there's no
  automated mechanism for this, on purpose (matches how
  `chrontabulator` already vendors `dpdk-app-example`'s `common.c`).
- **Static composition, no dynamic plugin loading.** Every stage type
  is a compile-time entry in `src/stage_registry.c`'s table - no
  `dlopen`, no runtime `.so` loading for stage types, same reasoning as
  `chrontabulator` statically linking its one NIC driver rather than
  loading it via DPDK's `-d` plugin mechanism.
- **Startup-time validation over hot-path error handling.** A bad graph
  config is a refuse-to-run startup failure (`graph_config_load()`
  returning false with a clear message), never something discovered
  mid-run. Stage `process()` functions have exactly two outcomes -
  succeed, or a clean `ok=false` drop - no third "error" case; if a
  stage's `init()` accepted its config, `process()` is expected to
  always resolve one way or the other.
- **Real DPDK/hardware verification before calling something done.**
  Don't just confirm code compiles - build in the dev container,
  actually load a graph, actually send/receive real (or vdev) traffic,
  same standard the sibling projects hold themselves to.

## What's NOT built yet (don't assume otherwise)

- No web UI at all - no React Flow, no Vite build, no `web/` directory
  contents yet. Phase 3 - design sketch below.
- No real chrontabulator integration - `testgen.c`'s synthetic
  generator is the only input source that exists today. Phase 4.
- No `rte_reorder` - deliberately not used; the epoch/watermark barrier
  in `src/epoch_barrier.c` is the cheaper mechanism chosen instead (see
  that file's own header comment). Revisit only if testing ever shows
  it isn't enough.
- `src/stages/forward_udp_stage.c`'s `rte_eth_tx_burst()` call is
  hardcoded to TX queue 0 with no locking. Now that Phase 2 lets
  multiple workers call it concurrently, any graph ending in
  `forward_udp` has a real (not hypothetical) race there. Not fixed -
  the output/forward mechanism itself (DPDK TX vs. a kernel socket vs.
  writing timestamped files) is still an open decision, out of scope
  until that's settled.

Don't build ahead into Phase 3/4 without being asked - the schema
needed a chance to stabilize against real Phase 1/2 use before the web
UI locks it in, and that's now the case.

## Phase 3 design sketch: React Flow web UI

Not started. When picked up, the shape (already decided, not open
questions):

- **New `web/` directory**: a real Vite + React + `@xyflow/react`
  project (`@xyflow/react` is MIT, ~183KB min / ~58KB gzip per
  Bundlephobia at the time this was chosen - light enough that bundle
  size isn't a real constraint here). The graph JSON schema
  (`testdata/example_graph.json`, validated by `src/graph_config.c`)
  already echoes React Flow's own `nodes`/`edges` shape on purpose, so
  the editor serializes/deserializes it directly - no schema version
  bump needed for this phase.
- **Backend additions**: a `GET /api/stage-types` endpoint serializing
  `src/stage_registry.c`'s compile-time table (name, `in_type`,
  `out_type`) - the UI's palette and its "can I connect these two
  ports" edge-validation both derive from this table alone, never from
  a stage's internal behavior. A `POST` to save/replace the running
  graph, which re-runs the exact validation `graph_config_load()`
  already does (linear chain, port types match, known stage types) and
  rejects with the same clear errors on failure.
- **Live-reload is an open question, not decided**: whether saving a
  graph from the UI hot-swaps the running `pipeline_chain` (freeze
  workers at the next epoch boundary via `epoch_barrier.c`, swap,
  resume) or just requires a process restart is unresolved - the
  original plan flagged this as "Phase-2-dependent" and deferred it;
  Phase 2 now exists, so this is worth deciding explicitly before
  building the save path, not assuming either way.
- **Serving the built UI**: Vite's built `web/dist/` is multi-file and
  content-hashed, so it doesn't fit this project family's "one big C
  string" status-page convention (`dpdk-app-example`/`chrontabulator`'s
  `web_status.c`) - serve it as static files off disk instead, via a
  small extension of `src/web_status.c`'s existing HTTP-server pattern.
- **Airgap requirement (hard constraint, confirmed feasible)**: build
  the UI to `web/dist/` at Docker-image-build time only - the one point
  network access is already assumed, same as this project's `apt-get`/
  `git clone`/`pip install` today - then serve those static files with
  zero npm/Node/internet involvement at runtime. `.devcontainer/
  Dockerfile` needs Node/npm added as build-time-only tooling in this
  same layer (verify Ubuntu 24.04's bundled `nodejs`/`npm` is new
  enough for Vite/`@xyflow/react`; use NodeSource's setup script if
  not). Before shipping, `grep` the built `dist/` for `http`/`https` -
  zero CDN references anywhere in the output (unlike the Google Fonts
  links `dpdk-app-example`/`chrontabulator`'s existing status pages
  already have) - self-host fonts or use system fonts instead.
- **Dev container port**: the status/API server already listens on
  8080 in-container, mapped to host port 8092 in
  `.devcontainer/devcontainer.json` (8090 is `dpdk-app-example`'s,
  8091 is `chrontabulator`'s) - reuse this same port for the web UI,
  no new port needed.
