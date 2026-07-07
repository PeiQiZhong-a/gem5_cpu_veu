# Phase 1 unified memory platform

## 目标

将 `PipelineMiniCPU` 的运行入口统一到 `configs/brs/run_pipeline_mini.py`，通过 `--mem-system` 选择不同 memory platform。优先使用 gem5 原生 `SimpleMemory`、`SystemXBar` 和 `in_addr_map` 能力，避免过早新增自定义内存 SimObject。

## 统一入口

- `configs/brs/run_pipeline_mini.py`

支持三种模式：

| 参数 | 平台 | 用途 |
|---|---|---|
| `--mem-system ddr3` | `SystemXBar -> MemCtrl -> DDR3_1600_8x8` | 原始 gem5 DDR3 平台 |
| `--mem-system simple` | `SystemXBar -> SimpleMemory` | 单 fixed-latency memory，对齐早期基线 |
| `--mem-system split` | `IBus -> IMEM` / `DBus -> DMEM` | I/D 分离 fixed-latency memory，更接近 Spirit testbench |

## simple 模式

```text
PipelineMiniCPU.inst_port/data_port
    -> SystemXBar
    -> SimpleMemory(latency=1ns by default)
```

在 1GHz CPU 时钟下，默认 `1ns` 对应约 1 个 CPU cycle 的固定内存响应延迟。

## split 模式

```text
PipelineMiniCPU.inst_port -> SystemXBar(IBus) -> IMEM SimpleMemory
PipelineMiniCPU.data_port -> SystemXBar(DBus) -> DMEM SimpleMemory
```

IMEM 和 DMEM 可以使用相同架构地址范围。为避免全局地址映射重叠：

```python
imem.in_addr_map = True
dmem.in_addr_map = False
```

DMEM 不进入全局 address map，但仍会通过私有 DBus 响应 `data_port` 请求。

## 与原 DDR3 配置的区别

原脚本 `configs/brs/run_pipeline_mini.py` 使用：

```text
SystemXBar -> MemCtrl -> DDR3_1600_8x8
```

`simple` 模式使用：

```text
SystemXBar -> SimpleMemory
```

这样减少：

- DRAM timing 变化
- memory controller 排队影响
- DDR row/bank 行为影响

## 与 Spirit testbench 的差距和限制

当前平台仍不是完全等价的 Spirit memory testbench：

- `split` 模式已分离 IMEM/DMEM，但仍使用 gem5 `SystemXBar`。
- 仍使用 gem5 `SimpleMemory`，不是自定义 4-word block fetch 的 IMEM。
- 当前 `PipelineMiniCPU` 的 data-side 主路径仍未接入 `data_port`，load/store 仍需 Phase 2 改造。
- 当前 IF 还不是 Spirit 的 `IBU/PFU/IFU` block-fetch 行为。

## 使用示例

```bash
build/RISCV/gem5.opt configs/brs/run_pipeline_mini.py \
  --binary path/to/test.elf \
  --mem-system simple \
  --no-icache \
  --max-cycles 200 \
  --mem-latency 1ns
```

I/D 分离模式：

```bash
build/RISCV/gem5.opt configs/brs/run_pipeline_mini.py \
  --binary path/to/test.elf \
  --mem-system split \
  --no-icache \
  --imem-latency 1ns \
  --dmem-latency 1ns
```

原 DDR3 平台仍可用：

```bash
build/RISCV/gem5.opt configs/brs/run_pipeline_mini.py \
  --binary path/to/test.elf \
  --mem-system ddr3
```

## 后续步骤

1. Phase 2：让 `data_port` 真正接管 load/store。
2. Phase 3：把 MEM/IF 改成 request/response 驱动的 hold/advance 流水。
3. 若 gem5 原生组件无法满足更严格对齐，再考虑自定义 IMEM/DMEM：I-side 4-word block fetch，D-side byte-enable fixed-latency RAM。
