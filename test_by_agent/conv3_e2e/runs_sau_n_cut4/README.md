# SAU-N cutbit=4 仿真产物

本目录保存四个卷积压缩包使用 `cutbit=4` 修改后的 `instruction.hex`、原始 `memory.hex` 以及对应的 gem5 仿真输出。

每个子目录包含：

- `instruction.hex`：将调用参数中的 `cutbit=12` 改为 `cutbit=4` 后的指令镜像；
- `memory.hex`：压缩包中的原始数据镜像；
- `stats.txt`、`cycle_trace.log`、`config.ini`、`config.json`：gem5 结果；
- `stdout.log`、`stderr.log`、`citations.bib`：运行辅助产物。

| 子目录 | 配置 | 输出字节 | CPU 周期 | SAU ROI 周期 | model ticks | Golden |
|---|---|---:|---:|---:|---:|---|
| `conv_package_h8w5_p0` | H=8, W=5, pad=0 | 288 | 1022 | 532 | 530 | PASS |
| `sau_n_conv3_h8w5_p1` | H=8, W=5, pad=1 | 640 | 1228 | 733 | 731 | PASS |
| `sau_n_conv3_h8w32_p0` | H=8, W=32, pad=0 | 2880 | 2987 | 2484 | 2482 | PASS |
| `sau_n_conv3_h8w32_p1` | H=8, W=32, pad=1 | 4096 | 3767 | 3273 | 3271 | PASS |

Golden 校验按 NCHW 3×3 卷积、int24 饱和累加、bias 和 `cutbit=4` int8 量化计算，四组输出均逐字节一致。
