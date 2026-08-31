# Mikui `dut_mikui_dma` VEU 时序最终补齐任务书

## 0. 任务目标

这是最后一轮工作站任务书。请基于已有结果：

```text
/home/lenovo-pccarryking/gem5_workspace/
  mikui_dma_veu_timing_supplement_20260828_e7c05d0
```

只补齐仍缺失或格式不合格的数据，不要重复已经完成的三 bank Track B 全矩阵。

最终返回物必须能够直接用于修改并验证：

```text
configs/brs/veu_timing_profile.csv
src/brs/veu/timing_veu.{hh,cc}
src/brs/veu/veu_timing_profile.{hh,cc}
src/brs/memory/npu_lpnpu_mikui_dma_crossbar.{hh,cc}
src/brs/memory/npu_lpnpu_mikui_memory_model.{hh,cc}
```

RTL 身份必须是：

```text
commit = e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8
top    = dut_mikui_dma
xbar   = crossbar_mi_full
beat   = 128 bit / 16 byte
```

## 1. 已有数据的处理结论

### 1.1 可以直接复用，不再全量重测

以下内容已经可信：

```text
NUM_CROSS       = 2
NUM_SLAVE       = 3
SRAM_RESP_DELAY = 1
SRAM_SPLIT_BASE = 0x20008000
SRAM_SPLIT_END  = 0x20028000
sram_A/B/C/D    = 2/4/8/12
master[0]       = SAU
master[1]       = VEU
```

地址映射：

| 区域 | 地址范围 | slave |
|---|---|---:|
| ISRAM | `0x00000000..0x00003fff` | independent |
| stack | `0x20010000..0x20017fff` | 0 |
| ping | `0x20018000..0x2001ffff` | 1 |
| pong | `0x20020000..0x20027fff` | 2 |

已有 Track B 已经覆盖：

- 三 master × 三 bank 的单笔、burst 和交替读写；
- VEU/SAU、VEU/DBUS 同 bank；
- 六个有序异 bank 对；
- DBUS start/active/done/release window；
- VEU 覆盖 SAU、DBUS 单项 pending、持续压力时请求覆盖/丢失；
- decode hole、END 地址和未对齐地址行为。

除第 7 节的两个相位补充探针外，不得重跑整套 Track B。

### 1.2 已证明不需要继续分支的维度

| 维度 | 最终处理 |
|---|---|
| mode `0/1/2/3` | 不改变拍数，profile 中写 `mode=*` |
| 普通/边界输入值 | 不改变拍数，不分支 |
| shift value | 不改变拍数，不分支 |
| 无争用 stack/ping/pong | 不改变 VEU 内部拍数，不分支 |
| 同一 timing family 的已证明等价成员 | 允许复用，但必须返回 family evidence |

### 1.3 仍然必须分支的维度

```text
operation/timing family
scalar_en / 实际 source_set
chunks = 1/2/4/8
mask = full/partial/zero
same-bank / different-bank contention
```

`chunks` 表示128-bit数据块数量：

| chunks | VLEN | 数据量 |
|---:|---:|---:|
| 1 | 128 bit | 16 B |
| 2 | 256 bit | 32 B |
| 4 | 512 bit | 64 B |
| 8 | 1024 bit | 128 B |

## 2. 现有补测结果中必须修正的问题

### 2.1 CSV 行尾

现有 `veu_timing_profile_v4.csv` 使用 CRLF，当前 gem5 会报：

```text
invalid VEU timing profile header
```

最终 CSV 必须使用 UTF-8/ASCII + LF：

```text
禁止 CRLF
禁止 UTF-8 BOM
每行恰好18列
最后一列不得包含 \r
```

### 2.2 mask 名称

当前 gem5 选择键只有：

```text
full
partial
zero
```

不得再输出 `nonzero_full`。最终 profile 的 `mask_class` 只能是上述三个值。

### 2.3 Track A 尾请求的分类

旧补测把7个族的44个 Track A case统一写成 `RTL_ORPHAN_ACTIVITY`，此结论不能直接
用于实际顶层：Track A responder 在 lock 结束后仍接受 `VE_CORE` 尾请求，而实际
`crossbar_mi_full` 会按 ownership/state 阻断其中一部分。

因此：

1. 实际顶层行为以 Track C 为准；
2. Track A 只能测 VFU/FIFO 内部阶段，不能把 lock 后、crossbar 本应拒绝的请求记为
   accepted transaction；
3. 必须分别记录 `read_candidate`、`xbar_request_accept` 和 `blocked_after_lock`；
4. 不得把 `blocked_after_lock` 计入 read conservation 的 accepted 集合。

### 2.4 profile覆盖

现有27行只有 clean op 的：

```text
chunks = 1/8
mask   = full（旧文件误写为 nonzero_full）
```

缺少 chunks 2/4、zero、partial，以及 scalar/slide/shift/clip 的正式行。

### 2.5 “0拍”与“1个时钟沿”

已有 Track B 的 post-NBA 标签得到：

```text
request_accept_cycle == master_response_cycle
delta = 0
```

但 SRAM/ack 实际在该 posedge 寄存更新，物理上经历了一个采样边沿，且
`SRAM_RESP_DELAY=1`。最终文件必须同时返回：

```text
post_nba_label_delta = 0
registered_edge_count = 1
```

gem5 memory/crossbar 模型必须按一次 `clock()` 边沿产生响应，不能配置成组合零延迟。

## 3. 最终指令/timing family

### 3.1 已有 clean c1/c8，可复用

```text
vadd vector
vsub
vand / vor / vxor
vmin / vmax
vredmin / vredmax
vmv vector
vredsum
vmul
```

现有 c1/c8 events、transactions 和 wave 可以复用，但最终 profile 行必须修正 LF 和
mask 名称。

### 3.2 实际顶层已有部分 PASS，但需要补齐

已有 Track C 表明：

| 操作 | chunks=1 | chunks=8 | 当前结论 |
|---|---|---|---|
| `vadd scalar` | PASS | PASS | 补2/4和mask |
| `vslideup` | PASS | PASS | 补2/4和mask |
| `vssrl` | PASS | PASS | 补2/4和mask |
| `vssra` | PASS | PASS | 补2/4和mask |
| `vnclip` | PASS | PASS | 补2/4和mask |
| `vslidedown` | PASS | FAIL | c8需诊断，补2/4 |
| `vmv scalar` | FAIL | FAIL | 必须诊断 |

这些操作必须通过实际 crossbar 的 Track C 测试，不能用无 ownership gate 的 Track A
结果替代。

### 3.3 illegal/unsupported

已有 RTL evidence 可以复用：

```text
vcompress
vmac
vmsub
vmulhsu
vwredsum
vmulh
unknown start bit
```

它们不得生成正常 timing profile 行。最终返回 `veu_terminal_behavior.json`，说明每个
操作是 `ILLEGAL_COMPLETE`、`ILLEGAL_STUCK` 还是其他实测终态。

## 4. 最小补测矩阵

### 4.1 clean op补 chunks 2/4

对以下每个精确 op 执行 `chunks=2,4, mask=0xffff`：

```text
vadd vector
vsub
vand
vor
vxor
vmin
vmax
vredmin
vredmax
vmv vector
vredsum
vmul
```

必须返回每个精确 op 的 profile 行。即使两个 op属于同一 family，当前 gem5 profile
仍按 op精确匹配；不得只返回 family代表而缺少成员行。

### 4.2 scalar/slide/shift/clip全 chunks

使用 Track C，对以下操作执行：

```text
vadd scalar
vmv scalar
vslideup scalar
vslidedown scalar
vssrl scalar
vssra scalar
vnclip scalar
```

正式矩阵：

```text
chunks = 1,2,4,8
mask   = 0xffff
```

c1/c8可复用现有 Track C，但必须重新归一化；新增动态仿真至少包含 c2/c4。

### 4.3 zero mask

zero mask 指 `mask=0x0000`。它仍可能读取和计算，但不得产生实际 SRAM write commit。

对以下每个 timing family代表执行 `chunks=1,2,4,8`：

```text
vadd vector                 # two-source streaming
vmin                        # compare
vredmin                     # reduce compare
vmv vector                  # one-source move
vredsum                     # reduce sum
vmul                        # multiply
vadd scalar                 # scalar single-source
vslideup                    # slide
vslidedown                  # slide-down special
vssrl                       # shift
vnclip                      # clip
vmv scalar                  # scalar no-source/special
```

如果 family代表的事件序列通过等价证明，可为 family成员生成精确 op行；每个复制行的
`evidence_id` 必须同时引用代表case和family-equivalence证据。

zero case通过条件：

```text
bank_write_commit_count = 0
write_request_count      = 0
status/lock终态符合实际RTL
accepted read、FIFO和VFU token守恒
```

若 RTL 在某个 zero case本身 stuck，记录终态，不得伪造正常 profile 行。

### 4.4 partial nonzero mask

partial 指任意非0且非 `0xffff` 的16-bit byte mask，本轮固定代表值：

```text
mask = 0x5555
chunks = 2,8
```

至少测试以下 family代表：

```text
vadd vector
vmin
vredmin
vmv vector
vredsum
vmul
vadd scalar
vslideup
vslidedown
vssrl
vnclip
vmv scalar
```

若 partial 与 full 的逐事件序列一致，只允许复用 timing数值，功能检查仍须验证实际
wstrb和目标字节。最终 profile仍要生成 `mask_class=partial` 的精确行，不能使用通配。

## 5. 两个必须解决的真实失败点

### 5.1 `vmv scalar`

现有 Track C：

```text
c1: 7 write requests, 1 commit
c8: 14 write requests, 8 commits
```

需要判断：

```text
RTL重复产生store candidate
crossbar在lock释放时阻断合法尾写
testbench把held request重复计数
vmv scalar的VLEN/source语义配置错误
```

必须返回：

- 每个 store candidate/token的唯一ID；
- VSU accept、write candidate、crossbar accept、bank commit；
- held-valid连续多拍是否是一笔事务；
- status clear和lock finish前后的20拍波形；
- destination reference；
- 最终分类及依据。

### 5.2 `vslidedown chunks=8`

现有 Track C：

```text
VFU accept   = 9
VFU complete = 8
```

必须返回 token 9 的完整路径：

```text
source read/FIFO pop
VFU unit enable
internal valid/ready
complete
VSU/store
status/lock
```

同时补测：

```text
chunks=2
chunks=4
chunks=8, shift/slide amount = 0,1,15
```

shift/slide amount只用于定位c8失败，不建立新的 timing分支；若三者事件序列不同，再
明确报告该维度需要升级为分支。

### 5.3 最终分类

每个失败点只能选择：

```text
PASS_AFTER_TB_FIX
RTL_TOP_STUCK
RTL_TOP_DROPS_TAIL
RTL_TOP_DUPLICATE_ACTIVITY
UNRESOLVED
```

若不是 PASS，必须写入 `veu_terminal_behavior.json`，使 gem5能够显式模拟 stuck、
illegal或尾部行为；不得用相邻 op拍数替代。

## 6. 功能检查

现有 Track C 除 vector vadd外，多数case只有动态守恒，没有完整功能oracle。最终补测
必须为以下操作提供字节级reference：

```text
vadd scalar
vmv scalar
vslideup
vslidedown
vssrl
vssra
vnclip
```

至少覆盖：

```text
chunks=1,8 full
chunks=2 partial
chunks=1,8 zero（验证目的地址不变）
```

功能检查必须输出初始值、期望值、实际值和逐字节match。`functional_errors=0` 只有在
真实执行oracle后才有效；未执行写 `functional_checked=false`。

## 7. crossbar只补两个相位探针

### 7.1 state entry

返回：

```text
master_crossbar_start posedge
state IDLE->ACTIVE可见周期
first master request present
first slave request sampled
first master response
```

填写：

```text
state_entry_edge_count
start_to_first_accept_edge_count
```

### 7.2 registered response phase

用一笔 VEU read和一笔 DBUS read明确返回：

```text
request_pre_edge_present
bank_sample_edge
slave_ack_post_nba
master_response_post_nba
post_nba_label_delta
registered_edge_count
```

预期要区分“同一个周期标签”和“经历一次边沿”，不能只写 latency=0。

## 8. profile周期定义与gem5映射

### 8.1 控制边界

每个正常case返回：

```text
Creq
first_read_accept
status_set
status_clear
lock_start
lock_finish
top_quiescent
last_read_return
last_vfu_complete
last_bank_write_commit
```

当前 gem5 `TimingVeu` 在完成时同时产生 `lock_finish` 和 `operation_finish`，所以最终
profile按以下定义填写：

```text
lock_start_delay    = lock_start - Creq
finish_drain_cycles = lock_finish - status_clear
operation_cycles    = lock_finish - Creq
```

实际顶层的：

```text
top_quiescent_cycles = top_quiescent - Creq
post_lock_drain      = top_quiescent - lock_finish
```

必须写入独立的 crossbar/memory字段，不得加进 profile `operation_cycles`，否则 gem5
内部控制时序和外部memory drain会重复计数。

### 8.2 startup

已有事件表明：

```text
first_read_accept - Creq = 5
```

当前 `TimingVeu::issueOneMemoryRequest()` 使用 `modelCycle <= start + N`，所以：

```text
observed_startup_delta = 5
gem5_veu_startup_cycles = 4
```

最终 JSON 必须同时返回这两个值，避免直接把5写入配置后多插一拍。

### 8.3 VFU/VSU

每个 profile key必须返回逐 token样本：

```text
vfu_latency = same_token_complete - same_token_accept
vfu_ii      = 饱和输入下相邻accept的稳定间隔
vsu_latency = same_token_store_candidate - same_token_vsu_accept
```

禁止用 first/last event、不同 token或总操作时间推算。

## 9. 最终 `veu_timing_profile_v4.csv`

固定表头：

```csv
profile_id,op,mode,scalar_en,mask_class,source_set,chunk_class,vfu_latency,vfu_ii,write_policy,fifo_depth,max_outstanding_reads,vsu_latency,lock_start_delay,finish_drain_cycles,operation_cycles,timing_source,evidence_id
```

字段要求：

```text
mode        = *
scalar_en   = 0 或 1
mask_class  = full / partial / zero
chunk_class = 1 / 2 / 4 / 8
timing_source = rtl_sim
```

`op/scalar_en/mask_class/source_set/chunk_class` 必须精确，禁止 `*`。只有
`PASS + protocol clean + functional_checked=true` 的正常操作可以生成行。

source set必须与gem5名称完全一致：

```text
src1+src2
src1
src2
none
```

CSV交付前必须通过：

1. LF/BOM/18列检查；
2. 数值字段范围检查；
3. 重复 timing key冲突检查；
4. 每个 evidence路径存在检查；
5. profile coverage检查；
6. 使用当前 gem5实际加载，不允许只用自制parser。

必须返回 `profile_load_test.log`，其中至少出现一次：

```text
profile loaded successfully
profile_hits > 0
profile_fallbacks = 0
```

## 10. `profile_coverage.json`

返回所有目标key及状态：

```json
{
  "schema": "mikui-veu-profile-coverage-v1",
  "required_chunks": [1, 2, 4, 8],
  "required_masks": ["full", "partial", "zero"],
  "keys": [
    {
      "op": "vadd",
      "scalar_en": 0,
      "source_set": "src1+src2",
      "chunk": 1,
      "mask_class": "full",
      "status": "PROFILED",
      "profile_id": null,
      "evidence_id": null
    }
  ],
  "missing_keys": [],
  "terminal_behavior_keys": [],
  "unexpected_fallback_keys": []
}
```

每个目标key必须恰好属于：

```text
PROFILED
TERMINAL_BEHAVIOR
```

`missing_keys` 和 `unexpected_fallback_keys` 必须为空才允许最终成功。

## 11. `model_update_values_v2.json`

必须返回：

```json
{
  "schema": "mikui-gem5-model-update-v2",
  "rtl_identity": {},
  "veu": {
    "profile_file": "veu_timing_profile_v4.csv",
    "observed_startup_delta": 5,
    "gem5_veu_startup_cycles": 4,
    "read_issue_width": 1,
    "fifo_depth_src1": 4,
    "fifo_depth_src2": 4,
    "max_outstanding_reads": 3,
    "lock_start_delay_values": [1],
    "finish_drain_cycle_values": [4]
  },
  "crossbar": {
    "master_map": {"0": "SAU", "1": "VEU"},
    "state_entry_edge_count": null,
    "start_to_first_accept_edge_count": null,
    "post_nba_label_delta": 0,
    "registered_response_edge_count": 1,
    "same_bank_winner": "VEU_OVER_SAU",
    "sau_loser_action": "DROP_WITH_POSSIBLE_DUPLICATE_ACK",
    "dbus_pending_depth": 1,
    "dbus_same_bank_action": "PENDING_OR_OVERWRITE",
    "different_bank_parallel": true,
    "request_loss_observed": true,
    "decode_hole_rule": null,
    "transaction_end_rule": null
  },
  "memory": {
    "beat_bytes": 16,
    "registered_read_edge_count": 1,
    "registered_write_commit_edge_count": 1,
    "post_nba_read_delta": 0,
    "post_nba_write_delta": 0,
    "isram_range": ["0x00000000", "0x00003fff"],
    "stack_range": ["0x20010000", "0x20017fff"],
    "ping_range": ["0x20018000", "0x2001ffff"],
    "pong_range": ["0x20020000", "0x20027fff"],
    "unaligned_rule": null,
    "out_of_range_rule": null
  },
  "terminal_behavior_file": "veu_terminal_behavior.json",
  "unresolved_fields": []
}
```

所有 `null` 必须在最终返回时填写实测规则；无法填写则加入 `unresolved_fields`，总体
状态不得写 SUCCESS。

## 12. `veu_terminal_behavior.json`

```json
{
  "schema": "mikui-veu-terminal-behavior-v1",
  "operations": [
    {
      "op": "vmv",
      "scalar_en": 1,
      "chunk": 1,
      "mask_class": "full",
      "classification": null,
      "status_clear_cycle": null,
      "lock_finish_cycle": null,
      "stuck": null,
      "tail_request_pattern": null,
      "destination_changed": null,
      "evidence_id": null
    }
  ]
}
```

illegal/unsupported以及不能生成正常profile的 `vmv scalar/vslidedown` key都必须覆盖。

## 13. 每个case返回文件

```text
cases/<case_id>/metadata.json
cases/<case_id>/events.csv
cases/<case_id>/transactions.csv
cases/<case_id>/tokens.csv
cases/<case_id>/raw_signals.csv
cases/<case_id>/functional_vectors.csv
cases/<case_id>/destination_memory_dump.csv
cases/<case_id>/checks.json
cases/<case_id>/wave.vcd 或 wave.fsdb
cases/<case_id>/sim.log
```

`transactions.csv` 一行一笔实际事务，held-valid多拍不能重复生成ID。`tokens.csv` 一行
一个 VFU/VSU token。

正常 PASS必须满足：

```text
accepted read -> 唯一return
FIFO push/pop守恒
VFU accept -> 同token complete
需要写回的token -> 唯一crossbar accept -> 唯一bank commit
zero mask无write/commit
response可回溯到唯一事务
status/lock终态正确
功能oracle通过
无X/Z、timeout、未解释pending
```

## 14. 最终返回目录

```text
mikui_dma_veu_timing_final_<YYYYMMDD>_e7c05d0/
```

必须包含：

```text
README.md
manifest.json
environment.json
source_and_tb.patch
rtl_file_sha256.txt
testbench_file_sha256.txt
normalizer_sha256.txt
reused_evidence.json
dimension_equivalence_final.json
timing_family_map.json
timing_summary.csv
veu_timing_profile_v4.csv
profile_coverage.json
profile_load_test.log
crossbar_timing_final.json
model_update_values_v2.json
veu_terminal_behavior.json
composition_report_final.json
model_gap_report_final.md
logs/*.log
cases/...
```

## 15. 完成状态

### SUCCESS

只有同时满足以下条件：

1. profile通过当前 gem5实际加载；
2. 正常目标key全部是 PROFILED；
3. 非正常key全部有 TERMINAL_BEHAVIOR；
4. chunks 1/2/4/8完整；
5. full/partial/zero完整；
6. `vmv scalar` 和 `vslidedown c8` 有最终结论；
7. edge count与post-NBA delta分开；
8. `model_update_values_v2.json.unresolved_fields=[]`；
9. 每个profile行有功能、时序、事务和波形证据。

### PARTIAL_SUCCESS

返回全部已完成数据，但 `missing_keys/unresolved_fields` 非空。不得声称可直接完成全部
VEU拍数对齐。

### BLOCKED

必须返回实际命令、返回码、第一个真实错误、完整日志、最小复现和解除阻塞所需输入。

## 16. 可直接复制给工作站AI的指令

```text
严格执行 MIKUI_DMA_VEU_TIMING_FINAL_COMPLETION.md。复用
mikui_dma_veu_timing_supplement_20260828_e7c05d0 中已经可信的三 bank/crossbar 全矩阵，
不要重跑整个 Track B；只补 state-entry/registered-edge 两个相位探针。

修复现有profile：输出LF、无BOM、18列，mask_class只能为 full/partial/zero。补齐所有
正常op的chunks 1/2/4/8和full/partial/zero精确key。clean vector op复用c1/c8并补c2/c4；
scalar add、scalar move、slideup/slidedown、vssrl/vssra、vnclip使用实际
VE_CORE+crossbar_mi_full Track C补齐。Track A lock后、真实crossbar会阻断的candidate
不得计为accepted orphan。

重点定位 vmv scalar重复写和vslidedown c8少一个VFU complete。能正常完成则生成profile；
否则写入veu_terminal_behavior.json，禁止用相邻op估值。为scalar/slide/shift/clip提供
真实字节级功能oracle。

profile operation_cycles按lock_finish-Creq，top_quiescent/post-lock drain单独返回。
同时区分post-NBA标签delta=0和registered edge count=1；返回observed startup delta=5及
gem5 startup config=4。

最后必须用当前gem5实际加载profile并证明profile_hits>0、profile_fallbacks=0，返回
profile_coverage.json、profile_load_test.log、model_update_values_v2.json和全部逐事务、
逐token、功能、波形证据。只有missing_keys、unexpected_fallback_keys和
unresolved_fields全部为空才能写SUCCESS。
```
