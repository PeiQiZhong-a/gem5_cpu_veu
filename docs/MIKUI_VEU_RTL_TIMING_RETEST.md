# Mikui 128-bit VEU RTL 时序重新测试任务书

## 0. 给工作站 AI 的任务

请重新执行 Mikui 128-bit VEU RTL 时序采集。本轮不能继续使用已经确认失效的旧入口：

```text
sim/vcs/script/case_mikui/verilog.f
sim/testbench/tb/top_mikui_tb.sv
hardware/src/top/dut_mikui.sv
```

上一轮在源文件装载阶段就失败，动态仿真 case 数为 0。此次目标不是让旧命令勉强通过，
而是建立一套可编译、可运行、可追溯的专用 VEU 测试基线，采集足以直接修改以下 gem5
组件的真实 RTL 数据：

```text
configs/brs/veu_timing_profile.csv
src/brs/veu/timing_veu.{hh,cc}
src/brs/veu/veu_timing_profile.{hh,cc}
src/brs/memory/npu_lpnpu_mikui_crossbar.{hh,cc}
src/brs/memory/npu_lpnpu_mikui_memory_model.{hh,cc}
```

必须先完成第 4 节的解阻门禁。门禁通过后再执行测试矩阵。不要从源码静态推导数值，
不要复制 Yinglong/256-bit VEU 的数据，也不要用当前 gem5 legacy profile 作为期望值。

## 1. 上一轮失败事实

上一轮基线为：

```text
RTL commit: e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8
目标宽度:   128 bit / 16 byte
状态:       BLOCKED before elaboration
动态 case:  0
```

已确认的阻塞项：

1. `case_mikui/verilog.f` 把 AHB package 写成了不存在的
   `sim/testbench/ahb_driver_pkg.sv`；实际文件在
   `sim/testbench/AHB/ahb_driver_pkg.sv`。
2. 旧 filelist 还引用了不存在的 `PSAU.sv`、`PSFPMU.sv` 和
   `crossbar_mi.sv`。
3. 当前 `VFU.sv` 使用 `PSMAU.sv`，说明旧 filelist 没有随 VEU RTL 更新。
4. `dut_mikui.sv` 实例化不存在的 `crossbar_mi`；仓库中可见的是
   `crossbar_mi_full`、`crossbar_mi_mpw` 等不同实现，不能静默改名。
5. `VE_CORE.sv` 的 `veu_clk_en` 控制 `veu_gclk`，但旧 `dut_mikui.sv` 没有连接该输入。
6. 上一轮仓库快照没有所引用的 `testcase/mikui/gate_probe` firmware。

因此上一轮没有任何 latency、II、operation cycles 或 crossbar timing 可用于校准。

## 2. 本轮允许和禁止的操作

### 2.1 允许

允许在独立工作目录中新增以下仿真专用文件：

```text
sim/testbench/veu_timing_retest/...
sim/vcs/script/case_veu_timing_retest/...
```

允许新增：

- 专用 `VE_CORE` wrapper；
- 专用 `crossbar_mi_full` testbench 或 integration wrapper；
- 精简且真实存在的 filelist；
- directed CSR/SRAM/DBUS 激励；
- assertion、event logger、波形 dump 和结果归一化脚本。

所有新增或修改文件必须保存 diff 和 SHA-256。专用 wrapper 中允许固定
`veu_clk_en=1'b1`，但必须在 manifest 中标记为 `simulation_only_clock_enable_tieoff`。

### 2.2 禁止

禁止：

- 修改 VEU/VFU/VLU/VSU/VCU 功能 RTL 来让测试通过；
- 修改流水级、FIFO 深度、ready/valid 或 SRAM 延迟来迁就 gem5；
- 把 `crossbar_mi_full.sv` 简单重命名为 `crossbar_mi.sv` 而不核对接口；
- 继续以旧 `case_mikui` 的失败作为本轮最终结果；
- 因缺 firmware 而停止全部工作；Track A/B 必须使用自包含 directed testbench；
- 用系统时钟时间除法推算 VEU 拍数；
- 只返回最终总拍数、平均数或最终内存正确结果；
- 在 RTL 没有运行时生成 `rtl_sim` profile。

## 3. 冻结目标

### 3.1 RTL 身份

开始时记录：

```text
git rev-parse HEAD
git status --short
git submodule status（如有）
```

优先使用 commit：

```text
e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8
```

如果工作站必须使用不同 commit，允许继续，但必须：

1. 返回实际 commit；
2. 返回与上述 commit 的相关 VEU/top/crossbar/filelist diff；
3. 不得把不同 commit 的结果标记为上述 commit；
4. 所有结果使用实际 commit short SHA 命名。

### 3.2 目标结构

```text
VEU SRAM beat = 128 bit = 16 byte
地址步长      = 0x10 byte/chunk
写掩码        = 16 bit
bank 0        = 0x20010000..0x2001ffff
bank 1        = 0x20020000..0x2002ffff
```

不得使用 256-bit `dut_kui` 或 Yinglong VEU 数据。

### 3.3 必须分开的三个测试层次

| Track | DUT | 用途 | 可写入哪些模型 |
|---|---|---|---|
| A | `VE_CORE` + 确定性 128-bit SRAM responder | VEU 内部 CSR、VLU、FIFO、VFU、VSU、operation 时序 | profile、TimingVeu；不能作为真实 crossbar latency |
| B | 实际 `crossbar_mi_full` + SRAM bank model | crossbar 仲裁、请求/响应流水、bank、DBUS/VEU/SAU 争用 | Mikui memory/crossbar 模型 |
| C | `VE_CORE` + 实际 `crossbar_mi_full` 集成 wrapper，或已证明一致的 `dut_mikui_dma` 系统路径 | 端到端周期和 Track A/B 组合校验 | 最终对齐验收 |

Track A 和 Track B 都必须执行。Track C 若因接口/系统环境阻塞，允许返回
`PARTIAL_SUCCESS`，但必须提交 A/B 完整数据和 C 的首个真实阻塞点。

`case_mikui_dma`、`case_mikui_rvtest`、`dut_mikui_dma` 和
`crossbar_mi_full` 只是当前仓库中较一致的候选基线，不预先假定它们一定可以编译。
必须用实际 compile/elaboration log 证明。

## 4. 解阻门禁

### Gate R0：文件闭包

建立新的精简 filelist，逐条确认文件存在。VEU 部分至少核对当前依赖：

```text
hardware/ip/GCLK/v_clock_latch.v
hardware/ip/GCLK/v_clock_pass.v
hardware/src/veu/VCU.sv
hardware/src/veu/Async_FIFO_1I1O.sv
hardware/src/veu/VLU.sv
hardware/src/veu/HAU.sv
hardware/src/veu/HAU_4.sv
hardware/src/veu/PSMAU.sv
hardware/src/veu/extern_modules.sv
hardware/src/veu/PSLU.sv
hardware/src/veu/CSNU.sv
hardware/src/veu/CSNU_4.sv
hardware/src/veu/PSCU.sv
hardware/src/veu/PSMU.sv
hardware/src/veu/BSU.sv
hardware/src/veu/BSU_4.sv
hardware/src/veu/PSSRU.sv
hardware/src/veu/PSCDU.sv
hardware/src/veu/PSRSU.sv
hardware/src/veu/VFU.sv
hardware/src/veu/VSU.sv
hardware/src/veu/VSPBU.sv
hardware/src/veu/VEU.sv
hardware/src/veu/VE_CORE.sv
```

以编译器的真实依赖和报错为准；不要把不存在且未被当前 RTL 实例化的
`PSAU.sv/PSFPMU.sv` 放回新 filelist。

交付：

```text
build/track_a/filelist.resolved.txt
build/track_b/filelist.resolved.txt
build/track_c/filelist.resolved.txt（若执行）
rtl_file_sha256.txt
testbench_file_sha256.txt
```

### Gate R1：编译与 elaboration

分别保存 Track A/B/C 的完整 compile 和 elaboration 命令、返回码和日志。

通过条件：

```text
compile return code = 0
elaboration return code = 0
目标 top 唯一
无 unresolved module
无 missing source
无 implicit port-width truncation
无 clock/reset/关键控制输入悬空
```

warning 不能全部忽略。将每条 warning 分类为：

```text
accepted_with_reason
fixed_in_testbench
blocking
```

### Gate R2：时钟、复位和 X/Z

Track A/C 必须在 smoke test 波形中证明：

```text
veu_clk_en = 1 或设计规定的确定值
veu_gclk 正常翻转
reset 释放后 cycle 计数从第一个 veu_gclk posedge 开始
CSR、status、lock、SRAM request/response 不含 X/Z
```

统一采样规则：

```text
cycle 0 = reset 释放后的第一个有效 veu_gclk 上升沿
每拍在 posedge 的 NBA 更新完成后采样
所有 profile 周期使用 veu_gclk_cycle
同时记录 system_cycle
```

若使用 wrapper 固定 `veu_clk_en=1`，报告中不得声称测到了生产 SoC 的动态 clock-gating
开销；它只用于得到 VEU 有效时钟域内的拍数。

### Gate R3：自包含 smoke test

在完整矩阵前执行：

```text
R3A: reset + CSR read/write
R3B: vadd vector, chunks=1, full mask
R3C: crossbar 单笔 VEU read、单笔 VEU write、单笔 DBUS read/write
```

必须观察到：

- CSR 有确定响应；
- VEU status/lock 能启动并结束；
- 至少一笔 read issue/return 和 write/bank commit；
- 目标数据正确；
- 无 timeout、X/Z、重复 return 或丢请求。

只有 R0-R3 全部通过，才能生成正式 timing profile。

## 5. Track A：VEU 内部时序测试

### 5.1 SRAM responder 冻结要求

Track A 使用简单、确定、无争用的 128-bit SRAM responder。必须记录：

```text
request acceptance 规则
read response latency
write commit latency
ready/valid 定义
是否允许 backpressure
同拍 read/write 处理规则
```

基础 profile 先固定为无随机 backpressure。另做少量 backpressure probe，但不能把
随机 stall 混入 VFU latency。

### 5.2 真正改变拍数的维度

正式分支：

| 维度 | 处理 |
|---|---|
| operation/timing family | 分支 |
| scalar enable | 分支 |
| effective chunk count | 分支：1/2/4/8 |
| zero mask 与 nonzero mask | 分支 |
| source set | 由 op + scalar 派生，不做独立笛卡尔积 |

先做一次等价证明、通过后不再分支：

| 维度 | 代表测试 |
|---|---|
| mode 0/1/2/3 | 每个底层 VFU 单元选一个代表 op、chunks=2 |
| full 与 partial nonzero mask | vadd vector、chunks=8，比较 `0xffff` 与 `0x5555` |
| 输入数据 | vadd vector、chunks=8，普通数据与边界/饱和数据 |
| shift/scalar 数值 | `0,1,7,15`，代表 vssrl 和 vslideup |
| 无争用 bank 0/1 | vadd vector、chunks=8 |

若事件序列不同，相关维度自动升级为正式分支，并补齐必要矩阵。禁止取平均值。

### 5.3 时序族和正常操作矩阵

每个代表操作执行 `chunks=1,2,4,8`、`mask=0xffff`：

| 时序族 | 代表操作 | scalar | source set |
|---|---|---:|---|
| add/sub vector | `vadd` | 0 | src1+src2 |
| add scalar | `vadd` | 1 | src2 |
| logic | `vand` | 0 | src1+src2 |
| compare | `vmin` | 0 | src1+src2 |
| reduce compare | `vredmin` | 0 | src1 |
| move vector | `vmv` | 0 | src1 |
| move scalar | `vmv` | 1 | none |
| slide up | `vslideup` | 1 | src2 |
| slide down | `vslidedown` | 1 | src2 |
| shift | `vssrl` | 1 | src2 |
| narrow/clip | `vnclip` | 1 | src2 |
| reduce sum | `vredsum` | 0 | src1 |
| multiply | `vmul` | 0 | src1+src2 |

族内等价成员先只测 chunks 1/8：

```text
vsub      vs vadd vector
vor/vxor  vs vand
vmax      vs vmin
vredmax   vs vredmin
vssra     vs vssrl scalar
```

事件序列一致则复用代表族；不一致则拆族并补 chunks 2/4。

### 5.4 zero-mask

对每个代表族执行：

```text
mask=0x0000, chunks=1,8
```

必须验证：

```text
write_request_count = 0
bank_write_count = 0
status 和 lock 正常结束
```

若 1/8 两点不能证明稳定公式，补测 chunks 2/4。

### 5.5 VLEN 边界

仅用 vadd vector 测试：

```text
requested VLEN = 0,1,127,128,129,255,256,257,511,512
```

返回 effective VLEN/chunks、读写数量、status/lock 终态和是否 timeout。非 128-bit
对齐行为属于协议语义，不需要对所有 op 重复。

### 5.6 illegal/unsupported

分别测试：

```text
vcompress
vmac
vmsub
vmulhsu
vwredsum
vmulh
unknown start bit
```

记录 illegal cycle、读/VFU/写活动、status/lock 终态、destination 是否改变及 timeout。
卡住是有效观察结果，必须标为 `stuck/timeout`，不能改写成 no-op PASS。

## 6. Track B：实际 Mikui crossbar/memory 时序

Track B 必须直接实例化实际选定且 elaborated 的 crossbar 模块。当前优先候选是
`crossbar_mi_full`，但选择前必须记录选择理由、模块参数、接口和 SHA-256。

### 6.1 冻结参数

至少返回：

```text
NUM_CROSS
NUM_SLAVE
SRAM_RESP_DELAY
SRAM_SPLIT_BASE/END 或实际地址映射参数
每个 bank SRAM 模型的读/写 latency
请求、ack、response 的寄存边界
仲裁优先级
lock/start/done 语义
```

### 6.2 无争用基础矩阵

对 bank 0 和 bank 1 分别执行：

```text
VEU single read
VEU back-to-back 8 reads
VEU single full write
VEU back-to-back 8 writes
VEU alternating read/write
DBUS single read/write
SAU single read/write（接口存在时）
```

每笔返回：

```text
master request cycle
request accepted cycle
slave request cycle
bank commit/return cycle
master response/data cycle
transaction id
address/bank/wstrb
```

### 6.3 争用矩阵

至少测试：

```text
VEU + DBUS，同 bank，同周期请求
VEU + DBUS，不同 bank，同周期请求
VEU + SAU，同 bank，同周期请求
VEU + SAU，不同 bank，同周期请求
DBUS request 位于 VEU start 同周期
DBUS request 位于 VEU active/lock 窗口
DBUS request 位于 VEU done/release 同周期
连续 8 拍 DBUS 压力下的 VEU burst
连续 8 拍 VEU 压力下的 DBUS burst
```

明确返回 winner、loser 重试/保持/丢弃行为、最大等待拍数、公平性、same-bank 和
different-bank 是否可以并行。

## 7. Track C：端到端组合校验

优先方案是新增一个仿真专用 integration wrapper：

```text
directed CSR driver
  -> VE_CORE（veu_clk_en 明确连接）
  -> actual crossbar_mi_full
  -> actual/等价参数化 SRAM banks
```

也可以使用 `dut_mikui_dma + top_mikui_rvtest_tb/top_mikui_dma_tb`，前提是实际
compile/elaboration/smoke test 通过，并能施加所需 VEU directed case。

Track C 至少执行：

```text
vadd vector, chunks=1,8, nonzero
vadd vector, chunks=1,8, zero mask
vmul, chunks=1,8
vslidedown scalar, chunks=1,8
vredsum, chunks=1,8
vadd chunks=8 + DBUS same-bank contention
vadd chunks=8 + DBUS different-bank contention
```

比较 Track C 的事件序列与“Track A VEU 事件 + Track B 实际 memory latency”的组合结果。
如不一致，返回首个分歧 cycle，不能使用常数 offset 强行平移。

## 8. 事件和拍数定义

每个正常 case 至少记录：

| 事件 | 定义 |
|---|---|
| `csr_request` | CSR 请求第一次满足接受条件 |
| `csr_response` | CSR response/ready 有效 |
| `operation_start` | 最终启动请求周期，记为 Creq |
| `status_set/status_clear` | busy/status bit 首次置位/清零 |
| `lock_start/lock_finish` | VEU crossbar ownership 开始/结束 |
| `read_issue/read_return` | VLU 请求发射及对应数据返回 |
| `fifo_push/fifo_pop` | source FIFO 实际动作 |
| `vfu_accept/vfu_complete` | 运算单元真实接受/完成 token |
| `vsu_accept` | VSU 接受 VFU 结果 |
| `store_candidate/store_grant` | 写候选及获得 VSPBU 端口 |
| `write_request/bank_write` | 外部写请求及 bank 真正提交 |
| `operation_finish` | lock 释放且所有内部/外部事件 drain |
| `illegal_assert` | illegal/error 有效 |

统一计算：

```text
csr_response_latency = csr_response - csr_request
startup_cycles       = first_read_issue - Creq（无读为 null）
lock_start_delay     = lock_start - Creq
read_return_latency  = matched_read_return - read_issue
vfu_latency          = matched_vfu_complete - vfu_accept
vfu_ii               = 饱和输入时相邻 vfu_accept 的稳定最小间隔
vsu_latency          = write_request/store_candidate - vsu_accept
bank_write_latency   = bank_write - write_request
finish_drain_cycles  = lock_finish - status_clear
operation_cycles     = operation_finish - Creq
```

所有 read、VFU token 和 write 必须有稳定 `transaction_id/token_id` 配对，不能只凭周期
接近猜测。

## 9. 每个 case 必须返回的数据

### 9.1 元数据

```text
case_id
track
result = PASS/FAIL/TIMEOUT/BLOCKED
rtl commit/dirty state
top/DUT/VEU/crossbar hierarchy
filelist hash
testbench/wrapper hash
compile/elaboration/run command
clock/reset/sampling rule
op/mode/scalar/mask/requested VLEN/effective VLEN/chunks
source/destination addresses and bank
SRAM/crossbar parameters
timeout cycles
```

### 9.2 计数和时序

```text
csr_response_latency
startup_cycles
lock_start_delay
read_return_latency_min/max
fifo_depth and maximum occupancy per source
max_outstanding_reads
vfu_latency_min/max
vfu_ii
vsu_latency
bank_write_latency
status_active_cycles
lock_active_cycles
finish_drain_cycles
operation_cycles
read_issue/return counts
fifo push/pop counts
vfu accept/complete counts
write_request/bank_write counts
read blocked by store cycles
crossbar stall/retry/drop counts
first/last event offsets
```

### 9.3 功能与守恒检查

```text
每个 read_issue 恰好匹配一个 return 和正确 source FIFO push
每个 FIFO pop 有已有 entry
每个 vfu_accept 匹配一个 complete
每个 write_request 匹配一个 bank_write
zero mask 无 write/bank_write
最后写提交不晚于 lock_finish/operation_finish
结束时 request/return/FIFO/VFU/VSU/store 全部 drain
destination memory 与 reference 一致
```

功能正确不能代替时序守恒。

## 10. 返回目录

返回：

```text
mikui_veu_rtl_timing_retest_<date>_<rtl_short_sha>/
```

必须包含：

```text
README.md
manifest.json
environment.json
baseline_resolution.md
source_and_tb.patch
rtl_file_sha256.txt
testbench_file_sha256.txt
global_timing.json
timing_equivalence.json
timing_summary.csv
veu_timing_profile_v4.csv
crossbar_timing.json
model_gap_report.md
logs/track_a_compile.log
logs/track_a_elab.log
logs/track_b_compile.log
logs/track_b_elab.log
logs/track_c_compile.log（如执行）
logs/track_c_elab.log（如执行）
cases/<case_id>/metadata.json
cases/<case_id>/events.csv
cases/<case_id>/raw_signals.csv
cases/<case_id>/functional_vectors.csv
cases/<case_id>/destination_memory_dump.csv
cases/<case_id>/checks.json
cases/<case_id>/sim.log
cases/<case_id>/wave.fsdb 或 wave.vcd
```

### 10.1 `events.csv` 固定表头

```csv
event_seq,track,veu_cycle,system_cycle,event,case_id,op,scalar_en,mode,mask,requested_vlen,effective_vlen,chunk,source,token_id,transaction_id,address,bank,wstrb,data,status,lock,fifo1,fifo2,outstanding,detail
```

### 10.2 `timing_summary.csv` 固定表头

```csv
case_id,track,result,op,timing_family,scalar_en,mode,mask_class,source_set,requested_vlen,effective_vlen,chunk_class,csr_response_latency,startup_cycles,lock_start_delay,read_return_latency_min,read_return_latency_max,fifo_depth,max_outstanding_reads,vfu_latency_min,vfu_latency_max,vfu_ii,vsu_latency,bank_write_latency,status_active_cycles,lock_active_cycles,finish_drain_cycles,operation_cycles,read_count,vfu_accept_count,vfu_complete_count,write_count,bank_write_count,crossbar_stall_cycles,write_policy,illegal_cycle,timeout_cycle,evidence_id
```

### 10.3 `global_timing.json`

```json
{
  "schema": "mikui-veu-global-timing-retest-v1",
  "track_a": {
    "csr_response_latency": null,
    "startup_cycles": null,
    "lock_start_delay": null,
    "fifo_depth_src1": null,
    "fifo_depth_src2": null,
    "read_issue_width": null,
    "max_outstanding_reads": null,
    "vsu_latency": null,
    "finish_drain_cycles": null,
    "evidence_case_ids": []
  },
  "track_b": {
    "crossbar_module": null,
    "crossbar_parameters": {},
    "master_to_slave_request": null,
    "slave_request_to_bank": null,
    "bank_to_ack": null,
    "ack_to_master_data": null,
    "bank_write_latency": null,
    "same_bank_policy": null,
    "different_bank_parallel": null,
    "evidence_case_ids": []
  },
  "track_c": {
    "executed": false,
    "composition_matches": null,
    "first_mismatch": null,
    "evidence_case_ids": []
  }
}
```

### 10.4 `timing_equivalence.json`

至少包含：

```json
{
  "mode_affects_timing": null,
  "data_affects_timing": null,
  "shift_value_affects_timing": null,
  "full_vs_partial_nonzero_affects_timing": null,
  "zero_vs_nonzero_affects_timing": null,
  "bank_affects_timing_without_contention": null,
  "family_equivalence": [],
  "mismatches": [],
  "evidence_case_ids": []
}
```

布尔值必须来自事件序列比较。未测试写 `null`，不能写推测值。

### 10.5 `veu_timing_profile_v4.csv`

固定表头：

```csv
profile_id,op,mode,scalar_en,mask_class,source_set,chunk_class,vfu_latency,vfu_ii,write_policy,fifo_depth,max_outstanding_reads,vsu_latency,lock_start_delay,finish_drain_cycles,operation_cycles,timing_source,evidence_id
```

规则：

1. 只有 Gate R0-R3 通过且对应 Track A/C case PASS 才能生成数据行；
2. `timing_source=rtl_sim`；
3. mode 等价证明通过后才允许 `mode=*`；
4. op/scalar/mask/source/chunk 不得使用无证据通配；
5. zero mask 必须引用 zero-mask evidence；
6. Track B crossbar latency不能写进 `vfu_latency`；
7. 每行 `evidence_id` 必须定位到 case、events、wave 和 hashes。

### 10.6 `crossbar_timing.json`

每类 transaction/争用组合返回原始样本，不只返回平均值：

```json
{
  "schema": "mikui-crossbar-timing-v1",
  "module": "crossbar_mi_full",
  "parameters": {},
  "no_contention": [],
  "same_bank_contention": [],
  "different_bank_contention": [],
  "lock_window_cases": [],
  "request_loss_observed": null,
  "retry_required": null,
  "evidence_case_ids": []
}
```

## 11. 结果状态与失败返回规则

### SUCCESS

R0-R3、Track A、Track B、Track C 和全部必要矩阵完成，可生成 profile。

### PARTIAL_SUCCESS

Track A/B 已完整完成，但 Track C 因明确原因阻塞。返回 A/B 全部实测数据，不生成未经
Track C 验证的“最终对齐完成”结论。

### BLOCKED

只有在完成独立 wrapper/filelist 尝试后仍无法 compile/elaborate 或缺少不可替代的
授权/IP/model 时使用。必须返回：

```text
失败 track 和 gate
实际执行命令
返回码
第一个真实错误
完整 log
已解析 filelist
相关文件 hash
已尝试的最小修复
为什么不能继续 Track A 或 Track B
解除阻塞所需的一个具体输入
```

不要因为旧 `case_mikui` 缺文件再次直接返回 BLOCKED；本报告已经要求绕开该入口。

## 12. 工作站最终报告必须回答的问题

1. 实际测量的 RTL commit、top、VEU hierarchy、crossbar module 是什么？
2. `veu_clk_en` 如何驱动，`veu_gclk` 如何编号？
3. Track A 的 SRAM responder latency 是多少？它是否与 Track B 真实 crossbar 区分？
4. 哪些配置维度实际改变事件周期，哪些已由等价测试证明不改变？
5. 每个时序族的 VFU latency、II、operation cycles 和 chunk scaling 是什么？
6. zero mask 与 nonzero mask 的时序差异是什么？
7. 实际 crossbar 的无争用流水和同/异 bank 争用规则是什么？
8. status clear、最后 bank write、lock finish 的真实顺序是什么？
9. Track C 能否由 Track A + Track B 精确组合？首个不一致周期在哪里？
10. 当前 gem5 profile、TimingVeu、memory/crossbar 分别缺少哪些字段或状态？

## 13. 返回数据到 gem5 的修改映射

| RTL 返回数据 | gem5 修改位置 |
|---|---|
| per-family VFU latency/II | timing profile、`VeuTimingSelection` |
| FIFO depth/occupancy/outstanding | profile、`TimingVeu::readCanIssue/makeReadRequest` |
| startup/lock/status/drain | `TimingVeu::startVectorOperation/enterDrainingIfDone` |
| VSU/write policy | `TimingVeu::advancePipelines` 和 store queue |
| requested/effective VLEN | VLEN CSR/start 处理 |
| illegal/stuck 行为 | functional describe 与状态机 |
| crossbar request/ack/response | `npu_lpnpu_mikui_crossbar` |
| bank read/write commit | `npu_lpnpu_mikui_memory_model` |
| DBUS/VEU/SAU 争用 | crossbar arbitration、retry 和 CPU blocking |
| Track C 组合差异 | 新增必要状态/流水，而不是常数补偿 |

返回上述 artifacts 后，gem5 侧应能直接据 evidence 修改模型并运行逐事件回归。

## 14. 完成标准

只有同时满足以下条件，才可声明重测完成：

1. 没有复用失效的旧 `case_mikui` 作为最终仿真入口；
2. Track A/B 均成功 compile、elaborate 和动态运行；
3. clock/reset/X-Z 门禁通过；
4. 正常时序族完成要求的 chunk 和 mask 测试；
5. mode/data/shift/partial/bank 合并有动态证据；
6. read/FIFO/VFU/write 守恒检查通过；
7. crossbar 无争用和争用流水有逐 transaction 证据；
8. 每个 profile 行可追溯到 events、wave、commit 和 hashes；
9. Track A 的测试 SRAM timing 与 Track B 的真实 crossbar timing 没有混写；
10. 未完成项明确标为 null/PARTIAL_SUCCESS/BLOCKED，没有伪造数据。

## 15. 可直接复制给工作站 AI 的简短指令

```text
严格执行 MIKUI_VEU_RTL_TIMING_RETEST.md。

不要再次使用已失效的 case_mikui + dut_mikui 入口作为最终基线。创建可追溯的仿真
专用 wrapper 和精简 filelist：Track A 使用 VE_CORE + 明确 veu_clk_en + 确定性
128-bit SRAM responder，采集 VEU 内部逐拍数据；Track B 独立实例化实际
crossbar_mi_full 和 SRAM banks，采集无争用及 DBUS/VEU/SAU 争用；Track C 再做
VE_CORE + 实际 crossbar 的端到端组合校验。

先通过文件闭包、compile/elaboration、clock/reset/X-Z 和 smoke test 四个门禁，再执行
时序族、chunk 1/2/4/8、zero/nonzero、等价证明、VLEN 边界和 illegal 矩阵。所有周期按
reset 后 veu_gclk 上升沿、NBA 后采样。返回逐事件 CSV、raw signals、波形、功能/守恒
检查、compile/elab/run 日志、patch、hash、summary、global/equivalence/crossbar JSON、
候选 v4 profile 和 model gap report。

旧 case_mikui 缺文件不再是停止理由。若独立 Track 仍失败，返回该 Track 的第一个真实
错误及完整证据；不得生成静态估值或伪造 rtl_sim profile。
```
