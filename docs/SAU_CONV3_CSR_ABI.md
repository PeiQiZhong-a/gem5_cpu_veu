# SAU Conv3 four-instruction CSR ABI

Status: frozen wire version 1, document revision 1.2. Confirmed on 2026-08-02.
Revision 1.1 added the user-requested 3-bit kernel field. Revision 1.2 freezes
the two permitted backend memory-completion proofs without changing any wire
field or CPU-visible behavior.

This document is the normative contract for the gem5 four-instruction Conv
SAU CSR decoder, fixture generator, tests, and verifier. The model is required to
produce the specified instruction-visible and data-memory-visible behavior;
it is not required to reproduce the real RTL's internal configuration count
or cycle-by-cycle latency.

No decoder, fixture, or generated hex implemented the earlier layout, so this
pre-implementation amendment replaces it without consuming a wire version.
After revision 1.2, any incompatible field, layout, validation, or completion
semantics change requires a new ABI version and explicit review. A backend may
prove D visibility through either the `ExternalBeat` or
`LocalScratchpadBacking` mode defined below; this does not alter the wire ABI.
`PLAN.md` is
explanatory; if its summary differs from this document, this document wins.

## Scope and instruction transport

- One Conv2d operation is configured by exactly `msetins1`, `msetins2`,
  `msetins3`, and `msetins4`, in that order.
- The existing CPU decoder may retain legacy `msetins1~7` and `mgetins*`
  support. The four-instruction Conv backend accepts only `msetins1~4`; slots
  5 through 7 are not Conv3 extensions, and v1 does not use `mgetins*` polling.
- Each instruction carries one 64-bit payload:

```text
payload[31:0]  = rs1[31:0]
payload[63:32] = rs2[31:0]
```

- `msetins1~3` update shadow words and each completes with exactly one normal
  HC write response.
- A valid `msetins4` atomically snapshots all four words into an immutable
  active configuration and starts the operation.
- The CPU holds the `msetins4` request stable. The endpoint returns exactly
  one HC response only after all valid D bytes are visible in the CPU-readable
  output range and the selected backend memory mode has drained. There is no
  early acknowledgement or busy poll in v1.
- Reset discards shadow validity, the active operation, pending HC/backend
  memory state, and completion state. A new transaction must rewrite all four
  slots. Reset does not clear CPU-visible SRAM contents.

The HC payload packing, tick behavior, and backend memory modes follow
`SAU_GEM5_ENDPOINT_CONTRACT.md`. An endpoint selects exactly one memory mode
for an operation and must not mix them. Conv v1 does not inherit a requirement
to match legacy RTL internal latency.

## Frozen payload layout

All multi-bit integers are unsigned unless stated otherwise. Every address is
a complete 32-bit byte address; no implicit data-base offset is added.

### `msetins1`: source addresses

| Payload bits | Width | Field | Meaning |
| ---: | ---: | --- | --- |
| `[31:0]` | 32 | `input_base` | First byte of contiguous signed INT8 NCHW input A |
| `[63:32]` | 32 | `weight_base` | First byte of contiguous signed INT8 `[C][3][3][OC]` weights B |

### `msetins2`: bias and destination addresses

| Payload bits | Width | Field | Meaning |
| ---: | ---: | --- | --- |
| `[31:0]` | 32 | `bias_base` | First byte of mandatory contiguous signed INT16 bias |
| `[63:32]` | 32 | `output_base` | First byte of contiguous signed INT8 NCHW output D |

### `msetins3`: logical Conv shape

| Payload bits | Width | Field | Legal values |
| ---: | ---: | --- | --- |
| `[15:0]` | 16 | `input_h` | `1..65535`, subject to valid output shape |
| `[31:16]` | 16 | `input_w` | `1..65535`, subject to valid output shape |
| `[47:32]` | 16 | `batch_n` | `1..65535` |
| `[53:48]` | 6 | `input_c` | `1..63` |
| `[58:54]` | 5 | `output_c` | `1..16` |
| `[63:59]` | 5 | `reserved0` | Must be zero |

### `msetins4`: ABI, numeric control, and atomic start

| Payload bits | Width | Field | Legal values / meaning |
| ---: | ---: | --- | --- |
| `[3:0]` | 4 | `abi_version` | Must be `1` |
| `[4]` | 1 | `padding` | Symmetric padding: `0` or `1` |
| `[5]` | 1 | `stride_minus_1` | `0` means stride 1; `1` means stride 2 |
| `[10:6]` | 5 | `cutbit` | `0..23` |
| `[13:11]` | 3 | `kernel_size` | Must be `3`, meaning 3x3 |
| `[30:14]` | 17 | `reserved0` | Must be zero |
| `[31]` | 1 | `start` | Must be `1`; atomically commit and start |
| `[55:32]` | 24 | `reserved1` | Must be zero |
| `[63:56]` | 8 | `magic` | Must be `0xC3` |

## Fixed computation contract

The 3-bit `kernel_size` has the same width as real RTL `conv_kernal`, which is
loaded from RTL `csr_wdata[12:10]`. Conv3 v1 places it at payload `[13:11]` to
avoid moving the already assigned version/padding/stride/cutbit fields. The
RTL stores the literal kernel dimension, so v1 encodes 3x3 as
`kernel_size=3`. Values other than 3 are invalid in Conv3 v1; the field does
not advertise support for other kernel sizes.

Together, `magic=0xC3`, `abi_version=1`, and `kernel_size=3` select the fixed
properties below:

```text
operator           = standard Conv2d
dilation_h/w       = 1/1
groups             = 1
input/weight       = signed INT8
bias                = mandatory signed INT16
output              = signed INT8
input/output layout = NCHW
weight layout       = [C][KH=kernel_size][KW=kernel_size][OC]
                      (K-major, OC-minor)
accumulator         = signed 24-bit saturation after every add
rounding            = none; arithmetic right shift by cutbit
final conversion    = signed INT8 saturation
SA shape            = 16 rows x 16 columns
```

For each output element, K order is:

```text
k = ((c * kernel_size) + kh) * kernel_size + kw
```

Bias is added exactly once per output element after the K products have been
accumulated, before the arithmetic right shift and INT8 saturation.

## Memory layout

The byte offsets from the four base addresses are:

```text
A[n,c,h,w]       = (((n * C + c) * H + h) * W + w)
B[c,kh,kw,oc]    = ((((c * kernel_size + kh) * kernel_size + kw) * OC) + oc)
bias[oc]         = oc * 2
D[n,oc,oh,ow]    = (((n * OC + oc) * out_h + oh) * out_w + ow)
```

Bias elements are little-endian signed INT16. A, B, and D elements occupy one
byte. Padding coordinates outside A evaluate to zero and must not issue a
memory read.

## Derived values and checked arithmetic

The endpoint derives:

```text
P          = padding
S          = stride_minus_1 + 1
KH = KW    = kernel_size = 3
out_h      = floor((H + 2*P - kernel_size) / S) + 1
out_w      = floor((W + 2*P - kernel_size) / S) + 1
K          = C * kernel_size * kernel_size
input_size = N * C * H * W
weight_size= C * kernel_size * kernel_size * OC
bias_size  = OC * 2
output_size= N * OC * out_h * out_w
```

All intermediate shape, size, and `base + size` calculations use checked
arithmetic wide enough to detect overflow; they must not wrap at 16 or 32
bits. `H + 2P >= kernel_size` and `W + 2P >= kernel_size` are required before
calculating output shape.

The implementation may derive tiling, SRAM burst addresses, valid masks,
flow counts, and step values from these logical shapes. Those internal values
are not ABI fields and must not change the logical memory order above.

## Validation and failure behavior

Before asserting `crossbarStart` or exposing any backend memory access, the
endpoint validates all of the following:

1. Slots 1 through 4 were written once in order since reset or the previous
   commit; slot 4 is the commit instruction.
2. `magic`, `abi_version`, `kernel_size=3`, `start`, and all reserved bits are
   valid.
3. Every shape and numeric field is in its declared range and produces a
   positive output shape.
4. All four sizes and half-open ranges `[base, base + size)` are representable
   and entirely inside the configured data-memory address range.
5. `bias_base` is 2-byte aligned. A, B, and D need only byte alignment. The
   endpoint preserves exact byte addressing. An `ExternalBeat` backend handles
   unaligned and cross-32-byte-beat accesses; a `LocalScratchpadBacking`
   backend maps the same logical bytes directly through its backing view.
6. The four non-empty A, B, bias, and D ranges do not overlap.
7. No previous Conv3 transaction is active.

On successful commit, all shadow-valid bits are cleared. A new transaction
must rewrite slots 1 through 4. While busy, no new Conv3 configuration write
is accepted.

Any invalid configuration or illegal sequence terminates the simulation with
a diagnostic containing the instruction PC, all four raw payloads when
available, and the failing field/value or address range. The endpoint must not
issue partial memory traffic, silently ignore the error, or return fake
success.

## Completion and crossbar ownership

For a valid commit, the endpoint acquires SRAM ownership with a one-tick
`crossbarStart` pulse and releases ownership with one `crossbarDone` pulse only
after the selected memory mode has drained and every valid D byte is visible.

Completion is proved according to exactly one mode from
`SAU_GEM5_ENDPOINT_CONTRACT.md`:

- `ExternalBeat`: the final D write is complete only after its SRAM write
  response is observed and no response remains outstanding.
- `LocalScratchpadBacking`: the final sau_n D write grant is complete when its
  byte commit through the shared backing is visible to CPU reads, and the
  model reports drained with no internal request, response, or pending D work.

The endpoint may then complete `msetins4` exactly once, after the ownership
release edge. It must not expose a later D write, outstanding backend work, or
a second HC completion.

## Encoding example

For:

```text
input_base  = 0x20010000
weight_base = 0x20012000
bias_base   = 0x20012900
output_base = 0x20012920

N=1, C=16, H=16, W=32, OC=16
kernel_size=3, padding=1, stride=1, cutbit=12
```

the four payloads are:

```text
msetins1 = 0x2001200020010000
msetins2 = 0x2001292020012900
msetins3 = 0x0410000100200010
msetins4 = 0xc300000080001b11
```

Pack/unpack unit tests must derive and decode these words from fields; the
decoder must not special-case the example constants.

## Relationship to the real RTL and generated hex

The ABI deliberately preserves concepts that are necessary to execute the
same logical operator: two source addresses, bias/output addresses, a 3-bit
kernel size, padding, stride, cutbit, and an explicit start. Datatype/mode
bits, feeder steps, bursts, flow counts, and valid windows are fixed or
derived because the gem5 model has its own Im2Col and execution organization.

The user-provided toolchain archive remains immutable evidence for legacy CPU
instruction mix and memory organization. Because the original operator
compiler is unavailable locally, authorized four-instruction comparison hex
may be assembled from a reviewable RV32 source using the project's pinned
LLVM tools. Generated artifacts must carry a manifest and must preserve the
legacy program skeleton and data layout except for explicitly declared
configuration-instruction differences. Hand-editing instruction words without
a source and reproducible generator is not an accepted v1 workflow.
