# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working
with code in this repository.

## What this repo is

A pluggable data-processing pipeline: chrontabulator will eventually
feed it sorted, replayed capture records over a shared `rte_ring`
(DPDK multi-process); this project runs them through a chain of small
modules (validate, extract, convert raw-to-engineering, forward) and
transmits the result as UDP. See `README.md` for the user-facing
overview and `~/.claude/plans/noble-kindling-lemon.md` for the full
design/phasing this was built from.

**v1 status**: single-process, single-core, no web UI. A pipeline is a
strictly linear chain (input ring -> N stages -> one UDP-forwarding
terminal stage), described by a hand-written JSON graph file loaded at
startup (`src/graph_config.c`). Chrontabulator's replay feature doesn't
exist yet, so a built-in synthetic generator (`src/testgen.c`) feeds
the input ring in its place.

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

Two tiers (`make test` in `src/`), same convention as
`dpdk-app-example`'s `tests/test_common.c`:
- `tests/test_stage_chain.c` - `validate`/`extract`/`convert` are
  deliberately mbuf-free (see `src/stage.h`'s header comment), so this
  runs as a plain host binary with zero DPDK involvement.
- `tests/test_graph_config.c` - exercises `graph_config_load()`'s
  schema/chain validation. Links DPDK (via `stage_registry.c`'s
  `forward_udp` table entry) but never calls into EAL-dependent code,
  so it still runs as a plain process, no special EAL flags needed.

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

- No multi-core worker pool, no `rte_reorder`, no epoch/watermark
  barriers - v1 is single-core and gets exact ordering for free from
  that. Phase 2.
- No web UI at all - no React Flow, no Vite build, no `web/` directory
  contents yet. Phase 3.
- No real chrontabulator integration - `testgen.c`'s synthetic
  generator is the only input source that exists today. Phase 4.

Don't build ahead into these phases without being asked - the project
plan deliberately sequenced them this way so the stage/graph schema has
a chance to stabilize against real v1 use before the web UI locks it in,
and so barrier-ordering logic gets built against a real multi-core
baseline instead of speculatively.
