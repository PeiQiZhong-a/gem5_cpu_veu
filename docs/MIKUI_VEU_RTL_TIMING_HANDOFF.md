# Mikui 128-bit VEU RTL 时序采集与 gem5 对齐交接规范

## 1. 文档目的

本文档交给工作站上的 AI，用于对 Mikui 128-bit VEU RTL 进行可复现的逐拍仿真，
并返回足以直接修改以下 gem5 模型的数据：

```text
src/brs/veu/timing_veu.{hh,cc}
src/brs/veu/veu_timing_profile.{hh,cc}
configs/brs/veu_timing_profile.csv
src/brs/memory/npu_lpnpu_mikui_*.{hh,cc}
```

目标是同时对齐：

1. CSR 请求/响应；
2. status busy 和 SRAM lock；
3. VLU 读调度、SRAM 返回和 FIFO；
4. VFU 接受、完成及吞吐；
5. VSU/VSPBU 写回和 store-priority；
6. operation 完成周期；
7. CPU DBUS 与 VEU 共享 SRAM 时的阻塞行为。

本文档只要求测量和返回数据。工作站 AI 不应自行修改 RTL 功能，也不应通过调整
testbench 延迟来“校准”周期。

## 2. 冻结的目标和源码基线

### 2.1 目标必须是 Mikui 128-bit VEU

目标特征：

```text
VEU SRAM beat       = 128 bit = 16 byte
地址步长             = 0x10 byte
写掩码               = 16 bit
正常 full mask       = 0xffff
数据 bank            = 2
bank 0               = 0x20010000..0x2001ffff
bank 1               = 0x20020000..0x2002ffff
```

不要使用 Yinglong/旧 256-bit VEU 的周期数据。即使模块名相同，数据宽度、source
路径、VLEN 粒度和部分 illegal-op 行为也不同。

本次静态审查使用的 Mikui 仓库为：

```text
repository: /home/lenovo-pccarryking/gem5_workspace/mikui
commit: e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8
commit subject: update mikui clk tree doc
```

关键 RTL：

```text
hardware/src/veu/VE_CORE.sv
hardware/src/veu/VEU.sv
hardware/src/veu/VCU.sv
hardware/src/veu/VLU.sv
hardware/src/veu/VFU.sv
hardware/src/veu/VSU.sv
hardware/src/veu/VSPBU.sv
hardware/src/veu/Async_FIFO_1I1O.sv
hardware/src/veu/PSMAU.sv
hardware/src/veu/PSLU.sv
hardware/src/veu/PSCU.sv
hardware/src/veu/PSMU.sv
hardware/src/veu/PSSRU.sv
hardware/src/veu/PSCDU.sv
hardware/src/veu/PSRSU.sv
hardware/src/top/dut_mikui.sv
hardware/src/memory/sram_tcdm.sv
sim/testbench/tb/top_mikui_tb.sv
```

### 2.2 工作站开始仿真前的强制检查

工作站 AI 必须报告实际 elaboration 使用的文件列表及 SHA-256，不能只报告 Git
commit。当前源码快照存在一个必须核实的接口风险：

- `VE_CORE.sv` 声明了 `veu_clk_en` 输入并用它产生 `veu_gclk`；
- 当前 `dut_mikui.sv` 中的 `VE_CORE VEU_1_inst` 未显式连接 `veu_clk_en`。

在任何时序采集前必须从 elaboration log 和波形确认：

```text
veu_clk_en 为确定的 1（或按设计要求正确拉高）
veu_gclk 连续翻转
VEU reset 正常释放
无 X/Z 传播到 clock、reset、CSR 或 SRAM 接口
```

若上述条件不满足，应停止采集并返回 `BLOCKED`，不能生成 profile。允许在专用
testbench wrapper 中显式连接 clock-enable，但必须记录补丁和 hash；不得静默修改
生产 RTL。

还必须确认实际编译使用的是哪个 `crossbar_mi` 实现。仓库中存在多个历史 crossbar
版本，顶层实例名本身不足以确定行为。

## 3. Cycle 和采样约定

所有结果统一使用 VEU 有效时钟 `veu_gclk` 的上升沿编号。

```text
cycle 0 = VEU reset 解除后的第一个有效 veu_gclk 上升沿
reset 周期不计入 operation latency
每个周期在上升沿 NBA 更新完成、信号稳定后采样
```

如果同时记录系统 `clk` 和 `veu_gclk`，两个 cycle 编号都要输出；用于 profile 的周期
必须是 `veu_gclk_cycle`。若 clock gating 在 operation 前后停钟，还必须记录每次 gated
edge，不能用墙钟时间除以周期进行推算。

### 3.1 标准事件定义

| 事件 | 定义 |
|---|---|
| `csr_request` | CSR 请求第一次满足 RTL 接受条件的采样周期 |
| `csr_response` | `csr_ready/hc2rv_csr_valid` 有效周期 |
| `operation_start` | 最终使 `VCU.r_status[0]` 从 0 变 1 的请求周期，记为 `Creq` |
| `status_set` | `r_status[0]` 第一次为 1 |
| `lock_start` | `status_out_lock_start/veu_crossbar_start` 有效 |
| `read_candidate` | 某 source 尚有数据且 FIFO/仲裁条件允许尝试读 |
| `read_issue` | `VLU.issue_pulse`，并记录 `issue_tag`、地址和 chunk |
| `read_return` | 对应 SRAM 数据在 VLU 返回流水可消费 |
| `fifo_push` | source FIFO 实际 push |
| `fifo_pop` | source FIFO 实际 pop |
| `vfu_accept` | `VFU.operandsReady` 或等价的真实运算单元 enable |
| `vfu_complete` | `VFU.w_complete` |
| `vsu_accept` | `vf2vs_vf_ready` 被 VSU 采样 |
| `store_candidate` | VSU 有有效非零写掩码结果 |
| `store_grant` | VSPBU 选择 store 而非 VLU read |
| `write_request` | `ve2tcm_sram_enable && ve2tcm_sram_wstrb != 0` |
| `bank_write` | 目标 SRAM bank 真正提交写入的边沿 |
| `status_clear` | `r_status[0]` 从 1 变 0 |
| `lock_finish` | `status_out_lock_finish/veu_crossbar_done` 有效 |
| `operation_finish` | lock 已释放且 operation 内所有事件 drain |
| `illegal_assert` | `illegal_error/veu_crossbar_error` 有效 |

VMAC/VMSUB 等可能存在两次命令/CSR 请求时，以最终改变 status/illegal 状态的请求作为
`Creq`；第一次请求的握手仍单独记录。

## 4. 哪些配置会改变拍数

以下结论来自上述 Mikui RTL 的控制和 complete 链静态审查。标记为“不拆分”的维度
仍要完成本文要求的一次性等价验证；验证通过后不再扩展笛卡尔积。

### 4.1 最终判定表

| 配置维度 | 是否作为时序分支 | 原因和处理方式 |
|---|---:|---|
| `op` | 是，但按时序族合并 | 不同运算单元 complete 寄存级数不同；部分 op 还有不同 VCU `cycles_mode`、reduction 或 slide 控制 |
| `scalar_en` | 是 | 会取消 source1 SRAM 读取；`vmv scalar` 甚至不需要 source FIFO，改变首输入和总周期 |
| `chunk_count` / effective VLEN | 是 | 改变读、VFU token、写次数，并引入 FIFO/store 重叠；reduction/slidedown 可能非简单线性 |
| `mask == 0` 与 `mask != 0` | 是 | zero mask 使 VSU 不产生写，消除 store-priority stall；非零 mask 会写 SRAM |
| full mask 与任意 partial nonzero mask | 否 | RTL 对时序只判断写掩码是否非零；具体 bit 只决定写入哪些 byte |
| `source_set` | 否，不独立扫描 | 它由 `op + scalar_en` 决定；在输出 profile 时填写精确派生值 |
| `mode[1:0]` | 否 | mode 改变 8/16-bit、signed/unsigned 和数据选择，但 complete 链不依赖 mode |
| 输入数据值 | 否 | 当前数据通路无 data-dependent ready/complete/early-exit |
| shift 数值 | 否 | shift amount 只进入组合移位/clip 逻辑，不改变 complete 链 |
| scalar 数值 | 否 | 只影响运算数据或 slide amount，不改变 source 调度结构 |
| partial mask 的具体位形 | 否 | 只影响 byte write enable；`0x0001`、`0x00ff`、`0x5555` 均属于 nonzero |
| SRAM 地址内容 | 否 | VEU 自身控制不依赖数据地址内容；地址必须 16-byte 对齐 |
| SRAM bank | 正常无竞争时否 | VEU 每周期最多一个外部请求；有 CPU/SAU 同银行竞争时另做系统级 contention case |
| reset 长度 | 否 | 只要满足 reset 时序并从统一 cycle 0 开始，不作为 profile 分支 |
| clock frequency | cycle 数通常否 | profile 使用 cycle；频率、门控和跨时钟同步仍必须冻结并记录 |

### 4.2 mode 不拆分的 RTL 依据

`mode` 进入 PSMAU/PSCU/PSSRU/PSCDU/PSRSU 的数据宽度、有符号和结果选择逻辑，
但各单元的 `*_out_complete` 来自固定寄存链。它不进入：

```text
VCU busy/lock/status 状态推进
VLU read issue 数量
FIFO push/pop 条件
VSU valid 寄存级数
VSPBU read/store 优先级
```

因此 profile 的 `mode` 可以写 `*`。工作站只需按第 7 节执行 7 个 VFU 单元的
mode-equivalence 代表测试；不需要为每个 op × mode × VLEN 重跑。

如果实际波形显示任一 mode 的事件 cycle 序列不同，必须停止合并，返回
`mode_timing_mismatch=true` 和全部原始证据；不能平均。

### 4.3 mask 只分 zero/nonzero

VSU 的写请求条件为：

```text
vf2vs_vf_ready && (|vf2vs_wstrb)
```

所以：

- `0xffff` 和任意 partial nonzero mask 的写请求周期结构相同；
- `0x0000` 完全取消外部写，可能改变后续读的 stall 和总周期。

当前 gem5 profile key 使用 `full/partial/zero` 三类。完成一次 full-vs-partial 等价
证明后，可以让 full 和 partial profile 行引用同一 timing evidence；无需对每个操作
分别重跑 partial mask。zero 必须保留独立时序分支。

### 4.4 source_set 不是输入维度

工作站不得把所有 source_set 与所有 op 做组合。按 RTL/当前 gem5 支持路径派生：

| 形式 | source_set |
|---|---|
| 普通 vector 二元运算 | `src1+src2` |
| scalar 二元/shift/slide | `src2` |
| vector move、正常 reduction | `src1` |
| scalar move | `none` |

如工作站发现实际 RTL 与上表不同，应返回实际 read_issue 序列，由 gem5 侧修正
functional/source 描述，不能手工把结果改成上表。

## 5. 时序族及静态预期

下面的“VFU latency 静态预期”是从 complete 寄存链推导的检查值，不是最终校准结果。
最终值必须以统一采样约定下的 `vfu_complete_cycle - vfu_accept_cycle` 为准。

| 时序族 | 覆盖操作 | VCU/VFU 特点 | VFU latency 静态预期 |
|---|---|---|---:|
| `addsub_vector` | `vadd vector`, `vsub vector` | PSMAU add/sub，双 source | 1 |
| `add_scalar` | `vadd scalar`，以及确实软件可达的同类 scalar op | 单 source，首读/总周期不同 | 1 |
| `logic_vector` | `vand`, `vor`, `vxor` | PSLU，三者控制相同 | 1 |
| `compare_vector` | `vmin`, `vmax` | PSCU；VCU 特殊 cycles_mode | 2 |
| `reduce_compare` | `vredmin`, `vredmax` | PSCU、单 source、reduction mask/waddr | 2 |
| `move_vector` | `vmv vector` | PSMU、单 source | 1 |
| `move_scalar` | `vmv scalar` | PSMU、无 SRAM source | 1 |
| `slideup_scalar` | `vslideup scalar` | PSMU，保留前一 chunk | 1 |
| `slidedown_scalar` | `vslidedown scalar` | PSMU 首/尾 token 抑制和 delay-enable | 不能仅按普通 1-cycle 处理 |
| `shift_scalar` | `vssrl scalar`, `vssra scalar` | PSSRU、单 source | 1 |
| `nclip_scalar` | `vnclip scalar` | PSCDU、单 source | 1 |
| `reduce_sum` | `vredsum` | PSRSU、跨 chunk 累加、特殊写回 | 1 |
| `multiply_vector` | `vmul` | PSMAU multiplier pipeline；VCU cycles_mode=2 | 3 |

以下不是正常时序族，必须独立做 legality/termination probe：

| 操作 | 当前静态状态 |
|---|---|
| `vcompress` | VCU 有状态位概念，但当前 Mikui VFU 无完整 datapath/select |
| `vmac`, `vmsub` | Mikui 无第三 source VLU 路径；不可套用 Yinglong 结果 |
| `vmulhsu` | unsupported/illegal |
| `vwredsum` | VFU 有相关数据路径，但 VCU `illegal_error` 包含该状态位 |
| `vmulh` | VFU/PSMAU 有路径，但 VCU 将对应高位状态归入 illegal |

## 6. 真正需要返回的参数

### 6.1 全局结构参数：只测一次，不按 op 重复

| 参数 | 测量/确认方法 | gem5 用途 |
|---|---|---|
| `csr_response_latency` | `csr_response - csr_request` | HC/CBU handshake 检查 |
| `startup_cycles` | `first_read_issue - Creq`；无读操作记 `null` | `VeuTimingConfig.startupCycles`；若按族不同则需扩展 profile |
| `lock_start_delay` | `lock_start - Creq` | v4 `lock_start_delay` |
| `fifo_depth_src1/src2` | RTL 结构 + occupancy/full 波形 | v4 `fifo_depth`，当前静态结构为 4 |
| `read_issue_width` | 同周期最多多少个 `read_issue` | TimingVeu 发射模型；当前静态预期为 1 |
| `read_return_latency` | 每笔 `fifo_push - read_issue` | memory/VLU 边界；不能混入 VFU latency |
| `max_outstanding_reads` | `max(累计 issue - 累计 return)` | v4 `max_outstanding_reads` |
| `vsu_latency` | `write_request/store_candidate - vsu_accept`，并保留事件 | v4 `vsu_latency`；当前 VSU 为一级寄存 |
| `bank_write_latency` | `bank_write - write_request` | Mikui memory/crossbar 模型 |
| `finish_drain_cycles` | `lock_finish - status_clear` | v4 `finish_drain_cycles` |
| `crossbar_start_latency` | VEU lock_start 到 crossbar ACTIVE/首请求可接受 | memory model |
| `crossbar_response_pipeline` | request、bank request、ack、master data 每一级 cycle | memory model |
| `dbus_block_window` | Creq/lock_start/lock_finish 前后的 DBUS 接受情况 | CPU/VEU SRAM ownership |

全局参数应使用 long-vector、双 source、nonzero mask 的饱和 case 测量，推荐
`vadd vector, chunk_count=8`。

### 6.2 每个时序族、scalar 分支、chunk 分支需要的数据

每个被要求的 case 都返回：

```text
vfu_latency
vfu_ii
operation_cycles
status_active_cycles
lock_active_cycles
read_issue_count
read_return_count
fifo_push_count
fifo_pop_count
vfu_accept_count
vfu_complete_count
write_request_count
bank_write_count
store_candidate_cycles
store_grant_cycles
read_candidate_cycles
reads_blocked_by_store_cycles
vspbu_stall_cycles
first_read_issue_offset
first_vfu_accept_offset
first_vfu_complete_offset
first_write_offset
last_bank_write_offset
status_clear_offset
lock_finish_offset
write_policy
```

其中：

```text
operation_cycles = lock_finish_cycle - Creq
vfu_latency      = matched_vfu_complete_cycle - vfu_accept_cycle
vfu_ii           = 饱和输入下相邻 vfu_accept 的稳定最小间隔
finish_drain     = lock_finish_cycle - status_clear_cycle
```

不要把 `vspbu_stall_cycles` 等同于 `reads_blocked_by_store_cycles`。store 可能占用
VSPBU 并产生 stall，但该周期原本不一定存在可发射的 read。

## 7. 最小仿真矩阵

### 7.1 Phase A：环境和一次性等价证明

这些 case 不生成每个 op 的新 profile 分支，只用于证明可以合并维度。

| Case | 配置 | 目的 |
|---|---|---|
| `A00_reset_csr` | reset + CSR 读写 | reset、CSR latency、mask/status 初值 |
| `A01_global_pipeline` | vadd vector, mode=0, full, chunks=8 | startup、lock、FIFO、read return、VSU、crossbar 全局参数 |
| `A02_partial_equiv` | 与 A01 相同，仅 mask=`0x5555` | 证明 full 与 partial nonzero 事件 cycle 完全一致 |
| `A03_data_equiv` | 与 A01 相同，替换为边界/饱和数据 | 证明无 data-dependent timing |
| `A04_bank_equiv` | 与 A01 相同，tensor 移到另一个 bank，无并发 master | 证明无竞争时 bank 不影响周期 |

mode-equivalence 不需要对每个 op 做。按 7 个底层 VFU 单元各选一个代表，固定
`chunks=2`、nonzero mask，分别跑 mode 0/1/2/3：

```text
PSMAU  -> vadd（另保留 vmul 的正常校准）
PSLU   -> vand
PSCU   -> vmin
PSMU   -> vmv vector
PSSRU  -> vssrl scalar
PSCDU  -> vnclip scalar
PSRSU  -> vredsum
```

每组比较时忽略功能数据值，只要求以下事件的 cycle 序列完全一致：

```text
read_issue/fifo_push/fifo_pop/vfu_accept/vfu_complete/vsu_accept/
write_request/bank_write/status_clear/lock_finish
```

shift-value-equivalence 只测 `vssrl scalar` 和 `vslideup scalar` 两个代表，shift/scalar
值使用 `0, 1, 7, 15`；不得扩展到所有 op。

### 7.2 Phase B：正常 nonzero-mask 校准

每个时序族运行：

```text
chunk_count = 1, 2, 4, 8
effective VLEN = 128, 256, 512, 1024 bit
mode = 代表性固定值（建议 0）
mask = 0xffff
```

必须执行的代表操作：

```text
vadd vector
vadd scalar
vand
vmin
vredmin
vmv vector
vmv scalar
vslideup scalar
vslidedown scalar
vssrl scalar
vnclip scalar
vredsum
vmul
```

共享同一时序族但未作为代表的 op，只需在 `chunk_count=1` 和 `8` 做族等价验证：

```text
vsub     对比 vadd vector
vor/vxor 对比 vand
vmax     对比 vmin
vredmax  对比 vredmin
vssra    对比 vssrl scalar
```

如果事件 cycle 序列一致，这些操作可复用代表族的参数；如果不同，立即拆成独立族，
再补跑 chunk 2/4。

### 7.3 Phase C：zero-mask 分支

zero mask 确实可能改变总周期，但无需测试 partial 的不同位形。

对 Phase B 的每个代表时序族先运行：

```text
mask = 0x0000
chunk_count = 1, 8
```

如果 zero-mask 的 `operation_cycles` 能由同一条、经过 1/8 两点验证的明确公式从
nonzero case 推导，返回该公式和证据；否则补跑 chunk 2/4，并生成独立 zero profile
行。任何情况下都要验证：

```text
write_request_count = 0
bank_write_count = 0
VLEN/waddr 按 RTL 规定推进
status 和 lock 正常结束
```

### 7.4 Phase D：VLEN 边界，只做协议测试

非 128-bit 对齐的 requested VLEN 不作为每个 op 的 profile 分支。用 `vadd vector`
统一测试：

```text
requested VLEN = 0, 1, 127, 128, 129, 255, 256, 257, 511, 512
```

返回 RTL 实际的：

```text
effective chunk 数
每拍 r_vlen/r_vlen1/r_vlen2
是否 underflow/timeout
read/write 数量
status/lock 终态
```

当前 gem5 会按 128 bit 向上对齐。若 RTL 对非对齐 VLEN 的行为不同，应作为协议修复
处理，而不是为每个 requested VLEN 建 timing profile。

### 7.5 Phase E：系统争用

固定使用 `vadd vector, chunks=8, nonzero mask`，测试：

1. DBUS 请求与 `lock_start` 同周期；
2. DBUS 请求发生在 Creq 到 lock_start 的空隙；
3. lock active 期间 DBUS 连续请求；
4. `lock_finish` 同周期 DBUS 请求；
5. 如 SAU 同时启用，SAU/VEU 同 bank 和不同 bank 各一组；
6. VSPBU 同周期存在 read candidate 和 store candidate。

这些结果用于 memory/crossbar 模型，不生成 per-op VFU profile 分支。

### 7.6 Phase F：illegal/unsupported

分别运行：

```text
vcompress
vmac
vmsub
vmulhsu
vwredsum
vmulh
unknown start bit
```

每项返回：

```text
illegal_assert_cycle
是否发出 read
是否产生 vfu_accept/vfu_complete
是否产生 write/bank_write
status_clear_cycle 或 null
lock_finish_cycle 或 null
timeout_cycle 或 null
最终 status/lock/illegal
destination memory 是否改变
```

timeout、status 卡住或 lock 卡住均是有效硬件观察结果，但结果状态必须写为
`timeout` 或 `stuck`，不能转换成 PASS/no-op。

## 8. 波形信号清单

层次前缀必须从实际 elaboration hierarchy 获取。典型完整层次可能是：

```text
top_mikui_tb.dut_mikui_inst.VEU_1_inst.VEU_1_inst
```

### 8.1 VE_CORE/VEU 外部接口

```text
clk
veu_clk_en
veu_gclk
rst_n
csr_we
csr_re
csr_write_type[1:0]
csr_addr[11:0]
csr_wdata[63:0]
csr_vestart[31:0]
csr_rdata[31:0]
csr_ready
veu_sram_enable
veu_sram_addr[31:0]
veu_sram_wdata[127:0]
veu_sram_wstrb[15:0]
veu_sram_rdata[127:0]
veu_crossbar_start
veu_crossbar_done
veu_crossbar_error
```

### 8.2 VCU

```text
r_csr_valid
r_csr_rdata[31:0]
r_status[31:0]
r_config[31:0]
r_vlen[31:0]
r_vlen1[31:0]
r_vlen2[31:0]
r_raddr1[31:0]
r_raddr2[31:0]
r_raddr3[31:0]
r_waddr[31:0]
r_mask[31:0]
r_cycles_mode[1:0]
r_busy_0/r_busy_1/r_busy_2
r_busy_delay_0/r_busy_delay_1/r_busy_delay_2/r_busy_delay_3
w_busy
w_scalar_en
r_lock_start/r_lock_busy/r_lock_finish
vc2vl_busy
vl2vc_load_data1_complete
vl2vc_load_data2_complete
vl2vc_load_data3_complete
vl2vc_hands_success
vb2vc_stall
illegal_error
```

### 8.3 VLU/FIFO

```text
canIssue
need1
need2
rrPtr[1:0]
issue_pulse
issue_tag[1:0]
vl2vb_sram_enable
vl2vb_sram_addr[31:0]
vb2vl_stall
ret_pulse_r
ret_pulse
ret_tag_r[1:0]
ret_tag[1:0]
_input_buffer1_fifo_in_push
_input_buffer1_fifo_in_pop
_input_buffer1_fifo_out_full
_input_buffer1_fifo_out_empty
input_buffer1.r_cnt_status
_input_buffer2_fifo_in_push
_input_buffer2_fifo_in_pop
_input_buffer2_fifo_out_full
_input_buffer2_fifo_out_empty
input_buffer2.r_cnt_status
w_vl_ready
vl2vf_input_buffer1_valid
vl2vf_input_buffer2_valid
vl2vf_busy
vl2vf_mode[1:0]
vl2vf_scalar_en
vl2vf_mask[31:0]
vl2vf_delay_enable
vl2vf_first_not_last
```

### 8.4 VFU/VSU/VSPBU

```text
VFU.w_psmau_enable
VFU.w_pslu_enable
VFU.w_pscu_enable
VFU._psmu_i_psmu_in_enable
VFU.w_pssru_enable
VFU.w_pscdu_enable
VFU.w_psrsu_enable
VFU.w_complete
VFU.vf2vs_vf_ready
VFU.vf2vs_waddr[31:0]
VFU.vf2vs_wstrb[15:0]
VSU.r_sram_enable
VSU.r_sram_addr[31:0]
VSU.r_sram_wstrb[15:0]
VSPBU.vl2vb_sram_enable
VSPBU.vs2vb_sram_enable
VSPBU.ve2tcm_sram_enable
VSPBU.ve2tcm_sram_addr[31:0]
VSPBU.ve2tcm_sram_wstrb[15:0]
VSPBU.vb2vl_stall
VSPBU.vb2vc_stall
```

### 8.5 crossbar/SRAM

必须记录实际 elaborated crossbar 的等价信号：

```text
crossbar state
master_crossbar_start/done
master_req/address/wstrb
master accepted/dropped（若有）
slave_req/address/wstrb
slave_ack/resp
master_rdata
same-bank winner/collision（若有）
sram_enable/address/wstrb/wdata/rdata/ready
```

如果信号因优化不可见，使用现有 simulator 的 debug/visibility 选项保留；不得根据
源码预期伪造 trace。

## 9. 每个 case 的守恒和验收检查

正常 case 必须满足：

```text
每个 read_issue 恰好匹配一个 read_return 和一个正确 source 的 fifo_push
每个 fifo_pop 对应已存在的 FIFO entry
每个 vfu_accept 恰好匹配一个 vfu_complete，特殊 slide 抑制必须显式记录
每个 store_candidate 的 grant/抑制原因明确
每个 write_request 恰好匹配一个 bank_write
zero mask 不产生 write_request/bank_write
status_clear 时不再产生新的有效计算 token
lock_finish 前最后一个有效 bank_write 已提交
operation_finish 时 request/return/FIFO/VFU/VSU/store 全部 drain
地址每个正常 chunk 递增 0x10，reduction/slide 的例外有明确证据
```

禁止仅凭最终 destination memory 正确就判定时序 PASS。

## 10. 工作站必须返回的目录和文件

返回一个根目录：

```text
mikui_veu_rtl_timing_<date>_<rtl_short_sha>/
```

至少包含：

```text
README.md
manifest.json
rtl_file_sha256.txt
environment.json
global_timing.json
timing_equivalence.json
timing_summary.csv
veu_timing_profile_v4.csv
model_gap_report.md
cases/<case_id>/metadata.json
cases/<case_id>/events.csv
cases/<case_id>/raw_signals.csv
cases/<case_id>/functional_vectors.csv
cases/<case_id>/destination_memory_dump.csv
cases/<case_id>/checks.json
cases/<case_id>/sim.log
cases/<case_id>/wave.fsdb 或 wave.vcd
```

### 10.1 `environment.json`

必须包含：

```json
{
  "rtl_repo": "...",
  "rtl_commit": "...",
  "rtl_dirty": false,
  "top_module": "top_mikui_tb",
  "dut_path": "...",
  "veu_path": "...",
  "crossbar_module": "...",
  "simulator": "...",
  "simulator_version": "...",
  "compile_command": "...",
  "run_command_template": "...",
  "system_clock_period": "...",
  "veu_clock_period": "...",
  "sampling_rule": "posedge veu_gclk after NBA",
  "reset_rule": "...",
  "veu_clk_en_rule": "...",
  "sram_model": "...",
  "sram_response_delay": "...",
  "bank_map": {
    "bank0": "0x20010000..0x2001ffff",
    "bank1": "0x20020000..0x2002ffff"
  },
  "timeout_cycles": 10000
}
```

### 10.2 `events.csv`

固定表头：

```csv
event_seq,veu_cycle,system_cycle,event,case_id,op,scalar_en,mode,mask,requested_vlen,effective_vlen,chunk,source,token_id,transaction_id,address,wstrb,data,status,lock,fifo1,fifo2,outstanding,detail
```

同周期多事件用 `event_seq` 给出稳定顺序。`token_id`、`transaction_id` 必须能完成
read/VFU/write 的一一配对，不能只靠 cycle 猜测。

### 10.3 `timing_summary.csv`

固定表头：

```csv
case_id,result,op,timing_family,scalar_en,mode,mask_class,source_set,requested_vlen,effective_vlen,chunk_class,csr_response_latency,startup_cycles,lock_start_delay,read_return_latency_min,read_return_latency_max,fifo_depth,max_outstanding_reads,vfu_latency_min,vfu_latency_max,vfu_ii,vsu_latency,bank_write_latency,status_active_cycles,lock_active_cycles,finish_drain_cycles,operation_cycles,read_count,vfu_accept_count,vfu_complete_count,write_count,bank_write_count,write_policy,illegal_cycle,timeout_cycle,evidence_id
```

若同一 case 的 latency 不恒定，必须分别填写 min/max，并在 `model_gap_report.md`
解释变化条件；不得只返回平均值。

### 10.4 `global_timing.json`

固定结构：

```json
{
  "schema": "mikui-veu-global-timing-v1",
  "csr_response_latency": null,
  "startup_cycles": null,
  "lock_start_delay": null,
  "fifo_depth_src1": 4,
  "fifo_depth_src2": 4,
  "read_issue_width": 1,
  "read_return_latency": null,
  "max_outstanding_reads": null,
  "vsu_latency": null,
  "bank_write_latency": null,
  "finish_drain_cycles": null,
  "crossbar_pipeline": {
    "start_to_active": null,
    "master_to_slave_request": null,
    "slave_request_to_ack": null,
    "ack_to_master_data": null
  },
  "evidence_case_ids": []
}
```

数值必须来自 trace；静态已知值也要用一个 representative case 验证。

### 10.5 `timing_equivalence.json`

固定结构：

```json
{
  "schema": "mikui-veu-timing-equivalence-v1",
  "mode_affects_timing": false,
  "data_affects_timing": false,
  "shift_value_affects_timing": false,
  "partial_pattern_affects_timing": false,
  "full_vs_partial_nonzero_affects_timing": false,
  "zero_vs_nonzero_affects_timing": true,
  "bank_affects_timing_without_contention": false,
  "family_equivalence": [
    {
      "representative": "vand",
      "members": ["vand", "vor", "vxor"],
      "equivalent": true,
      "evidence_case_ids": []
    }
  ],
  "mismatches": []
}
```

布尔值必须由 Phase A/B 的事件序列比较产生。若与静态预期不同，以 RTL 仿真为准，
但保留 mismatch 证据。

### 10.6 `veu_timing_profile_v4.csv`

使用 gem5 已支持的精确表头：

```csv
profile_id,op,mode,scalar_en,mask_class,source_set,chunk_class,vfu_latency,vfu_ii,write_policy,fifo_depth,max_outstanding_reads,vsu_latency,lock_start_delay,finish_drain_cycles,operation_cycles,timing_source,evidence_id
```

规则：

1. `timing_source` 必须为 `rtl_sim`；
2. `op/scalar_en/mask_class/source_set/chunk_class` 不得写 `*`；
3. mode-equivalence 通过后 `mode` 写 `*`；
4. full 和 partial 通过等价证明后可以生成数值相同的两行并引用共同证据；
5. zero 必须引用 zero-mask case；
6. 不得从 Yinglong profile 复制数值；
7. 每个 `evidence_id` 必须能在 `manifest.json` 定位到 case 和 artifacts hash。

### 10.7 `model_gap_report.md`

以下任一情况必须写入 gap report，不能强行生成看似完整的 profile：

- `startup_cycles` 随 timing family 改变；当前 v4 CSV 没有 per-row startup 字段；
- SRAM return latency 不是常数；
- 同一五元组的 `vfu_latency` 或 `operation_cycles` 依赖 mode/data/shift；
- VFU II 不是单一常数；
- operation 总周期不能由当前 TimingVeu 的 FIFO/仲裁模型复现；
- RTL 的 requested/effective VLEN 语义与 gem5 向上对齐不同；
- status_clear 早于数据 drain，或 lock_finish 早于最后写提交；
- crossbar 有 gem5 未建模的 dropped request、collision 或 backpressure；
- illegal 操作继续计算、写内存或不终止；
- clock gating 改变 cycle 计数语义。

报告中给出：影响 case、首个分歧 cycle、RTL 信号证据、建议新增的模型字段或状态。

## 11. 返回数据后的 gem5 修改映射

工作站返回上述文件后，gem5 侧可以按以下映射直接实施：

| 返回数据 | 修改位置 |
|---|---|
| v4 profile rows | `configs/brs/veu_timing_profile.csv` |
| per-family startup 差异 | 扩展 `VeuTimingSelection`、v5 CSV parser 和 `TimingVeu::startVectorOperation()` |
| VFU latency/II | `VeuTimingSelection.latency/initiationInterval` |
| FIFO/outstanding | profile selection 与 `TimingVeu::readCanIssue/makeReadRequest` |
| VSU latency | `TimingVeu::advancePipelines()` |
| lock/status/operation cycles | `TimingVeu::startVectorOperation/enterDrainingIfDone()` |
| read return/crossbar pipeline | `npu_lpnpu_mikui_crossbar` 和 memory model |
| zero/nonzero 差异 | TimingVeu store queue、profile rows或必要的新控制字段 |
| write policy | `VeuFunctionalExecutor` token 语义和 `TimingVeu` write scheduling |
| VLEN 边界差异 | `effectiveVeuLengthAtStart()` 和 CSR/VLEN 推进 |
| illegal/timeout 行为 | `VeuFunctionalExecutor::describe()` 与 start/status/lock 状态机 |

直接修改前仍会先运行 profile parser 单测、TimingVeu 定向测试、Mikui memory/crossbar
测试，以及 `test_by_agent/rv_veu_e2e` 的最小相关回归。

## 12. 工作站 AI 的最终执行指令

可以直接把下面内容作为任务提示：

```text
阅读 MIKUI_VEU_RTL_TIMING_HANDOFF.md，并严格按其基线、采样规则、最小矩阵和返回
schema 执行 Mikui 128-bit VEU RTL 仿真。

开始前先检查实际 elaboration 的 VE_CORE.veu_clk_en/veu_gclk、reset、crossbar_mi
具体实现和所有 RTL 文件 hash。如果 clock-enable 未确定连接、存在 X/Z 或无法确认
crossbar 来源，返回 BLOCKED，不生成 profile。

不要对 mode、输入数据、shift 数值、partial mask 位形和 source_set 做完整笛卡尔积。
mode/data/shift/partial 只按 Phase A 做一次性等价证明；source_set 从 op+scalar_en
派生。必须保留的时序分支是 timing family、scalar_en、chunk_count 和 zero/nonzero
mask。共享时序族先用 1/8 chunk 验证，发生差异才拆族并补 2/4 chunk。

输出完整 raw signals、normalized events、summary、global timing、equivalence、功能
结果、memory dump、checks、日志、波形、hash manifest、候选 v4 profile 和 model gap
report。禁止只返回总拍数或平均 latency。每一条 profile 行必须有可追溯 evidence_id。
```

## 13. 完成标准

只有同时满足以下条件，交付才算完成：

1. 目标确认为 Mikui 128-bit VEU，而非 Yinglong/256-bit；
2. VEU clock-enable、gated clock、reset 和采样边界已确认；
3. 所有文件/工具/命令/hash 可复现；
4. 全局结构参数有原始事件证据；
5. 所有必要时序族完成 1/2/4/8 chunk nonzero 校准；
6. zero mask 分支完成且无外部写；
7. mode/data/shift/full-vs-partial 合并结论有代表性证据；
8. read、FIFO、VFU、write 全部守恒；
9. status、lock 和最后写提交顺序明确；
10. illegal/unsupported 结果没有被伪装成正常 PASS；
11. 返回的 JSON/CSV 表头和字段符合本文 schema；
12. v4 profile 的每行均可追溯到 RTL commit、case、trace 和 waveform。
