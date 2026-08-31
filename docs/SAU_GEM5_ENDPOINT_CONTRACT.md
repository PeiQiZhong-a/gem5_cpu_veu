# SAU gem5 CPU-side endpoint contract

Status: frozen CPU-side abstraction, version 2.0.

The architectural reference is `npu_lpnpu` branch `mikui_v2.0`, commit
`86c289c`. The instruction truth source is generated RTL
`hardware/src/spirit/Decoder.sv`; operand packing is fixed by
`hardware/src/spirit/CBU.sv`; the register map is fixed by
`hardware/src/sa_element/csr.sv`; and the SRAM width is fixed by
`hardware/src/sa_element/SA_CORE.sv` and `hardware/src/dut_mikui.sv`.

This contract deliberately leaves SAU compute behavior and internal execution
latency open until the real SAU model is integrated. It does not permit those
future details to change the CPU instruction, HC, tick, or SRAM interfaces.

## Instruction and CSR contract

SAU uses RISC-V custom opcode `0x6b`, `funct3=1`, and exactly twelve encodings:

| Group | SET `funct7` / CSR | GET LSB `funct7` / CSR | GET MSB `funct7` / CSR |
| ---: | --- | --- | --- |
| 1 | `0 / 0x200` | `1 / 0x200` | `2 / 0x201` |
| 2 | `3 / 0x202` | `4 / 0x202` | `5 / 0x203` |
| 3 | `6 / 0x204` | `7 / 0x204` | `8 / 0x205` |
| 4 | `9 / 0x206` | `10 / 0x206` | `11 / 0x207` |

Every other `funct7` is unsupported. SAU routing is therefore limited to CSR
addresses `0x200` through `0x207`; `0x208` and above must not reach SAU.

The generated RTL enables both integer source dependencies and integer
destination writeback for every one of the twelve encodings. Normal MSET code
uses `rd=x0`, while normal MGET code uses `rs1=x0` and `rs2=x0`, but gem5 must
preserve the dependencies encoded in arbitrary instruction words.

This contract freezes the CPU/SAU tick boundary and the two permitted memory
execution modes. It deliberately does not freeze an RTL commit, compile macro,
clock frequency, bank capacity, or SAU internal execution latency.

## Tick contract

Every simulated clock has two phases:

1. `evaluate()` and `evaluateMemory()` expose current signals without mutating
   endpoint state.
2. `clockTick(request, memoryResponse)` samples all current inputs and commits
   exactly one rising edge.

HC and selected-backend memory state must commit atomically in `clockTick`.
Calling `clockSauMemory()` on `PipelineCore` only changes an input pin; it does
not advance the SAU endpoint until the next `clockOneCycle()`.

`reset()` cancels every in-flight HC/backend operation. The first evaluation
after reset must expose no HC completion, SRAM request, `crossbarStart`, or
`crossbarDone`. In `LocalScratchpadBacking` mode, reset destroys or detaches
the active model before its backing view and must not clear CPU-visible SRAM.

## HC interface

The C++ types are `SauRequest` and `SauResponse`, aliases of `HcRequest` and
`HcResponse` in `src/brs/hc/hc_protocol.hh`.

| Field | Direction | Width | Meaning |
| --- | --- | ---: | --- |
| `csrAddr` | CPU to SAU | 12 | SAU CSR address, `0x200`-`0x207` |
| `csrRead` | CPU to SAU | 1 | MGET request level |
| `csrWrite` | CPU to SAU | 1 | MSET request level |
| `writeType` | CPU to SAU | 2 | MSET uses `Set` |
| `writeData` | CPU to SAU | 64 | `{encoded rs2[31:0], encoded rs1[31:0]}` |
| `veStart` | CPU to SAU | 32 | Zero for all twelve SAU instructions |
| `valid` | SAU to CPU | 1 | Completion pulse |
| `readData` | SAU to CPU | 32 | MGET result |

`CBU.sv` constructs `writeData` as `{op2, op1}`. At the binary instruction
boundary this means encoded `rs1` occupies bits 31:0 and encoded `rs2` occupies
bits 63:32. Software intrinsic argument order does not override this RTL fact.

The CPU holds all HC request fields stable until the selected endpoint returns
`valid`. An endpoint accepts a held request once and returns exactly one
completion pulse, including for MSET.

## Memory execution mode

An endpoint fixes one memory execution mode at construction and uses it for the
whole operation. The two modes are mutually exclusive:

- `ExternalBeat`: endpoint memory traffic crosses the 256-bit request/response
  boundary described below.
- `LocalScratchpadBacking`: a cycle model such as
  `sau_n::StreamingConvPipelineModel` performs its own 16-bank arbitration and
  one-cycle scratchpad responses, while storage bytes are supplied by a shared
  CPU-visible backing view.

An endpoint must never emit external beats for some internal bank requests and
use local backing for others. Doing so would model two SRAM ports and two
latencies for one logical access.

## `ExternalBeat` 256-bit SRAM interface

The C++ types are `Sram256Request` and `Sram256Response` in
`src/brs/memory/sram_256_protocol.hh`.

The 256-bit path uses one 32-byte transfer line. `request.valid` is a
per-tick beat strobe, and each accepted read or write produces one response
after the configured RTL-aligned latency. The transfer line is
`address & ~0x1f`; strobe bit `n` controls byte `line+n`.

## Frozen 128-bit SRAM beat interface

The C++ types are `Sram128Request` and `Sram128Response` in
`src/brs/memory/sram_128_protocol.hh`.

| Field | Direction | Width | Meaning |
| --- | --- | ---: | --- |
| `request.valid` | SAU to memory | 1 | One SRAM beat is issued this tick |
| `request.address` | SAU to memory | 32 | Byte address |
| `request.writeStrobe` | SAU to memory | 16 | One bit per byte; zero means read |
| `request.writeData` | SAU to memory | 128 | Byte 0 is the least-addressed byte |
| `response.valid` | memory to SAU | 1 | One accepted beat completes this tick |
| `response.readData` | memory to SAU | 128 | Same byte ordering as request data |

`request.valid` is a per-tick beat strobe, not a ready/valid level handshake:

- Every tick with `request.valid=1` is a distinct beat, including consecutive
  high ticks.
- The SAU must not hold one beat high while waiting for a response.
- The RTL boundary has no request-ready or retry signal.
- Every accepted beat produces one `response.valid` pulse after the configured
  memory latency, including writes.
- The transfer line is `address & ~0x0f`; strobe bit `n` controls data byte `n`
  at `line+n`.

## Legacy 256-bit platform adapter

The existing gem5 `DutKuiMemoryModel` remains a 256-bit VEU/DBUS platform. Its
SAU port is compatibility-only and must not redefine the canonical endpoint:

- address bit 4 selects the lower or upper 128-bit half of the 256-bit line;
- a lower-half strobe/data byte remains at byte `n`;
- an upper-half strobe/data byte is shifted to byte `16+n`;
- responses are sliced back to canonical bytes 0 through 15 using the half
  recorded when that SAU beat was accepted;
- dropped or unmapped beats are not recorded and cannot consume a later
  response.

This adapter preserves existing VEU and 32-to-256 DBUS behavior while allowing
new SAU endpoints to implement only the frozen RTL-native 128-bit interface.

## Crossbar ownership and arbitration

`crossbarStart` and `crossbarDone` are one-tick ownership pulses. A start edge
moves an idle crossbar to accelerator-active state; the first legal SRAM beat
is presented in a later active tick. A done edge releases ownership after the
current active tick. Start and done must not be asserted together.

If SAU and VEU issue to the same bank in the same active tick, RTL loop order
gives VEU the bank pins. The SAU write is overwritten. However, the RTL delays
each master's bank selection independently, so both internal `master_rdata`
registers can later sample the winning bank data. `master_ack` is internal and
is not exposed at the SAU/VEU boundary. The native Mikui model therefore keeps
winner/drop diagnostics separate from its gem5-only read-data-update event.
Legal SAU/VEU schedules must still avoid same-bank collisions.

The `rtl-npu-lpnpu-mikui` platform is the cycle reference for this contract. It
models two 128-bit banks at `0x20010000-0x2001ffff` and
`0x20020000-0x2002ffff`, the registered 32-to-128 DBUS converter, and the exact
IDLE/ACTIVE/RVACTIVE transitions. In particular, RVACTIVE remains set until an
accelerator start pulse; DBUS deassertion alone does not return it to IDLE.
The RTL's `SRAM_RESP_DELAY` parameter is unused, so latency comes from the
actual request, selector, SRAM-data, and acknowledgement registers.

In the frozen `dut_mikui` baseline, `RV_CORE.sv` ties CPU interrupt inputs low.
`sau_crossbar_done` is not converted into CPU IRQ3. Standalone gem5 CPU tests
may still use explicitly configured interrupt inputs.

## `LocalScratchpadBacking` interface

This mode preserves the selected model's internal bank requests, arbitration,
grants, one-cycle responses, buffering, backpressure, and tick progression.
Only the byte storage behind its scratchpad is replaced by a non-owning backing
view into the same SRAM bytes visible to the CPU.

The following rules are frozen:

- `evaluateMemory().request.valid` remains false for the entire operation,
  including start, run, done, and response ticks.
- The endpoint does not consume `Sram256Response`. A valid external response in
  this mode is a protocol error and must fail fast; integration must not leave
  an external response outstanding when ownership is acquired.
- Every valid internal read obtains its byte from the shared backing. Every
  valid internal D write grant commits its byte to that backing in the model's
  normal clock edge, making it visible to subsequent CPU reads.
- The model's internal scratchpad is the sole latency and arbitration authority;
  no internal bank request is translated into `Sram256Request`.
- `crossbarStart` and `crossbarDone` still delimit exclusive ownership, but do
  not imply external beat traffic or external beat statistics.
- Completion requires the model to be drained, all internal responses and D
  pending work to be empty, and the final valid D byte to be committed to the
  shared backing. No D commit is permitted after `crossbarDone`.
- Reset cancels the active model and pending completion without rolling back or
  clearing bytes already committed to shared SRAM.

## `crossbarStart` / `crossbarDone`

Both signals are one-tick pulses in both memory modes:

- `crossbarStart` acquires accelerator ownership. In IDLE, the edge sampling
  `crossbarStart` changes the crossbar to ACTIVE; a request presented in that
  same IDLE tick is not forwarded. The first legal SRAM beat is in a later
  ACTIVE tick.
- In `ExternalBeat` mode, the currently traced SAU register-load path's first
  `sau_sram_enable` appears three rising edges after the observed
  `sau_crossbar_start`; the bank-facing `slave_req` appears one edge later.
  A replacement SAU model must reproduce its RTL's measured internal timing
  rather than assuming same-tick start/request behavior.
- `crossbarDone` releases ownership. The ACTIVE tick that carries `done` is
  sampled at its closing edge and the following tick is IDLE.
- In `ExternalBeat` mode, the current SAU RTL registers `crossbarDone` one edge
  after `flow_end`. Its last unload request has already passed the output/memory
  drain pipeline, so a legal endpoint emits no SRAM request in the `done` tick
  and has no outstanding memory completion after `done`.
- A new transaction must emit a new `crossbarStart`; start and done must not be
  asserted together.

In `LocalScratchpadBacking` mode, the model starts ticking only after the edge
that acquires ownership. `crossbarDone` is emitted only after the final internal
D commit is visible and the model is drained. The endpoint returns the held HC
completion only after the release edge. There is no internal model tick in the
start or done pulse tick.

In `dut_kui`, `sau_crossbar_done` also feeds external interrupt source bit 3.
The CPU-side model ORs that dynamic bit with the configured external IRQ input
before the edge sampled by the CPU CSRU/IRCU.
