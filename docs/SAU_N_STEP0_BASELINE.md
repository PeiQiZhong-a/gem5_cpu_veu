# sau_n CPU 接入 Step 0 基线

状态：Step 0 历史快照。记录当时 Step 0 已完成、尚未导入 sau_n 源码且尚未修改
计算后端；后续 Step 1–Step 7 已完成，当前总体状态以 [`PLAN.md`](../PLAN.md) 为准。

基线记录时间：2026-08-02T18:19:14+08:00

## 冻结来源

| 项目 | 值 |
|---|---|
| source worktree | `/home/xch/workspace/gem5/.worktrees/sau-command-types` |
| source branch | `feature/sau-command-types` |
| source commit | `8b3682c962f86e6b64b9b035e8229fd168c29c54` |
| source commit time | `2026-07-31T10:53:44+08:00` |
| source commit subject | `sau_n: add shared scratchpad double buffering` |
| target repository | `/home/xch/workspace/soc_gem5/gem5_cpu_veu` |
| target branch | `sau16x16` |
| target HEAD | `c270a3ac3246e87b18c43712a9f32c360bcc3e47` |
| file hash manifest | `docs/SAU_N_SOURCE_MANIFEST.sha256` |

来源 worktree 的 `src/sau_n` 在基线记录时 clean。来源 worktree 另有一个不属于本次
导入范围的未跟踪文件：`src/sau/docs/reports/plan3-gemm-resource-comparison-zh.md`。

## Step 1 导入清单

manifest 只覆盖当前计划确定的最小依赖闭包，共 28 个文件：

- `SConscript`；
- `im2col_types`、`im2col_address`、`banked_scratchpad`；
- `sau_types`、`sau_generators`、`sau_model`；
- `streaming_pipeline_contract`、`pipelined_im2col_model`、
  `streaming_conv_pipeline_model`；
- 上述模块的 `.cc`、`.hh` 和直接对应的 `.test.cc`。

每个文件的 SHA-256 以及来源 commit 见
[`SAU_N_SOURCE_MANIFEST.sha256`](SAU_N_SOURCE_MANIFEST.sha256)。该校验应在来源 worktree
或目标仓库完成原始文件导入、尚未聚焦改写 `SConscript` 时执行；`SConscript` 后续允许
按目标仓库构建需要做最小登记适配，适配差异不得冒充来源文件内容。

以下内容明确不属于本次最小导入清单：`src/sau_n/rtl/`、RTL provenance/PDF、Python
SimObject timing wrapper、旧的 `conv_pipeline_*`/`im2col_*` 非依赖模型、历史计划/状态
文档以及不在上述测试闭包内的测试文件。

## 目标仓库 dirty 状态快照

以下是 Step 0 开始时 `git status --short` 的完整快照。所有条目均作为 Step 0 前已存在
的基线，后续步骤不得 reset、clean、覆盖或批量删除：

```text
 M configs/brs/run_pipeline_mini.py
 M docs/SAU_GEM5_ENDPOINT_CONTRACT.md
 M src/brs/PipelineMiniCPU.py
 M src/brs/SConscript
 M src/brs/pipeline/frontend_fetch_unit.cc
 M src/brs/pipeline/frontend_fetch_unit.test.cc
 M src/brs/pipeline/pipeline_core.cc
 M src/brs/pipeline/pipeline_core.hh
 M src/brs/pipeline/pipeline_sau.test.cc
 M src/brs/pipeline/program_image.hh
 M src/brs/pipeline/program_image.test.cc
 M src/brs/pipeline/sau_decode.test.cc
 M src/brs/pipeline/sau_issue.test.cc
 M src/brs/pipeline/stage_ex.cc
 M src/brs/pipeline/stage_wb.cc
 M src/brs/pipeline_mini_cpu.cc
 M src/brs/pipeline_mini_cpu.hh
 M src/brs/pipeline_stats.hh
?? PLAN.md
?? docs/SAU_CONV3_CSR_ABI.md
?? fixtures/
?? src/brs/sau/conv3_compute_core.cc
?? src/brs/sau/conv3_compute_core.hh
?? src/brs/sau/conv3_compute_core.test.cc
?? src/brs/sau/conv3_csr_config.cc
?? src/brs/sau/conv3_csr_config.hh
?? src/brs/sau/conv3_csr_config.test.cc
?? src/brs/sau/conv3_memory_adapter.cc
?? src/brs/sau/conv3_memory_adapter.hh
?? src/brs/sau/conv3_memory_adapter.test.cc
?? src/brs/sau/conv3_sau_endpoint.cc
?? src/brs/sau/conv3_sau_endpoint.hh
?? src/brs/sau/conv3_sau_endpoint.test.cc
?? test_by_agent/conv3_e2e/
```

## 旧 Conv3 路径处置边界

以下文件只标记为“待替换实现”，Step 0 不删除、不移出构建、不修改其测试期望：

```text
src/brs/sau/conv3_compute_core.cc
src/brs/sau/conv3_compute_core.hh
src/brs/sau/conv3_compute_core.test.cc
src/brs/sau/conv3_csr_config.cc
src/brs/sau/conv3_csr_config.hh
src/brs/sau/conv3_csr_config.test.cc
src/brs/sau/conv3_memory_adapter.cc
src/brs/sau/conv3_memory_adapter.hh
src/brs/sau/conv3_memory_adapter.test.cc
src/brs/sau/conv3_sau_endpoint.cc
src/brs/sau/conv3_sau_endpoint.hh
src/brs/sau/conv3_sau_endpoint.test.cc
```

上述列表是 Step 0 的历史 dirty 快照，不代表当前文件集合。Step 7 已通过，Step 8
已按用户确认逐文件移除 `conv3_compute_core.*`、`conv3_memory_adapter.*` 和
`conv3_sau_endpoint.*` 及其测试；`conv3_csr_config.*` 因仍被 `sau_n` adapter 使用而
保留。

## 冻结合同

实现依据为：

- [`PLAN.md`](../PLAN.md)；
- [`SAU_CONV3_CSR_ABI.md`](SAU_CONV3_CSR_ABI.md)，wire version 1、document revision 1.2；
- [`SAU_GEM5_ENDPOINT_CONTRACT.md`](SAU_GEM5_ENDPOINT_CONTRACT.md)，version 1.3；
- `LocalScratchpadBacking` 模式：集成 operation 不产生外部 `Sram256Request`，共享
  SRAM backing 是 CPU 与 sau_n 的唯一数据权威。

Step 0 结束时的下一步是按本 manifest 执行最小 sau_n 导入和 standalone baseline
建立；不在 Step 0 预先实现 backing、CSR adapter 或 endpoint。该历史步骤现已完成，
当前验证结果和下一步请以 [`PLAN.md`](../PLAN.md) 为准。
