# Mikui `dut_mikui_dma` VEU 时序补测与返回清单

## 0. 工作站 AI 的任务

请对 RTL commit `e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8` 的
`dut_mikui_dma` 路径执行一次定向补测，替换
`mikui_veu_rtl_timing_retest_20260827_e7c05d0` 中不能直接用于 gem5 校准的数据。

本轮目标不是重复所有旧用例，而是补齐以下数据，使返回目录可以直接用于修改：

```text
configs/brs/veu_timing_profile.csv
src/brs/veu/timing_veu.{hh,cc}
src/brs/veu/veu_timing_profile.{hh,cc}
src/brs/memory/npu_lpnpu_mikui_dma_crossbar.{hh,cc}
src/brs/memory/npu_lpnpu_mikui_memory_model.{hh,cc}
```

必须动态运行 RTL。禁止从源码静态估算拍数、沿用错误分区、人工补固定拍数或用
gem5 当前值作为 RTL 期望值。

## 1. 本轮必须修正的旧数据问题

旧结果不得原样作为最终参数，原因如下：

1. Track B/C 使用了错误参数：

   ```text
   SRAM_SPLIT_BASE = 0x20010000
   SRAM_SPLIT_END  = 0x20030000
   sram_B_reg      = 0
   sram_C_reg      = 8
   sram_D_reg      = 12
   ```

   这不是 `top_mikui_dma_tb.sv` 驱动 `dut_mikui_dma` 时的默认结构。
2. 旧 Track B 主要覆盖 `0x2001xxxx` 和 `0x2002xxxx`，没有独立覆盖真正的第三个
   SRAM slave。
3. 旧汇总器可能按事件顺序或 `zip(requests, responses)` 配对。多 master、多 bank
   并行时这种方法不可靠。
4. 旧 Track A 在 `veu_crossbar_done` 后固定等待 5 拍，再写入
   `operation_finish`，导致 `operation_cycles` 含人为 `+5`。
5. scalar add/move、slideup/slidedown、shift、clip 出现未配对 read/VFU 事件；当前
   无法区分 RTL 行为、采样错误、source tag 错误或测试协议错误。
6. 旧报告把“观测到的最大 FIFO 占用 1”和“RTL FIFO 容量 4”混在一个参数中。
7. reduction 等用例的 `vsu_latency` 出现 1/4/8 等值，需要逐 token 配对后重算。
8. done/lock 后的尾部请求没有区分合法 drain、重复采样和真正 orphan request。

## 2. 冻结 RTL 顶层和结构参数

### 2.1 身份

首要使用：

```text
RTL root: /home/lenovo-pccarryking/gem5_workspace/mikui
commit:   e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8
top:      dut_mikui_dma
VEU:      VE_CORE/VEU
xbar:     crossbar_mi_full
beat:     128 bit / 16 byte
```

如果工作站路径不同可以继续，但必须核对 commit 和相关文件 SHA-256。若 commit
不同，返回实际 SHA 和相关 RTL diff，不得把结果标成 `e7c05d0`。

### 2.2 唯一允许的正式分区

```text
NUM_CROSS       = 2            # SAU + VEU
NUM_SLAVE       = 3
SRAM_RESP_DELAY = 1
SRAM_SPLIT_BASE = 0x20008000
SRAM_SPLIT_END  = 0x20028000
sram_A_reg      = 2
sram_B_reg      = 4
sram_C_reg      = 8
sram_D_reg      = 12
```

顶层 master 编号也必须保持：

```text
master[0] = SAU
master[1] = VEU
DBUS      = 独立 dbus 端口
```

同 bank 时 `for` 循环的后写覆盖行为与 master 编号有关，不能沿用旧 testbench 中
`master[0]=VEU/master[1]=SAU` 的标注。

由 RTL 公式计算并在 smoke test 波形中确认：

| 端口 | 用途 | 起始地址 | 结束地址 | 大小 |
|---|---|---:|---:|---:|
| independent ISRAM | instruction | `0x00000000` | `0x00003fff` | 16 KiB |
| slave 0 | stack SRAM | `0x20010000` | `0x20017fff` | 32 KiB |
| slave 1 | ping SRAM | `0x20018000` | `0x2001ffff` | 32 KiB |
| slave 2 | pong SRAM | `0x20020000` | `0x20027fff` | 32 KiB |

正式结果中不能用 `bank0/bank1/bank2` 代替地址而不说明映射。每个 case 的 metadata
必须同时写入 slave 编号和 stack/ping/pong 名称。

## 3. 统一采样、接受和拍数定义

### 3.1 周期编号

```text
cycle 0 = reset 释放后的第一个有效 veu_gclk 上升沿
采样点  = posedge 后所有 NBA 更新完成后
主单位  = veu_gclk_cycle
同时记录 system_cycle、时间戳和 clk 比例
```

如果 Track B 只使用 `clk`，其 `xbar_cycle` 单独编号。禁止把不同时钟域的拍数直接
相减；跨时钟域必须返回两侧握手事件。

### 3.2 事务定义

每笔事务必须在 testbench 发起时分配唯一 `transaction_id`：

```text
<case_id>:<master>:<sequence>
```

必须分别维护 `dbus`、`veu`、`sau` 的 pending queue，并以 master、地址、读写属性、
bank、接受周期和返回数据共同核对。禁止按两个 CSV 列表位置配对，禁止使用全局
FIFO 跨 source 配对。

需要区分以下事件：

| 事件 | 精确定义 |
|---|---|
| `request_present` | master 请求信号为 1，可能尚未被接受 |
| `request_accept` | 根据实际 RTL 协议，本拍请求成为唯一有效事务 |
| `slave_request` | 指定 slave 端口上的请求有效 |
| `bank_read_sample` | SRAM 模型采样读地址 |
| `bank_write_commit` | SRAM 字节写掩码真正提交 |
| `bank_read_return` | SRAM 模型产生该事务的读数据 |
| `master_response` | 对应 master 可消费响应/数据 |
| `request_retry` | 未接受请求按协议保持或重发 |
| `request_drop` | 请求不再存在且没有接受/返回 |

若 RTL 没有显式 ready，工作站 AI 必须依据状态、slave 选择和端口占用给出
`request_accept` 判据，并在 `acceptance_contract.md` 中引用 RTL 层级和信号，不能默认
“看到 req 就算接受”。

### 3.3 VEU operation 边界

```text
Creq = CSR/vestart 最终被接受的周期
operation_finish = status 已清零、lock 已释放，且 read/FIFO/VFU/VSU/store/
                   crossbar pending 全部为空后的第一个周期
```

要求连续两个周期保持 quiescent，第二个周期只用于确认稳定；`operation_finish` 仍记
第一个 quiescent 周期。禁止固定 `repeat(5)`、禁止人为加 offset。

以下事件分别返回，不允许互相替代：

```text
status_set, status_clear
lock_start, lock_finish
last_read_issue, last_read_return
last_vfu_accept, last_vfu_complete
last_write_request, last_bank_write_commit
operation_finish
```

## 4. 哪些维度需要分支

### 4.1 正式分支

这些维度会改变拍数或尚未得到可靠结论：

| 维度 | 必测值 |
|---|---|
| operation/timing family | 第 5 节列出的指令族 |
| scalar/source set | vector 与 scalar 分开，source set 从实测事件推导 |
| effective chunks | `1,2,4,8` |
| mask class | `nonzero_full`；另测 `zero` 的 chunks `1,8` |
| local SRAM | stack、ping、pong |
| contention relation | no contention、same bank、different bank |
| competing master | DBUS、SAU |
| contention placement | start、active、done/release、持续 burst |

### 4.2 不再展开笛卡尔积

旧动态证据已表明以下维度不改变无争用 VEU 内部时序。每项只做一个回归点；若事件
序列与旧证据不一致，再升级为正式分支：

| 维度 | 单一回归点 |
|---|---|
| mode `0/1/2/3` | `vadd vector, chunks=2` |
| 普通/边界输入数据 | `vadd vector, chunks=8` |
| full/partial nonzero mask | `vadd vector, chunks=8, 0xffff/0x5555` |
| shift value | `vssrl scalar, chunks=2, shift=0/15` |
| 无争用 bank | `vadd chunks=8` 在 stack/ping/pong 各一次 |

地址本身在同一 bank 内不作为拍数分支，只测边界正确性。时钟频率不作为 VEU
有效时钟拍数分支，但必须记录频率和 clock-gating 配置。

## 5. Track A：修正 VEU 内部缺失数据

使用 `VE_CORE + 确定性 128-bit SRAM responder`，固定无 backpressure、读返回 1 拍、
写提交 1 拍。Track A 不能用于填写真实 crossbar latency。

### 5.1 旧 PASS 族

以下旧 PASS 族不要求全量重跑，但必须从旧 `events.csv/raw_signals.csv` 重新归一化，
移除人工 `+5`，并生成新的逐 token 配对结果：

```text
vadd vector, vsub
vand, vor, vxor
vmin, vmax
vredmin, vredmax
vmv vector
vredsum
vmul
```

每族至少重跑 `chunks=1,8, mask=0xffff` 作为归一化器回归。旧 artifact 不可访问时，
重跑该族的 `chunks=1,2,4,8`。

### 5.2 必须重新动态测试的失败/不可信族

每项执行 `chunks=1,2,4,8, mask=0xffff`，以及 `chunks=1,8, mask=0x0000`：

| 类别 | 指令/形式 | 不得预设的 source set |
|---|---|---|
| scalar add | `vadd scalar` | 从实际 read accept/tag/FIFO push 得出 |
| scalar move | `vmv scalar` | 不得预设为 none |
| slide up | `vslideup scalar` | 实测 |
| slide down | `vslidedown scalar` | 实测 |
| logical shift | `vssrl scalar` | 实测 |
| arithmetic shift | `vssra scalar` | 实测 |
| narrow/clip | `vnclip scalar` | 实测 |

对每个失败 case，必须给出以下三者之一：

```text
RTL_PROTOCOL_VALID       # 所有已接受事务最终守恒，旧 logger/driver 错
RTL_ORPHAN_ACTIVITY      # operation finish 后仍产生无法归属的已接受事务
TESTBENCH_UNRESOLVED     # 证据不足，不能生成 profile 行
```

不得只返回 `FAIL`。必须返回首个不守恒事务 ID、周期、信号层级和前后至少 10 拍波形。

### 5.3 FIFO、VFU、VSU 必须分开统计

返回：

```text
rtl_fifo_capacity_src1
rtl_fifo_capacity_src2
max_observed_occupancy_src1/src2
max_outstanding_accepted_reads
read_issue_width
vfu_latency_samples_by_token
vfu_accept_cycle_by_token
vfu_complete_cycle_by_token
vsu_accept_cycle_by_token
store_candidate_cycle_by_token
write_request_cycle_by_token
bank_write_commit_cycle_by_transaction
```

`fifo_depth` 写 RTL 容量；`max_observed_occupancy` 作为证据单独返回。`vsu_latency` 必须
对同一 token 计算，不能用 first/last event 相减。

## 6. Track B：正确三 bank crossbar 补测

直接实例化实际 `crossbar_mi_full`，使用第 2.2 节的参数。SRAM 模型必须明确实现
1 拍 read return/1 拍 write commit，并记录请求在 posedge 前后各阶段的值。

### 6.1 门禁和地址边界

先动态证明以下映射，每个地址各做单读和全掩码写：

```text
0x20010000, 0x20017ff0     -> stack/slave0
0x20018000, 0x2001fff0     -> ping/slave1
0x20020000, 0x20027ff0     -> pong/slave2
```

另外探测但不写入正常 timing profile：

```text
0x2000fff0
0x20017fff                 # 非 16-byte 对齐边界行为
0x20028000
```

返回 `xbar_error`、slave select、是否产生 request/response。若顶层参数或实际译码与表格
不一致，立即标为 `STRUCTURE_MISMATCH` 并返回波形，不得自行换回旧分区。

### 6.2 无争用矩阵

对 stack、ping、pong 三个 bank，分别用 VEU、SAU、DBUS 执行：

```text
single read
single full write
back-to-back 8 reads
back-to-back 8 writes
alternating 8 read/write
```

每笔返回原始周期：

```text
request_present -> request_accept -> slave_request
slave_request -> bank_read_sample/bank_write_commit
bank_read_return -> master_response
```

禁止只返回平均值；返回所有样本、min/max 和直方图。

### 6.3 争用矩阵

对每个 bank 执行：

```text
VEU + DBUS，同周期 single read
VEU + DBUS，同周期 full write
VEU + SAU，同周期 single read
VEU + SAU，同周期 full write
连续 8 拍 VEU 与连续 8 拍 DBUS 同 bank
连续 8 拍 VEU 与连续 8 拍 SAU 同 bank
```

异 bank 并行至少覆盖所有有序 bank 对：

```text
stack->ping, stack->pong, ping->stack,
ping->pong, pong->stack, pong->ping
```

每个 bank 对分别测 `VEU+DBUS` 和 `VEU+SAU` 的同周期 read；再选一个 bank 对测
full write 和 8 拍 burst。

额外执行：

```text
DBUS request 与 VEU start 同周期
DBUS request 位于 VEU active/lock 中间
DBUS request 与 VEU done 同周期
DBUS request 位于 lock release 后第一周期
```

每个争用 case 必须返回 winner、实际 accepted transaction、loser 是否 hold/retry/drop、
等待拍数、是否多 master 错误 ack、最终数据归属和所有未完成 transaction ID。

## 7. Track C：端到端组合补测

使用：

```text
VE_CORE -> actual crossbar_mi_full -> stack/ping/pong SRAM models
```

优先直接使用 `dut_mikui_dma` 可观察路径；若使用 integration wrapper，必须保证参数、
端口方向、时钟、复位和 SRAM responder 与顶层一致，并返回 wrapper diff/hash。

### 7.1 无争用

```text
vadd vector: chunks=1,8, full/zero，stack/ping/pong
vmul: chunks=1,8，至少覆盖 ping
vredsum: chunks=1,8，至少覆盖 pong
```

### 7.2 旧失败族

```text
vadd scalar, vmv scalar, vslideup, vslidedown, vssrl, vssra, vnclip
每项 chunks=1,8, mask=0xffff
```

### 7.3 争用

```text
vadd chunks=8 + DBUS same-bank burst
vadd chunks=8 + DBUS different-bank burst
vadd chunks=8 + SAU same-bank burst
vadd chunks=8 + SAU different-bank burst
```

对每个 case 比较 Track C 与 Track A 内部事件序列，并使用 Track B 的逐事务等待解释
差异。返回第一个不一致事件和周期；禁止用常数 offset 强行对齐。

## 8. 可选 Track D：解压 DMA 边界

这部分不写入 VEU profile，但用于确认 `rtl-npu-lpnpu-mikui-decompress-dma` 整体模式。
若工作站环境能够运行外部解压 DMA，返回：

```text
DMA register programming/accept cycles
source read request/response cycles
destination write request/commit cycles
DMA done/IRQ cycle
DMA 是否经过 dut_mikui_dma 的 DBUS/crossbar
DMA 与 VEU 对同一 local SRAM 是否可能真实争用
```

如果 DMA 不经过该三 bank crossbar，明确返回
`dma_shares_local_crossbar=false`，不得把 DMA 延迟混入 Track B/C。

## 9. 固定返回格式

返回目录：

```text
mikui_dma_veu_timing_supplement_<YYYYMMDD>_<rtl_short_sha>/
```

必须包含：

```text
README.md
manifest.json
environment.json
acceptance_contract.md
source_and_tb.patch
rtl_file_sha256.txt
testbench_file_sha256.txt
normalizer_sha256.txt
parameter_lock.json
dimension_equivalence.json
veu_global_timing.json
veu_timing_profile_v4.csv
crossbar_timing_v2.json
composition_report.json
model_update_values.json
model_gap_report.md
logs/*.log
cases/<case_id>/metadata.json
cases/<case_id>/events.csv
cases/<case_id>/transactions.csv
cases/<case_id>/raw_signals.csv
cases/<case_id>/checks.json
cases/<case_id>/wave.vcd 或 wave.fsdb
```

### 9.1 `events.csv`

```csv
event_seq,track,veu_cycle,xbar_cycle,system_cycle,time_ns,event,case_id,op,scalar_en,mode,mask,requested_vlen,effective_vlen,chunk,source,token_id,transaction_id,address,slave,bank_name,wstrb,data,status,lock,fifo1,fifo2,outstanding,detail
```

### 9.2 `transactions.csv`

一行对应一笔事务，不能一行对应一次信号采样：

```csv
transaction_id,case_id,track,master,read_write,address,slave,bank_name,wstrb,request_present_cycle,request_accept_cycle,slave_request_cycle,bank_sample_cycle,bank_return_or_commit_cycle,master_response_cycle,retry_count,stall_cycles,result,response_data,evidence_event_seq
```

`result` 只能为：

```text
COMPLETED, RETRIED, DROPPED, DUPLICATE_RESPONSE, UNMATCHED_RESPONSE, PENDING_AT_END
```

### 9.3 `veu_timing_profile_v4.csv`

必须严格使用 gem5 当前可加载表头：

```csv
profile_id,op,mode,scalar_en,mask_class,source_set,chunk_class,vfu_latency,vfu_ii,write_policy,fifo_depth,max_outstanding_reads,vsu_latency,lock_start_delay,finish_drain_cycles,operation_cycles,timing_source,evidence_id
```

要求：

1. 所有数值字段必须非空；`lock_start_delay` 可为 0，其余周期/深度字段必须大于 0。
2. `timing_source` 必须为 `rtl_sim`。
3. `op/scalar_en/mask_class/source_set/chunk_class` 必须精确，不能写 `*`。
4. `mode` 可以在 mode 等价回归通过后写 `*`。
5. 只允许写 `PASS + transaction conservation clean + functional check clean` 的行。
6. `operation_cycles = operation_finish - Creq`，不得包含固定观察尾巴。
7. failed/unresolved 族不生成 profile 行，在 `model_gap_report.md` 单列。

### 9.4 `crossbar_timing_v2.json`

至少包含：

```json
{
  "schema": "mikui-dma-crossbar-timing-v2",
  "module": "crossbar_mi_full",
  "parameters": {
    "NUM_CROSS": 2,
    "NUM_SLAVE": 3,
    "SRAM_RESP_DELAY": 1,
    "SRAM_SPLIT_BASE": "0x20008000",
    "SRAM_SPLIT_END": "0x20028000",
    "sram_A_reg": 2,
    "sram_B_reg": 4,
    "sram_C_reg": 8,
    "sram_D_reg": 12
  },
  "address_map": [],
  "no_contention_samples": [],
  "same_bank_samples": [],
  "different_bank_samples": [],
  "lock_window_samples": [],
  "arbitration_priority": null,
  "different_bank_parallel": null,
  "retry_contract": null,
  "request_loss_observed": null,
  "duplicate_ack_observed": null,
  "pending_at_end": []
}
```

每个 sample 必须引用 `case_id + transaction_id + events.csv event_seq + wave`。

### 9.5 `model_update_values.json`

这是 gem5 修改的直接输入，必须填写实测值或 `null`，禁止推测：

```json
{
  "schema": "mikui-gem5-model-update-v1",
  "rtl_identity": {},
  "veu_profile_file": "veu_timing_profile_v4.csv",
  "veu_global": {
    "read_issue_width": null,
    "fifo_depth_src1": null,
    "fifo_depth_src2": null,
    "max_outstanding_reads": null,
    "csr_response_latency": null,
    "lock_start_delay_values": [],
    "finish_drain_cycle_values": []
  },
  "crossbar": {
    "state_entry_delay": null,
    "master_request_to_slave": [],
    "slave_request_to_bank_sample": [],
    "bank_return_to_master_response": [],
    "read_latency_by_master_bank": {},
    "write_commit_latency_by_master_bank": {},
    "same_bank_winner": null,
    "same_bank_loser_action": null,
    "different_bank_parallel": null,
    "dbus_pending_depth": null,
    "dbus_bubble_rule": null,
    "transaction_end_rule": null,
    "address_error_rule": null
  },
  "memory": {
    "beat_bytes": 16,
    "isram_range": ["0x00000000", "0x00003fff"],
    "stack_range": ["0x20010000", "0x20017fff"],
    "ping_range": ["0x20018000", "0x2001ffff"],
    "pong_range": ["0x20020000", "0x20027fff"],
    "bank_read_latency": null,
    "bank_write_commit_latency": null,
    "unaligned_access_rule": null,
    "out_of_range_rule": null
  },
  "unresolved_cases": [],
  "evidence_ids": []
}
```

## 10. 每个 case 的检查门禁

正常 PASS case 必须全部满足：

```text
每个 request_accept 恰好对应一个 slave_request
每个 accepted read 恰好对应一个正确 master_response
每个 accepted write 恰好对应一个 bank_write_commit
每个 response 都能回溯到唯一 transaction_id
每个 FIFO push 能回溯到正确 source read
每个 FIFO pop 发生前 occupancy > 0
每个 vfu_accept 恰好对应同 token 的 vfu_complete
每个需要存储的 VFU token 恰好对应 write/commit
zero mask 的 write_request 和 bank_write_commit 均为 0
operation_finish 时所有 pending queue 和流水级为空
destination memory 与 reference 一致
无 X/Z、timeout、duplicate response、unmatched response
```

若 RTL 协议本身允许请求保持多拍，`request_present` 可重复，但只能产生一次
`request_accept`。守恒检查必须基于 accepted transaction，而不是 req 高电平拍数。

## 11. 工作站最终报告必须明确回答

1. 实际 commit、top、crossbar module、wrapper 和 filelist 是什么？
2. 是否严格使用 B/C/D=`4/8/12` 和 `0x20008000..0x20028000`？
3. stack/ping/pong 的首尾地址分别选中了哪个 slave？
4. master request 的准确接受条件是什么？事务如何配对？
5. 无争用时，三个 bank、三个 master 的逐阶段 latency 是否相同？
6. same-bank 的 winner、loser、retry/drop/duplicate-ack 行为是什么？
7. different-bank 是否真正同拍并行？所有六个有序 bank 对是否一致？
8. scalar/move/slide/shift/clip 的旧失败属于 testbench 还是 RTL？第一笔异常事务是什么？
9. FIFO 容量、最大观测占用和最大 outstanding read 分别是多少？
10. operation finish 的真实判据和拍数是什么？确认没有固定 `+5`。
11. Track C 是否能由 Track A 内部时序和 Track B 事务等待精确解释？
12. 哪些返回值可以直接写入 profile、TimingVeu、crossbar 和 memory model？

## 12. 完成状态

### `SUCCESS`

Track A 必补族、Track B 三 bank 全矩阵、Track C 核心组合均完成；所有可校准行有
完整证据和 `model_update_values.json`。

### `PARTIAL_SUCCESS`

Track B 完整，Track A/C 至少有一部分不可校准。返回所有已完成数据，并将缺口写为
`null` 和 `unresolved_cases`；不得生成缺乏证据的 profile 行。

### `BLOCKED`

只有独立 filelist/wrapper 仍无法 compile/elaborate/run 时使用。必须返回实际命令、
返回码、第一个真实错误、完整日志、已尝试修复、相关 hash，以及解除阻塞所需的一个
具体输入。旧 `case_mikui` 入口损坏不能作为停止 Track A/B 的理由。

## 13. 可直接复制给工作站 AI 的指令

```text
严格执行 MIKUI_DMA_TIMING_SUPPLEMENT_RETEST.md，对 RTL e7c05d0 的
dut_mikui_dma 路径补测。Track B/C 必须使用 crossbar_mi_full 的顶层默认结构：
NUM_CROSS=2、NUM_SLAVE=3、SRAM_RESP_DELAY=1、SRAM_SPLIT_BASE=0x20008000、
SRAM_SPLIT_END=0x20028000、A/B/C/D=2/4/8/12，对应 stack/ping/pong 三个
32 KiB bank。旧报告的 BASE=0x20010000、END=0x20030000、B=0 数据无效。

不要重复所有旧 PASS case。Track A 重新归一化旧 PASS 数据并定向重测 scalar add、
scalar move、slideup/slidedown、vssrl/vssra、vnclip；Track B 完成三个 bank、三个
master 的无争用、同 bank、异 bank 和 lock-window 事务；Track C 做 VEU+真实三 bank
crossbar 组合验证。

所有周期在 posedge NBA 后采样。每笔事务在发起时分配唯一 ID，按 master 独立跟踪
request_present、request_accept、slave_request、bank return/commit、master_response；
禁止 zip 配对。operation_finish 必须由 status/lock 和全部流水/pending queue 动态 drain
确定，禁止固定等待 5 拍。返回 events.csv、transactions.csv、raw signals、波形、日志、
patch/hash、严格可加载的 veu_timing_profile_v4.csv、crossbar_timing_v2.json 和填写实测
值的 model_update_values.json。拿到这些文件后 gem5 侧应能直接修改 profile、TimingVeu、
Mikui DMA crossbar 和 memory model。
```
