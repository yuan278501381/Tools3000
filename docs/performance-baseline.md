# Tools3000 性能基线协议

本文定义可重复的测量方法，不是性能结果报告。任何“更快”“低内存”或
“零拷贝”结论都必须附带本协议生成的原始数据、设备信息和版本号。

## 数据源

- 宿主 IPC：`perf.getMetrics` 获取最新快照，`perf.getHistory` 获取最多 120 个
  周期快照。数据由 `PerformanceMonitor` 在后台采样；采样不会放在 UI 线程。
- ETW：TraceLogging provider `Tools3000.Performance`
  (`{92C5838F-961B-4D7D-8C31-38A1E92A137B}`)。事件包括 `ProcessSample`、
  `Latency` 和 `Counter`。没有 ETW consumer 时不会写入 ETW payload。
- 进程资源：Private Bytes、Working Set、Handle、GDI 和 USER 对象数。
  这些值是采样值，不应被表述为固定内存占用。

命名指标受到容量限制：最多 64 条延迟序列、64 个绝对计数器和 64 条插件
初始化指标；已有名称持续更新，新名称超出上限时被丢弃。这可避免异常插件
通过不断生成指标名使监控器自身长期增长。

## 固定场景

每次比较应在相同 Windows 版本、显示缩放、分辨率、GPU 驱动、电源策略和
索引库规模下进行，并记录这些环境信息。

| 场景 | 操作 | 记录项 |
| --- | --- | --- |
| 冷启动 | 完全退出 Tools3000 和搜索服务后启动一次 | `startup.core` 延迟、首个 `ProcessSample` |
| 首次截图 | 启动后首次打开截图并等待覆盖层出现 | `screenshot`、`screenshot.window`、`screenshot.freeze`、`screenshot.upload` |
| 搜索首开 | 启动后首次呼出搜索窗 | `search.hostShow`，并注明搜索服务是否已驻留 |
| 录屏 | 相同区域、帧率、时长和编码设置下录制 | `recording.capture`、`recording.conversion`、`recording.encode`、`recording.pipeline`、`recording.outputFrames`、`recording.droppedFrames` |
| 空闲泄漏 | 空闲 10 分钟，至少采集首尾快照 | Private Bytes、GDI、USER、Handle 的差值 |
| 覆盖层泄漏 | 重复打开并关闭截图覆盖层 100 次 | 操作前后 Private Bytes、GDI、USER、Handle 的差值 |

录屏丢帧率以 `droppedFrames / (outputFrames + droppedFrames)` 计算；若分母为零，
该轮无效。不得以单独的丢帧数比较不同录制时长。

## 采集步骤

1. 冷启动可由 `scripts/collect-performance-baseline.ps1 -Action MeasureColdStart -Iterations 5`
   自动执行。它拒绝接管正在运行的 Tools3000，调用程序的显式、静默基准模式并保留每轮原始 JSON 与 P50/P95/max 汇总；该模式只测 `startup.core`，不把进程创建时间冒充为界面就绪时间。
2. 搜索首开可由 `scripts/collect-performance-baseline.ps1 -Action MeasureSearchFirstOpen -Iterations 5`
   自动执行。它启动一个独立的静默基准进程，走真实 `SearchWindow::show` 路径并记录
   `search.hostShow`，随后自行隐藏并退出；若已有 Tools3000 会话则拒绝运行。该指标是宿主
   窗口显示耗时，不代表 WebView 内容或搜索结果已经完成渲染。
3. 使用 `scripts/collect-performance-baseline.ps1 -Action Start` 启动 ETW 与资源
   采样。脚本只会停止它自己创建并在 `session.json` 中登记的采样进程和 ETW
   会话；结束时执行相同路径的 `-Action Stop`，保留 `.etl`、CSV、`machine.json`
   和资源 P50/P95/max/泄漏差值的 `summary.json`。每次会话使用唯一 ETW 名称，
   不会删除其他采集任务。
4. 记录 Git revision、构建配置、设备和显示器 DPI；关闭其他会造成显著负载的
   前台程序。
5. 在 WPA 或 PerfView
   中保留事件时间线。ETW 会话权限受系统策略影响，失败时仍可用 IPC 快照完成
   功能测量，但报告必须标注“未采集 ETW”。
6. 对每个场景先执行一次预热轮，再执行至少五次正式轮次。报告中给出每轮原始值
   与中位数、p95；不要只报告最佳值。
7. 对内存和 GDI/USER 计数比较首尾差值，并保留完整 `perf.getHistory` 输出，便于
   判断增长是否持续而非短暂缓存波动。
8. 若某项回归，附带 ETW 时间线、场景参数和原始快照；确认根因后再调整阈值或实现。

当前代码提供观测点和可重复协议，但尚未产生跨硬件的性能承诺。
