# 录屏 GPU 帧路径：渐进式实施与基准

## 现状与约束

当前录屏已使用 DXGI/WGC 获取桌面帧，并在 GPU 路径不可用时回退到 GDI；随后仍会把帧转换为 CPU 像素交给现有 FFmpeg 编码管道。该路径可靠，不能在没有可验证回退的情况下删除或替换。

当前代码状态：`RecordingGpuProbe` 仅在显式的 `experimentalGpuEncoding`
开关下、由录制工作线程执行能力探测并记录 D3D11 adapter LUID/feature
level、硬件 MFT 与 FFmpeg D3D11VA 编译支持。它不会向编码器提交 GPU 帧，也
不会改变当前会话的 CPU 管道；因此这不是“零拷贝”实现，也没有性能结论。

本计划的目标不是宣称“零拷贝”，而是逐步减少 **DXGI 到编码器之间的 CPU 读回与颜色转换**。最终是否采用某条路径，必须以目标硬件上的测量结果为准。

## 分层设计

```text
Desktop Duplication / WGC
          │ ID3D11Texture2D
          ├─ A. 当前可靠路径：GPU → staging → CPU BGR/BGRA → FFmpeg software/hardware encoder
          ├─ B. Media Foundation：共享 D3D11 设备 → DXGI surface sample → H.264/HEVC MFT
          └─ C. FFmpeg hardware frames：D3D11VA frames context → hardware encoder
                                      │
                                  任一步失败
                                      ▼
                         A. 当前 CPU 管道（同一时间戳与音频时钟）
```

`CaptureBackend` 继续是唯一桌面采集抽象；新增的编码实现只能消费 `ID3D11Texture2D`，不得把 COM/D3D 细节泄露到 UI、插件边界或音频线程。编码器选择应是会话开始时的不可变决定，避免录制中跨线程切换资源所有权。

## 实施顺序与退出条件

1. **能力探测（不改编码）**：在录制开始时记录适配器 LUID、D3D11 feature level、MFT/FFmpeg D3D11VA 可用性和失败 HRESULT，不记录窗口标题、路径或像素内容。
2. **Media Foundation 实验实现**：只在显式实验开关下启用，使用与采集共享的 D3D11 device；初始化、`ProcessInput`、`ProcessOutput` 任一失败立即停止实验实现并在下一次会话选择 CPU 管道。
3. **FFmpeg D3D11VA 实验实现**：单独的 encoder backend，不复用 MF 的 COM 对象。完成设备丢失、显示器热插拔、RDP、HDR/SDR 格式变化和编码器不可用测试后才可默认启用。
4. **默认策略**：仅当目标配置达到下列基准门槛、稳定性测试通过且 CPU 路径仍可用时，才按适配器白名单逐步放量；不以营销措辞替代数据。

## 基准协议

每条路径在同一台机器、相同分辨率/FPS/码率、关闭其他 GPU 密集应用的条件下，至少录制 3 次 120 秒。分别覆盖 1080p60、1440p60、4K60；若硬件不支持 4K60，结果标记为不适用而非失败。

记录到 `PerformanceMonitor`/ETW 的数据：

- `recording.capture`、`recording.conversion`、`recording.encode`、`recording.pipeline` 的均值、P95、最大值；
- 目标 FPS、编码帧数、主动跳帧数、音频丢帧数、A/V 时间戳漂移；
- 进程 private bytes、working set、CPU%、GPU engine utilization，以及 GDI/USER/handle 的起止差；
- GPU device removed/reset、回退次数和回退 HRESULT；
- 输出文件可解码性、首尾音画同步和连续帧时间戳单调性。

候选路径相对于当前 CPU 管道必须在相同画质设置下 **不增加丢帧、不增加 P95 pipeline 延迟、不出现资源计数增长**；性能收益仅在满足这些正确性门槛后才纳入决策。原始 CSV/ETL 与机器配置须随发布候选一同归档。

## 线程与生命周期

- 编码 backend 由 `ScreenRecorder` 唯一拥有；录制线程退出前先停止接收帧、flush encoder、释放 GPU frame，再释放 D3D11/MF/FFmpeg context。
- 设备丢失或取消只发出停止请求；join 在 recorder owner 内完成，禁止 detached worker。
- UI 只订阅状态和统计快照，绝不等待 GPU flush 或执行设备创建。
- 回退由录制线程串行执行，事件回调不得持有编码器锁，避免 UI/音频/采集线程死锁。
