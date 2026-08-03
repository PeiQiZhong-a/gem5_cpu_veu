# sau_n 多尺寸 Conv 仿真结果

日期：2026-08-03  
运行入口：`test_by_agent/conv3_e2e/run_custom_conv.py`  
仿真模型：`gem5 + PipelineMiniCPU + sau_n::StreamingConvPipelineModel`  
原始结果目录：`/tmp/sau-n-size-sweep-20260803-v1/`

## 1. 目的和结论

本次运行使用工具链风格的近似启动模型，对不同 N/C/H/W/OC、padding 和 stride
进行了六组完整 Conv2d 仿真。每组都执行完整流程：

```text
生成 instruction.hex / memory.hex
  -> CPU 执行近似 malloc_initial 和四次 malloc
  -> CPU 从 staging 区读取 input/weight/bias 并写入最终 tensor 地址
  -> mgetins4lsb 查询、四条 msetins 配置
  -> sau_n 读取 A/B/C 并写回 D
  -> msetins4 完成、CPU 到达 ebreak
  -> 独立 verifier 校验配置、时序、SRAM 访问和输出
```

六组全部通过，输出结果的实际 SHA-256 和参考 SHA-256 一致，没有外部 SRAM
request/response 泄漏。

## 2. 统一运行条件

除表格中明确列出的 Conv 参数外，各组使用相同设置：

| 项目 | 设置 |
|---|---|
| startup model | `toolchain-approx` |
| tensor 初始化 | `--tensor-init cpu` |
| weight pattern | `ramp` |
| bias pattern | `ramp` |
| seed | `7` |
| kernel / dilation | `3x3 / 1x1` |
| 数据类型 | input/weight/output signed INT8，bias signed INT16 |
| layout | input/output NCHW，weight `[C][3][3][OC]` |
| clock | `100MHz` |
| SAU | `sau_n`，LocalScratchpadBacking |
| data window | `0x29120000`，大小 `0x40000` |
| real SRAM | 3 个 bank，大小 `0x30000` |
| 最大周期 | `2,000,000` |

`toolchain-approx` 每组都包含一次近似 `malloc_initial`、四次固定地址
`malloc(input/weight/bias/output)`，以及提交前后的各一次 `mgetins4lsb`。
四条 `msetins` 和两条 `mgetins4` 会使 `sau_issue_count`、`sau_retire_count`
均为 6。

## 3. 配置和内存规模

输出尺寸按以下公式计算：

```text
out_h = floor((H + 2 * padding - 3) / stride) + 1
out_w = floor((W + 2 * padding - 3) / stride) + 1
```

scratchpad 行数是 runner 的默认连续布局估算：

```text
total = A_rows + B_rows + C_rows + D_rows
```

| Case | N | C | H | W | OC | padding | stride | 输出 HxW | Input/Weight/Bias/Output bytes | A/B/C/D/total rows |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| `small_1x4x8x8_oc4_p1_s1` | 1 | 4 | 8 | 8 | 4 | 1 | 1 | 8x8 | 256 / 144 / 8 / 256 | 16 / 36 / 2 / 64 / 118 |
| `medium_1x8x16x32_oc8_p1_s1` | 1 | 8 | 16 | 32 | 8 | 1 | 1 | 16x32 | 4096 / 576 / 16 / 4096 | 256 / 72 / 2 / 512 / 842 |
| `odd_stride2_1x8x17x33_oc8_p1_s2` | 1 | 8 | 17 | 33 | 8 | 1 | 2 | 9x17 | 4488 / 576 / 16 / 1224 | 408 / 72 / 2 / 153 / 635 |
| `batch2_n2x4x16x32_oc12_p0_s1` | 2 | 4 | 16 | 32 | 12 | 0 | 1 | 14x30 | 4096 / 432 / 24 / 10080 | 256 / 36 / 2 / 840 / 1134 |
| `large_1x16x16x32_oc16_p1_s1` | 1 | 16 | 16 | 32 | 16 | 1 | 1 | 16x32 | 8192 / 2304 / 32 / 8192 | 512 / 144 / 2 / 512 / 1170 |
| `batch2_n2x8x16x32_oc16_p1_s1` | 2 | 8 | 16 | 32 | 16 | 1 | 1 | 16x32 | 8192 / 1152 / 32 / 16384 | 512 / 72 / 2 / 1024 / 1610 |

所有 case 的 scratchpad 估算都小于 4096 行，符合当前 runner 和 sau_n 配置限制。

## 4. 总体性能结果

说明：

- `cycle_count`：gem5 pipeline 的总周期，包含 CPU 启动、CPU tensor 搬运、CSR
  handshake、SAU 阻塞等待和结束路径。
- `retired_inst_count`：整个 guest CPU 程序退休的指令数。
- `SAU ROI span`：`sau_roi_end_cycle - sau_roi_start_cycle`，包含 endpoint
  看到的 sau_n operation 区间。
- `model_ticks`：`StreamingConvPipelineModel` 实际推进的内部 tick 数。
- `CSR handshake`：从 SAU 指令提交到 completion 的统计周期，包含 endpoint
  控制和 release/response 周期，因此通常比 `model_ticks` 多一些。

| Case | cycle_count | retired inst | stall count | ROI start | ROI end | ROI span | model ticks | CSR handshake | status |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `small_1x4x8x8_oc4_p1_s1` | 19,263 | 10,616 | 6,288 | 18,854 | 19,250 | 396 | 394 | 408 | PASS |
| `medium_1x8x16x32_oc8_p1_s1` | 130,652 | 36,276 | 69,773 | 126,622 | 130,639 | 4,017 | 4,015 | 4,029 | PASS |
| `odd_stride2_1x8x17x33_oc8_p1_s2` | 146,947 | 38,626 | 73,927 | 144,251 | 146,934 | 2,683 | 2,681 | 2,695 | PASS |
| `batch2_n2x4x16x32_oc12_p0_s1` | 130,527 | 35,439 | 68,660 | 125,650 | 130,514 | 4,864 | 4,862 | 4,876 | PASS |
| `large_1x16x16x32_oc16_p1_s1` | 285,901 | 71,274 | 153,797 | 279,495 | 285,888 | 6,393 | 6,391 | 6,405 | PASS |
| `batch2_n2x8x16x32_oc16_p1_s1` | 252,748 | 64,364 | 139,229 | 244,782 | 252,735 | 7,953 | 7,951 | 7,965 | PASS |

### 4.1 结果观察

1. 总周期不仅由输出元素数量决定。它同时受到 input/weight/bias 的 CPU 搬运、
   输入空间尺寸、通道数、padding/stride 和 sau_n 内部排队的影响。
2. `large_1x16x16x32_oc16_p1_s1` 的 input 和 weight 较大，CPU retired 指令数
   和总周期最高；其 SAU 内部 tick 为 6391。
3. `batch2_n2x8x16x32_oc16_p1_s1` 输出元素最多，为 16384，SAU tick 为 7951，
   但总周期低于 C=16 的 large case，因为其 weight 和部分通道相关的工作量更小。
4. `odd_stride2_1x8x17x33_oc8_p1_s2` 的输出元素只有 1224，SAU tick 也较低，
   但不规则输入尺寸仍使 CPU 初始化和取指路径占据明显的总周期。
5. 每组 `model_ticks` 与 ROI span 相差 2 个周期，属于 endpoint 在 drained 后
   发布 `crossbarDone` 和 completion 的控制开销；CSR handshake 再包含额外的
   CPU/CSR 响应周期。

## 5. CPU 启动和 staging 数据

每组的 staging 数据由 host 预先写入 `memory.hex`，这些 host 写入不计入 CPU
周期；guest CPU 从 staging 区 load，再 store 到最终 tensor 地址的循环会计入
`cycle_count` 和 retired instruction。

| Case | staging source bytes | allocator base | input source | weight source | bias source | mget polls |
|---|---:|---:|---:|---:|---:|---|
| `small_1x4x8x8_oc4_p1_s1` | 408 | `0x29132a20` | `0x29132b20` | `0x29132c20` | `0x29132cb0` | before=1, after=1 |
| `medium_1x8x16x32_oc8_p1_s1` | 4688 | `0x29133920` | `0x29133a20` | `0x29134a20` | `0x29134c60` | before=1, after=1 |
| `odd_stride2_1x8x17x33_oc8_p1_s2` | 5080 | `0x29132de8` | `0x29132ee8` | `0x29134070` | `0x291342b0` | before=1, after=1 |
| `batch2_n2x4x16x32_oc12_p0_s1` | 4552 | `0x29135080` | `0x29135180` | `0x29136180` | `0x29136330` | before=1, after=1 |
| `large_1x16x16x32_oc16_p1_s1` | 10528 | `0x29134920` | `0x29134a20` | `0x29136a20` | `0x29137320` | before=1, after=1 |
| `batch2_n2x8x16x32_oc16_p1_s1` | 9376 | `0x29136920` | `0x29136a20` | `0x29138a20` | `0x29138ea0` | before=1, after=1 |

allocator 区固定为 256 bytes；其中的地址和 source 区由 runner 自动寻找空闲
real SRAM 区域，因此不会与四个最终 tensor 重叠。

## 6. sau_n scratchpad 访问结果

以下是 verifier 从 `stats.txt` 读取的内部 scratchpad grant 数量。LocalScratchpadBacking
模式下，访问不会转换成外部 256-bit SRAM beat。

| Case | A read grants | B read grants | C read grants | D write grants | D output elements | external SRAM req/resp |
|---|---:|---:|---:|---:|---:|---|
| `small_1x4x8x8_oc4_p1_s1` | 1,936 | 144 | 8 | 256 | 256 | 0 / 0 |
| `medium_1x8x16x32_oc8_p1_s1` | 34,592 | 576 | 16 | 4,096 | 4,096 | 0 / 0 |
| `odd_stride2_1x8x17x33_oc8_p1_s2` | 9,800 | 576 | 16 | 1,224 | 1,224 | 0 / 0 |
| `batch2_n2x4x16x32_oc12_p0_s1` | 30,240 | 432 | 24 | 10,080 | 10,080 | 0 / 0 |
| `large_1x16x16x32_oc16_p1_s1` | 69,184 | 2,304 | 32 | 8,192 | 8,192 | 0 / 0 |
| `batch2_n2x8x16x32_oc16_p1_s1` | 69,184 | 1,152 | 32 | 16,384 | 16,384 | 0 / 0 |

所有 D write grants 都等于 output elements，说明每个输出 byte 都成功写入共享
SRAM backing；所有 A/B/C grant 都等于对应 response，未出现 scratchpad 丢请求。

## 7. 输出校验和和原始报告

每组均满足：

```text
actual_output_sha256 == expected_output_sha256
status == PASS
sau_issue_count == 6
sau_retire_count == 6
```

| Case | output SHA-256 | report | manifest |
|---|---|---|---|
| `small_1x4x8x8_oc4_p1_s1` | `bea79127343771598e8ad7dafdb9271030d5a5905f8e341801828fe33f9acfbd` | [`report.json`](/tmp/sau-n-size-sweep-20260803-v1/small_1x4x8x8_oc4_p1_s1/run/report.json) | [`manifest.json`](/tmp/sau-n-size-sweep-20260803-v1/small_1x4x8x8_oc4_p1_s1/manifest.json) |
| `medium_1x8x16x32_oc8_p1_s1` | `e87f59474c08698930d314d404b6810c8e4a5e9e8a7e2b40346c9fb94293f8cc` | [`report.json`](/tmp/sau-n-size-sweep-20260803-v1/medium_1x8x16x32_oc8_p1_s1/run/report.json) | [`manifest.json`](/tmp/sau-n-size-sweep-20260803-v1/medium_1x8x16x32_oc8_p1_s1/manifest.json) |
| `odd_stride2_1x8x17x33_oc8_p1_s2` | `fd82a53c3c561964acfd2c1f33b29dbf537cd4b7ca9c4adbf2f2f4a62bf02a48` | [`report.json`](/tmp/sau-n-size-sweep-20260803-v1/odd_stride2_1x8x17x33_oc8_p1_s2/run/report.json) | [`manifest.json`](/tmp/sau-n-size-sweep-20260803-v1/odd_stride2_1x8x17x33_oc8_p1_s2/manifest.json) |
| `batch2_n2x4x16x32_oc12_p0_s1` | `91e18e3ef172e03285a1fded4c5a48775424f3320ab78c0f8d8192fab94faae1` | [`report.json`](/tmp/sau-n-size-sweep-20260803-v1/batch2_n2x4x16x32_oc12_p0_s1/run/report.json) | [`manifest.json`](/tmp/sau-n-size-sweep-20260803-v1/batch2_n2x4x16x32_oc12_p0_s1/manifest.json) |
| `large_1x16x16x32_oc16_p1_s1` | `0f67cab7d4e0f3b5b9f331c7374eac8eae2d8dd9027c46a0d5ddb99b3bca5e84` | [`report.json`](/tmp/sau-n-size-sweep-20260803-v1/large_1x16x16x32_oc16_p1_s1/run/report.json) | [`manifest.json`](/tmp/sau-n-size-sweep-20260803-v1/large_1x16x16x32_oc16_p1_s1/manifest.json) |
| `batch2_n2x8x16x32_oc16_p1_s1` | `16db7611d51a4a463dea983d761e2f2b1d47121c25cac41ee0b8f437657eaf08` | [`report.json`](/tmp/sau-n-size-sweep-20260803-v1/batch2_n2x8x16x32_oc16_p1_s1/run/report.json) | [`manifest.json`](/tmp/sau-n-size-sweep-20260803-v1/batch2_n2x8x16x32_oc16_p1_s1/manifest.json) |

## 8. 复现命令

在工程根目录执行下面的命令即可复现某一组；只需要替换 `<case-name>` 和形状
参数。当前示例会把结果写入新的 case 目录：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --output-root /tmp/sau-n-size-sweep-rerun \
  --name <case-name> \
  --n <N> --channels <C> --height <H> --width <W> \
  --out-channels <OC> --padding <padding> --stride <stride> \
  --cutbit 10 \
  --weight-pattern ramp --bias-pattern ramp --seed 7 \
  --tensor-init cpu --startup-model toolchain-approx
```

例如重跑最大通道数 case：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --output-root /tmp/sau-n-size-sweep-rerun \
  --name large_1x16x16x32_oc16_p1_s1 \
  --n 1 --channels 16 --height 16 --width 32 --out-channels 16 \
  --padding 1 --stride 1 --cutbit 12 \
  --weight-pattern ramp --bias-pattern ramp --seed 7 \
  --tensor-init cpu --startup-model toolchain-approx
```

## 9. 限制和解释

1. 本报告使用的是 `toolchain-approx`，不是工具链真实编译出的完整固件镜像。
   allocator 使用固定 256-byte metadata footprint、按申请尺寸计算的 virtual
   header capacity 和固定 tensor 返回地址，目的是近似 guest CPU 的指令、访存和
   控制流开销。
2. `msetins4` 在 gem5 endpoint 中阻塞到 sau_n 完成，因此报告中的工具链式
   `mgetins4` 是每次一次的 idle 查询，不会额外制造一段重复 busy-poll。
3. `cycle_count` 不能直接等同于 `sau_n model_ticks`。前者包含 CPU 初始化和
   pipeline stall，后者只表示 StreamingConvPipelineModel 的内部推进周期。
4. host 预先把 staging 数据写入 `memory.hex` 的时间不计入 CPU 周期；CPU 从
   staging 区复制到最终 tensor 地址的时间计入。
5. 当前报告覆盖 full-offload Conv；没有把卷积完成后的 `free`、结果打印或软件
   逐元素比较加入启动模型。
