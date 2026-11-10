# Project: Tools3000 Mouse Gesture Input & DirectComposition Pipeline Overhaul

> **法定版权**：Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved. | MIT License | [GitHub](https://github.com/yuan278501381)  
> **状态索引**：Living Master Document  
> **时间基准**：2026-09-20T16:45:00Z  

---

## Architecture
```
[物理鼠标硬件 1000Hz (1ms)]
       │
       ▼ (Windows 内核输入子系统)
[WH_MOUSE_LL 钩子 (THREAD_PRIORITY_TIME_CRITICAL)]
       │  (Wait-Free POD 封装, 0 堆分配, 0 锁争用, <10ns)
       ▼
[Input SpscRingBuffer<RawInputPacket, 4096>]
       │
       ▼ (批量排空 drainBatch)
[GestureDispatchWorker 独立线程 (THREAD_PRIORITY_HIGHEST)]
       ├─► 静态预分配 4096 轨迹缓冲 (GestureRecognizer 0 堆扩容)
       ├─► 增量式方向判定与紧凑数值编码
       ├─► 异步前台窗口/类名/全屏独占判定 (无阻塞钩子线程)
       ├─► 即时 Fallback Profile 匹配 (彻底解决单笔画灰态冻结)
       │
       ▼ (Wait-Free 纳秒级写入)
[Render SpscRingBuffer<TrailPoint, 4096>]
       │
       ▼ (物理刷新率 VBlank / QPC 高精度节拍器锁步驱动)
[GestureTrailOverlay::renderLoop (专用渲染线程, MMCSS)]
       ├─► 预分配全虚拟屏 DirectComposition 双缓冲硬件表面 (0 SetWindowPos, 0 CreateSurface)
       ├─► Toast HUD 单一 Visual 树收敛 (Child Visual，废黜独立 m_toastHwnd)
       ├─► GPU 增量局部绘制与脏矩形提交 (BeginDraw(&dirtyRect))
       ├─► 尖端 QPC 纳秒级采样补全 (相位差 < 4ms)
       ├─► DComp 硬件级 SetOpacity 渐变淡出 (0 CPU/GPU 重绘)
       └─► IDCompositionDevice::Commit() 原子翻转提交 DWM
```

---

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| F1 | Wait-Free WH_MOUSE_LL 钩子纯净化 | 钩子热路径消除所有互斥锁、堆分配与 Win32 同步系统调用，纯原子写入 POD 队列返回 | M1 | Survey 1 (R1) |
| F2 | 真实消费的 Input SpscRingBuffer | 激活并扩大单写单读无锁环形缓冲区，彻底解除此前 push 后立即同步执行的假队列 | M1 | Survey 1 (R1) |
| F3 | 专用异步手势分发工作线程 | 引入 GestureDispatchWorker 批量消费点位，解耦窗口判定、全屏检测与状态机 | M1 | Survey 1 (R1) |
| F4 | 识别器零堆扩容与增量方向解析 | GestureRecognizer 内部使用预分配静态缓冲与定长数值方向编码，消除 vector/string 堆锁争用 | M1 | Survey 1 (R1) |
| F5 | 单笔画即时 Fallback 匹配修复 | 修复 lookupProfileAction 中 profile 为空直接返回 nullopt 忽略 fallback 的竞态，首笔画即时响应高亮 | M1 | Survey 3 (HUD) |
| F6 | 预分配虚拟全屏 DComp 双缓冲硬件表面 | 创建时锁定 SM_XVIRTUALSCREEN 全屏尺寸，手势划动全程 0 次 SetWindowPos，0 次 CreateSurface | M2 | Survey 2 (R2) |
| F7 | 单一 Visual 树收敛 Toast HUD | Toast 废黜独立分层窗口 m_toastHwnd，作为 Child Visual 挂载主 Visual 树，消除双窗口 DWM 竞争 | M2 | Survey 2 (R2) |
| F8 | GPU 增量绘制与局部脏矩形提交 | 采用 BeginDraw(&dirtyRect) 仅刷新变动区域，避免每一帧 4K 全屏清屏与全量重新贝塞尔拟合 | M2 | Survey 2 (R2) |
| F9 | DComp 硬件级 Visual Opacity 淡出 | 手势松手后通过 SetOpacity(alpha) 交由 DWM 硬件着色器淡出，消除淡出期 CPU/GPU 循环重绘 | M2 | Survey 2 (R2) |
| F10 | 动态物理显示器刷新率感知 | 通过 MonitorFromPoint 与 EnumDisplaySettingsW 动态获取光标所在屏幕真实刷新率（60Hz~360Hz） | M3 | Survey 3 (R3) |
| F11 | 高精度 QPC 硬件锁步节拍器 | 废黜硬编码 7ms 粗粒度 sleep，基于 QPC 与高精度 Waitable Timer 精确锁步 DWM VBlank 截止点 | M3 | Survey 3 (R3) |
| F12 | 尖端亚毫秒级低延迟补全 | 绘制临门一脚时直接拉取最新物理光标并对齐，尖端采样相位差严格压至 < 4ms | M3 | Survey 3 (R3) |
| F13 | 1000Hz 唤醒系统调用合并与节流 | 在高刷渲染周期活跃期间合并 SetEvent(m_wakeEvent)，消除每秒 1000 次内核模式切换风暴 | M3 | Survey 1 & 3 |
| F14 | 4 级全域 E2E 自动化测试套件构建 | 覆盖 1000Hz 采样、边界极限、多笔画交互、多屏高刷及高负荷真实工作负载 | E2E Track | ORIGINAL_REQUEST |
| F15 | 交付验收与防回退流水线验证 | 通过全量 357+ 项单元测试、前端 7 重门禁、打包 Tools3000-Setup.exe 并执行端到端校验 | Final M | ORIGINAL_REQUEST |

---

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| E2E | E2E Testing Track | 构建 4 层全景手势测试套件（Tiers 1-4），发布 TEST_READY.md | none | DONE |
| M1 | 1000Hz 输入零阻塞采样与无锁分发 | F1, F2, F3, F4, F5 (输入钩子纯净化、SPSC队列、异步分发线程、单笔画即时匹配) | none | DONE |
| M2 | DComp 预分配硬件表面与增量渲染管线 | F6, F7, F8, F9 (全虚拟屏预分配、0 SetWindowPos/0 CreateSurface、单一Visual树、增量脏矩形、硬件淡出) | M1 接口 | DONE |
| M3 | VBlank 硬件锁步与高精度 QPC 节拍器 | F10, F11, F12, F13 (显示器刷新率自适应、QPC高精时钟、尖端<4ms相位对齐、内核唤醒节流) | M2 接口 | DONE |
| Final | 全量 E2E 验证、白盒对抗加固与发版交付 | F14, F15 (100% 通过 Tiers 1-4，Tier 5 对抗覆盖率加固，打包并运行 verify_lifecycle.ps1 / test_gesture_e2e.ps1) | E2E, M1, M2, M3 | DONE |

---

## Interface Contracts

### 1. Hook ↔ GestureDispatchWorker
- **结构体**: `RawInputPacket` (POD, 24 bytes, trivially copyable)
  ```cpp
  struct RawInputPacket {
      uint32_t message;    // WM_MOUSEMOVE, WM_RBUTTONDOWN, etc.
      int32_t x;           // 物理光标 X (绝对坐标)
      int32_t y;           // 物理光标 Y (绝对坐标)
      uint32_t mouseData;  // 滚轮 delta 或 X 键信息
      uint64_t qpcTime;    // 采样时的 QPC 时间戳 (纳秒级)
  };
  ```
- **队列契约**: `tools3000::core::SpscRingBuffer<RawInputPacket, 4096>`
  - 生产者：`WH_MOUSE_LL` 钩子回调线程，单线程推入 `push(packet)`，返回 `bool`（从不等待，满时记录溢出计数）。
  - 消费者：`GestureDispatchWorker` 线程，单线程批量弹出 `drainBatch(buffer, maxBatch)`。

### 2. GestureDispatchWorker ↔ GestureTrailOverlay
- **队列契约**: `tools3000::core::SpscRingBuffer<TrailPoint, 4096>`
  - 生产者：`GestureDispatchWorker` 线程，向渲染队列推入有效手势点位。
  - 消费者：`GestureTrailOverlay::renderLoop` 渲染线程，在 VBlank 周期批量排空。
- **HUD 状态契约**:
  - `GestureTrailOverlay::setLiveAction(const std::wstring& actionText, uint32_t statusFlags)`:
    无锁原子更新或双缓冲槽位更新，彻底解除 `m_trailMutex`。

### 3. Pacing ↔ DirectComposition Presentation
- **结构体**: `DisplayPacingInfo`
  ```cpp
  struct DisplayPacingInfo {
      uint32_t refreshRateHz;       // 60, 120, 144, 240, 360
      uint64_t framePeriodTicks;    // 物理单帧 QPC 周期
      uint64_t targetDeadlineQpc;   // 当前帧 DWM 提交截止点
  };
  ```
- **硬件提交**:
  - `IDCompositionSurface::BeginDraw(&dirtyRect, ...)`
  - `IDCompositionDevice::Commit()` 在截止点前 0.5ms~1.0ms 提交，确保绝对对齐下次硬件垂直同步。

---

## Code Layout
- **核心输入钩子**: `src/core/hotkey/MouseHook.h`, `src/core/hotkey/MouseHook.cpp`
- **手势输入钩子与分发**: `src/gesture/MouseHook.h`, `src/gesture/MouseHook.cpp`, `src/gesture/GestureDispatchWorker.h`, `src/gesture/GestureDispatchWorker.cpp`
- **手势状态机与识别引擎**: `src/gesture/GestureEngine.h`, `src/gesture/GestureEngine.cpp`, `src/gesture/GestureRecognizer.h`, `src/gesture/GestureRecognizer.cpp`, `src/gesture/GestureInputPolicy.h`
- **渲染覆盖层与 DComp 管线**: `src/gesture/GestureTrailOverlay.h`, `src/gesture/GestureTrailOverlay.cpp`, `src/gesture/GesturePacer.h`, `src/gesture/GesturePacer.cpp`
- **C++ 单元测试**: `tests/unit/test_gesture.inc`, `tests/unit/test_gesture_pipeline.cpp`
- **E2E 验证脚本**: `scripts/test_gesture_e2e.ps1`, `scripts/verify_lifecycle.ps1`, `scripts/stress_gesture_1000hz.ps1`
- **打包流水线**: `deploy.ps1`, `scripts/release.ps1`
