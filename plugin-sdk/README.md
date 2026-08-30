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
`enum stage_port_type`), `max_out_ports` (see "Output ports" below),
and three function pointers:

- `init(config)` - called once per graph node using this stage type,
  before any data flows. Receives that node's `data.config` JSON
  object. Return an opaque state pointer (or `NULL` if your stage needs
  no state), or `NULL` to signal a bad config - loomtabulator treats a
  failing `init()` as a startup-time error, never something discovered
  mid-run.
- `process(state, in, out)` - the hot path, called once per record.
  `in->type` is always your declared `in_type`. On success, fill in
  `*out` (type = your `out_type`, `data`/`len`/`capture_tsc` set) and
  return `{.ok = true}`. Return `{.ok = false, .drop_reason = "..."}`
  to drop the record - a normal outcome (e.g. failed validation), not
  an error.
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

## Output ports

`struct stage.max_out_ports` declares how many output ports your stage
type has. Every stage in this SDK - and every stage you're likely to
write - sets `max_out_ports = 1`: `process()` fills exactly one `out`
record per call, `struct stage_result.out_port` is always `0`, and
that's the only value loomtabulator's graph loader currently accepts.

Declaring `max_out_ports > 1` (and setting `stage_result.out_port` to
pick which one `out` targets) builds and loads fine - the ABI supports
it - but **`graph_config.c` currently rejects wiring such a stage into
a graph at all**, with an error naming "multi-port routing isn't
executable yet." Routing execution (a stage picking one of several
downstream destinations per record) is planned but not yet
implemented in loomtabulator's pipeline engine. If your use case needs
this, declare `max_out_ports` honestly for forward compatibility, but
don't expect a graph using it to load successfully yet.

## Optional: signaling integrity failures

`struct stage_record.flags` is a bitmask, currently defining one bit:
`STAGE_RECORD_FLAG_INTEGRITY_FAILED`. It's entirely opt-in - a stage
that never touches `out->flags` behaves exactly as before, and every
record's `flags` defaults to `0` ("no flags"). Set this bit on your
output record when you're deliberately passing through a known-suspect
payload instead of dropping it outright (for example, a checksum
validation failure where you'd rather your caller receive a
recognizably-invalid record than lose it and shift a downstream
consumer's sample alignment). A downstream stage that wants to honor
this just checks `in->flags & STAGE_RECORD_FLAG_INTEGRITY_FAILED` -
nothing else in loomtabulator inspects or acts on this bit today.

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

## Example

See `example_stage.c` in this directory for a minimal worked
passthrough stage (copies its input record to its output unchanged,
with one optional `"verbose"` config field). Build it with the command
at the top of that file.
