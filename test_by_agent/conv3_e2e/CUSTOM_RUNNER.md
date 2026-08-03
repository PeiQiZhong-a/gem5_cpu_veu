# 自定义 sau_n Conv 配置使用说明

脚本路径：`test_by_agent/conv3_e2e/run_custom_conv.py`

它用于生成并可选运行一组独立的 `sau_n` full-offload Conv2d case。每个 case
包含一段 RV32I 启动程序、四条 `msetins` 配置指令、tensor 数据以及独立
verifier 所需的 manifest，不会覆盖 `fixtures/conv3_step1/` 中的已验收 fixture。
input、weight、bias 可以分别选择由 guest CPU 写入或由 `memory.hex` 预加载。
默认启动模型是 `toolchain-approx`，会额外近似工具链中的 first-fit allocator 初始化、
四次 `malloc` 和前后 `mgetins4` 查询；如果只想复现旧的简化启动路径，可显式使用
`--startup-model minimal`。

## 1. 最简单的用法

在工程根目录执行：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py
```

默认配置为 `N=1, C=16, H=16, W=32, OC=16, kernel=3, padding=1,
stride=1, cutbit=12`，会生成 fixture、启动当前 `gem5 + sau_n`，最后调用
独立 verifier。

只生成文件、不启动 gem5：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --name my_conv \
  --n 1 --channels 8 --height 8 --width 8 --out-channels 8 \
  --padding 1 --stride 1 --cutbit 10 \
  --no-run
```

查看完整命令行帮助：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py --help
```

数字参数的规则如下：形状、模式 seed、cycle 参数使用十进制；地址和数据窗口
参数支持十进制或 `0x` 开头的十六进制，例如 `--input-base 0x29130000`。

## 2. 参数总览

### 2.1 输出目录和工具路径

| 参数 | 默认值 | 说明 |
|---|---|---|
| `--name` | `custom_conv` | case 名称，同时作为输出目录名和 manifest 中的名称。建议使用不含 `/` 的简单名字。 |
| `--output-root` | `<工程>/test_by_agent/conv3_e2e/custom_runs` | 所有自定义 case 的根目录；实际输出目录为 `<output-root>/<name>/`。 |
| `--gem5` | `<工程>/build/RISCV/gem5.opt` | 要启动的 gem5 可执行文件，必须存在且可执行。 |
| `--config` | `<工程>/configs/brs/run_pipeline_mini.py` | gem5 配置脚本，不是 Conv 参数配置文件。脚本会在它上面追加本说明中列出的运行时选项。 |
| `--llvm-mc` | `/usr/bin/llvm-mc` | 将生成的 RV32 assembly 汇编为目标文件的工具。 |
| `--llvm-objcopy` | `/usr/bin/llvm-objcopy` | 从目标文件提取 `.text`，生成 `instruction.hex` 的工具。 |
| `--help` | 无 | 显示 argparse 帮助并退出。 |

### 2.2 Conv 形状和计算参数

| 参数 | 默认值 | 合法范围 | 说明 |
|---|---:|---|---|
| `--n` | `1` | `1..65535` | batch 大小 `N`。 |
| `--channels` | `16` | `1..63` | 输入通道数 `C`。 |
| `--height` | `16` | `1..65535` | 输入高度 `H`。 |
| `--width` | `32` | `1..65535` | 输入宽度 `W`。当 `W<=16` 时，SAU 会采用一字中打包多个空间元素的布局。 |
| `--out-channels` | `16` | `1..16` | 输出通道数 `OC`。 |
| `--padding` | `1` | `0` 或 `1` | 高、宽两侧使用的对称 padding。当前 ABI 只支持 0/1。 |
| `--stride` | `1` | `1` 或 `2` | 卷积步长。 |
| `--cutbit` | `12` | `0..23` | 累加结果量化时使用的 cutbit；数值越大，输出缩放越小。 |

当前 runner 固定以下 Conv 属性，不能通过命令行修改：

- kernel 为 `3x3`；
- dilation 为 `1x1`；
- groups 为 `1`；
- 输入和权重为 signed INT8，bias 为 signed INT16，输出为 signed INT8；
- 数据布局为输入/输出 NCHW，权重布局为 `[C][3][3][OC]`；
- `msetins4` 会设置 `start=1`，因此 case 是一次完整的四指令 full-offload 流程。

输出空间尺寸为：

```text
out_h = floor((H + 2 * padding - 3) / stride) + 1
out_w = floor((W + 2 * padding - 3) / stride) + 1
```

对应的数据量为：

```text
input  = N * C  * H    * W     bytes
weight = C * 3  * 3    * OC    bytes
bias   = OC * 2                 bytes
output = N * OC * out_h * out_w bytes
```

### 2.3 Tensor 地址

以下地址都是 32-bit **字节地址**，不是 word index：

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--input-base` | `0x29130000` | 输入 A 的最终首地址。默认由 CPU 启动程序写入。 |
| `--weight-base` | `0x29132000` | 权重 B 的最终首地址。默认由 CPU 启动程序从 staging 区复制。 |
| `--bias-base` | `0x29132900` | bias C 的最终首地址。默认由 CPU 启动程序从 staging 区复制；每个元素占 2 bytes，必须 2-byte 对齐。 |
| `--output-base` | `0x29132920` | 输出 D 的首地址，SAU 写回后由 verifier 检查。 |

runner 会自动计算每个 tensor 的结束地址，并检查：

1. 四个 tensor 都位于 real SRAM 范围内；
2. input、weight、bias、output 互不重叠；
3. 地址范围不发生 32-bit 溢出；
4. `bias-base` 为偶数地址。

例如，当 `H` 从默认的 16 改成 32 时，输入和权重所需空间都会变化，不能机械
沿用默认地址。可以使用下面这组不重叠的地址：

```sh
--input-base  0x29130000 \
--weight-base 0x29134000 \
--bias-base   0x29134900 \
--output-base 0x29134920
```

### 2.4 RTL data window 和 bank 参数

| 参数 | 默认值 | 说明 |
|---|---:|---|
| `--data-base` | `0x29120000` | gem5 RTL DUT data window 的首地址。 |
| `--data-size` | `0x40000` | data window 总大小；生成的 `memory.hex` 也按此大小生成。 |
| `--data-bank-size` | `0x10000` | 每个 bank 的字节容量。 |
| `--data-bank-count` | `4` | 解码出来的 bank 数量，当前 runner 允许 `1..4`。 |
| `--data-real-bank-count` | `3` | 实际存在且可访问的 bank 数量，必须不大于 `data-bank-count`。 |

必须满足：

```text
data-size = data-bank-size * data-bank-count
```

tensor 必须落在 real SRAM，而不是仅落在声明的 data window 中：

```text
real_sram = [data-base, data-base + data-bank-size * data-real-bank-count)
```

使用默认值时，data window 是 `0x29120000..0x29160000`，但 tensor 实际可用的
real SRAM 是 `0x29120000..0x29150000`，即 `0x30000` bytes。增大或减少
`data-real-bank-count` 时，需要同步重新安排四个 tensor 地址。

### 2.5 权重、bias 和运行控制

| 参数 | 默认值 | 合法值 | 说明 |
|---|---|---|---|
| `--weight-pattern` | `ramp` | `zero`, `ones`, `ramp`, `random` | 权重 B 的生成模式。 |
| `--bias-pattern` | `zero` | `zero`, `ones`, `ramp`, `random` | bias 的生成模式。 |
| `--seed` | `1` | 整数 | `random` 模式的固定 seed；相同配置和 seed 会得到相同数据。 |
| `--input-init` | `cpu` | `cpu` 或 `memory` | 输入 A 的初始化位置：由 guest CPU 写入，或由 `memory.hex` 预加载。 |
| `--weight-init` | `cpu` | `cpu` 或 `memory` | 权重 B 的初始化位置：由 guest CPU 从 staging 区复制，或由 `memory.hex` 直接预加载到最终地址。 |
| `--bias-init` | `cpu` | `cpu` 或 `memory` | bias C 的初始化位置：由 guest CPU 从 staging 区复制，或由 `memory.hex` 直接预加载到最终地址。 |
| `--tensor-init` | 未设置 | `cpu` 或 `memory` | 同时设置 input、weight、bias 三种初始化方式；会覆盖三个单独的 `*-init` 参数。 |
| `--startup-model` | `toolchain-approx` | `toolchain-approx` 或 `minimal` | 启动语义。近似模式加入 allocator、四次 `malloc`、工具链式 source load 和前后 `mgetins4`；`minimal` 保留旧的手写启动路径。 |
| `--max-cycles` | `2000000` | 整数 | 传给 gem5 的最大运行周期数；超限通常表示 case 没有在预期时间内完成。 |
| `--reset-cycles` | `10` | 整数 | 传给 gem5 的 reset 周期数。 |
| `--clock-frequency` | `100MHz` | gem5 可接受的频率字符串 | 传给 gem5 的时钟频率。 |
| `--no-run` | 未设置 | 开关 | 只生成 assembly/镜像/manifest，不启动 gem5，也不运行 verifier。 |

模式的具体值如下：

- `zero`：全部为 0；
- `ones`：全部为 1；
- `ramp`：按元素索引重复 `-3,-2,-1,0,1,2,3`；
- `random`：权重使用 `[-8,8]`，bias 使用 `[-32,32]` 的确定性随机值。

输入没有 `--input-pattern` 参数，数据内容始终是：

```text
input[i] = i // 16
```

默认配置下，三个 tensor 都由 guest CPU 初始化：

```text
input  A: CPU 从 staging 区 load 后按 byte 写入 input[i] = i / 16
weight B: CPU 从 memory.hex 的 staging 区按 byte 复制到 weight-base
bias   C: CPU 从 memory.hex 的 staging 区按 halfword 复制到 bias-base
```

这些 CPU load/store、循环和访存延迟都会计入 `cycle_count`。staging 区本身由
host 在启动前写入 `memory.hex`，这部分不计入 CPU 周期；计入的是 guest CPU 将
它们复制到最终 tensor 地址的过程。

### 2.6 启动模型的计时语义

默认的 `--startup-model toolchain-approx` 对齐工具链 `7_sau` 测试的相关顺序，
但不伪装成工具链真实编译出的固件。它会在四条 CSR 之前执行：

1. 近似 `malloc_initial()`/`init_heap()` 的 heap bounds、对齐、哨兵和首个空闲块初始化；
2. 对 input、weight、bias、output 各执行一次 shadow `ff_malloc`，包含 16-byte
   对齐、header 单位换算、空闲链表遍历、canary/range 校验和 split/exact-fit；
3. 对选择为 `cpu` 的 tensor 从 staging 地址 load，再 store 到最终地址；
4. 在 CSR 提交前后各执行一次 `mgetins4lsb` idle poll。

shadow allocator 只用于产生 guest CPU 的指令、访存和控制流开销，不返回真实的
动态 heap 地址。它使用 256-byte metadata 区保存哨兵、空闲块、状态和四个 allocation
record；virtual heap 的 header 容量按本次四个申请的 `ff_malloc` header 单位计算。
manifest 的 `startup.allocator` 会记录 footprint、virtual header capacity、canary、
调用列表和实现的 allocator 操作。
当前 gem5 的 `msetins4` 是阻塞到 sau_n 完成为止，因此近似模式把
`msetins4` 等待记录为 `gem5 blocking completion`，不会额外制造一段不存在的重复
busy-poll 周期。

`--startup-model minimal` 不执行上述 allocator 和 `mgetins4`，input CPU 初始化
仍使用旧的直接计算 `i // 16` 循环，适合和旧结果作基准对照。两种模式都不包含
卷积完成后的结果比较、`free` 或调试打印；这部分不是当前启动阶段模型。

如果要让三个 tensor 都由 host 预加载，使用：

```sh
--tensor-init memory
```

在默认 `toolchain-approx` 下，此时程序不执行 input/weight/bias 数据初始化循环，
但仍保留 allocator 初始化、四次 shadow `malloc`、前后 `mgetins4`、CSR 配置、SAU
等待和结束路径。也可以使用 `--input-init`、`--weight-init`、`--bias-init`
分别选择混合模式。两种模式的 tensor 内容和最终 Conv 输出应相同。

当前脚本也没有从外部 input/weight/bias 二进制文件导入数据的选项；如果要改变数据
分布，使用上述 pattern 参数，或后续扩展 runner。

需要注意，程序把四条配置指令放在固定的 `0x4000` 区域。启动和所选 tensor
初始化代码结束后到 `0x4000` 之间的零填充指令区仍会被 CPU 顺序取指，这部分
是程序布局开销，不是软件 padding。比较不同初始化模式时，这段开销保持一致。

## 3. 四条 msetins 的内容

脚本会把命令行参数编码成以下四个 payload，并在 assembly 中执行对应指令：

| 指令 | 内容 |
|---|---|
| `msetins1` | `input-base` 和 `weight-base` |
| `msetins2` | `bias-base` 和 `output-base` |
| `msetins3` | `H/W/N/C/OC` |
| `msetins4` | ABI version、padding、stride、cutbit、固定 kernel=3、`start=1` 和 magic `0xC3` |

脚本输出中的 payload 可用于确认配置是否已经按预期编码。

## 4. 资源限制和自动检查

除了参数范围和地址检查，runner 还会检查默认 `sau_n` shared scratchpad 是否够用。
计算方式为：

```text
rows_per_word = 16 // W                         (W <= 16)
rows_per_word = 1                               (W > 16)

spatial_words_per_channel = ceil(H / rows_per_word)       (W <= 16)
spatial_words_per_channel = H * ceil(W / 16)              (W > 16)

A_rows = N * C * spatial_words_per_channel
B_rows = C * 3 * 3
C_rows = 2
D_rows = N * out_h * out_w
total  = A_rows + B_rows + C_rows + D_rows <= 4096
```

因此增大 `N/C/H/W/OC` 后，如果出现 `default sau_n shared scratchpad needs ... rows;
maximum is 4096`，需要减小形状，或先确认当前 SAU 实现是否支持更大的
scratchpad；本 runner 不会自动截断配置。

## 5. 输出文件

例如使用 `--output-root /tmp/my-runs --name case_a` 后，输出结构为：

```text
/tmp/my-runs/case_a/
├── program.S       # 生成的 RV32I 启动程序和四条 msetins
├── instruction.hex # 由 program.S 生成的指令镜像
├── memory.hex      # 预加载 tensor/staging 数据和其余 data image
├── manifest.json   # 形状、地址、模式、startup、payload、镜像 hash
└── run/             # 未使用 `--no-run` 时生成
    ├── stdout.log
    ├── stderr.log
    ├── cycle_trace.log
    ├── stats.txt
    └── report.json  # verifier 通过后生成
```

`--no-run` 适合先检查地址、镜像和 payload；完整运行成功时，终端会打印
`PASS: report written to ...`，`report.json` 中包含 verifier 的输出摘要。

## 6. 推荐工作流程

先生成一个小 case：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --output-root /tmp/sau-n-custom \
  --name smoke_8x8 \
  --n 1 --channels 8 --height 8 --width 8 --out-channels 8 \
  --padding 1 --stride 1 --cutbit 10 \
  --weight-pattern ramp --bias-pattern ramp --seed 7 --tensor-init cpu \
  --no-run
```

确认生成成功后，去掉 `--no-run`，或者直接使用同一条命令重新运行。需要替换
配置时，通常只修改 Conv 参数、四个 tensor 地址和 pattern 参数；data window
参数保持默认即可。默认已经启用工具链风格近似启动；若要比较是否包含完整的 CPU
tensor 初始化，分别使用
`--tensor-init cpu` 和 `--tensor-init memory`，并使用不同的 `--name` 保存两个
结果。例如：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --name all_cpu_init --tensor-init cpu

python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --name all_memory_init --tensor-init memory
```

和旧简化启动路径比较：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --name minimal_cpu --tensor-init cpu --startup-model minimal
```

## 7. 常见错误

- `... is outside real SRAM`：tensor 地址或 tensor 大小超出
  `[data-base, data-base + data-bank-size * data-real-bank-count)`。
- `input and weight ranges overlap`（或其他 tensor 名称）：调整四个 `*-base`
  地址，给每个区域预留完整空间。
- `data-size must equal data-bank-size * data-bank-count`：三个 data window/bank
  参数不一致。
- `default sau_n shared scratchpad needs ... maximum is 4096`：形状需要的 SAU
  scratchpad 行数过多。
- `sau_n requires output_w <= width when width <= 16`：宽度较小时当前硬件布局
  不接受该输出宽度组合，请调整 `width/padding/stride`。
- `gem5 executable is missing or not executable`：检查 `--gem5` 或先完成工程编译。
- `no free SRAM region is available for CPU tensor initialization sources`：
  `toolchain-approx` 需要额外的 allocator 区和 staging 区；调整 tensor 地址、
  `data-real-bank-count` 或改用 `--startup-model minimal`。
- 运行到 `max-cycles` 仍未结束：先保留 `run/` 下的 `stdout.log`、`stderr.log`、
  `cycle_trace.log` 和 `stats.txt`，再检查 SAU 配置、地址布局和周期上限。
