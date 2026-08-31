# `brs-cycle-trace-v3` contract

`brs-cycle-trace-v3` is the canonical, strict cycle trace used to compare the
Spirit RTL CPU + VEU integration with `PipelineMiniCPU` + `TimingVEU`.

## Cycle origin and sampling

- Reset/setup edges are excluded.
- `edge=1 cpu_cycle=1` is the first `clk_acc` rising edge for which the CPU
  reset input is deasserted.
- RTL samples in the active region of `always @(posedge clk_acc)`, before NBA
  state updates (`posedge-pre-nba`).
- gem5 records after combinational `evaluateOneCycle()` and before
  `clockOneCycle()`. This is the same logical pre-update boundary and is
  normalized to `phase=posedge-pre-nba`.
- One record is required for every active edge. Edges must be strictly
  increasing and must not be renumbered after a mismatch.

The header is one line and must contain all of these keys:

```text
# brs-cycle-trace-v3 source=<rtl|gem5> sampling=posedge-pre-nba platform=<name> predictor_present=<0|1> btb_enabled=<0|1>
```

`predictor_present` describes physical/model presence. RTL may report `1`
while its BTB is disabled; gem5 reports `0`. `btb_enabled` describes the
effective run mode and must be `0` for the no-BTB comparison.

## Required record fields

All fields below must occur on every record. Payload values are written as
zero when their valid/enable field is zero; the comparator only compares such
payloads when meaningful.

| Group | Required fields | Meaning |
|---|---|---|
| Identity | `edge reset phase source platform cpu_cycle` | `reset` is always `0` in the active-only v3 interval. `source` and `platform` must match the header. |
| IBus | `ibus_req ibus_addr ibus_re ibus_resp ibus_r0 ibus_r1 ibus_r2 ibus_r3` | CPU-facing accepted fetch request and 128-bit response, in low-to-high 32-bit words. `ibus_re` equals `ibus_req`. |
| DBus | `dbus_req dbus_addr dbus_re dbus_we dbus_wstrb dbus_wdata dbus_resp dbus_rdata` | CPU-facing accepted 32-bit request/response. Exactly one of `dbus_re/dbus_we` is set for a request. |
| Retire/writeback | `retire retire_pc retire_instr wb_we wb_fp wb_rd wb_data` | Architectural retirement event and its normalized register writeback. |
| Control | `stall_mask redirect redirect_target grant` | Spirit five-bit SCU stall mask, redirect pulse/target, and HC response-valid alias. |
| No-BTB evidence | `set_btb_off btb_match predict_failed` | Effective disable input, raw BTB match, and ID-stage branch prediction-failure pulse. In the no-BTB run these must remain `1/0/<branch pulse>`. |
| CPU–VEU HC | `hc_req hc_addr hc_re hc_we hc_write_type hc_wdata hc_vestart hc_valid hc_rdata` | Spirit CBU/HC request and response boundary. `hc_req=hc_re|hc_we`. |
| Completion | `done done_value error error_value` | Pulses on the accepted DBus write to `0x4001e004`; bit 1 means done and bit 2 means error. The corresponding value is the complete 32-bit write data. |

`grant` is retained for compatibility with the v1/v2 comparator and is
strictly defined as `hc_valid`; it is not a memory-bus grant.

Writers may append diagnostic fields. v3 readers must reject missing fields,
unknown (`x/z`) values, non-contiguous edges, non-boolean valid bits, and
inconsistent aliases (`ibus_re`, DBus read/write, `hc_req`, or `grant`). Unknown
extra fields are ignored.

## Conditional comparison

- IBus address/read-enable is compared when either `ibus_req` is asserted;
  response words are compared when either `ibus_resp` is asserted.
- DBus request payload is compared when either `dbus_req` is asserted;
  `dbus_rdata` is compared when either `dbus_resp` is asserted.
- Retire payload and writeback are compared only for retire/writeback events.
- Redirect target is compared only on a redirect.
- HC request payload is compared only on `hc_req`; HC read data only on
  `hc_valid`.
- Completion/error values are compared only on their corresponding pulse.
- Valid bits, stall/control signals, and the three no-BTB evidence fields are
  compared unconditionally on every active edge.

Archived v1/v2 traces remain readable in compatibility mode and are not
subject to the v3 required-field check.
