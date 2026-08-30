# TEST_DRIVER_REQUIREMENTS.md

Byte-level structure of the GFP frames a test data generator must
produce for `gfp_extract` (`src/gfp_stage.c`) to accept and correctly
route them, for the specific scenario this project cares about:
**channel IDs 1-4, each carrying a 1024-byte client payload**, matching
[examples/gfp_route_by_cid.json](examples/gfp_route_by_cid.json).

Every field, offset, and algorithm below was cross-checked by
constructing real frames with these exact values and feeding them
through the actual built plugin (`build/gfp.so`) - not hand-derived.
See "Verified test vectors" for the frames used.

## 1. Frame anatomy

```
byte:   0     2     4     6     8    10    12                      1036   (1040 if PFI=1)
       +-----+-----+-----+-----+----+----+------------------------+------+
       | PLI |cHEC |Type |tHEC |CID |eHEC|   client payload        | pFCS |
       +-----+-----+-----+-----+----+----+------------------------+------+
       \___________/\___________/\_______/\________________________/\____/
        core header   payload hdr  linear    payload information       optional
        (4 bytes)     (4 bytes)    ext hdr   field (1024 bytes for      (4 bytes,
                                   (4 bytes)  this test scenario)        only if
                                                                         PFI=1)
```

Every multi-byte field is **big-endian (network byte order)**, matching
G.7041's own bit/octet transmission order.

## 2. Field-by-field layout

| Offset | Size | Field | Value for this test driver |
|---|---|---|---|
| 0 | 2 | PLI | Length in bytes of everything *after* the core header: `4 (payload hdr) + 4 (ext hdr) + 1024 (payload) [+ 4 (pFCS)]`. **`0x0408` (1032) without pFCS, `0x040C` (1036) with pFCS.** |
| 2 | 2 | cHEC | CRC-16 (see §3) over the 2 PLI bytes. |
| 4 | 2 | Type | `PTI\|PFI\|EXI\|UPI` packed into 16 bits (bit 15 = MSB): PTI=`000` (user data), EXI=`0001` (linear extension header - required, since CID only exists under EXI=0001), UPI=`0`. PFI selects the two allowed values: **`0x0100`** (PFI=0, no pFCS) or **`0x1100`** (PFI=1, pFCS present). |
| 6 | 2 | tHEC | CRC-16 over the 2 Type bytes. |
| 8 | 1 | CID | Channel ID. **Must be 1, 2, 3, or 4** — anything else is dropped (see §5). |
| 9 | 1 | Spare | `0x00`. |
| 10 | 2 | eHEC | CRC-16 over the CID+Spare bytes (offsets 8-9). Depends only on CID, not on PFI/payload. |
| 12 | 1024 | Client payload | Opaque to `gfp_extract` — it is copied through unexamined. Use whatever pattern is convenient for the test (a fixed value, an incrementing byte counter, per-frame content for uniqueness, etc.). |
| 1036 | 4 | pFCS (optional) | Only present if PFI=1. CRC-32 (see §3) over the 1024 payload bytes, transmitted MSB-first. |

Total frame length: **1036 bytes without pFCS, 1040 bytes with pFCS.**

## 3. Checksum algorithms

**cHEC / tHEC / eHEC** — identical algorithm, different 2-byte input:
CRC-16, polynomial `x^16 + x^12 + x^5 + 1` (`0x1021`), MSB-first,
initial value `0x0000`, no final XOR (this is CRC-16/XMODEM). Reference
implementation (also in `gfp_stage.c`):

```c
uint16_t crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0x0000;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)(data[i] << 8);
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
    }
    return crc;
}
```

- cHEC = `crc16(PLI bytes, 2)`
- tHEC = `crc16(Type bytes, 2)`
- eHEC = `crc16(CID/Spare bytes, 2)`

**pFCS** (only if PFI=1) — standard reflected CRC-32, same as IEEE
802.3/zlib/PNG: polynomial `0x04C11DB7` (reflected form `0xEDB88320`),
init/xorout `0xFFFFFFFF`, computed over the 1024 payload bytes, and the
32-bit result is written **MSB-first** as the frame's trailing 4 bytes.

```c
uint32_t crc32_(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return crc ^ 0xFFFFFFFF;
}
```

## 4. What determines acceptance

A frame is accepted (and, with `route_by_cid` config enabled, routed to
a port) only if **all** of the following hold. Any failure is a silent
drop from this stage's perspective — no output record is produced for
that input:

1. Frame is at least 4 bytes (core header fits).
2. cHEC matches the PLI bytes.
3. PLI is not 0 (0 means a GFP idle frame — not a valid test frame).
4. PLI is at least 4 (room for the mandatory payload header).
5. Total frame length equals exactly `4 + PLI` — no padding, no
   truncation.
6. tHEC matches the Type bytes.
7. EXI (bits 11-8 of Type) is `0001` (linear) — this test driver's
   frames must always set this, since a CID only exists under a linear
   extension header. (EXI=`0000`/null is also accepted by the plugin in
   general, but such a frame has no CID and — with `route_by_cid`
   enabled — gets dropped for that reason; not useful for this
   scenario.)
8. eHEC matches the CID+Spare bytes.
9. If PFI=1: pFCS matches the CRC-32 of the 1024 payload bytes.
10. **If `route_by_cid` is enabled** (as in `examples/gfp_route_by_cid.json`):
    CID must be one of the configured values. For this project's example
    config, that's exactly `{1, 2, 3, 4}`, mapped one-to-one to output
    ports `{0, 1, 2, 3}`. Any other CID — or a frame that satisfies
    everything else but uses EXI=`0000` (no CID at all) — is dropped
    with a distinct reason, and separately tallied for the
    troubleshooting summary `gfp_teardown()` prints at shutdown (see
    `CLAUDE.md`).

## 5. Verified test vectors

Generated with payload bytes `payload[i] = i & 0xFF` for `i` in
`0..1023`, and confirmed via the actual built plugin: all 8 combinations
below produced `ok=true`, the expected `out_port`, `len=1024`, and a
byte-for-byte payload match; CID 99 (not in `examples/gfp_route_by_cid.json`)
was confirmed dropped.

| CID | PFI | PLI | cHEC | Type | tHEC | eHEC | pFCS | Frame length | Routed port |
|---|---|---|---|---|---|---|---|---|---|
| 1 | 0 | `0x0408` | `0x4DCC` | `0x0100` | `0x3331` | `0x3331` | — | 1036 | 0 |
| 1 | 1 | `0x040C` | `0x0D48` | `0x1100` | `0x3042` | `0x3331` | `0xB70B4C26` | 1040 | 0 |
| 2 | 0 | `0x0408` | `0x4DCC` | `0x0100` | `0x3331` | `0x6662` | — | 1036 | 1 |
| 2 | 1 | `0x040C` | `0x0D48` | `0x1100` | `0x3042` | `0x6662` | `0xB70B4C26` | 1040 | 1 |
| 3 | 0 | `0x0408` | `0x4DCC` | `0x0100` | `0x3331` | `0x5553` | — | 1036 | 2 |
| 3 | 1 | `0x040C` | `0x0D48` | `0x1100` | `0x3042` | `0x5553` | `0xB70B4C26` | 1040 | 2 |
| 4 | 0 | `0x0408` | `0x4DCC` | `0x0100` | `0x3331` | `0xCCC4` | — | 1036 | 3 |
| 4 | 1 | `0x040C` | `0x0D48` | `0x1100` | `0x3042` | `0xCCC4` | `0xB70B4C26` | 1040 | 3 |

Note PLI, cHEC, Type, tHEC, and pFCS are identical across all four CIDs
in each PFI variant — only eHEC changes, since it's the only field that
covers the CID byte. pFCS is also identical across CIDs within a PFI
variant here only because the *payload content* used for this vector
(`i & 0xFF`) is the same for every CID; a real test driver using
different payload bytes per CID will get a different pFCS per frame.

Full hex for the CID=1, no-pFCS frame's header (offsets 0-11), payload
starting at offset 12:

```
04 08 4D CC 01 00 33 31 01 00 33 31  00 01 02 03 04 05 06 07 ...
\_________/ \_________/ \_________/  \___________________________
  core hdr   payload hdr  linear ext    client payload (1024 bytes,
                           hdr           this vector: byte i = i & 0xFF)
```

## 6. Explicitly out of scope

- **Ring extension header (EXI=`0010`)** — undefined in the ITU-T
  G.7041/Y.1303 (08/2016) revision this plugin implements against
  ("For further study"). Don't generate these; they're dropped.
- **Any other EXI value** — reserved, dropped.
- **Single-bit error correction** — the spec allows optionally
  correcting a single-bit error in cHEC/tHEC/eHEC-protected fields
  instead of dropping. This plugin never does; any checksum mismatch,
  single-bit or otherwise, is a drop. A test driver wanting to exercise
  *invalid* frames (for negative testing) can rely on this — any bit
  flip in a checksum-protected field reliably produces a drop, never a
  "corrected and accepted" frame.
