# MVP-5 实验复现

范围与公式见 [设计方案](../../docs/design/MVP5_Validation_and_Experiments.md)，结果见 [实验报告](../../docs/experiments/MVP5_Results.md)。

## 构建与验证

在仓库根目录运行（SystemC 安装方式见仓库 README）：

```bash
cmake -S . -B build
cmake --build build -j 4
ctest --test-dir build --output-on-failure
```

Python 3 用于独立验证；CMake 找到 Python 时会注册 `mvp5_validation`。如未找到，需要安装 Python 后重新配置。测试包含单 tile 三种调度、多 slice 串行/paired 手算锚点、无效调度参数和 JSON 写入失败。

## 运行 62 个固定用例

```bash
python scripts/mvp5/run_experiments.py --output results/mvp5/my-run
python scripts/mvp5/validate_results.py results/mvp5/my-run
```

输出目录必须尚不存在，避免覆盖旧结果。每个用例超时为 60 秒。`cases.json` 是固定实验输入，采用 CLI 已支持的参数；HBM 带宽和延迟等固定参数从结果 JSON 读取。扫描器保存源码 commit、dirty 状态、源码哈希、构建配置、二进制哈希、命令和原始日志。源码未提交时必须同时保留对应改动，不能仅凭基线 commit 重建当前二进制。

单独运行示例：

```bash
./build/npu_sim --schedule paired 16 16 16 16 256 --output-json /tmp/paired.json
```

`--serial` 仍可使用；不能与 `--schedule` 同时指定。旧终端 CSV 兼容保留，其中 FLOP 标签为历史命名，本实验以 JSON 的 OP 指标为准，1 MAC = 2 OP。

## 绘图

验证和扫描只需 Python 标准库，绘图单独安装依赖：

```bash
python -m venv /tmp/npu-mvp5-plot
/tmp/npu-mvp5-plot/bin/pip install -r experiments/mvp5/requirements.txt
/tmp/npu-mvp5-plot/bin/python scripts/mvp5/plot_results.py results/mvp5/my-run
```

生成五组 SVG 和 PNG 图。扫描结果是仿真时间，不是宿主机程序耗时；无需重复大量运行取平均。

归档结果位于 `docs/experiments/mvp5_baseline/`，包含 results.json/CSV、逐项验证表、manifest 及图表。原始逐用例日志保留在本机 `results/` 中，不默认入库。
