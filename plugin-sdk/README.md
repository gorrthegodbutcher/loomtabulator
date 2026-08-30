# loomtabulator plugin SDK

A loomtabulator stage type is a `.so` file loaded with `dlopen()` at
startup from `--plugins-dir` (default `../plugins`, relative to the
`loomtabulator` binary). This directory is everything needed to build
one, outside of the loomtabulator source tree entirely - no DPDK, no
loomtabulator checkout.

`stage.h`, `stage_abi.h`, `json.h`, and `json.c` in this directory are
generated copies of loomtabulator's own `src/` originals (see
`src/Makefile`'s `plugin-sdk` target) - always the same commit's
version as whatever's building the plugin loader itself. Don't hand-edit
them here; regenerate instead if you need a newer copy.

## Building a plugin

```
cc -O2 -Wall -Wextra -fPIC -I. -shared -o myname.so my_stage.c json.c
```

Every plugin embeds its own compiled `json.c` rather than resolving it
against the host binary at load time - your build has zero link-time
dependency on how loomtabulator itself was built. `dlopen()` is called
with `RTLD_NOW | RTLD_LOCAL`, so a plugin that forgets to include
`json.c` (or has any other undefined symbol) fails to load with a clear
error at startup, not a crash mid-run - and `RTLD_LOCAL` means two
plugins each embedding their own `json.c` never interpose on each
other.

Drop the resulting `.so` into loomtabulator's plugins directory. It's
picked up on the next startup (there is no hot-reload) and shows up as
a stage type in `GET /api/stage-types`, wireable into a graph exactly
like any built-in stage.

## The ABI

A plugin exports exactly two functions (see `stage_abi.h` for the full
contract):

```c
uint32_t loom_stage_abi_version(void);       /* return STAGE_ABI_VERSION */
const struct stage *loom_stage_entry(void);  /* &your_static_stage; NULL = reject */
```

`loom_stage_abi_version()` is checked first, before anything else is
touched from the handle - a mismatch is a clean rejection, not a
crash. `loom_stage_entry()` returns a pointer to one `static const
struct stage` describing your stage type: its name (must match a graph
node's `"type"` field), declared input/output port types (`stage.h`'s
`enum stage_port_type` - see "Input types" below for why `in_types` is
a set, not a single value), and four function pointers:

- `init(config)` - called once per graph node using this stage type,
  before any data flows. Receives that node's `data.config` JSON
  object. Return an opaque state pointer (or `NULL` if your stage needs
  no state), or `NULL` to signal a bad config - loomtabulator treats a
  failing `init()` as a startup-time error, never something discovered
  mid-run.
- `out_port_count(state)` - see "Output ports" below. Optional (`NULL`
  means "always 1 port") - the overwhelming majority of stages, single-
  output ones, never need to implement this at all.
- `process(state, in, out)` - the hot path, called once per record.
  `in->type` is guaranteed to be one of your declared `in_types` (see
  "Input types" below) - not always the same fixed value if you accept
  more than one, so branch on it yourself if the shapes genuinely
  differ. `*out` is
  zero-initialized before every call - a guarantee loomtabulator's
  pipeline runner commits to, not an incidental default - so any field
  you don't explicitly care about (currently only `flags`) already
  reads as its zero value on entry and can be left untouched. On
  success, still explicitly fill in the fields you *do* own (`type` =
  your `out_type`, `data`/`len`/`capture_tsc` always; `flags` only if
  you're intentionally setting a bit) and return `{.ok = true}`. Return
  `{.ok = false, .drop_reason = "..."}` to drop the record - a normal
  outcome (e.g. failed validation), not an error.
- `teardown(state)` - mirrors `init()`, called once per node at
  shutdown. Must handle a `NULL` state gracefully.

This is the entire per-instance lifecycle - there is no separate
module-level init/shutdown pair. A stage that needs several resources
from one node's config (multiple destination sockets, for example) sets
them all up inside its own `init()` and tears them all down inside its
own `teardown()`. Two graph nodes of the same plugin type get two
independent `init()` calls with two independent state pointers, so
per-node resources (e.g. `forward_udp`'s own outbound socket) never
collide between them.

## Input types

`struct stage.in_types` is a bitmask, not a single `enum
stage_port_type` value - a stage can accept more than one input type.
Build it from `PORT_TYPE_BIT(...)`:

```c
.in_types = PORT_TYPE_BIT(PORT_TYPE_RAW_RECORD) | PORT_TYPE_BIT(PORT_TYPE_WIRE_FRAME),
```

Most stages still only need one bit. `enum stage_port_type` only has
three values (`raw_record`, `engineering`, `wire_frame` - see
`stage.h`'s own comment for why `validated`/`extracted` were removed:
"has this been checked" and "has this been narrowed to a slice" are
`struct stage_record.flags` attributes now, not separate types), so
multi-bit `in_types` mostly matters for a stage like the built-in
`dump_binary` - the model case: it accepts `raw_record` and
`wire_frame` alike, because both are opaque byte blobs it writes
verbatim, with nothing to reinterpret. Nothing accepts `engineering` by
mistake this way - a double is a specific host-order numeric value, not
an opaque blob, so treating its raw bytes as one would be silently
wrong. If your stage accepts several types that need genuinely
different handling, check `in->type` inside `process()` to know which
one you actually got.

`graph_config.c` rejects wiring an edge whose upstream `out_type` isn't
one of your declared `in_types`, with an error naming what it actually
got and everything you accept.

## Output ports

Most stages have exactly one output - `process()` fills one `out`
record per call, and the graph always knows exactly where that record
goes next. Leave `out_port_count` as `NULL` (the default every stage in
this SDK except `example_router_stage.c` uses) and you never need to
think about this section again.

A stage that needs to route records to different destinations - a demux
by some field in the payload, for example - implements
`out_port_count(state)`, returning how many output ports *this graph
node* has. This is called once per node, right after that node's own
`init()` succeeds, so the count can depend on that node's own config
(e.g. how many entries are in a routing table) rather than being a
fixed constant for the whole plugin type. `process()` then sets
`struct stage_result.out_port` to pick which port `out` targets for
that record (`0` if you don't set it, so a single-output stage's
`process()` needs no changes at all).

Rules `graph_config.c` enforces at graph-load time, all worth knowing
before you wire a graph:

- Returning a value greater than `STAGE_MAX_OUT_PORTS` (`stage.h`) is a
  startup error.
- **Every port in `[0, out_port_count())` must have exactly one
  outgoing edge.** Declaring 3 ports but wiring only 2 fails to load -
  this exactness is the whole point of a per-instance count instead of
  a static ceiling.
- Returning `0` declares this node a **leaf** - the end of a path
  through the graph. A leaf must have zero outgoing edges; its
  `out_type` is unused and unconstrained (nothing ever reads a leaf's
  `*out`), whatever's most descriptive of what it actually does - the
  three built-in leaves each pick something different: `forward_udp`
  (transmits) uses `wire_frame`, `dump_binary`/`dump_text` (write to a
  file) use `raw_record`/`engineering` respectively. There's no
  required value.
- All of a node's output ports share that instance's single `out_type`
  - there's no per-port output type.

On the graph side, an edge picks which of its source node's ports it
comes from via an optional `"source_port"` integer field (default `0`
- see `testdata/example_branching_graph.json` in the main repo for a
worked graph using this).

## Optional: signaling integrity failures

`struct stage_record.flags` is a bitmask, currently defining one bit:
`STAGE_RECORD_FLAG_INTEGRITY_FAILED`. It's entirely opt-in - a stage
that never touches `out->flags` behaves exactly as before, since (per
"process" above) `out` arrives zero-initialized and a stage that
doesn't set the bit simply leaves it at that default. Set this bit on
your output record when you judge it bad (a failed checksum, an
out-of-range engineering value, whatever your stage checks) instead of
returning `{.ok = false}` outright - `ok = false` unconditionally drops
a record with no way to route it anywhere, so use the flag instead of a
hard drop whenever the record is still structurally usable and a graph
author might want to *do* something with the bad ones (capture them for
diagnostics, count them, whatever) rather than just lose them silently.

Since `STAGE_ABI_VERSION` 5, loomtabulator's own pipeline engine acts on
this bit automatically, for any stage that sets it - built-in or
third-party, no extra code on your part: the graph (not your stage)
decides whether a flagged record is dropped anyway (the default), still
passed on down its normal edge unchanged, or routed to a completely
separate downstream chain via a dedicated edge, configured per node in
the graph JSON (`"on_invalid": "drop"|"pass"` and an edge's
`"invalid_target": true` - see the main repo's `graph_config.h` for the
full schema). Your `process()` never declares extra output ports or
writes any routing logic for this - set the bit, and the graph handles
the rest. `in->type` is completely unaffected either way: a flagged
record is still the exact same `out_type` shape, just judged bad.

## Build rules

- **No bitfields, no `#pragma pack`** on anything in `struct stage`,
  `struct stage_record`, or `struct stage_result` (you don't define
  these structs, but keep this in mind if you build any config struct
  of your own that crosses the ABI boundary - it doesn't, today, but
  don't add one that does).
- **Bump `STAGE_ABI_VERSION`** (in `stage_abi.h`) on any change to the
  layout or meaning of those three structs - even a purely additive
  field. loomtabulator's loader does a hard equality check, never a
  compatibility-window check.
- **Build for the host's own target triple.** loomtabulator doesn't
  cross-plugin-arch-check beyond what `dlopen()` itself refuses to
  load.
- Both ABI exports are **functions, not data symbols**, deliberately -
  this protects against a plugin's own build flags (LTO, dead-symbol
  stripping) dropping an unreferenced-looking `const` global before
  `dlsym()` ever looks for it. Don't change that shape.

## Trust boundary

`dlopen()`-ing a `.so` runs arbitrary code in the loomtabulator
process. loomtabulator does no sandboxing or vetting of plugins beyond
the ABI-version check - that's an accepted tradeoff (loomtabulator
already runs inside a container), not something this SDK or the loader
tries to mitigate. Only load plugins you trust.

## Examples

- `example_stage.c` - a minimal worked passthrough stage (copies its
  input record to its output unchanged, with one optional `"verbose"`
  config field). Single output port, `out_port_count` left `NULL`.
- `example_router_stage.c` - a minimal worked multi-output router (see
  "Output ports" above), reading a config-driven `{"byte_value", "port"}`
  table plus a `"default_port"`. The template to copy and adapt if your
  stage needs to route records to different destinations.

Build either with the command at the top of its own file.
