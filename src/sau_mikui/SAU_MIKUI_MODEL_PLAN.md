# Mikui SAU gem5 周期级事务模型设计与实施计划

## 0. 文档状态

- 状态：已确认的设计合同，实施阶段按本文推进。
- 目标目录：`/home/xch/work/gem5_cpu_veu/src/sau_mikui`。
- gem5 基线提交：`aca1433dd940aa1d87fd5606e73a9b1460e663ce`。
- RTL 基线仓库：`/home/xch/work/mikui-debug-dma`。
- RTL 基线提交：`e7c05d066fd5bc52f7ec1f31d53d8c5cef5651a8`。
- RTL 集成 filelist：`sim/vcs/script/case_mikui_dma/verilog.f`。
- RTL 顶层行为基线：`hardware/src/sa_element/SA_CORE.sv` 的实际实例链。
- 冻结日期：2026-08-25。

本文同时承担五项职责：定义最终目标、冻结行为边界、给出 C++ 模块架构、安排分阶段实施、定义适量但有效的验证与验收方法。本文不是代码完成状态报告；各阶段完成情况应在实施时另行维护。

## 1. 最终目标

在 gem5 中实现当前 Mikui RTL SAU 的完整周期级事务模型。模型必须：

1. 覆盖当前 RTL 支持的 GEMM、卷积、转置和矩阵加功能。
2. 覆盖 A/B/D 转置组合、reuse mode、register mode、flow mode、cutbit、stride 和 shift。
3. 以 SAU 时钟上升沿为状态推进边界，使用 gem5 `Tick` 和 `clockEdge(Cycles(1))` 调度。
4. 在 CSR、scheduler、memory、register file、feeder、transposer、PE array 和 writeback 等关键边界逐拍对齐 RTL。
5. 使用真正的 16×16 PE 状态阵列逐拍传播 activation、weight、控制和累加状态。
6. 使用 128-bit SRAM beat 事务接入 Mikui memory/crossbar，不给 RTL 接口虚构 ready/retry 行为。
7. 最终替换 CPU bring-up 使用的 `StubSau`，同时保留必要的 stub 测试用途。
8. 作为独立 `ClockedObject` 支持可配置 SAU 时钟；同频是首个严格对拍基线，最终提供一次 CPU:SAU=1:2 的异频集成检查。
9. 当现有 CPU、CBU、clock/CDC、memory 或 crossbar 实现与当前 RTL 不一致并阻碍功能或拍数对齐时，允许做有 RTL 依据的最小必要修改。

这里的“周期级事务模型”不是按指令一次性计算结果再等待固定延迟，也不是门级仿真。它表示：外部访问以事务表达，内部硬件状态按每个 SAU 时钟沿原子更新，关键流水和资源占用真实存在。

## 2. 明确不做的内容

以下内容不属于本计划：

- 门级延迟、组合毛刺、X/Z 传播、亚稳态和功耗建模。
- 时钟门控单元的物理实现；只建模 `sau_clk_en` 对状态冻结/唤醒的架构效果。
- 无关的 CPU、VEU、DMA 或 SoC 重构。
- 为了通用性支持任意 PE 阵列尺寸或任意数据位宽。
- 大规模随机验证、覆盖率收敛或所有模式的重复压力回归。
- 静默修复 RTL 缺陷或把 RTL 总线“美化”为可靠 ready/valid 协议。
- 第一版中的中途 DVFS、checkpoint/restore 和功耗统计；如后续需要，应单独立项。

## 3. 权威来源与冲突处理

### 3.1 权威优先级

出现冲突时按以下顺序判断：

1. 当前基线提交实际 RTL 仿真波形。
2. `case_mikui_dma/verilog.f` 中编入的 RTL 源码及其实际实例链。
3. RTL testbench、固件和数据生成脚本体现的合法用法。
4. `docs/SAU_SPEC.md`、`docs/SubModules_Specification.md` 等规格文档。
5. `sim/simulator/src/SAU/SAU.py` 等功能模型。
6. 源码中的自然语言注释。

若 RTL 源码和波形不一致，先检查宏、参数、filelist、仿真 case 和采样边界。仍无法解释时停止该项实现，记录最小复现并请求确认，不能自行挑选更“合理”的行为。

### 3.2 已确认的基线冲突

| 项目 | 旧描述或现有 gem5 | 当前权威 RTL | 本模型要求 |
|---|---|---|---|
| 阵列规模 | 旧 `SAU_SPEC.md` 多处写 8×8 | `SA_pkg::SA_SIZE=16`，`SA_CORE` 默认 16×16 | 固定实现 16×16 |
| feeder 对齐延迟 | 旧文档写 `STATE_DELAY=11` | `SRAM_DELAY=1`、`ADDR_DELAY=6`、`MEMCTRL_DELAY=2`，合计 9 | 按当前参数和流水寄存器复现 |
| 计算模式注释 | 部分端口注释把 01/10 写成转置/卷积 | `SA_pkg` 定义 00 MATMUL、01 CONV、10 TRANSPOSER、11 ADD，PE 也按该枚举判断 CONV | 以枚举和实际使用为准 |
| SAU CSR 基线 | gem5 `sau_protocol.hh` 注释仍指向旧提交 `86c289c` | 当前 RTL 提交为 `e7c05d...` | 重新审计四组 64-bit command，不依赖旧注释 |
| SRAM bank | gem5 Mikui crossbar 仅两个 bank | 当前 `crossbar_mi_full` 为三个 slave bank | 集成阶段补齐第三 bank |
| crossbar 时序 | gem5 保留旧 `Idle/Active/RvActive` 和旧寄存路径 | 当前 `crossbar_mi_full` 为 IDLE/ACTIVE，注释和实现目标为 1-cycle SRAM response | 对齐当前 RTL，不沿用旧延迟 |
| transposer 测试宏 | `MODULE_TEST` 改变输出索引方向 | `case_mikui_dma` 未定义 `MODULE_TEST` | 整机模型使用非 `MODULE_TEST` 行为 |
| PE 可选宏 | 源码存在 `FULL_PRECISION`、`MAX_USE` 分支 | 当前 `define.f` 未启用 | 第一版只实现当前宏配置 |

## 4. 当前 RTL 结构事实

### 4.1 有效层级

```text
SA_CORE
├── clock gate（只保留使能语义）
├── csr
├── scheduler
├── mem_addr
├── mem_ctrl
├── register_file
├── feeder
│   └── shift_register
└── sa_feeder
    ├── transposer_tiny[0]：输入优先
    ├── transposer_tiny[1]：输入或 16-bit 输出高半部
    ├── transposer_tiny[2]：输出低半部
    └── SA_ENGINE
        └── SA_ROW[16]
            └── SA_PE[16]
```

`trans2sa_top.v`、`SA_TOP.v`、`SA_pe.v`、`SA_row_unit.v` 等文件虽存在或被某些通用 filelist 收录，但当前 `SA_CORE` 实际路径使用 `sa_feeder.sv`、`SA_ENGINE.sv`、`SA_ROW.sv` 和 `SA_PE.sv`。不能根据相似文件名混用旧实现。

### 4.2 固定架构常量

| 常量 | 当前值 | RTL 来源 | 建模要求 |
|---|---:|---|---|
| PE rows | 16 | `SA_pkg::SA_SIZE` / `SA_CORE` | 第一版仅支持 16 |
| PE columns | 16 | 同上 | 第一版仅支持 16 |
| PE count | 256 | 16×16 | 每个 PE 保留独立状态 |
| activation/weight | signed 8-bit | `INT8` / `INPUTDW` | 使用显式定宽有符号运算 |
| accumulator | signed 24-bit | `OUTPUTDW` | 每步按 RTL 饱和加法 |
| quantized output | signed 16-bit | `QUANTDW` | 8-bit 模式饱和到 [-128,127] 并置于 16-bit 容器；shift 模式饱和到 int16 |
| SRAM beat | 128-bit / 16 bytes | `SRAM_DATA_WIDTH` | 地址为 byte address，wstrb 每 bit 对应一个 byte |
| register depth | 48 | `SA_CORE::REGDEEPTH` | 按当前索引、上下半区和复用行为实现 |
| transposer count | 3 | `sa_feeder` | 三份独立存储、计数和 ready 状态 |
| transposer depth | 16 rows | `DIM_R=ROW_NUM` | 装满后才能读出 |
| address pipeline | 6 cycles | `mem_addr ADDR_DELAY` | 不折叠为即时地址计算 |
| SA core SRAM delay | 1 | `SA_CORE SRAM_DELAY` | 可配置但默认 1，严格模式需与黄金 RTL 相同 |
| PE multiply pipeline | RTL `DW02_mult_2_stage` | `SA_PE` | 保留乘法、加法和 valid 的拍间关系 |

这些架构常量应集中放在一个配置/常量头文件中。若 SimObject 参数传入不支持的 row/column/width，构造阶段直接报错，不能悄悄运行成未经验证的配置。

## 5. 指令、CSR 和模式合同

### 5.1 CPU 侧传输

现有 CPU 使用四组 SAU set/get 指令，把两个 32-bit 操作数组成 64-bit `csr_wdata`。CSR 地址范围为 `0x200` 到 `0x207`，偶数地址对应一组 64-bit 写，奇偶地址用于读取其低/高 32-bit 内容。

实现前必须重新核对 `sau_protocol.hh`、Spirit `Decoder.sv`、`CBU.sv` 和当前 `csr.sv`，然后更新协议注释中的基线提交。编码仍一致时不改变公开指令枚举；不一致时以最小兼容修改为原则。

### 5.2 四组 64-bit command 位域

以下表格来自当前 `csr.sv` 的实际赋值，而不是旧软件模型：

| Command | 位域 | 含义 |
|---|---|---|
| 1 | `[1:0]` | `register_mode` |
| 1 | `[3:2]` | `conv_kernal` 的当前 CSR 编码 |
| 1 | `[3]` | 当前 RTL 同时用于形成 `reuse_mode={1'b0,wdata[3]}`，属于重叠位域，必须原样核对波形 |
| 1 | `[4]` | `stride_flag` |
| 1 | `[5]` | `shift_flag` |
| 1 | `[33:32]` | `trans_mode` |
| 1 | `[38:34]` | `cutbit` |
| 1 | `[39]` | `last_ins_flag` |
| 2 | `[7:0]` / `[15:8]` / `[23:16]` | vertical/horizontal/output x step |
| 2 | `[39:32]` / `[47:40]` / `[55:48]` | vertical/horizontal/output channel step |
| 3 | `[19:0]` | vertical base address |
| 3 | `[51:32]` | horizontal base address |
| 4 | `[0]` | start，经 `csr_operation` 的 set/clear 语义处理 |
| 4 | `[8:1]` | instruction id |
| 4 | `[28:9]` | output base address |
| 4 | `[51:32]` | bias address |
| 4 | `[53:52]` | PE work/calculation mode |
| 4 | `[55:54]` | SA flow mode |
| 4 | `[61:56]` | flow loop times |

Command 1 的 bit 3 重叠和 `conv_kernal` 位宽扩展必须作为首个 CSR directed test 验证点。模型不能擅自按软件模型中更整齐的旧布局重新解释。

### 5.3 模式枚举

计算模式：

| 编码 | 模式 |
|---:|---|
| 00 | MATMUL / GEMM |
| 01 | CONV |
| 10 | TRANSPOSER |
| 11 | ADD / matrix add |

转置模式：

| 编码 | 模式 |
|---:|---|
| 00 | ABD |
| 01 | ATBD |
| 10 | ABTD |
| 11 | ABDT |

Flow 模式：

| 编码 | 模式 |
|---:|---|
| 00 | CNORMAL：普通输出并清理 |
| 01 | CTRANS：转置输出 |
| 10 | RETAIN：累加结果留存 |
| 11 | TRETAIN：转置/留存组合 |

Register mode 至少需要覆盖标准卷积相关模式和 `10` 的 depthwise/single-column 行为。Reuse、register、flow、trans、shift、stride 不能各自做成互不相干的固定延迟开关，它们共同决定 scheduler、地址、feeder、transposer 和 PE 清理时刻。

## 6. gem5 总体架构

### 6.1 两层模型

模型分成纯周期核心和 gem5 包装层：

```text
PipelineMiniCPU / HC CBU
          │ CPU-domain request/response mailbox
          ▼
MikuiSau (ClockedObject)
├── 独立 clock domain 与 tick event
├── CPU/SAU 边界锁存
├── SRAM transaction boundary
├── stats / trace
└── MikuiSauCycleModel
    ├── SauCsr
    ├── SauScheduler
    ├── SauAddressGenerator
    ├── SauMemoryController
    ├── SauRegisterFile
    ├── SauShiftRegister
    ├── SauFeeder
    ├── SauTransposeBank (3 × SauTransposer)
    ├── SauArrayEngine
    ├── SauPeArray (16 × 16 SauPe)
    └── SauOutputPath
```

`MikuiSauCycleModel` 是普通 C++ 对象，便于无 gem5 Python 配置的 GTest 精确驱动。`MikuiSau` 是唯一的 SAU SimObject/ClockedObject，负责真实 Tick 调度、跨对象连接和统计注册。内部 256 个 PE 是对象或定长状态数组，不注册为 256 个 SimObject。

### 6.2 独立时钟和 CDC

- `Tick` 是全局时间单位，不等于一个 SAU cycle。
- SAU 周期事件调度到 `clockEdge(Cycles(1))`。
- `sauCycle` 只在有效 SAU 上升沿递增。
- `sau_clk_en=0` 时周期核心冻结；包装层仍需能够观察唤醒条件。
- 空闲且没有跨域请求、内存返回或未完成操作时可以停止自调度。
- CPU 提交请求后，包装层把它放入 CPU→SAU mailbox，并为下一个合法 SAU edge 安排唤醒。
- SAU 响应通过 SAU→CPU mailbox 返回；不能在同一个逻辑边沿产生零拍跨域穿透。
- 同频严格对拍时，mailbox/adapter 的边界必须配置成与 `dut_mikui_dma` 直连行为等价。
- 1:2 smoke test 验证独立时钟可运行，但 CDC 包装延迟不计入 SAU 内部 start-to-done 周期。
- 若最终需要完全复现 `dut_mikui_crossclk/csr_CDC_bridge.sv` 的 FIFO 深度和可见延迟，应作为包装层扩展，不能污染核心模块的周期合同。

### 6.3 每拍原子更新规则

每个 SAU edge 固定执行：

1. 读取当前状态和该边沿前已经可见的外部输入。
2. 由所有模块的 current state 计算组合输出和 next state。
3. 先保存所有 next state，不在模块调用过程中修改 current state。
4. 所有模块统一 commit，模拟 SystemVerilog nonblocking assignment。
5. 发布提交后的外部可见输出。
6. 更新统计并按需写 trace。
7. 若仍 active/in-flight，则调度下一个 SAU edge。

严禁用 `moduleA.tick(); moduleB.tick();` 直接让 B 读取 A 已经提交的新状态。这会产生 RTL 不存在的同拍穿透。推荐每个模块提供类似：

```cpp
Outputs evaluate(const Inputs &) const;
void computeNext(const Inputs &, const Outputs &);
void commit();
void reset();
```

也可以使用 `State current/next` 和纯函数 `nextState(...)`，但必须保持全局两阶段语义。

### 6.4 同 Tick 多时钟边沿规则

CPU、SAU 或 memory edge 落在同一个全局 Tick 时，跨域接收方只能采样该 Tick 之前已经发布的 mailbox 内容；发送方本 Tick 新提交的内容在接收方下一个边沿才可见。实现应使用显式 current/next mailbox 或 gem5 event priority，不能依赖对象构造顺序。

## 7. 内部模块设计

### 7.1 `SauCsr`

职责：

- 实现 `0x200..0x207` 的 set/get 行为。
- 保存四组 command 对应的配置字段。
- 复现 `start_reg`、`busy`、`csr_ready`、读数据打一拍及 `flow_end` 清理关系。
- `start` 同时生成 `crossbar_start`。
- 工作中再次写 SAU CSR 时复现 `crossbar_error` 脉冲。
- 保留 readback 的当前拼接布局。

关键状态：配置寄存器、`startReg`、`busy`、`isProcessing`、`readyReg`、`readDataReg`、error pulse。CSR 请求是电平信号时必须避免重复接受同一个 CPU 请求；这一责任由 CPU/SAU mailbox 事务化边界和 CSR 单拍接受共同保证。

### 7.2 `SauScheduler`

RTL 顶层 scheduler 状态：

```text
IDLE -> FIRST_LOAD / REGISTER_LOAD
REGISTER_LOAD -> TRANSPOSE_LOAD / REUSE_LOAD
TRANSPOSE_LOAD -> REUSE_LOAD
FIRST_LOAD -> REUSE_LOAD / D_OUT
REUSE_LOAD -> FIRST_LOAD / D_OUT
D_OUT -> IDLE
```

必须保存：

- 延迟后的 start/ins-valid。
- `flow_times_cnt`、`transload_state_cnt`。
- `input_switch[1:0]`。
- `last_flow_time` 置位和清除时刻。
- `data_last` 的一拍延迟。
- 卷积复用判定及其锁存。
- `flow_end` 组合脉冲和 `crossbar_done` 的寄存延迟。

Scheduler 不直接计算最终操作延迟。它只根据来自 memory、feeder、array 和 output 的真实完成信号推进。

### 7.3 `SauAddressGenerator`

职责：

- 在 start 时锁存 vertical/horizontal/output/bias 基地址、步长和模式。
- 分别维护 vertical、horizontal、output 的 x/channel/flow 计数器。
- 复现 6-stage 地址流水和相关 valid/last 延迟链。
- 实现 pointwise、标准卷积、depthwise、stride、shift 和 concat 的地址分支。
- 实现 output address ping-pong，支持前一条命令结果写回与后一条命令并行存在。
- 输出逐拍 `sram_mem_addr`、读写 enable、read-last、write-last 和 register-clear。

20-bit command 地址经 `BASE_ADDR=0x2000_0000` 形成 byte address。地址计算必须使用定宽无符号中间量复现 RTL 截断，不能直接用无限精度表达式后再随意转换。

### 7.4 `SauMemoryController`

状态为 IDLE、REQUESTING、WAITING，但当前 RTL 的读写输出在各状态下基本保持同类行为。模型仍保留状态，以便对拍和后续 RTL 变化。

读路径要求：

- 每个 `sram_rd_enable` 高电平周期产生一个独立 128-bit read beat。
- `sram_rd_enable` 和 `sram_rdaddr_last` 进入 `SRAM_DELAY+1` 延迟链。
- 返回数据与延迟后的 valid/last 在 feeder 入口对齐。
- strict 模式下，缺失、提前或多余 response 记为错误，不能自动把请求重发。

写路径要求：

- `sram_wr_enable` 每拍发出独立 write beat。
- 当前有效 16×16 配置通常使用全 16-byte wstrb；具体以 `mem_ctrl.sv` 和实际 case 为准。
- `last_ins_wr_done` 仅在 last instruction、D_OUT 和最后写数据相遇时产生。

### 7.5 `SauRegisterFile` 和 `SauShiftRegister`

Register file 有 48 项深度并具有以下状态：

```text
IDLE, LOADING, PADDING, SHIFTING, STRIDING, STRSHIFT, STORING
```

模型需要保留：

- 8-bit 与 shift/16-bit 数据的上下半区布局。
- padding、stride、extra kernel 数据拼接。
- 标准卷积和 depthwise 的不同写入/读出索引。
- 首 flow、后续复用 flow、clear 和 wrap 行为。
- read valid/last 的流水延迟。

Shift register 需要按 kernel 计数器逐拍形成滑窗。对于 kernel>3 的额外取数、stride 拼接和 almost-last 必须按寄存器状态实现，不能把整窗一次性切片后立即交给 feeder。

### 7.6 `SauFeeder`

职责：

- 把 memory 返回数据按照延迟后的 `core_state` 和 `input_switch` 标记为 A、B 或 C。
- 对 register-file 和 SRAM 两种来源进行仲裁。
- 实现 NO_INPUT、ONE_INPUT、TWO_INPUT 输出数量状态。
- 支持 pointwise 16-bit 的偶/奇 byte 重组。
- 生成 A/B data、valid、last 和 C/bias data、valid。
- 收集阵列结果到 16-row output buffer。
- shift=0 时按 16 行输出 16 个 128-bit beat；shift=1 时每行拆成低/高两半，输出 32 个 128-bit beat。
- 保留写回启动到真正 output-buffer 读出的地址流水延迟。

当前有效参数下 feeder 的控制标签延迟为：

```text
STATE_DELAY = SRAM_DELAY(1) + ADDR_DELAY(6) + MEMCTRL_DELAY(2) = 9
```

该数值不应散落为 magic number；由配置常量计算，并在 strict 模式断言与黄金配置一致。

### 7.7 `SauTransposer`

每个 transposer 保存 16×16 个 8-bit 元素及：

- `cntIn`、`cntOut`。
- 输入可写 `ready`。
- 装满后的输出可读 `ready_o`。
- `valid_o`、`last_o` 和 sticky `error`。
- transpose/non-transpose 读出选择。
- reuse 行为接口；当前三个实例传入 `reuse_en=0`，但复用效果由上层资源选择和保留状态共同产生。

装入 16 行后才能开始输出。输入侧优先使用 transposer 0；transposer 0 不可用时才考虑 transposer 1。transposer 1 在 shift 输出期间可能被高半部输出占用，transposer 2 专用于输出低半部。

### 7.8 `SauArrayEngine`、`SauPeArray` 和 `SauPe`

Array engine 状态：

```text
IDLE, START, WORK, STORAGE, DONE
```

核心要求：

- 保存 16 行×16 列的 PE 状态，不能用一个整矩阵乘结果和固定 latency 替代。
- activation 从左向右逐列传播，weight 从上向下逐行传播。
- `active_delay` 和 `weight_delay` 产生的行列 skew 必须存在。
- stop-enable、write-strobe、instruction、shift-control 和 accumulator-valid 随阵列传播。
- multiply 使用两级流水，随后进入 24-bit 饱和累加。
- `keep_mode` 控制一次结果输出后是否清 accumulator。
- CONV/ADD 路径的 bias/add 时刻按 `acc_finish_flag` 处理。
- depthwise/single-column 模式只打开轮转的列 mask。
- matrix-add 输出行顺序使用反向 `cnt_o_switch`。
- 输出逐行产生 `row_score_valid`、row index 和 `cal_finish`。

PE 运算必须避免 C++ 未定义或实现相关行为：

- 所有输入先显式符号扩展。
- 乘法中间位宽按 RTL 的 9×9 和截取规则实现。
- 每次加法调用等价于 `SA_pkg::saturate_add_signed` 的 24-bit 饱和函数。
- shift 的低 8-bit/high 8-bit 部分乘积组合按 RTL 的 `shift_mode` 传播时刻处理。
- 右移必须是明确的算术右移，不能依赖负数右移的编译器差异。

### 7.9 `SauOutputPath`

`sat_truncate_func` 的等价实现必须：

1. 把 signed 24-bit accumulator 算术右移 `cutbit`。
2. shift=1 时检查 16-bit 范围，饱和到 `0x8000..0x7fff`。
3. shift=0 时检查 8-bit 范围，饱和到 -128..127，并保留 RTL 的 16-bit 表示。
4. 与 transposer 1/2 的高低半部交织顺序一致。
5. 正确生成 result valid/last、storage ready、execute done 和 update finished。

## 8. SRAM 与 crossbar 合同

### 8.1 SAU 端口语义

`Sram128Request.valid` 是每周期 beat strobe，不是等待 ready 的 level handshake。每个 valid 周期都是新请求；`writeStrobe==0` 表示读。正常运行依赖 `crossbar_start/done` 形成 accelerator 独占窗口。

模型不得：

- 因未收到 response 自动重复上一个请求。
- 遇到 collision 时静默暂停 SAU 内部流水。
- 把多个连续 beat 合并为一个大事务后丢失逐拍地址。

模型应统计并可在 strict 模式报错：request drop、未映射地址、response 早到/迟到、response 数量不匹配、写掩码非法。

### 8.2 现有 gem5 memory/crossbar 的必要修正

当前 `brs/memory/npu_lpnpu_mikui_crossbar.*` 与基线 RTL 不一致。整机接入前必须进行最小升级：

1. 从两个 bank 扩展到三个 bank。
2. 支持 `sram_B/C/D` 定义的动态分区边界；这些值在 `dut_mikui_dma` 是顶层输入。
3. 对齐当前 `crossbar_mi_full.sv` 的 IDLE/ACTIVE 行为和 1-cycle SRAM response。
4. 对齐 DBUS pending/bubble 逻辑及 accelerator master 选择。
5. 保留 SAU、VEU 和 DBUS 公开请求结构，必要时扩展数组宽度而不是重写所有调用方。
6. 更新 `NpuLpnpuMikuiMemoryModel` 的 bank storage、trace 和测试。
7. 审计 `PipelineMiniCPU` trace 中硬编码的 bank 数和旧 state 输出。

这部分属于 SAU 完整集成的必要前置修正。禁止绕过 crossbar 让 SAU 私下直接访问另一份 memory，因为那会破坏 CPU/VEU/SAU memory visibility。

## 9. 建议文件布局

```text
src/sau_mikui/
├── MikuiSau.py
├── SConscript
├── SAU_MIKUI_MODEL_PLAN.md
├── sau_types.hh
├── sau_constants.hh
├── sau_csr.hh / .cc
├── sau_scheduler.hh / .cc
├── sau_address_generator.hh / .cc
├── sau_memory_controller.hh / .cc
├── sau_register_file.hh / .cc
├── sau_shift_register.hh / .cc
├── sau_feeder.hh / .cc
├── sau_transposer.hh / .cc
├── sau_pe.hh / .cc
├── sau_array_engine.hh / .cc
├── sau_output_path.hh / .cc
├── sau_cycle_model.hh / .cc
├── mikui_sau.hh / .cc
└── tests/
    ├── sau_csr_scheduler.test.cc
    ├── sau_transposer.test.cc
    ├── sau_pe_array.test.cc
    ├── sau_memory_feeder.test.cc
    └── sau_end_to_end.test.cc
```

允许在实现过程中合并过小的 `.cc`，但不允许把所有逻辑堆进一个 `timing_sau.cc`。类边界应对应可独立解释和测试的硬件职责，而不要求与每个 RTL 文件一一机械对应。

对 `src/brs` 的预计最小修改：

- `brs/SConscript`：加入新 SimObject/source/test。
- `brs/pipeline_mini_cpu.*` 和 `PipelineMiniCPU.py`：选择/连接 Mikui SAU、配置独立时钟边界。
- `brs/pipeline/pipeline_core.*`：把当前同步 `SauEndpoint::clockTick()` 驱动改成适配 mailbox/独立对象，同时保留 stub 测试路径。
- `brs/sau/sau_protocol.hh`、`sau_endpoint.hh`：仅在当前接口无法表达异步边界时最小扩展，并保持已有测试可迁移。
- `brs/memory/npu_lpnpu_mikui_crossbar.*` 与 memory model：对齐当前三 bank RTL。

任何公开接口调整都必须先列出受影响调用方和测试，避免一次提交同时重构 VEU 或其他无关路径。

## 10. SimObject 参数、统计与 trace

### 10.1 建议参数

| 参数 | 默认值/规则 | 用途 |
|---|---|---|
| `clock` | 由 gem5 clock domain 配置 | SAU 时钟周期 |
| `sram_delay_cycles` | 1 | 必须与黄金 RTL case 一致 |
| `strict_timing` | true（开发/测试） | 异常时 panic 或测试失败 |
| `cycle_trace_file` | 空 | 非空时输出结构化逐拍 trace |
| `trace_internal_pe` | false | 仅定位 PE 问题时开启，避免巨大输出 |
| `rows/cols` | 16，只接受 16 | 防止误配置 |

### 10.2 最小统计集

- accepted/completed command 数。
- 按 MATMUL/CONV/TRANSPOSER/ADD 分类的 command 数。
- SAU active、idle、clock-gated cycles。
- SRAM read/write beat 数。
- scheduler 各状态周期。
- feeder、transposer、array、output 活跃周期。
- transposer error、crossbar drop、未映射地址、异常 response、非法配置次数。
- 每条 command 的 start、first-read、first-array-input、first-result、last-write、done cycle。

统计用于定位，不应用统计值反向驱动模型时序。

### 10.3 trace 采样格式

建议每行代表一个提交后的 SAU edge，至少包含：

```text
tick,sau_cycle,reset,clk_en,event,
csr_we,csr_re,csr_addr,csr_ready,start,busy,
scheduler_state,input_switch,flow_count,last_flow,flow_end,
sram_req,sram_addr,sram_wstrb,sram_rvalid,
feeder_a_valid,feeder_a,feeder_b_valid,feeder_b,feeder_c_valid,
trans0_ready,trans0_valid,trans1_ready,trans1_valid,trans2_ready,trans2_valid,
array_state,array_en,pe_finish,row_valid,row_index,
result_valid,result_last,result_data,xbar_start,xbar_done,detail
```

PE 全阵列状态不进入默认 trace。需要时只选定 PE 坐标或输出独立 debug 文件。

## 11. 分阶段实施计划

每一阶段都必须可编译、可测试，不允许用空实现或硬编码成功占位。阶段之间采用小提交，避免同时修改大量 CPU、memory 和 SAU 文件。

### 阶段 0：基线审计与合同测试

- 固定 RTL/gem5 提交和 filelist。
- 建立 current/next 统一类型、定宽算术工具和 cycle 编号约定。
- 审计四组 command、CBU request/response 和 CSR ready 时序。
- 建立一份小型 RTL trace 信号清单。
- 用一个最小 SAU case确认实际阵列规模、SRAM latency 和 command 位域。

完成条件：所有已知文档冲突有明确结论；未确认项列为波形校准点。

### 阶段 1：周期核心骨架与独立时钟包装

- 创建 `MikuiSau.py`、SConscript、CycleModel 和 ClockedObject。
- 实现 reset、事件唤醒、idle 停调度、clock enable、cycle counter。
- 实现 CPU/SAU current/next mailbox，不接计算功能。
- 建立 trace 和 strict error 基础设施。

完成条件：同频下单个 CSR 空事务能够在定义的边沿穿过包装层；没有零拍 CDC 穿透。

### 阶段 2：CSR 与 scheduler

- 实现四组 command 的写、读、set/clear、start、busy、ready、error。
- 实现 scheduler 全状态、flow counter、input switch、last-flow 和 done。
- 接入现有 SAU CBU，保留 StubSau 测试。

完成条件：CSR/scheduler directed GTest 通过，并与一个 RTL command trace 对齐。

### 阶段 3：transposer

- 实现一个 16×16 transposer 的逐拍装入、普通/转置读出、ready/valid/last/error。
- 实现三个实例的仲裁和输出资源保留。
- 明确非 `MODULE_TEST` 的元素顺序。

完成条件：普通输出、转置输出、装满前不可读和 ping-pong 输入四类定向测试通过。

### 阶段 4：PE 与 16×16 array engine

- 实现定宽饱和运算。
- 实现单 PE 的乘法/累加/clear/keep/shift/bias。
- 扩展为 16×16 波前和 row engine 状态。
- 实现 depthwise 列 mask、matrix-add 行顺序和 quantize。

完成条件：单 PE 拍级测试、首/末 PE 波前测试和小型阵列手工结果测试通过；正式模型仍固定实例化 16×16。

### 阶段 5：address、memory 和三 bank crossbar 前置修正

- 实现地址计数和 6-stage pipeline。
- 实现 memory controller 固定 latency beat 流。
- 升级现有 gem5 Mikui crossbar/memory 到当前三 bank RTL。
- 增加最少量 DBUS/VEU 兼容回归，避免修 SAU 时破坏已有路径。

完成条件：SAU 读写 beat 的地址、wstrb、response 周期与 RTL 代表 trace 对齐；三个 bank 均可见且 CPU/SAU 共享内容。

### 阶段 6：register file、shift register 和 feeder

- 实现 48-depth 存储和全部 register 状态。
- 实现卷积窗口、stride、shift、pointwise 重组。
- 实现 A/B/C 通道路由和 9-cycle 标签对齐。
- 实现 output buffer、16/32 beat 写回。

完成条件：标准卷积和 depthwise 各有一个 memory→feeder→array-input 定向 trace 对齐。

### 阶段 7：完整输出闭环与全部模式

- 连通 scheduler、memory、feeder、transposer、array 和 output feedback。
- 完成 CNORMAL/CTRANS/RETAIN/TRETAIN。
- 完成 GEMM、CONV、TRANSPOSER、ADD。
- 完成 A/B/D 转置、reuse/register/flow、cutbit、stride/shift 组合。

完成条件：约六类代表 case 的最终结果和 SRAM beat 序列正确；一个综合 case 关键边界逐拍一致。

### 阶段 8：CPU 整机接入和异频检查

- 用 MikuiSau 替换正常配置下的 StubSau。
- 修正 PipelineCore/PipelineMiniCPU 的同步 endpoint 假设。
- 同频执行完整固件 case。
- 配置 CPU:SAU=1:2，执行一次 CSR→计算→done smoke test。
- 确认 CDC 包装延迟与 SAU 内部周期统计分离。

完成条件：CPU 可发四组 command、等待响应、观察 done，最终共享 SRAM 结果正确；异频无丢请求和重复请求。

### 阶段 9：收尾

- 关闭所有临时 debug 输出。
- 保留可配置 trace 和必要 stats。
- 更新 README/使用说明、RTL 基线和已知限制。
- 运行最小相关测试、`git diff --check` 和工作树范围检查。

## 12. 适量验证方案

### 12.1 原则

验证以“少而能定位”为目标：

- 不做覆盖率收敛。
- 不做大规模随机回归。
- 不对同一行为反复使用大量尺寸。
- 每个关键模块保留一组定向单测。
- 选少量端到端 case 覆盖全部特性组合。
- 只选一个综合代表 case 做完整关键边界逐拍 diff。

### 12.2 模块测试

| 测试 | 最小覆盖内容 |
|---|---|
| CSR/Scheduler | 四组 command、readback、start/ready、非法工作中写、各 scheduler 主分支 |
| Transposer | 16 行装入、普通读、转置读、last、忙时误写、0/1 ping-pong 选择 |
| PE/Array | 正负乘法、24-bit 饱和、cutbit、keep/clear、shift 高低半部、首末 PE 波前 |
| Memory/Feeder | 6-stage address、固定 response、A/B/C 标记、register reuse、16/32 beat 写回 |

### 12.3 端到端代表 case

建议保留约六类，不要求每类多个尺寸：

1. 基础 GEMM，普通输出。
2. A 转置、B 转置和 D/output 转置，可由一个短 command 序列覆盖。
3. Matrix add，包含 bias/加法输出顺序。
4. Reuse + register + RETAIN→CNORMAL 的跨 command 流。
5. 标准卷积，覆盖 cutbit、stride 和 shift。
6. Depthwise/pointwise，覆盖 single-column mask 和 register-file 复用。

每类检查最终内存和 SAU read/write beat；选择第 4 或第 5 类作为完整逐拍综合 case。

### 12.4 验收允许差异

允许：

- C++ enum 数值与 RTL enum 编码不同，只要 trace 归一化后的语义一致。
- CDC 包装的 CPU-visible 延迟与 SAU 内部周期分开报告。
- 未开启的内部 debug/PE trace 不比较。

不允许：

- 关键边界相差“约一拍”而没有确定采样原因。
- start、memory beat、valid/last、first result、last write 或 done 周期不一致。
- 最终结果、地址、数据顺序或 wstrb 不一致。
- 请求丢失后自动重发并假装成功。
- 通过放宽测试期望掩盖 RTL/gem5 差异。

## 13. Mikui RTL 正确启动流程

### 13.1 前置检查

当前 checkout 中 `testcase/` 目录并不存在，运行前必须先确认 case 已生成或已从项目测试集取得：

```bash
cd /home/xch/work/mikui-debug-dma
test -d testcase/<case-group>/<case-name>
```

项目预期的 SAU case group 包括：

- `lkssfull_sau_matmul`
- `lkssfull_sau_fc`
- `lkssfull_sau_stdconv`
- `lkssfull_sau_dconv`
- `lkssfull_sau_pconv`

可研究现有 `make gen_testcase` 生成流程，但执行前要确认 Python 依赖和输出范围，不能为生成 case 擅自安装新依赖。

### 13.2 首次编译并运行单 case

```bash
cd /home/xch/work/mikui-debug-dma
make run_verdi \
  CASE_NAME=mikui_dma \
  REGRESS_NAME=<case-group> \
  TESTCASE=<case-name> \
  TEST_MODE=sau
```

`TEST_MODE=sau` 必须显式给出，默认值是 `veu`。`run_verdi` 会编译、运行并生成 FSDB，但不会打开 Verdi GUI。

### 13.3 RTL 未变化时复用 simv

```bash
cd /home/xch/work/mikui-debug-dma
make run_only \
  CASE_NAME=mikui_dma \
  REGRESS_NAME=<case-group> \
  TESTCASE=<case-name> \
  TEST_MODE=sau
```

输出位于：

```text
sim/vcs/build/mikui_dma/sim.log
sim/vcs/build/mikui_dma/minsys_uart.log
sim/vcs/build/mikui_dma/uvm_lpnpu.fsdb
```

通过判据至少包括：

1. `minsys_uart.log` 最后一个非空行包含 `Set NPU done!` 或 `verify result: 0`。
2. `sim.log` 无 UVM error/fatal 和 SRAM scoreboard mismatch。
3. 需要时检查最终 SRAM 或软件参考结果，不只看 testbench 正常退出。

## 14. FSDB 波形与逐拍对比流程

需要阅读波形时，必须使用：

```text
/home/xch/work/npi_fsdb_probe
```

使用前完整阅读该工程 `README.md`；若命令或构建方式变化，以 README 为准。当前工具基于 Verdi NPI，可把指定信号的 value change 导出为 CSV。

### 14.1 构建工具

```bash
cd /home/xch/work/npi_fsdb_probe/src
make
```

若 Verdi 不在默认路径，按 README 显式传入 `VERDI_HOME` 和 `NPI_PLATFORM`。构建依赖本机 Verdi/NPI，不应擅自安装替代依赖。

### 14.2 Mikui 指定信号导出示例

不要使用工具内置的 Yinglong `--axi-driver` 默认信号。Mikui 应传入完整层级：

```bash
cd /home/xch/work/npi_fsdb_probe/src
./npi_fsdb_probe \
  --begin <fsdb-begin-time> \
  --end <fsdb-end-time> \
  --csv /tmp/mikui_sau_rtl.csv \
  /home/xch/work/mikui-debug-dma/sim/vcs/build/mikui_dma/uvm_lpnpu.fsdb \
  top_mikui_dma_tb.dut_mikui_inst.clk \
  top_mikui_dma_tb.dut_mikui_inst.rst_n \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.start \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.core_state_s \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.input_switch_s \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.sau_sram_enable \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.sau_sram_addr \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.sau_sram_wstrb \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.result_final_valid_o \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.result_last_o \
  top_mikui_dma_tb.dut_mikui_inst.SAU_1_inst.sau_crossbar_done
```

若信号名改变，先用工具限制 scope/signal 打印数量扫描真实层级，再更新清单。工具 CSV 是 value-change 记录，不是天然的逐周期表；对拍脚本应：

1. 找到复位释放后的 SAU clock 第一个上升沿，定义为 cycle 0。
2. 对每个后续上升沿执行 sample-and-hold。
3. 归一化 X/Z、位宽、枚举名称和十六进制格式。
4. 与 gem5 提交后 edge trace 按 cycle 对齐。
5. 报告第一个差异以及前后少量周期，不输出巨大无关 diff。

## 15. gem5 验证建议

优先运行目标 GTest 或增量构建，不默认完整重编译。实际命令应以项目 SConscript 和当前构建目录为准。建议顺序：

1. 新增模块对应的单个 GTest。
2. SAU 相关 GTest 集合。
3. 受影响的 pipeline/memory 既有测试。
4. 一个 gem5 端到端固件 case。
5. `git diff --check`。

若使用常见 RISCV gem5 构建，建议开发者确认后手动执行类似：

```bash
scons build/RISCV/gem5.opt -j32 --ignore-style --limit-ld-memory-usage
```

不要为了测试方便删除失败测试、降低断言、吞掉 timing error 或把 strict mode 默认关闭。

## 16. 关键风险与处理办法

### 16.1 RTL 文档过时

风险：8×8、状态名、模式编码和 delay 描述与当前源码冲突。

处理：每个模块实现注释引用具体 RTL 文件和基线提交；规格文档只作辅助。

### 16.2 RTL 本身存在重叠位域或可疑注释

风险：Command 1 bit 3 同时参与 kernel/reuse；部分注释与枚举相反。

处理：用实际固件 command 和 FSDB 校准；模型复现已执行行为。若确认是 RTL bug，先记录，再决定是否同步修改 RTL 和模型。

### 16.3 C++ 调用顺序造成少/多一拍

风险：顺序调用内部模块产生零拍穿透。

处理：强制 current/evaluate/next/commit 两阶段结构；首个综合 trace 从 start 到 done 检查所有关键边界。

### 16.4 定宽有符号运算错误

风险：C++ integer promotion、负数移位和溢出与 SystemVerilog 不同。

处理：集中实现 sign-extend、mask、saturate、arithmetic-shift 工具，并用边界值单测。

### 16.5 独立时钟与现有同步 Endpoint 冲突

风险：PipelineCore 当前每 CPU edge 直接 clock SAU，无法表达异频。

处理：通过 SimObject mailbox/adapter 解耦；同频先严格对拍，异频只在包装层增加延迟。

### 16.6 旧 gem5 crossbar 破坏 SAU 时序

风险：两 bank、旧状态机和旧响应延迟导致错误地址或系统性错拍。

处理：在完整 SAU 接入前先完成三 bank/current RTL 对齐，并运行原有 CPU/VEU memory 小回归。

### 16.7 trace 体积失控

风险：256 PE 每拍输出会生成巨大日志。

处理：默认只记录模块边界；PE trace 按坐标和时间窗显式开启；FSDB 使用 `--begin/--end/--max-changes`。

## 17. 最终验收清单

功能验收：

- [ ] GEMM 正确。
- [ ] 标准卷积、depthwise 和 pointwise 正确。
- [ ] 独立转置和 matrix add 正确。
- [ ] ABD、ATBD、ABTD、ABDT 正确。
- [ ] reuse/register/flow mode 正确。
- [ ] CNORMAL、CTRANS、RETAIN、TRETAIN 正确。
- [ ] cutbit、stride、shift 正确。
- [ ] 8-bit 和 shift/16-bit 输出饱和、排列和写回正确。

周期验收：

- [ ] CSR request/ready/start 周期一致。
- [ ] Scheduler 状态、input switch 和 flow_end 周期一致。
- [ ] 每个 SRAM beat 的周期、地址和 wstrb 一致。
- [ ] Feeder A/B/C valid/last 周期一致。
- [ ] Transposer ready/valid/last 和资源选择一致。
- [ ] PE 波前首达、完成和逐行输出周期一致。
- [ ] Result valid/last、last write 和 crossbar done 周期一致。
- [ ] 综合代表 case 不存在未解释的 ±1 拍差异。

集成验收：

- [ ] 正常配置使用 MikuiSau 而非 StubSau。
- [ ] StubSau 仍可供原有轻量 CPU 测试选择。
- [ ] 三个 SRAM bank 与当前 RTL 分区一致。
- [ ] CPU、VEU 和 SAU 共享 memory visibility 正确。
- [ ] 同频端到端 case 通过。
- [ ] CPU:SAU=1:2 smoke test 无丢失、重复或死锁。
- [ ] Trace 默认关闭，stats 正常注册。
- [ ] 没有未确认依赖、无关重构或隐藏失败。

## 18. 完成定义

只有同时满足以下条件，才能称为“功能完善的 Mikui SAU gem5 模型”：

1. 本文列出的全部 SAU 功能模式已实现，而非以固定延迟或预计算结果占位。
2. 16×16 PE、feeder、register/shift、transposer 和 output path 均有真实逐拍状态。
3. 代表性整机 case 的最终内存结果正确。
4. 综合代表 case 的关键模块边界逐拍对齐 RTL。
5. 现有 CPU/memory/crossbar 中阻碍当前 RTL 对齐的差异已做最小修正。
6. 同频主路径稳定，独立时钟 1:2 smoke test 通过。
7. 所有修改有 RTL 依据、最小相关验证和清晰交付说明。

