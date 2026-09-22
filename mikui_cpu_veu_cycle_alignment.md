# Mikui CPU + VEU 逐拍对齐结果

本次修改位于 `/home/zpq/下载/gem5_cpu_veu` 的 `mikui` 分支，gem5 固定使用 `--mem-system rtl-npu-lpnpu-mikui-decompress-dma`。对照的是同一批 Mikui 固件在无 BTB RTL 下产生的 trace；没有修改 RTL 测试输入。

## 结果与边界

- 输入：`/home/zpq/下载/逐拍对齐结果和固件输入/m5out_mikui_supported_veu_v1`，共 36 项（18 种操作/模式组合，各 256 和 2048 bit）。
- RTL 基准：`/home/zpq/下载/逐拍对齐结果和固件输入/mikui_rtl_alignment_reduction_v1_all`。
- 最终 gem5 输出：`/home/zpq/下载/逐拍对齐结果和固件输入/mikui_cpu_veu_alignment_decompress_dma_verified_all`，总表为其中的 `summary.csv`。
- 结果：RTL 功能 PASS 36/36；gem5 到达 DONE 36/36；CPU 严格逐拍 PASS 36/36；VEU 128-bit SRAM 物理请求和锁边沿逐拍 PASS 36/36；DONE edge 相同 36/36。

CPU 比较检查有效载荷、总线握手、退休及写回拍、控制/停顿/重定向和 DONE 拍；VEU 物理比较检查 `veu_req/addr/we/wstrb/wdata` 及 `lock_start/finish`。本结论**不等于** VLU、VFU、VSU 的全部内部寄存器和 FIFO 均已逐拍比较，也不覆盖这 36 项以外的 mask、mode、shift、scalar/vector 组合。

## 关键修改

1. 实现严格 `brs-cycle-trace-v3` 字段验证、CPU 逐拍比较和可选的 VEU 物理引脚比较；批量脚本固定 Mikui decompress-DMA 内存模型。
2. 校正 Mikui IBus 响应采样相位、DBus/VEU 返回顺序、前端 FIFO 同拍发请求和重定向、执行停顿期间预取、译码重定向寄存，从而对齐 CPU 总线、退休和分支拍。
3. 校正 VEU `VectorStart` CSR 返回、scalar c2 状态清零可见拍、VMUL 双源读取顺序。
4. 将 c8 数据通路参数外推到 c16，但保持 c16 的完成控制与真实数据通路相关；c16 归约仅写回最终累计值，`vslidedown` 单独校正尾写回拍。
5. 模拟 Mikui scalar 操作锁释放时起连续 4 拍的物理尾脉冲。尾脉冲地址是 scalar 值，并非有效向量数据源；`physicalOnly` 保证其可出现在引脚 trace，却不会污染功能内存或等待不存在的 SRAM 返回。此行为仅在 Mikui 集成内存模型中启用。

## 复现

在 `/home/zpq/下载/gem5_cpu_veu` 中运行：

```bash
python3 util/brs/run_mikui_matrix.py \
  --input-root '/home/zpq/下载/逐拍对齐结果和固件输入/m5out_mikui_supported_veu_v1' \
  --rtl-root '/home/zpq/下载/逐拍对齐结果和固件输入/mikui_rtl_alignment_reduction_v1_all' \
  --output-root '/home/zpq/下载/逐拍对齐结果和固件输入/mikui_cpu_veu_alignment_decompress_dma_verified_all' \
  --check-veu-physical
```

脚本内部固定 `--mem-system rtl-npu-lpnpu-mikui-decompress-dma`、TimingVEU、profile、terminal behavior、10 个 reset edge 和 DONE store 终止。退出码 0 表示所有可比较项的 CPU 和所选 VEU 物理检查均 PASS。

验证：TimingVEU 单测 30/30、Mikui 内存模型 9/9、前端 20/20、trace 比较器 11/11，通过 `git diff --check`。
