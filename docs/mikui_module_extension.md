# Mikui gem5 模块接入约定

这份约定用于在当前 `PipelineMiniCPU` + Mikui 128-bit SRAM 平台上探索新模块。
基准配置仍是 `--mem-system rtl-npu-lpnpu-mikui-decompress-dma`，默认启用
`sau` 和 `decompress_dma`。模块化接入**不改变**既有 CPU/VEU/内存时序。

## 接入层与配置

`configs/brs/mikui_modules.py` 是平台模块注册表。每个 `ModuleSpec` 声明：

- `supported_memory_systems`：允许接入的内存拓扑。
- `default_memory_systems`：不写任何额外参数时启用的拓扑。
- `required_memory_systems`：该拓扑不可关闭的模块；例如三 bank DMA 拓扑
  必须带解压 DMA。
- `connections`：CPU、PIO、DMA、IRQ、SRAM 连接的文字契约。
- `mmio_windows`：本模块独占的 PIO 地址范围，选择模块时检查重叠。
- `before_cpu`、`after_cpu`、`after_memory`：对应系统组装阶段的接入函数。

在 `configs/brs/run_pipeline_mini.py` 中可以重复使用
`--enable-mikui-module NAME` 或 `--disable-mikui-module NAME`。
不带这些参数时保持原有行为。当前 `sau` 可关闭来做架构实验；
`decompress_dma` 在三 bank DMA 拓扑中不可关闭。关闭 SAU 会切到现有
`StubSau`，但该试验配置不再代表原 RTL 平台。

## 新模块最小工作流

1. 把模块功能与时序实现放在独立的 `src/brs/<module>/` 目录；如果是新的
   gem5 `SimObject`，在 `src/brs/SConscript` 注册源码和 Python 参数类。
2. 明确模块输入/输出端口、复位、时钟边沿、请求/响应协议及地址窗口。
   CPU 旁计算单元可参照 `SauEndpoint` / `VeuEndpoint`；MMIO/DMA 外设可参照
   `MikuiDecompressDma`。不要让模块直接修改 CPU 流水线内部状态。
3. 在 `mikui_modules.py` 注册 `ModuleSpec` 和接入函数；默认先保持关闭，
   通过 `--enable-mikui-module` 验证。新增 SRAM master 时还必须在
   `NpuLpnpuMikuiMemoryModel` 的仲裁路径建模，单接 gem5 bus 不足以重现
   Mikui RTL 的逐拍 SRAM 时序。
4. 为模块写功能、边界和时序单测；对 PIO、DMA、IRQ、SRAM 连线写组装测试。
   若有新的资源窗口，注册 `mmio_windows` 并测试与现有模块共存。
5. 先在模块**关闭**时跑原 36 项作为不变性回归，再在模块**开启**时跑目标
   固件。新模块改变了 RTL 架构而尚无对应 RTL trace 时，只能报告 gem5
   功能与时序结果，不能报告“RTL 逐拍对齐”。

## 验证命令

在仓库根目录运行：

```bash
python3 -m unittest discover -s configs/brs -p 'test_mikui_modules.py'
python3 util/brs/run_mikui_matrix.py \
  --input-root '/home/zpq/下载/逐拍对齐结果和固件输入/m5out_mikui_supported_veu_v1' \
  --rtl-root '/home/zpq/下载/逐拍对齐结果和固件输入/mikui_rtl_alignment_reduction_v1_all' \
  --output-root '/tmp/mikui_module_baseline' \
  --check-veu-physical
```

第二条命令应有 36 项 `gem5=DONE`、`cpu=PASS`、`veu=PASS`，退出码为 0。
输出根目录应使用新的路径，避免覆盖已有对齐报告。试验模块时可给批量脚本
传递 `--enable-mikui-module NAME`，它会转发给每次 gem5 运行。

注意：`rtl-*` 模式下 CPU/VEU 访存仍经过 `PipelineMiniCPU` 内部的逐拍
内存模型；外部 gem5 端口有一部分只用于组装连通性。因此，把普通 gem5
设备接到 `membus` 不意味着它已经参与内部 SRAM 仲裁。新增模块需要按
实际总线主从关系选择接入点，不要把功能通过误认为逐拍可比。
