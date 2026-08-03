# SAU Step 1 comparison fixtures

本目录保存 Step 1 的三个 **assembly-generated comparison fixture**。它们是独立的
对比输入，不覆盖或冒充用户归档中的原始 `instruction.hex` / `memory.hex`。

- `generated_legacy_control`：保留 legacy 控制结构；当前 shape 使用
  `msetins1/2` 各一次、`msetins3/4` 各 32 次。
- `generated_four_ins_control_matched`：保持相同 startup、input 初始化、固定
  allocation map 和软件 padding preprocessing，只把配置区替换为一组有序的新
  `msetins1~4`。它只用于 `StubSau` control-only 对比，不承担 Conv 数值验证。
- `generated_four_ins_full_offload`：保留逻辑 NCHW input，不执行软件 padding；padding
  由活动 `sau_n` 后端处理，用于最终 sau_n e2e。当前 sau_n 验收报告位于
  `test_by_agent/conv3_e2e/runs_sau_n_final/`。

原始 baseline 归档固定为：

```text
/home/xch/workspace/kuiloong-conv2d-testcase2.tar.gz
```

每个 fixture 的 `manifest.json` 记录归档、源文件、工具、命令、输出 hash、token
格式、静态 SAU PC 和内存布局（含 startup/input-init/padding/config 区间）。归档的
`memory.hex` 在三个 fixture 中逐字节复用。

实际使用的是当前 `rtl-dut-kui-tb` 地址窗口，而不是 ABI 文档中的独立
`0x2001...` 编码示例：

| 项目 | 地址 |
|---|---:|
| rtl-tb data base | `0x29120000` |
| input | `0x29130000` |
| weight | `0x29132000` |
| bias | `0x29132900` |
| output | `0x29132920` |

生成链固定为：

```text
RV32 assembly source
  -> /usr/bin/llvm-mc (standard RV32 instructions; SAU instructions use .word)
  -> /usr/bin/llvm-objcopy (extract binary section)
  -> reviewed word-token conversion/padding
  -> 65536 x 32-bit little-endian readmemh tokens
```

manifest 将记录 source/tool/command/input/output SHA-256、归档 provenance、token
格式、内存布局以及唯一允许出现差异的配置区。`generated_legacy_control` 与
`generated_four_ins_control_matched` 的公共尾部会固定在同一地址，并由生成脚本验证
配置区外完全相同。

## 生成和验证

在工程根目录运行：

```sh
python3 fixtures/conv3_step1/generate_fixtures.py
python3 fixtures/conv3_step1/verify_fixtures.py
```

脚本使用 `/usr/bin/llvm-mc` 和 `/usr/bin/llvm-objcopy`，将 RV32 assembly 的
`.text` 转为 65536 个 32-bit little-endian、每行 8 个十六进制字符的 token。最终
`instruction.hex` 不应手工编辑；重新运行生成脚本即可重建三个目录。

验证器检查：

- 三组镜像均为 65536 tokens，且 `memory.hex` 与归档 member 完全一致；
- legacy 的 SAU 次数为 `1/1/32/32`，两个四指令镜像均为 `1/1/1/1`；
- legacy 与 control-matched 只允许在 `[0x4000, 0x5000)` 配置区不同；
- control-matched 与 full-offload 只允许在 `[0x0200, 0x4000)` padding 区不同；
- 两个四指令镜像的配置区相同，公共尾部 `0x5000` 为 `ebreak`。

## Baseline 观察

从不可修改的 archive `instruction.hex` 按 2-byte 对齐扫描到的六个 SAU 静态站点为：

| PC | word | slot | rs1 | rs2 |
|---:|---:|---:|---:|---:|
| `0x09ca` | `0x00ab906b` | 1 | 23 | 10 |
| `0x0a08` | `0x06c5906b` | 2 | 11 | 12 |
| `0x0b40` | `0x0d5d106b` | 3 | 26 | 21 |
| `0x0b46` | `0x135b906b` | 4 | 23 | 21 |
| `0x0b64` | `0x0c53906b` | 3 | 7 | 5 |
| `0x0b7a` | `0x1272906b` | 4 | 5 | 7 |

archive 中可静态观察到压缩 `c.ebreak` 位于 `0x0c32`；归档没有附带 retire
trace，因此 manifest 将动态 `1/1/32/32` 标为 archive source/static program
analysis，不冒充运行时 trace。Step 1 的 CPU 真实取指、解码和 `StubSau` request
回归仍由工程中的 gem5 C++ 测试负责。

这些 fixture 不是原 C++ 工具链的逐指令编译结果；full-offload 的数据正确性和 e2e
验证由 `test_by_agent/conv3_e2e/` 中的 gem5 runner 和独立 verifier 完成。

## 自定义 Conv 配置

不修改上述已验收 fixture 时，可以使用 custom runner 生成并运行一组新的
`sau_n` full-offload case：

```sh
python3 test_by_agent/conv3_e2e/run_custom_conv.py \
  --name conv_1x16x32x32_oc16 \
  --n 1 --channels 16 --height 32 --width 32 --out-channels 16 \
  --padding 1 --stride 1 --cutbit 12 \
  --input-base 0x29130000 --weight-base 0x29134000 \
  --bias-base 0x29134900 --output-base 0x29134920 \
  --weight-pattern ramp --bias-pattern zero
```

脚本会在 `test_by_agent/conv3_e2e/custom_runs/<name>/` 生成 assembly、
`instruction.hex`、`memory.hex` 和 `manifest.json`，然后启动当前 `gem5 + sau_n`
并调用独立 verifier。使用 `--no-run` 可以只生成镜像。权重和 bias 可用
`zero`、`ones`、`ramp` 或固定 seed 的 `random` 模式生成；输入保持与 verifier
一致的 `input[i] = i / 16` 初始化。

完整参数表、默认值、地址布局、资源限制和常见错误见
[`CUSTOM_RUNNER.md`](../../test_by_agent/conv3_e2e/CUSTOM_RUNNER.md)。
