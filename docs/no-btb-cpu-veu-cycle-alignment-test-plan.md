# 无 BTB CPU 联合 VEU 完整逐拍对齐测试方案

## 1. 测试目标

在同一份单 VEU 操作固件上，对比以下两个系统：

- RTL 基准：`npu_lpnpu-origin` 中的 Spirit CPU + VEU + `dut_kui` 存储通路，通过 `set_btb_off=1` 关闭 BTB，使用 VCS 仿真。
- 待测模型：`gem5_cpu_veu` 中无分支预测器的 `PipelineMiniCPU` + `TimingVEU` + `rtl-dut-kui-tb` 存储模型。

主要验收目标是：从 CPU 复位释放后的第一个有效拍开始，直到固件写入 `NPU_DONE` 为止，两边的 CPU 退休、总线、停顿、重定向、CPU–VEU 握手和 VEU 内部关键事件在同一拍发生。

本方案区分两类结论：

- `CALIBRATED_MATCH`：使用由同类 RTL 用例生成的 timing profile 后对齐，证明集成和标定复现正确。
- `PREDICTIVE_MATCH`：待测用例未参与 profile 生成，仍能逐拍对齐，证明模型对未标定用例具有预测能力。

## 2. 对齐口径

### 2.1 时钟和“一拍”

- 唯一计数时钟为 RTL `clk_acc` 上升沿。CPU、VEU 和共享 SRAM 数据通路均使用该时钟。
- RTL testbench 的 `clk_acc` 周期为约 `1.666 ns`（约 600 MHz）。gem5 运行时设置 `--clock-frequency 600MHz`。
- 验收使用“边沿数”，不直接比较 ps/ns 时间，避免 timescale 和取整引入误差。

### 2.2 起止点

- `edge=1`：`rst_n_cpu==1` 后遇到的第一个 `clk_acc` 上升沿。RTL 和 gem5 均在此处开始 `cpu_cycle=1`。
- 终点：CPU DBUS 成功接受向 `0x4001_E004` 写入 `0x2` 的那一拍，记录 `done=1, done_value=0x2`。
- AHB 侧启动 CPU 前的复位、存储器初始化和 CDC 延迟不纳入 CPU 主对比区间，但作为单独的环境信息保存。

### 2.3 采样语义

- RTL 采用 `posedge-pre-NBA`：在 `always @(posedge clk_acc)` 的 active region 记录该边沿到来时可见的旧状态，NBA 更新在下一条 trace 可见。
- gem5 采用已定义的 `evaluate-before-clock`：先 `evaluateOneCycle()` 和记录，再 `clockOneCycle()`。
- 两边 trace header 必须声明 schema、source、sampling 和平台，对比器必须校验而不是忽略。

## 3. 测试前置门禁

任一项不满足时不得开始对齐测试：

1. 恢复可执行的 `build/RISCV/gem5.opt`，并记录其 SHA256。
2. 使用 Python 3.9 或更高版本；或修复 Python 3.8 下 `test_compare_cycle_traces.py` 的类型注解导入问题。
3. 修复 gem5 仓库损坏的 Git index，保证能记录确切 commit 和 dirty state。
4. VCS 可正常编译/运行，并记录 RTL commit、filelist、define 和编译日志。
5. 两边使用完全相同的逻辑指令镜像和数据镜像，在运行前比较 SHA256。
6. trace schema 为必填字段强校验；不允许两边同时缺少某字段时被当作相等。
7. RTL 必须有断言证明活动期内 `set_btb_off==1` 且 `btb2if_btb_match==0`。
8. gem5 必须输出 `predictor_present=0` 或等价元数据，并在 trace 中保持 `btb_match=0`。

## 4. 统一单操作固件

主用例使用最小汇编或已固化机器码，不使用不必要的 C 运行库，只执行一次非标量 VADD：

1. 加载 `src1`、`src2`、`dest` 地址。
2. 配置 `VEUCFG`：I8、vector-vector、write-back、clear output buffer。
3. 配置 `VEUWADDR`、`VEUVLEN=256`、`VEUMASK=0xffff_ffff`。
4. 发射且只发射一条 VADD。
5. 通过 `VGETCSR VEUSTATUS` + `ANDI` + `BNE` 轮询 busy，该分支环同时验证无 BTB 分支时序。
6. CPU 逐字读回结果，将不匹配累计到 `x10`。
7. `x10==0` 时向 `0x4001_E004` 写 `0x2`；否则写 `0x4`。
8. 写入完成后进入 `EBREAK` 或固定自环，防止执行未初始化指令。

固件必须产生并归档：

- `firmware.elf` 和完整 disassembly；
- RTL 使用的 `instruction.hex` / `memory.hex`；
- gem5 使用的 `instr_mem.hex` / `data_mem.hex`；
- 镜像转换程序和逻辑字节镜像 SHA256 清单。

## 5. RTL 侧改造与采集

### 5.1 通过 plusarg 关闭 BTB

不应把 testbench 的启动值永久写死为单一模式。增加 `+BTB_OFF=0/1`：

- `BTB_OFF=0`：控制寄存器写 `0x19`；
- `BTB_OFF=1`：控制寄存器写 `0x59`，即在原启动值上置 bit[6]。

测试时必须读回控制寄存器或直接监视 `ctrl_reg[6]`，避免“传入了 plusarg 但 RTL 实际未关闭”。

### 5.2 CPU canonical trace

在每个 `clk_acc` 上升沿记录一行，以下字段为必填：

- 基础：`edge reset cpu_cycle phase source platform`；
- IBus：`ibus_req ibus_addr ibus_re ibus_resp ibus_r0..ibus_r3`；
- DBus：`dbus_req dbus_addr dbus_re dbus_we dbus_wstrb dbus_wdata dbus_resp dbus_rdata`；
- CPU 管线：`retire retire_pc retire_instr wb_we wb_fp wb_rd wb_data stall_mask redirect redirect_target grant`；
- 无 BTB 证据：`set_btb_off btb_match predict_failed`；
- CPU–VEU：`hc_req hc_addr hc_re hc_we hc_write_type hc_vestart hc_valid hc_rdata`；
- 结束：`done done_value error error_value`。

RTL trace 文件头示例：

```text
# brs-cycle-trace-v3 source=rtl sampling=posedge-pre-nba platform=top_veu_regress_tb
```

### 5.3 VEU event trace

另外生成事件 CSV，至少包括：

- `operation_start`、`status_set`、`lock_start`、`status_clear`、`lock_finish`、`operation_finish`；
- `read_issue`、`read_response`、`fifo_push`；
- `vfu_accept`、`vfu_complete`；
- `write`、`bank_write`；
- `cycle`、`chunk`、`source`、`address`、`transaction_id`、FIFO 深度和 outstanding read 数。

所有 VEU 事件的 cycle 必须使用与 CPU canonical trace 相同的 `clk_acc edge`编号，不另起计数器。

### 5.4 RTL 断言

至少增加以下断言：

- CPU 活动期 `set_btb_off` 不得降低；
- `set_btb_off |-> !btb2if_btb_match`；
- VEU `operation_start` 仅发生一次；
- 测试结束前必须观测到一次 `operation_finish`；
- 不允许未知的 CSR 握手、SRAM 请求、retire PC 或指令。

## 6. gem5 侧改造与采集

### 6.1 锁定无 BTB 配置

- 禁用 gem5 通用 CPU 的分支预测器和 I-cache；使用 `PipelineMiniCPU` 和 `--no-icache`。
- 保留 ID 阶段分支解析、redirect 和 frontend flush 时序。
- 在配置日志与 trace header 显式写出 `predictor_present=0 btb_enabled=0`。

### 6.2 补齐 canonical trace schema

当前 gem5 writer 已输出多数 CPU/总线/退休字段，但必须补齐比较器要求的缺失字段，特别是：

- `ibus_re`、`ibus_r0..ibus_r3`；
- `dbus_re`、`dbus_we`；
- `grant`；
- `redirect`、`redirect_target`；
- `done`、`done_value`、`error`、`error_value`；
- `set_btb_off`、`btb_match`、`predict_failed`。

对比器应对 v3 schema 列表执行强制存在性校验；任一必填字段缺失都返回环境错误，不进入 PASS/FAIL 对比。

### 6.3 禁止 profile 误用

每次运行必须记录：

- `profile_id`、`timing_source`、`evidence_id`；
- `operation_cycles`、`lock_start_delay`、`finish_drain_cycles`；
- 当前用例是否参与了该 profile 行的生成。

若当前 RTL 用例就是 `evidence_id` 来源，最高只能判定为 `CALIBRATED_MATCH`，不得标记为独立预测成功。

## 7. 分层测试用例

### T0：环境与 schema 自检

- 验证工具版本、commit、固件 hash、profile hash。
- 用人工构造的一处 retire 差异、一处缺失字段和一处 VEU 事件差异证明比较器会报 FAIL/ERROR。
- 验收：不允许“故意注入差异却 PASS”。

### T1：无 BTB CPU 基线

固件只包含 ALU、load/store、taken/not-taken branch 和固定次数的循环，不启动 VEU。

- 目标：先证明 CPU、IBus、DBus、无 BTB 分支处罚和复位起点可逐拍对齐。
- 验收：CPU canonical trace 零差异。

### T2：单 VADD，VLEN=256（主用例）

- 只启动一次 VEU，1 个 256-bit chunk，full mask。
- 首先使用冻结的 RTL profile 完成 `CALIBRATED_MATCH`。
- 再移除本用例的 evidence 行或使用事先冻结的 hold-out profile，尝试 `PREDICTIVE_MATCH`。
- 验收：CPU canonical trace、VEU event trace、里程碑表和最终数据全部零差异。

### T3：单 VADD，VLEN=2048

- 8 个 chunk，用于覆盖多次读、FIFO、outstanding read、VFU pipeline 和多次写回。
- 验收标准与 T2 相同。

### T4：关键变体

在 T0–T3 通过后扩展：

- scalar VADD；
- partial mask 和 zero mask；
- VMIN/VMAX（两拍 VFU）；
- VMUL（多拍 VFU）；
- VREDSUM（final-only write）；
- VSLIDEDOWN（非均匀 completion pattern）。

## 8. 逐拍比较流程

### 8.1 预检

1. 校验两边指令/数据镜像 hash。
2. 校验两边 trace schema 和采样语义。
3. 校验 RTL `set_btb_off=1`和 gem5 `predictor_present=0`。
4. 校验两边均只出现一次 `operation_start`。

### 8.2 CPU canonical trace 比较

使用增强后的 `util/brs/compare_cycle_traces.py`，按以下顺序报告：

1. 第一个退休序列差异；
2. 第一个退休拍差异；
3. 第一个写回差异；
4. 第一个 IBus/DBus 差异；
5. 第一个 stall/redirect/HC 差异；
6. 差异前后各 12 拍窗口。

### 8.3 VEU event 比较

使用 `verify_dut_kui_rtl_case.py` 的严格签名思路，但将事件 cycle 映射到全局 CPU edge，严格比较：

- 事件类型和次数；
- 全局 edge；
- chunk/source/transaction ID；
- 地址和地址递增；
- FIFO/outstanding read；
- status/lock 边界。

### 8.4 里程碑比较

产生 `milestones.csv`：

```text
milestone,rtl_edge,gem5_edge,delta
cpu_active,...,...,...
first_ibus_req,...,...,...
first_ibus_resp,...,...,...
first_retire,...,...,...
veu_instruction_retire,...,...,...
operation_start,...,...,...
first_veu_read,...,...,...
first_vfu_accept,...,...,...
first_vfu_complete,...,...,...
first_veu_write,...,...,...
status_clear,...,...,...
operation_finish,...,...,...
poll_loop_exit,...,...,...
npu_done,...,...,...
```

每个 `delta` 都必须为 0。

## 9. 运行流程

### 9.1 RTL

```bash
cd /home/zpq/下载/npu_lpnpu-origin
make -f sim/vcs/script/case_veu_regress/Makefile compile
make -f sim/vcs/script/case_veu_regress/Makefile sim \
  FIRMWARE_DIR=<shared_case_dir> \
  SIM_EXTRA_ARGS="+BTB_OFF=1 +CYCLE_TRACE=<out>/rtl_cycle_trace.log +VEU_EVENT_TRACE=<out>/rtl_veu_events.csv"
```

Makefile 需增加 `SIM_EXTRA_ARGS` 透传，testbench 需实现上述 plusarg 和 trace writer。

### 9.2 gem5

```bash
cd /home/zpq/下载/gem5_cpu_veu
build/RISCV/gem5.opt -d <out>/gem5 \
  configs/brs/run_pipeline_mini.py \
  --mem-system rtl-dut-kui-tb \
  --entry-point 0x29110008 \
  --veu-model timing \
  --veu-timing-profile <frozen_profile.csv> \
  --program-file <shared_case_dir>/instr_mem.hex \
  --dmem-hex <shared_case_dir>/data_mem.hex \
  --no-icache \
  --clock-frequency 600MHz \
  --cycle-trace gem5_cycle_trace.log \
  --veu-cycle-trace gem5_veu_events.csv \
  --max-cycles 10000
```

### 9.3 对比

```bash
python3 util/brs/compare_cycle_traces.py \
  <out>/rtl_cycle_trace.log \
  <out>/gem5/gem5_cycle_trace.log \
  --window 12 \
  --json-report <out>/cpu_cycle_compare.json
```

最终应由一个 runner 顺序执行 schema 检查、CPU trace 比较、VEU event 比较、功能结果比较和里程碑汇总，任一子项失败都返回非零状态。

## 10. PASS/FAIL 标准

### PASS 的必要条件

- RTL 全程 `set_btb_off=1`，BTB 命中数为 0；
- gem5 不存在分支预测器；
- 固件和初始数据 hash 一致；
- 两边都恰好启动和完成一次 VEU 操作；
- CPU retire 序列、retire edge 和写回逐项相同；
- 所有有效 IBus/DBus/HC 请求与响应逐拍相同；
- stall、redirect 和 done edge 相同；
- VEU 可比事件的 edge、chunk、source、地址和次数相同；
- 目的 SRAM 结果和 CPU 读回结果完全相同；
- trace 中无 X/Z，无缺失必填字段，无 profile fallback；
- 同一用例独立运行 3 次，trace hash 一致。

对拍级验收不设容差，任一有意义字段的首次差异即判 FAIL。

### 不得计为 PASS 的情况

- 只比较最终数据正确；
- 只比较 `operation_start` 到 `operation_finish` 的总拍数；
- 比较字段缺失或为 `N/A`；
- 当前用例的 RTL 总拍数被直接填入 profile，然后仅验证完成 edge；
- CPU retire 拍未比较；
- RTL 实际未置 `set_btb_off`。

## 11. 失败定位顺序

1. `edge=1` 即不同：检查复位释放和采样语义。
2. 首个 IBus 差异：检查 IBU 起动、取指地址和返回延迟。
3. 首个 retire 拍差异：检查管线级数、stall mask 和 forwarding。
4. 在轮询 `BNE` 处差异：检查无 BTB 分支解析、flush 和重取延迟。
5. `operation_start` 前差异：检查 VEU CSR/HC 握手与 CPU stall。
6. 首个 VEU read 差异：检查 startup 和交叉开关仲裁。
7. VFU 差异：检查 FIFO 边界、latency 和 initiation interval。
8. write/status/finish 差异：检查 VSU、bank write、status clear 和 drain 时序。
9. VEU 完成后 CPU 差异：检查 status 读响应可见拍和轮询退出。

RTL 侧若需波形定位，使用 `npi_fsdb_probe` 查看 FSDB，结论必须落到具体层次信号、edge/时间点和 RTL 源码条件。

## 12. 交付物

每个用例使用独立目录：

```text
cycle_alignment_results/<case>/
  manifest.json
  firmware.elf
  firmware.disasm
  image_sha256.txt
  rtl_compile.log
  rtl_sim.log
  rtl_cycle_trace.log
  rtl_veu_events.csv
  gem5_run.log
  gem5_cycle_trace.log
  gem5_veu_events.csv
  cpu_cycle_compare.json
  veu_event_compare.json
  milestones.csv
  result_memory.sha256
  summary.md
```

`manifest.json` 必须包含 RTL/gem5 commit、dirty state、工具版本、固件/profile hash、时钟、复位、BTB 配置、用例参数和判定类型（`CALIBRATED_MATCH` 或 `PREDICTIVE_MATCH`）。

## 13. 建议实施顺序

1. 修复工具链、Git index 和缺失的测试证据。
2. 定义 `brs-cycle-trace-v3` 必填 schema，增加 schema 负向测试。
3. 实现 RTL `+BTB_OFF=1`、CPU trace writer、VEU event writer 和断言。
4. 补齐 gem5 canonical trace 字段。
5. 运行 T1，先关闭 CPU 对齐问题。
6. 运行 T2 calibrated 模式，完成首个 CPU+VEU 逐拍 PASS。
7. 运行 T2 hold-out 模式，评估预测能力。
8. 扩展至 T3 和 T4，建立回归矩阵。
