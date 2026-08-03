# CPU 接入 `sau_n::StreamingConvPipelineModel` 实施计划

状态：Step 0、Step 1、Step 2、Step 3、Step 4、Step 5、Step 6、Step 7 已完成；
Step 8 的旧后端代码清理、文档同步、增量重编译和活动 e2e 验证均已完成。

历史最终验证结果：`test_by_agent/conv3_e2e/runs_sau_n_final/` 下 7 个独立
report 和 `comparison_report.json` 全部为 `PASS`。其中
`generated_four_ins_control_matched + sau_n` 与
`generated_four_ins_full_offload + sau_n` 均已通过独立 verifier。

Step 8 清理后，活动 runner 只保留 5 个 `stub`/`sau_n` 对照运行；上述 7 个
report 作为历史验证证据保留，不再作为当前 `conv3` 后端的运行入口。

最终验收：用户已确认清理后的增量编译和 5 条活动 `stub`/`sau_n` e2e 全部通过。

附加工具：`test_by_agent/conv3_e2e/run_custom_conv.py` 已完成，可通过命令行配置
合法的 N/C/H/W/OC、padding、stride、cutbit、tensor 地址和权重/bias 模式，自动生成
独立 fixture 并运行 `gem5 + sau_n + verifier`；现有验收 fixture 不会被覆盖。
该 runner 默认使用 `--startup-model toolchain-approx --tensor-init cpu`，会近似
工具链 `7_sau` 测试中的 `malloc_initial`、四次 `malloc`、input/weight/bias 的
guest CPU source load/store 和前后 `mgetins4` 查询，并把这些 guest CPU 开销纳入
`cycle_count`。其中 allocator 已执行 shadow first-fit 路径：16-byte 对齐、header
单位换算、空闲链表/哨兵、header/canary/range 校验、first-fit、split/exact-fit 和
OOM 统计路径。使用 `--tensor-init memory` 可切换为 tensor 由 `memory.hex` 预加载，
但近似启动的 allocator 和 mgetins4 仍保留；使用 `--startup-model minimal` 可复现
旧的手写启动路径。两个模式的 report 都会记录初始化模式和 startup 语义。
weight/bias 以及近似模式下 input 的 staging 原始数据由 host 预加载，不计入 CPU 拍数；
guest CPU 的读取、复制和 allocator 控制流计入。该模型保留固定 tensor 返回地址，
用 256-byte metadata 区和按本次四次申请计算的 virtual header capacity 表示 heap
payload，因此仍不是工具链实际 malloc 固件的逐指令复刻。

目标仓库：`/home/xch/workspace/soc_gem5/gem5_cpu_veu`

SAU 来源：

```text
/home/xch/workspace/gem5/.worktrees/sau-command-types/src/sau_n
source branch: feature/sau-command-types
source commit: 8b3682c962f86e6b64b9b035e8229fd168c29c54
```

## 1. 最终目标

在当前 `PipelineMiniCPU` 中接入用户已经完成的
`gem5::sau_n::StreamingConvPipelineModel`，形成以下真实闭环：

```text
instruction.hex
  -> CPU 取指、解码并执行一次 msetins1~4
  -> 四个 CSR payload 被原子转换为 sau_n workload 配置
  -> msetins4 阻塞，CPU 等待 SAU 完成
  -> StreamingConvPipelineModel 读取 CPU 已准备好的 A/B/C 数据
  -> sau_n 内部 Im2Col、A FIFO、B 双缓冲、16x16 SA 和 D pending queue 逐拍运行
  -> D 写入 CPU/SAU 共享 SRAM 的 output_base
  -> sau_n drained 后 msetins4 返回一次 completion
  -> CPU 继续执行并到达 ebreak
```

最终活动计算路径必须使用 `src/sau_n/streaming_conv_pipeline_model.*`，不得继续调用
当前误写的 `src/brs/sau/conv3_compute_core.*` 产生卷积结果。

## 2. 已确认的设计选择

| 项目 | 冻结选择 |
|---|---|
| SAU 模型 | `gem5::sau_n::StreamingConvPipelineModel` |
| CPU 指令协议 | 已生成 fixture 使用的一次 Conv、四条 `msetins1~4` 新 ABI |
| 配置提交 | `msetins1~3` 写 shadow；`msetins4` 原子提交并启动 |
| CPU 等待语义 | `msetins4` 阻塞到 sau_n drained 且 D 已可见 |
| input/output layout | contiguous signed INT8 NCHW |
| weight layout | signed INT8 `[C][3][3][OC]`，即 K-major/OC-minor |
| bias layout | contiguous little-endian signed INT16 |
| kernel | 3x3 |
| scratchpad | 使用 sau_n 的 16-bank、一拍响应 shared scratchpad 时序模型 |
| 外部数据语义 | 到 SAU 算子启动时，A/B/C 已经位于 CPU/SAU 共享 SRAM |
| 搬运模型 | SAU 不新增 DMA staging；自定义 runner 可选择把工具链风格的 guest CPU 初始化近似纳入启动周期 |
| RTL | 本计划不接 Verilator，不运行 `sau_n/rtl` 作为 DUT |
| 最终代码位置 | 所有必需源码进入目标仓库，不依赖另一个 worktree 的绝对路径 |

## 3. SRAM 与 scratchpad 的统一语义

### 3.1 工具链和当前 fixture 的事实

工具链程序在发出 SAU start 前完成必要的软件准备；具体路径可能选择预装或运行时
初始化，但运行时的 guest CPU 访存应计入 CPU runtime：

- input、weight、bias 都可能由 CPU 运行时写入/复制；
- kernel、bias 等静态内容在部分路径也可以由 `memory.hex` 预装；
- legacy `padding=1` 路径由 CPU 执行软件 Padding，生成 padded buffer；
- CPU 随后通过 `msetins` 传递 A/B/bias/D 地址；
- SAU 使用这些地址读取输入并写回输出。

因此，“数据一开始就在 scratchpad”在本计划中的精确定义是：

```text
不是 gem5 时间 0 时所有数据都已就绪；
而是执行到 msetins4、SAU 算子正式启动时，所需 A/B/C 已在共享 SRAM 中就绪。
```

`generated_four_ins_full_offload` 不执行 CPU 软件 padding。它只准备原始 NCHW input，
padding zero 和 stride 取点由 sau_n 的 Im2Col 完成。

### 3.2 单一数据权威

集成模式下必须只有一份运行时数据权威：当前 `DutKuiMemoryModel` 中 CPU 可见的 SRAM。

不得采用以下错误实现：

- 保留 sau_n 的 `tb_act_value_v1`、`weightValue()`、`biasValue()` 作为正式输入；
- 让 sau_n 在运行时使用一份与 CPU SRAM 无关的私有数据副本；
- 先用 256-bit 总线完整搬一遍 A/B/C，再重复建模 sau_n 内部 scratchpad 访问；
- 用 host reference 的结果直接填 D。

正确做法是为 sau_n scratchpad 增加“外部 tensor backing”模式：

```text
sau_n 内部 (region, bank, row) request
  -> integration backing 映射到四指令 ABI 的逻辑 tensor byte
  -> 读取/写入同一个 DutKuiMemoryModel SRAM backing
```

该模式保留 sau_n 已有的：

- 16-bank request/grant/response；
- 每 bank 每拍单操作约束；
- 一拍 scratchpad response；
- A/B/C/D 仲裁；
- B0/B1 buffer、D pending queue 和 backpressure；
- `StreamingConvPipelineModel::tick()` 的周期推进。

它只替换 scratchpad byte 的来源，不替换 sau_n 的控制流、数值计算或时序。

### 3.3 与现有 256-bit crossbar 的关系

集成 endpoint 固定选择 `docs/SAU_GEM5_ENDPOINT_CONTRACT.md` v1.3 定义的
`LocalScratchpadBacking` 模式；不得在一次 operation 内与 `ExternalBeat` 混用。

集成时 `crossbarStart`/`crossbarDone` 仍用于表示 SAU 对共享 SRAM 的所有权：

- `msetins4` 成功提交后发出一次 `crossbarStart`；
- SAU active 期间 CPU data access 不得与 SAU scratchpad 操作并行；
- sau_n drained、D 全部可见后发出一次 `crossbarDone`；
- 随后才允许 msetins4 completion。

sau_n 已经自行建模 16-bank scratchpad request 和一拍 response，因此集成模式不再把
每个内部 bank request 二次转换成外部 `Sram256Request`。否则会同时建模两套 SRAM
端口、两套 latency，并引入并不存在的 DMA copy。

在该模式下：

- `evaluateMemory().request.valid` 在 start、run、done、response 全阶段都为 false；
- 外部 `Sram256Response.valid` 必须为 false，意外响应按协议错误 fail-fast；
- sau_n 内部最后一次有效 D grant 在时钟边沿提交到共享 backing 后，才算 D 可见；
- `crossbarStart`/`crossbarDone` 只表达 ownership，不代表产生过 256-bit beat；
- reset 销毁 active model 并取消 completion，但不清空 CPU 可见 SRAM。

这意味着本方案可以声明 sau_n 内部 scratchpad 周期和 CPU 阻塞周期，但不能把该模式
的内部访问统计解释成现有 256-bit crossbar 的逐 beat 性能结果。

## 4. 四条 CSR 到 sau_n 的配置映射

现有 `docs/SAU_CONV3_CSR_ABI.md` 继续作为 CPU wire ABI。配置适配器必须显式生成
`sau_n::PipelineResolvedConfig`：

| CSR 字段 | sau_n 字段/用途 |
|---|---|
| `input_base` | external A tensor backing base |
| `weight_base` | external B tensor backing base |
| `bias_base` | external C tensor backing base |
| `output_base` | external D tensor backing base |
| `batch_n` | `im2col.n` |
| `input_c` | `im2col.c` |
| `input_h` | `im2col.h` |
| `input_w` | `im2col.w` |
| 推导的 `output_h` | `im2col.outH` |
| 推导的 `output_w` | `im2col.outW` |
| `kernel_size=3` | `im2col.kernelH/kernelW=3` |
| `padding` | `im2col.padTop/padLeft`，其余对称 |
| `stride` | `im2col.strideH/strideW` |
| 固定 dilation | `im2col.dilationH/dilationW=1` |
| `output_c` | `outChannels` |
| `cutbit` | `cutbit` |

`sharedSpad` 采用 `validateStreamingConfig()` 的自动连续布局：A、B、C、D region
依次放置，B buffer depth、D pending rows、weight reuse 使用冻结默认值。首版不通过
CPU ABI增加新的 scratchpad row-base 或性能参数。

集成模式还冻结：

- `OutputReadyConfig{period=1, highCycles=1}`，避免把额外输出节流混入首版 CPU 集成；
- generator 名称固定为校验器接受的占位值：
  `im2col.inputGenerator="tb_act_value_v1"`、
  `weightGenerator="tb_weight_value_v1"`、
  `biasGenerator="tb_bias_value_v1"`；它们只用于配置合法性，运行时调用次数必须为 0；
- backing 模式在模型构造时固定，operation 运行中不得切换。

### 4.1 外部 tensor byte 映射

backing adapter 必须按以下规则映射 sau_n 的 bank/row 地址：

```text
A[n,c,h,w]
  -> input_base + (((n*C + c)*H + h)*W + w)

B[k,oc], k=((c*3)+kh)*3+kw
  -> weight_base + k*OC + oc

C[oc,byte]
  -> bias_base + oc*2 + byte

D[n,oc,oh,ow]
  -> output_base + (((n*OC + oc)*OH + oh)*OW + ow)
```

padding-zero lane 不读取 A backing；spatial tail 和 `OC<16` 不得读取或写入无效 byte。

### 4.2 合法范围取交集

四条 ABI 的 wire 位宽可以大于 sau_n 的实际容量。正式 endpoint 必须同时通过：

1. ABI version/magic/reserved/start/shape/address 校验；
2. `sau_n::validateStreamingConfig()`；
3. A/B/C/D 自动 region 不超过 16 banks × 4096 rows 的容量校验；
4. 四个外部 tensor byte range 完全位于当前有效 SRAM；
5. 四个外部 range 不重叠；
6. bias 2-byte 对齐。

不能因为 ABI 字段能编码更大 shape，就绕过 sau_n 的 scratchpad footprint 限制。

当前四指令主 fixture 的示例 footprint 必须在配置适配器单测中固定：

```text
A rows = 512
B rows = 144
C rows = 2
D rows = 512
total  = 1170 rows < 4096 rows/bank
```

## 5. 代码结构

### 5.1 保留的 CPU 侧成果

以下已完成工作继续保留：

- 21 条通用 SAU set/get 指令解码；
- `msetins` 的 `{rs2, rs1}` 64-bit payload packing；
- HC router/CBU 请求保持、response 和流水线 stall/retire；
- `SauEndpoint` evaluate/clock tick 边界；
- `DutKuiMemoryModel`、共享 SRAM 和 crossbar ownership；
- instruction/data hex loader；
- frontend stale-instruction 修复和终止 trace flush 修复；
- 当前四指令 fixture、manifest 和独立 Conv reference/verifier。

### 5.2 新增集成组件

建议新增：

```text
src/brs/sau/sau_n_endpoint.{hh,cc}
src/brs/sau/sau_n_endpoint.test.cc
src/brs/sau/sau_n_config_adapter.{hh,cc}
src/brs/sau/sau_n_config_adapter.test.cc
src/brs/sau/sau_n_memory_view.{hh,cc}
src/brs/sau/sau_n_memory_view.test.cc
```

职责：

- `SauNConfigAdapter`：复用四条 ABI decoder，生成并验证
  `PipelineResolvedConfig`；
- `SauNMemoryView`：把 sau_n A/B/C/D bank-row 地址映射到共享 SRAM byte；
- `SauNEndpoint`：实现 HC transaction、模型生命周期、每 CPU tick 推进一次 sau_n、
  crossbar ownership 和最终 completion。

现有 `conv3_csr_config` 只包含已确认 ABI 的部分可以复用或迁移到
`sau_n_config_adapter`；不得复用 `conv3_compute_core` 作为计算后端。

### 5.3 sau_n 的最小接口扩展

在保持 standalone 默认行为不变的前提下，为以下类增加显式数据源注入：

- `sau_n::BankedScratchpad`；
- `sau_n::StreamingConvPipelineModel`。

接口固定具备两种构造模式：

```text
OwnedGenerated
  现有 standalone 行为，继续 preload generator 数据

ExternalTensorBacking
  不调用 generator preload，read/write 通过注入的 backing interface
```

backing 必须是显式抽象接口，至少提供 bank/row byte 的 read/write；不得以捕获外部对象
的临时 `std::function` 代替生命周期合同。`BankedScratchpad` 在 standalone 模式继续拥有
原 `storage`，在 external 模式只持有 non-owning backing reference。

对象所有权和销毁顺序冻结为：

```text
SauNEndpoint
  -> owns SauNMemoryView
  -> owns StreamingConvPipelineModel
       -> BankedScratchpad holds non-owning reference to SauNMemoryView
```

上层 `PipelineMiniCPU`/memory owner 必须保证 `DutKuiMemoryModel` 的生命周期覆盖整个
`SauNEndpoint`；`SauNMemoryView` 对它只持 non-owning reference。
`SauNMemoryView` 必须先构造、后销毁；active model 必须在 reset、异常恢复和 endpoint
析构时先销毁。`StreamingConvPipelineModel` 的 external 构造路径必须跳过
`preloadSharedScratchpad()` 及其 `clear()`，不得覆盖 CPU 已准备的数据。

不得通过全局变量、绝对路径或在 `tick()` 中读取 fixture 文件实现注入。

## 6. SauNEndpoint 状态机

首版状态冻结为：

```text
Idle
  -> CollectConfig       msetins1~3
  -> AcquireScratchpad   msetins4 commit / crossbarStart
  -> Run                 每拍调用 StreamingConvPipelineModel::tick()
  -> ReleaseScratchpad   drained / crossbarDone
  -> Respond             msetins4 valid pulse
  -> Recovery
  -> Idle
```

要求：

- `msetins1~3` 各产生一次正常 write response；
- `msetins4` request 由 CPU 保持稳定，endpoint 只接受一次；
- 一个 Conv 只构造并启动一个 `StreamingConvPipelineModel`；
- sau_n 每个 CPU active tick 恰好推进一次，不可 host 循环跑到完成；
- D write grant 发生时立即更新共享 SRAM backing；
- `drained`、内部 request/response/D pending 清空且最后一个有效 D commit 对 CPU 可见前，
  不能进入 release；
- `crossbarDone` 的 release edge 之后才能返回 msetins4 completion；
- reset 必须取消配置、模型、ownership 和待返回 completion；
- start/done pulse tick 不推进内部模型，模型只在 ownership active 的 Run tick 推进；
- busy 时的新配置必须明确失败；
- exception 必须带四个原始 payload、解码字段和 sau_n 校验原因终止仿真。

## 7. 实施步骤

每次只实施一个 Step。Codex 完成代码和静态审查后不主动编译 gem5，由用户按建议命令
增量编译并反馈结果。

### Step 0：纠正基线和冻结来源

1. 以本计划替换之前“自写 Conv3 SAU”计划。
2. 记录 `sau_n` 来源 commit、文件列表和 SHA-256 manifest。
3. 记录当前目标仓库 dirty status，避免覆盖 CPU 已完成修改。
4. 把现有 `conv3_*` 标记为待替换实现，不在本 Step 删除。
5. 以 `SAU_CONV3_CSR_ABI.md` revision 1.2 和
   `SAU_GEM5_ENDPOINT_CONTRACT.md` v1.3 作为实现前冻结合同。
6. 生成 `docs/SAU_N_STEP0_BASELINE.md` 和
   `docs/SAU_N_SOURCE_MANIFEST.sha256`，记录来源、目标仓库 dirty 快照、最小导入文件
   清单、哈希和旧 `conv3_*` 的待替换边界。

完成标准：能够清楚区分 CPU 已完成部分、目标 sau_n 来源和错误路径代码；来源文件有
commit 与 SHA-256 可追溯记录；没有删除任何旧实现或用户已有文件。

### Step 1：导入原始 sau_n 并建立 standalone 回归

1. 从来源 commit 导入 `StreamingConvPipelineModel` 的最小依赖闭包，不使用绝对
   include path。首版范围固定为：
   - `im2col_types`；
   - `im2col_address`；
   - `banked_scratchpad`；
   - `sau_types`；
   - `sau_generators`，仅供 standalone 回归；
   - `sau_model`；
   - `streaming_pipeline_contract`；
   - `pipelined_im2col_model`；
   - `streaming_conv_pipeline_model`；
   - 上述模块直接对应的测试、来源 `SConscript` 和来源 manifest。
2. 不导入 `rtl/`、PDF、历史计划/报告、standalone SimObject timing wrapper，或不在上述
   编译依赖和定向测试闭包内的文件。
3. 在对目标仓库 `SConscript` 做聚焦适配前，先用 manifest 校验导入文件的源字节；适配
   后保留最小 source/test 登记，不改变模型行为。
4. 运行或由用户运行导入模块的定向测试，建立迁移后 baseline。

完成标准：manifest 能逐文件追溯最小闭包，目标仓库中的 sau_n standalone 行为与来源
commit 一致，且构建不引用来源 worktree。

当前进度：28 个 manifest 文件已导入，目标侧 `SConscript` 已聚焦到这组源码和对应
GTest；9 个 standalone GTest 共 76 个 test case 已全部通过，结果记录在
`docs/SAU_N_STANDALONE_BASELINE.md`。

### Step 2：实现 external tensor backing

1. 为 `BankedScratchpad` 增加显式 backing interface。
2. 保持 `OwnedGenerated` 为 standalone 默认值。
3. `ExternalTensorBacking` 禁止调用 generator preload 和 scratchpad clear。
4. 实现 A/B/C/D bank-row 到四个外部 tensor range 的映射。
5. 使用当前 `DutKuiMemoryModel::readByte/writeByte` 作为唯一 SRAM byte 权威。
6. 对重复地址、未映射有效请求、越界和无效 D write fail-fast。
7. 按第 5.3 节固定 endpoint、memory view、model 和 non-owning backing 的生命周期。

测试至少覆盖：

- A 的 N/C/H/W 首尾和跨 channel；
- B 的 K/OC 首尾和负数 byte；
- C 的 INT16 little-endian；
- D 的 NCHW 顺序；
- padding 不读、tail 不写；
- 修改任意 input/weight/bias byte 会改变对应计算结果；
- reset/销毁 active model 后 backing 不悬空，且共享 SRAM 已有 byte 不被清空；
- external 模式的 generator 调用计数为 0；
- standalone generator 回归保持原结果。

当前进度：`BankedScratchpad` 已支持 non-owning `ScratchpadBacking`；目标侧
`SauNMemoryView` 已将 A/B/C/D 映射到 `DutKuiMemoryModel` 的 byte backing，并增加了
未映射/重叠/越界检查；`DutKuiMemoryModel::dataMapped()` 已作为公开只读查询接口提供。
external 模式会跳过 generator preload 和 scratchpad clear。Step 2 相关测试已通过：
`BankedScratchpad` 6/6、`StreamingConvPipelineModel` 8/8、`SauNMemoryView` 集成测试
通过。

### Step 3：实现 CSR 到 PipelineResolvedConfig 适配

1. 复用已确认的四条 ABI pack/unpack 和 shadow/commit 逻辑。
2. 生成完整 `PipelineResolvedConfig`。
3. 使用 `validateStreamingConfig()` 作为最终 shape/scratchpad 合法性来源。
4. 校验外部 SRAM range 和 tensor layout。
5. 固定 `OutputReadyConfig{1,1}` 和第 4 节三个 generator 占位值，并由集成测试证明
   generator 从未被调用。
6. 单测每个 CSR 字段、1170-row 主 fixture footprint 和每个失败原因。

完成标准：合法四条 payload 生成唯一、可运行的 sau_n 配置；非法配置在构造模型前
失败。

当前进度：`SauNConfigAdapter` 复用 `Conv3CsrConfig` 的四 payload shadow/active
事务，并在 `msetins4` 发布前调用 `validateStreamingConfig()`；合法主 fixture 的
1170-row shared-scratchpad footprint、固定 generator 名称和所有 ABI/streaming 失败
路径均已有定向测试并通过。

### Step 4：实现 SauNEndpoint

1. 按第 6 节状态机实现 `SauEndpoint`。
2. 将 memory view 和 config adapter 注入
   `StreamingConvPipelineModel`。
3. 每 CPU tick 推进一个 sau_n tick。
4. 记录 crossbarStart、crossbarDone、drained 和 completion 的顺序。
5. `evaluateMemory()` 只输出 ownership pulse，`request.valid` 恒为 false；任何意外
   `Sram256Response.valid` fail-fast。
6. 增加 operation start/complete、sau_n cycles、scratchpad A/B/C/D 请求、B hit、
   D pending peak 和 CPU wait stats。

完成标准：直接驱动 endpoint 时，四条 CSR 能让真实 sau_n 从共享 SRAM 数据完成一次
Conv，并在 D 可见后返回。

当前进度：`SauNEndpoint` 已实现 `Starting -> Running -> Finishing -> Responding`
本地 scratchpad 状态路径；`Running` 每个 endpoint clock edge 恰好调用一次
`StreamingConvPipelineModel::tick()`，`evaluateMemory()` 全程不产生外部 SRAM beat。
独立测试已通过 ownership start/done、共享 SRAM D 写回、drained 后 completion、reset
保留 SRAM 和外部 response fail-fast 检查。

### Step 5：替换 CPU 活动后端

修改：

- `src/brs/PipelineMiniCPU.py`；
- `src/brs/pipeline_mini_cpu.{hh,cc}`；
- `configs/brs/run_pipeline_mini.py`；
- `src/brs/pipeline_stats.hh`；
- `src/brs/SConscript`。

要求：

- 新增正式选项 `--sau-model sau_n`；
- 默认仍为 `stub`，避免改变通用 CPU 回归；
- `sau_n` 绑定新 `SauNEndpoint`；
- `conv3` 不得继续作为正式目标名称；
- CPU HC、stall、retire 和 reset 行为不因后端替换而改变。

当前进度：`--sau-model sau_n` 已加入配置入口，CPU 已绑定
`SauNEndpoint`，默认 `stub` 行为保持不变，统计项和 SConscript 登记已完成；旧
`conv3` endpoint、测试、构建登记和运行选项已在 Step 8 移除，不属于 sau_n 活动计算
路径。待增量重编译确认清理后的构建闭包。

### Step 6：CPU/SAU 集成测试

测试必须证明：

1. CPU 真实执行四条指令，而非 host 注入配置；
2. 四个 payload 与 instruction hex 和寄存器值一致；
3. msetins4 在 sau_n 运行期间阻塞；
4. 每 CPU tick 只有一个 sau_n tick；
5. A/B/C 数据来自共享 SRAM，不来自 generator；
6. sau_n 内部 A/B/C/D request 和 tile/SA 统计非零且守恒；
7. D 写回共享 SRAM 后才完成；
8. reset 中断 active operation 后无残留 write/completion；
9. operation 全程无 SAU `Sram256Request`，crossbar ownership 期间 CPU DBUS 不并发；
10. reset 不清空 operation 前已有的共享 SRAM 内容；
11. StubSau 和无 SAU CPU 测试保持通过。

当前进度：SauNConfigAdapter、SauNMemoryView、SauNEndpoint 定向测试已通过；最终
fixture runner 已验证 CPU 真实执行四条指令、共享 SRAM backing、tick 级推进、reset/
ownership/completion 以及 StubSau 对照路径。最终报告位于
`test_by_agent/conv3_e2e/runs_sau_n_final/`。

### Step 7：四指令 e2e

正式运行至少包括：

```text
generated_four_ins_control_matched + sau_n
generated_four_ins_full_offload     + sau_n
```

verifier 必须独立检查：

- SAU instruction 数量恰好为 4；
- CSR active config 与 manifest 一致；
- sau_n operation start/complete 各一次；
- internal generator 使用次数为 0；
- input/weight/bias SRAM bytes 与 model 实际消费值一致；
- Im2Col padding/stride/K 顺序正确；
- D 与独立 direct Conv reference 逐 byte 一致；
- msetins4 completion 晚于最后一个 sau_n 有效 D grant/commit 对 CPU 可见的时刻；
- operation 全程无外部 SAU SRAM beat，crossbarStart/Done 各一次且顺序正确；
- CPU 到达 `0x5000` 的正常 ebreak。

此前 `conv3` 后端的五组 PASS 只作为 CPU/fixture 历史证据，不作为本 Step 的 sau_n
验收结果。

当前进度：Step 7 已通过。最终输出目录中的 7 个 report 和
`comparison_report.json` 均为 `PASS`；两个 sau_n fixture 均完成一次 operation，
D 与独立 direct Conv reference 逐 byte 一致，CPU 正常到达 `0x5000` 的 `ebreak`。

### Step 8：清理错误路径并更新文档

当前状态：用户已确认，旧 `conv3` endpoint 代码和测试已逐文件删除，构建登记、CPU
endpoint 选择逻辑和配置选项已同步清理；fixture、verifier、历史报告及 CSR decoder
保留。增量重编译和支持的 5-run e2e 回归均已通过，Step 8 完成。

本 Step 已逐文件处理：

- `src/brs/sau/conv3_compute_core.cc`、`.hh`、`.test.cc`；
- `src/brs/sau/conv3_memory_adapter.cc`、`.hh`、`.test.cc`；
- `src/brs/sau/conv3_sau_endpoint.cc`、`.hh`、`.test.cc`；
- 只服务于错误后端的测试和文档。

保留仍被新 endpoint 使用的四指令 CSR decoder、fixture、verifier 和历史报告；verifier
中的 `conv3` 分支仅用于历史报告复核。删除任何文件前先列出明确路径并由用户确认，
不批量删除目录。

## 8. 验收门

### Gate A：来源一致（已通过）

- sau_n 文件来源、commit 和 hash 可追溯；
- standalone GTest 结果与来源一致；
- 不依赖外部 worktree。

### Gate B：配置和数据映射正确（已通过）

- 四条 CSR 唯一映射到 `PipelineResolvedConfig`；
- external backing 使用真实 SRAM byte；
- generator 在集成模式完全禁用；
- A/B/C/D 地址和布局测试通过。

### Gate C：CPU 接口正确（已通过）

- CPU 真实发出四条命令；
- msetins4 阻塞、reset、completion 和 crossbar ownership 正确；
- 一个 CPU tick 对应一个 sau_n tick；
- `LocalScratchpadBacking` 模式全程不产生 `Sram256Request`，且 release 后才 completion。

### Gate D：端到端正确（已通过）

- 两个四指令 fixture 均由 sau_n 完成一次 Conv；
- D 与独立 reference 逐 byte 一致；
- CPU 正常 ebreak；
- 活动路径不调用自写 `Conv3ComputeCore`。

### Gate E：交付收口（部分完成，旧路径清理待确认）

- 错误路径代码经确认后移除或明确隔离；
- README、ABI 文档、运行命令和验证报告与实际路径一致；
- PLAN/STATUS 不再声明旧 `conv3` Gate D 等于 sau_n 接入完成。

## 9. 验证方式

Codex 不主动编译 gem5。建议用户在每个实现 Step 后进行增量构建：

```sh
scons build/RISCV/gem5.opt -j4 --ignore-style --limit-ld-memory-usage
```

需要构建并运行的测试 target 名称至少包括：

```text
banked_scratchpad.test
streaming_pipeline_contract.test
pipelined_im2col_model.test
streaming_conv_pipeline_model.test
sau_n_memory_view.test
sau_n_config_adapter.test
sau_n_endpoint.test
pipeline_sau.test
```

最终 e2e 入口沿用并修改：

```sh
test_by_agent/conv3_e2e/run_conv3_e2e.sh
```

脚本最终必须显式运行 `--sau-model sau_n`，并生成独立的 sau_n report，不能复用旧
`conv3` report 冒充通过。

每个 Step 还应运行：

```sh
git diff --check
```

本轮最终 e2e 验证输出：

```text
test_by_agent/conv3_e2e/runs_sau_n_final/
```

该目录历史上包含 archive/stub、legacy/stub、四指令 control 的 stub/conv3/sau_n、
full-offload 的 conv3/sau_n 共 7 个 report，及独立的 `comparison_report.json`，所有
历史 report 和 comparison 均为 `PASS`。清理后的活动 runner 只生成 archive/stub、
legacy/stub、四指令 control 的 stub/sau_n 和 full-offload/sau_n 五个支持路径。

清理后的 5-run 活动 e2e 以及对应增量编译已由用户确认通过。

## 10. 风险和限制

1. `StreamingConvPipelineModel` 是用户明确选择的 shared-scratchpad architecture
   exploration 模型；除已有证据外，不新增“与 fused RTL 严格逐拍一致”的声明。
2. 四条 ABI 可编码的 shape 大于 sau_n 4096-row/bank footprint，正式支持范围是两者
   交集。
3. external backing 复用 sau_n 内部 scratchpad latency，不代表现有 256-bit crossbar
   的逐 beat 性能、same-bank collision 行为或 RTL SRAM timing。
4. 当前 msetins4 阻塞意味着 CPU 与 SAU 不重叠；首版不增加异步 query/status。
5. 来源仓库和目标仓库是两个独立 Git 历史，必须用 manifest 固定迁移来源，不能依靠
   cherry-pick 假设。
6. 当前工作树已有 CPU 修改和未跟踪文件，实施时必须小步编辑，不能 reset、clean 或
   覆盖无关改动。

## 11. Definition of Done

当前结论：以下功能接入条件均已由 standalone baseline、定向测试和最终 e2e report
满足；旧 `conv3` endpoint 路径已清理，不再作为活动 CPU 后端。

只有同时满足以下条件，才能声明“sau_n 已接入 CPU”：

1. CPU 从 hex 真实执行一次 `msetins1~4`；
2. 四条 CSR 正确生成 `StreamingConvPipelineModel` 配置；
3. 集成模式不使用任何 deterministic generator 作为 tensor 数据；
4. 到 msetins4 时 CPU 已准备的数据由 sau_n scratchpad request 实际消费；
5. sau_n 完整执行 Im2Col、B buffer、16x16 SA 和 D pending/write；
6. D 写回 CPU 可读的同一份 SRAM；
7. D 与独立 Conv reference 逐 byte 一致；
8. msetins4 只在 drained 和 D 可见后完成；
9. 集成 operation 不产生外部 `Sram256Request`，ownership release 后才返回 completion；
10. CPU 正常到达 ebreak；
11. sau_n standalone 和 CPU 既有回归通过；
12. 活动路径不再调用自写 `Conv3ComputeCore`；
13. 构建、运行、来源和验证报告可由仓库内文件独立复现。
