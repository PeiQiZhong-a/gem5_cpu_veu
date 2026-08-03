# SAU gem5 CPU-side endpoint contract

Status: frozen CPU-side abstraction, version 1.2.

This document freezes the CPU/SAU tick boundary and the behavior already
visible in the current `dut_kui` RTL. It deliberately does not freeze an RTL
commit, compile macro, clock frequency, bank capacity, or SAU internal
execution latency.

## Tick contract

Every simulated clock has two phases:

1. `evaluate()` and `evaluateMemory()` expose signals for the current tick and
   do not mutate endpoint state.
2. `clockTick(request, memoryResponse)` samples all current inputs and commits
   exactly one rising edge.

HC and SRAM state must commit atomically in `clockTick`. Calling
`clockSauMemory()` on `PipelineCore` only changes an input pin; it does not
advance the SAU endpoint until the next `clockOneCycle()`.

`reset()` cancels every in-flight HC/SRAM operation. The first evaluation after
reset must expose no HC completion, SRAM request, `crossbarStart`, or
`crossbarDone`.

## HC interface

The C++ types are `SauRequest` and `SauResponse`, aliases of `HcRequest` and
`HcResponse` in `src/brs/hc/hc_protocol.hh`.

| Field | Direction | Width | Meaning |
| --- | --- | ---: | --- |
| `csrAddr` | CPU to SAU | 12 | SAU CSR address, currently `0x200`-`0x20d` |
| `csrRead` | CPU to SAU | 1 | Read request level |
| `csrWrite` | CPU to SAU | 1 | Write request level |
| `writeType` | CPU to SAU | 2 | Write/Set/Clear/VectorStart encoding |
| `writeData` | CPU to SAU | 64 | `{rs2[31:0], rs1[31:0]}` |
| `veStart` | CPU to SAU | 32 | Zero for the current SAU instruction set |
| `valid` | SAU to CPU | 1 | Completion pulse |
| `readData` | SAU to CPU | 32 | MGET result |

The CPU holds all request fields stable until the selected endpoint returns
`valid`. The endpoint accepts a held HC request once and returns exactly one
completion pulse, including for a write.

## 256-bit SRAM beat interface

The C++ types are `Sram256Request` and `Sram256Response` in
`src/brs/memory/sram_256_protocol.hh`.

| Field | Direction | Width | Meaning |
| --- | --- | ---: | --- |
| `request.valid` | SAU to memory | 1 | One SRAM beat is issued in this tick |
| `request.address` | SAU to memory | 32 | Byte address |
| `request.writeStrobe` | SAU to memory | 32 | One bit per byte; zero means read |
| `request.writeData` | SAU to memory | 256 | Byte 0 is the least-addressed byte |
| `response.valid` | memory to SAU | 1 | One accepted beat completes this tick |
| `response.readData` | memory to SAU | 256 | Read data with the same byte ordering |

`request.valid` is a per-tick beat strobe, not a ready/valid level handshake:

- Every tick with `request.valid=1` is a new beat. Consecutive high ticks mean
  consecutive independent beats.
- The SAU must not hold one beat high while waiting for a response.
- The RTL boundary has no request-ready or retry signal.
- Every accepted beat produces one `response.valid` pulse after the configured
  RTL-aligned memory latency. This includes writes; `readData` is meaningful
  only for reads.
- The transfer line is `address & ~0x1f`. `writeStrobe[n]` controls
  `writeData[n]` at byte address `line+n`.

## Same-bank arbitration

SAU is crossbar master 0 and VEU is master 1. If both issue a beat to the same
bank in the same active tick, the current RTL loop writes the slave signals in
master order, so VEU wins:

- VEU's beat is accepted and receives the response.
- SAU's beat is dropped and receives no response.
- The hardware has no retry path for the dropped beat.

Legal architecture simulations therefore must avoid same-bank SAU/VEU
collisions. The gem5 model still reproduces the RTL winner and exposes
`sameBankCollision`, `masterAccepted`, and `masterDropped` in the cycle trace
so an illegal schedule is immediately diagnosable.

Different-bank SAU and VEU beats in the same tick are both accepted.

## `crossbarStart` / `crossbarDone`

Both signals are one-tick pulses:

- `crossbarStart` acquires accelerator ownership. In IDLE, the edge sampling
  `crossbarStart` changes the crossbar to ACTIVE; a request presented in that
  same IDLE tick is not forwarded. The first legal SRAM beat is in a later
  ACTIVE tick.
- In the currently traced SAU register-load path, the first
  `sau_sram_enable` appears three rising edges after the observed
  `sau_crossbar_start`; the bank-facing `slave_req` appears one edge later.
  A replacement SAU model must reproduce its RTL's measured internal timing
  rather than assuming same-tick start/request behavior.
- `crossbarDone` releases ownership. The ACTIVE tick that carries `done` is
  sampled at its closing edge and the following tick is IDLE.
- The current SAU RTL registers `crossbarDone` one edge after `flow_end`.
  Its last unload request has already passed the output/memory drain pipeline,
  so a legal endpoint emits no SRAM request in the `done` tick and has no
  outstanding memory completion after `done`.
- A new transaction must emit a new `crossbarStart`; start and done must not be
  asserted together.

In `dut_kui`, `sau_crossbar_done` also feeds external interrupt source bit 3.
The CPU-side model ORs that dynamic bit with the configured external IRQ input
before the edge sampled by the CPU CSRU/IRCU.
