# sau_n standalone baseline

状态：Step 1 历史快照。记录 baseline 建立时 Step 1 已完成、Step 2 external tensor
backing 尚未开始；后续 Step 2–Step 7 已完成，当前总体状态以
[`PLAN.md`](../PLAN.md) 为准。

## 构建环境

| 项目 | 值 |
|---|---|
| target repository | `/home/xch/workspace/soc_gem5/gem5_cpu_veu` |
| target branch | `sau16x16` |
| sau_n source commit | `8b3682c962f86e6b64b9b035e8229fd168c29c54` |
| runtime executable | `build/RISCV/gem5.opt` |
| test executable directory | `build/RISCV/sau_n/` |
| test invocation | each `.test.opt --gtest_color=no` |

## 测试结果

以下 9 个来源定向测试全部通过，共 76 个 test case：

| Test | Cases | Result |
|---|---:|---|
| `im2col_types.test.opt` | 12 | PASS |
| `im2col_address.test.opt` | 7 | PASS |
| `banked_scratchpad.test.opt` | 6 | PASS |
| `sau_types.test.opt` | 4 | PASS |
| `sau_generators.test.opt` | 3 | PASS |
| `sau_model.test.opt` | 8 | PASS |
| `streaming_pipeline_contract.test.opt` | 18 | PASS |
| `pipelined_im2col_model.test.opt` | 10 | PASS |
| `streaming_conv_pipeline_model.test.opt` | 8 | PASS |
| **合计** | **76** | **PASS** |

测试覆盖了配置校验、CHW 地址映射、16-bank scratchpad、生成器、SA 数值模型、共享
scratchpad 仲裁、Im2Col pipeline 以及 StreamingConvPipelineModel 的 standalone 行为。

## 来源一致性

- manifest 中 27 个 `.cc`、`.hh` 和 `.test.cc` 与来源 worktree 逐字一致；
- `SConscript` 保留来源测试 target 名称，但在目标仓库聚焦为 9 个核心模块和对应测试；
- `rtl/`、Python timing wrapper、旧 pipeline 变体和历史文档未导入；
- 本 baseline 建立时尚未验证 external tensor backing、CPU SRAM 可见性、crossbar
  ownership 或四条 CSR e2e；这些内容已在后续 Step 2–Step 7 中完成。最终 e2e 报告位于
  `test_by_agent/conv3_e2e/runs_sau_n_final/`。

## 复现命令

```sh
cd /home/xch/workspace/soc_gem5/gem5_cpu_veu

build/RISCV/sau_n/im2col_types.test.opt --gtest_color=no
build/RISCV/sau_n/im2col_address.test.opt --gtest_color=no
build/RISCV/sau_n/banked_scratchpad.test.opt --gtest_color=no
build/RISCV/sau_n/sau_types.test.opt --gtest_color=no
build/RISCV/sau_n/sau_generators.test.opt --gtest_color=no
build/RISCV/sau_n/sau_model.test.opt --gtest_color=no
build/RISCV/sau_n/streaming_pipeline_contract.test.opt --gtest_color=no
build/RISCV/sau_n/pipelined_im2col_model.test.opt --gtest_color=no
build/RISCV/sau_n/streaming_conv_pipeline_model.test.opt --gtest_color=no
```
