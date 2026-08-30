# RECOMMENDATIONS.md

Proposed changes to the loomtabulator stage plugin ABI (`stage.h`,
`stage_abi.h`) to support multi-output routing and richer per-node
configuration. Submitted for change-review-board consideration; nothing
in this document is implemented. **These files are generated copies
(see `README.md`) — any approved change lands in loomtabulator's own
`src/` first, then gets regenerated here, never hand-edited in this
directory.**

## 1. Motivation

`gfp_extract` (`src/gfp_stage.c`) parses a GFP frame's linear extension
header, which carries an 8-bit Channel ID (CID) — see G.7041 clause
6.1.2.1.3.2.1. The natural use of a CID is to demultiplex traffic: frames
for channel 3 go one place, channel 7 goes another. The current ABI
can't express that. Every stage type declares exactly one output port
(`struct stage.out_type`, `stage.h:129`) and `process()` fills exactly
one output record (`stage.h:150-151`) or drops it. There is no concept
of "which of several downstream destinations does this record go to."

Today the only workaround is N parallel graph nodes of the same plugin
type, each configured to accept one CID and drop everything else (see
the "filter-per-node" pattern discussed alongside this proposal). That
works but scales linearly in graph size and configuration with the
number of channels, and duplicates the parse/checksum work N times per
frame instead of once.

This document proposes:

- **A. N output ports per stage**, selected per-record at runtime.
- **B. Config-driven parsing/validation behavior** at `init()` time,
  instead of being fixed in each stage's source.
- **C. A drop-vs-zero-fill failure policy**, with a supporting
  provenance flag on `struct stage_record`.
- **D. A generic field-based routing table**, so port selection isn't
  hardcoded to "CID" in the ABI itself.

Each is separable — the review board could accept A without C, etc. —
but B/C/D are informed by A, so they're presented together.

## 2. Current ABI (baseline, for reference)

```c
enum stage_port_type { PORT_TYPE_RAW_RECORD, PORT_TYPE_VALIDATED,
                        PORT_TYPE_EXTRACTED, PORT_TYPE_ENGINEERING,
                        PORT_TYPE_WIRE_FRAME };

struct stage_record {
        enum stage_port_type type;
        uint8_t *data;
        uint32_t len;
        uint64_t capture_tsc;
};

struct stage_result {
        bool ok;
        const char *drop_reason;
};

struct stage {
        const char *name;
        enum stage_port_type in_type;
        enum stage_port_type out_type;
        void *(*init)(const struct json_value *config);
        struct stage_result (*process)(void *state, const struct stage_record *in,
                                        struct stage_record *out);
        void (*teardown)(void *state);
};
```

(`src/stage.h:48-159`.) `STAGE_ABI_VERSION` is currently `1`
(`src/stage_abi.h:51`); every change below requires bumping it, since
the loader does a hard equality check with no compatibility window
(`src/stage_abi.h:43-50`).

## 3. Proposal A — N output ports

### A.1 What changes

```c
struct stage_result {
        bool ok;
        unsigned out_port;       /* NEW - which output port `out` targets;
                                   * caller-ignored when ok=false */
        const char *drop_reason;
};
```

`process()` keeps its existing signature and still fills exactly one
`out` record per call — this proposal is "pick one of N destinations
per record," not "fan out one record to multiple destinations
simultaneously." Broadcast/multicast fan-out is a different, more
expensive feature (it would need an array of `out` records and a bitmap
of which were filled) and isn't needed for the CID-demux use case that
motivated this document. Recommend treating it as explicitly
out-of-scope for this proposal (see §6).

### A.2 How a stage declares its port count

Two options, with a recommendation. Both need graph-side changes
outside this repo (`graph_config.c`, whatever validates a JSON graph's
edges at startup) — flagging that explicitly since it's outside what
this plugin-sdk checkout can speak to authoritatively.

**Option 1 (recommended): static per-plugin-type max, dynamic per-node
use.**

```c
struct stage {
        ...
        unsigned max_out_ports;   /* NEW - compile-time upper bound, e.g. 256
                                    * for an 8-bit CID; part of the static
                                    * struct stage, same as in_type/out_type */
};
```

A given graph node's config then declares how many of those ports it
actually wires up (e.g. `"num_outputs": 4`), and `process()`/routing
logic is responsible for staying within `[0, max_out_ports)`. Graph
validation at startup checks each wired output index is `<
max_out_ports`. Simple, no new function pointer, no lifecycle change —
but it can't catch "this node's config wires port 4 but only meant to
use 2" at startup, since `max_out_ports` is a ceiling, not an exact
count.

**Option 2: dynamic per-instance exact count.**

```c
struct stage {
        ...
        unsigned (*out_port_count)(void *state);  /* NEW - called once per
                                                     * node, right after a
                                                     * successful init(),
                                                     * before that node's
                                                     * graph edges are
                                                     * validated */
};
```

Lets each node's own config (`"num_outputs": N`) be the source of
truth, and lets startup validation reject a graph that wires more (or
fewer) edges than the node actually declared. More precise, but changes
the loader's lifecycle: today, every stage type's shape is known before
`graph_config.c` ever runs (`stage.h:117-124`); this makes one node's
port count depend on that node's own `init()` having already run, which
is a bigger structural change to the load → validate → run pipeline
than Option 1. Flagging this as the key open question for the review
board — it affects code well outside this SDK.

### A.3 Backward compatibility

Existing single-output stage types set `max_out_ports = 1` (Option 1)
or return `1` from `out_port_count()` (Option 2), and always return
`out_port = 0`. `example_stage.c` needs no behavior change either way,
just the new struct field.

## 4. Proposal B — config-driven parsing behavior

Right now `gfp_extract`'s `init()` only reads one config key
(`"verbose"`). Proposed additions to its config schema (this is a
per-plugin config schema change, not an ABI change — no `stage.h` edits
needed for this section on its own):

| Config key | Type | Meaning |
|---|---|---|
| `"exi_mode"` | string: `"null_only"` \| `"linear_only"` \| `"auto"` | Restrict which EXI values are accepted instead of always auto-detecting null/linear. `"auto"` matches today's behavior; `"null_only"`/`"linear_only"` let a deployment that knows its peer's framing reject the other shape outright as a config-time assertion, rather than silently tolerating it. |
| `"validate_chec"`, `"validate_thec"`, `"validate_ehec"`, `"validate_fcs"` | bool, default `true` each | Per-checksum opt-out. A deployment that already trusts an upstream integrity check (or is talking to a peer with a known-broken FCS implementation) can disable just that one check instead of losing all validation. |
| `"strict_length"` | bool, default `true` | Today the stage requires `in->len == 4 + PLI` exactly (`gfp_stage.c`'s "GFP frame length does not match PLI" drop). Setting this `false` would instead accept `in->len >= 4 + PLI` and ignore trailing bytes — useful if a transport wraps frames with padding. |

## 5. Proposal C — drop-vs-zero-fill failure policy

Some downstream consumers (particularly fixed-rate/sample-aligned ones —
this matches the broader project family's chrono/telemetry pipeline
style, where dropping a record breaks a downstream consumer's sample
count or timing alignment) would rather receive a record they can
identify as corrupt than lose it and shift alignment. Proposed config:

| Config key | Type | Meaning |
|---|---|---|
| `"on_checksum_failure"` | string: `"drop"` (default) \| `"zero_fill"` | `"drop"` is today's behavior (`ok=false`). `"zero_fill"` returns `ok=true` with the output payload zeroed instead. |
| `"zero_fill_bytes"` | uint, default = full payload | Only meaningful when `on_checksum_failure = "zero_fill"`. Number of leading bytes of the output payload to zero; a value `>= client_len` zeros the whole thing. Lets a consumer that only cares about a fixed-width leading struct get that struct zeroed without paying to zero a potentially large trailing bulk-data region. |

This direction needs **no `process()` signature change** — `ok=true` +
a zeroed `out->data` is already expressible with the existing ABI.
It does need one new field, to avoid a zero-filled record being
indistinguishable from a legitimately-all-zero one:

```c
struct stage_record {
        enum stage_port_type type;
        uint8_t *data;
        uint32_t len;
        uint64_t capture_tsc;
        uint32_t flags;      /* NEW - bitmask, first bit:
                               * STAGE_RECORD_FLAG_INTEGRITY_FAILED */
};
```

A stage that zero-fills sets this flag; downstream stages/consumers can
count or branch on it without inspecting payload bytes. This field is
generically useful beyond `gfp_extract` — any stage with a validation
step could set it — so it belongs on `struct stage_record`, not in a
GFP-specific side channel.

## 6. Proposal D — generic field-based routing (not CID-specific)

Rather than hardcoding "route by CID" into the ABI or into
`gfp_extract` specifically, propose a small per-node routing table in
config, so the same mechanism generalizes to other stages/fields later
(e.g. routing by PTI, by UPI, by a convert-stage output range):

```json
"routing": {
  "field": "cid",
  "table": [
    { "value": 3,  "port": 0 },
    { "value": 7,  "port": 1 },
    { "value": 42, "port": 2 }
  ],
  "default_port": null,
  "drop_unmatched": true
}
```

`"field"` names something the stage itself knows how to read (for
`gfp_extract`, initially just `"cid"`); `"default_port"` /
`"drop_unmatched"` control what happens to a value with no table entry.
This keeps the routing *policy* in config/JSON (data), not in
recompiled C (code), matching this SDK's existing philosophy of
"startup-time config, not hardcoded behavior" already used for e.g.
`forward_udp`'s destination.

## 7. Combined worked example

Illustrative `gfp_extract` node config using all of the above:

```json
{
  "type": "gfp_extract",
  "data": {
    "config": {
      "exi_mode": "linear_only",
      "validate_chec": true,
      "validate_thec": true,
      "validate_ehec": true,
      "validate_fcs": true,
      "on_checksum_failure": "zero_fill",
      "zero_fill_bytes": 64,
      "num_outputs": 3,
      "routing": {
        "field": "cid",
        "table": [
          { "value": 3, "port": 0 },
          { "value": 7, "port": 1 }
        ],
        "default_port": 2,
        "drop_unmatched": false
      }
    }
  }
}
```

## 8. Impact summary

| Change | `stage.h`/`stage_abi.h` diff? | ABI version bump? | Touches code outside this SDK? |
|---|---|---|---|
| A — N output ports | Yes (`stage_result.out_port`, plus A.2's chosen option) | Yes | Yes — loader + graph validation + graph JSON schema + web UI connection rules |
| B — config-driven parsing | No | No | No |
| C — drop-vs-zero-fill | Yes (`stage_record.flags`) | Yes | Downstream stages that want to honor the flag |
| D — field-based routing | No (config-only, rides on A) | No (beyond A's own bump) | No |

Any of A or C alone forces a `STAGE_ABI_VERSION` bump and a rebuild of
every existing plugin (`stage_abi.h:43-50` — hard equality check, no
compatibility window), so if both are going to happen, doing them in
the same version bump avoids forcing two separate rebuild cycles on
every third-party plugin author.

## 9. Explicitly out of scope for this proposal

- **Broadcast/multicast fan-out** (one input record duplicated to
  multiple output ports simultaneously) — a materially different,
  more expensive ABI shape (array of output records + a filled-bitmap).
  Nothing in the motivating use case needs it; call it out separately
  if a future need arises.
- **Per-port output types** (each of the N ports declaring a different
  `enum stage_port_type`) — this proposal assumes all output ports of
  a given stage instance carry the same `out_type`. Worth revisiting
  only if a concrete stage needs to emit structurally different data
  on different ports.
- **Hot-reload of routing tables** — routing config is read once at
  `init()`, same lifecycle as every other config value today. Runtime
  reconfiguration without a restart is a separate feature.
- **Per-drop/per-route statistics counters** — useful, but additive and
  independent of the ABI shape questions above; worth its own proposal.

## 10. Open questions for the review board

1. Option 1 vs Option 2 in §3.2 (static max vs. dynamic exact port
   count) — this is the one item here with real implications for
   `graph_config.c`'s load-order guarantees, outside what this SDK
   checkout can resolve unilaterally.
2. Should `"routing"` (§6) be a first-class, ABI-documented config
   shape that `graph_config.c` itself understands and validates at
   startup (e.g. "port indices used in a routing table must be `<`
   this node's declared output count"), or purely a per-plugin config
   convention with no host-side awareness? The former gives better
   startup-time error messages; the latter is less coupling.
3. Does `STAGE_RECORD_FLAG_INTEGRITY_FAILED` (§5) belong as a single
   bit, or should `flags` reserve bit ranges for future per-checksum
   detail (e.g. "which of cHEC/tHEC/eHEC/FCS specifically failed")?
