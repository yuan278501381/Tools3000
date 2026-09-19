# Tools3000 工程活记忆 (Project Memory)

> Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved. | MIT License

---

## 1. 核心架构决策与业务暗坑记录 (Architectural Decisions & Pitfalls)

### [2026-09-18] 鼠标手势轨迹零延迟跟手调优门禁化
- **背景与痛点**：
  对标 WGestures2 的极致跟手体验，此前在 `GestureTrailOverlay` 中引入了即时唤醒渲染、最高线程优先级（`THREAD_PRIORITY_HIGHEST`）及物理光标尖端原子同步插值技术。为防止后续重构引发性能退化，需将上述规则形式化为纯策略逻辑并建立防回退自动化单测。
- **架构方案**：
  - 在 `src/gesture/GestureInputPolicy.h` 中形式化定义纯策略函数：
    - `gestureRenderThreadPriority()`：保证渲染线程恒定为 `THREAD_PRIORITY_HIGHEST`；
    - `gestureShouldWakeRenderImmediately(bool hasHwnd)`：持有效窗口句柄时立即唤醒渲染，消除时钟轮询延迟；
    - `gestureShouldInterpolateCursorTip(...)`：尾部原子同步插值策略，当最新光标距离最后一个采样点大于最小阈值时，补齐尖端原子点消除视觉滞后。
  - 在 `src/gesture/GestureTrailOverlay.cpp` 中全面接入上述策略。
  - 在 `tests/unit/test_gesture.inc` 中挂载 `GestureZeroLatencyPipelineTest`（3 个专项测试），实现持续集成单向防回退守护。

---

### [2026-09-18] 截图标注选区原图像变形根除方案
- **背景与痛点**：
  用户在截图标注模式下，选区内的图像出现拉伸、压缩或畸变变形。
- **根因深度分析**：
  1. **DWM 窗口阴影边界溢出与相交裁剪不对齐**：Windows DWM 窗口通常包含负向不可见阴影边界（如 `-8, -8, 1928, 1088`）。旧逻辑直接对冻结屏幕做相交裁剪得到 `1920x1080` 的底图，但 Direct2D 渲染目标矩形却按选区原始宽高 `1936x1096` 进行 `DrawBitmap` 绘制，导致底图被拉伸 16 像素并产生 8 像素偏移。
  2. **选区微调与窗口二次吸附底图未重构**：底图就绪状态仅为一个全局布尔标志 `markupBaseReady`。当用户拖拽手柄微调尺寸或吸附新窗口时，底图未随新选区重新构建，导致旧尺寸底图被拉伸填充至新选区。
  3. **采样模式与双线性插值模糊**：D2D 位图绘制时缺省源矩形与整数对齐，在像素边界触发线性插值导致边缘模糊虚化。
- **架构治理与单一事实源（SSOT）**：
  - 新增 `src/capture/MarkupBaseHelper.h` 纯算法工具库：
    - `cropMarkupBase(frozenScreen, targetRect)`：严格构建 `targetRect.size()` 尺寸的无损画布，将相交部分精准拷贝至对应局部偏移位置，屏幕外区域安全补零，保证物理像素比例恒为 1.0；
    - `syncMarkupBase(...)`：统筹底图更新、图元平移与缓存失效的单一事实源（SSOT），在 `CaptureInput` 与 `CaptureOverlay` 间消除重复逻辑；
    - `calculateMarkupPoint(screenPoint, targetRect)`：提供屏幕物理坐标向画布局部坐标的 1:1 映射；
    - `calculateMarkupDestRect` / `calculateMarkupSrcRect`：计算 Direct2D 1:1 目标与源矩形；
    - `clampToScreenBounds`：安全夹取窗口物理边界，并在 `detectWindowHierarchy` 中真实接入生产路径。
  - **放大镜边界等比保护算法 (`MagnifierHandler::render`)**：
    - 针对放大镜靠近画布边界时非正方形 ROI 被 `cv::resize` 挤压畸变的严重暗坑，引入正方形等比源补丁画布 `srcPatch`，将有效采样区居中放置后再等比放大，实现严格 1:1 宽高比与绝对坐标对齐。
  - 在 `CaptureState` 引入 `markupBaseRect`，在 `prepareMarkupBase` 中感知选区尺寸及位置变更，若发生位移/尺寸变化自动重建底图并对已有图元应用 `translateAll` 偏移补偿。
  - 在 `CaptureRenderer::drawMarkupPreview` 中，明确指定 1:1 源矩形与目标矩形，采用 `D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR` 并结合复用型 `m_markupClipLayer` 几何图层进行圆角裁剪，消除每帧重复创建图层的开销，彻底消除拉伸畸变与边缘虚化。
  - 在 `tests/unit/test_capture.inc` 中增加 `CaptureMarkupImageDeformationTest`（8 个专项测试），全面覆盖越界裁剪、1:1 坐标映射、D2D 贴图矩形计算、屏幕边界夹取、底图平移生命周期、`syncMarkupBase` 单一事实源、多显示器负坐标映射以及 `MagnifierBoundaryZeroDistortion` 边界等比防御。
  - 在 `tests/unit/test_gesture.inc` 中挂载 `GestureZeroLatencyPipelineTest`（4 个专项测试，含 `HighFrequencyHookNoWakeDecimation` 1000Hz 高频零时钟截断门禁），实现持续集成单向防回退守护。

---

### [2026-09-18] 截图标注手柄拉伸反向根治与全场景剪贴板粘贴治理
- **背景与痛点**：
  1. 用户反馈截图方框标注拉动手柄时，行为反向（拉大变缩小，拉小变放大）；
  2. 逆向画框（`startPt > endPt`）导致手柄物理位置与图元逻辑坐标错位；
  3. 复制截图后在 Windows 资源管理器粘贴无效，在微信聊天窗口粘贴偶尔异常或为纯黑底；
  4. 复制操作在某些流程下会连带触发贴图置顶。
- **根因分析**：
  1. `DefaultBoxHandler::resize` 原逻辑使用通用 `dx/dy` 计算偏移，未区分正向与反向坐标系，导致拖拽左/上手柄时向外拉（dx<0）反而使矩形缩小；
  2. 微信优先消费 `CF_DIB`，若先注入带透明通道的 `CF_DIBV5`，部分 Win32 应用会误将透明区域解析为全黑；
  3. 资源管理器 `CF_HDROP` 写盘使用非宽字符 CRT `fopen`，在中文或特殊字符 AppData 路径下静默失败；
  4. 选区二次调整中 `adjustSelection` 冗余平移导致图元产生 2 倍位移漂移。
- **架构方案**：
  1. **手柄拉伸几何彻底重构 (`DefaultBoxHandler::resize`)**：
     - 统一归一化提取图元物理边界 `l = min(start.x, end.x), r = max(start.x, end.x), t = min(start.y, end.y), b = max(start.y, end.y)`；
     - 针对 8 个物理手柄（LT, T, RT, R, RB, B, LB, L）分别独立调节对应的边界坐标；
     - 引入 `kMinSize = 4` 物理尺寸夹取保护，防止边界倒塌与坐标轴翻转，写回 `startPt = (l, t), endPt = (r, b)`；
     - 在 `drawRectangle`, `drawEllipse`, `drawHighlight`, `applyMosaic` 等初始构造时即保证 `startPt <= endPt` 物理归一化；
     - 在 `RectangleHandler::resize` 中增加 `cornerRadius <= min(w, h) * 0.5f` 边界防御；
     - 在 `PenHandler::resize` 中基于外接包围盒对画笔轨迹 `penPoints` 实施等比例物理缩放映射。
  2. **剪贴板全场景兼容与写盘加固 (`ClipboardUtils`)**：
     - 调整剪贴板注入顺序：优先注入 24 位白底合并的 `CF_DIB` 与 `CF_BITMAP`，再注入 32 位 `CF_DIBV5`，确保微信、Office 等 Win32 GDI 程序无损粘贴且杜绝透明黑底；
     - 使用 `cv::imencode` 与 `tools3000::common::atomicWriteBinaryFileWithFlush` 宽字符原子写盘，生成带毫秒时间戳的规范命名文件 `Tools3000_YYYYMMDD_HHMMSS_xxx.png`，彻底消除中文 AppData 路径下 CRT 失败问题；
     - 写入 `CF_HDROP` 保证 Windows 资源管理器直接 Ctrl+V 无缝生成规范命名图片文件。
  3. **交互与工具栏解耦**：
     - 在工具栏增加独立的“复制到剪贴板”按钮（`ToolbarCommand::Copy`），明确区分“复制到剪贴板 (Ctrl+C)”与“贴图置顶 (Ctrl+T)”及“完成截图 (Enter)”；
     - 消除选区移动中 `adjustSelection` 的二次冗余平移，保证图元与选区 1:1 绝对同步。
  4. **全套自动化防回退单元测试与手柄优先级保障**：
     - **手柄命中测试优先级防劫持**：在 `RectangleHandler::hitTest` 中明确划分优先级（外框 8 方向拉伸手柄 > 内角圆角调节把手 > 图元 Body），彻底防止矩形四角内向微晶圆角把手及对角线导轨容差误拦截 LT/RT/RB/LB 外框拉伸手柄；
     - **关键链路结构化日志跟踪**：在 `DefaultBoxHandler::resize` 与 `CaptureInput::handleMessage`（LBUTTONDOWN / MOUSEMOVE）中增补纯英文结构化诊断日志，支持精准定位手柄命中区域、位移与边界重算；
     - 在 `tests/unit/test_capture.inc` 补充 `CaptureMarkupResizeTest`（4 个测试，含 `RectangleHandlerHitTestPriorityOverCornerRadius`）与 `ClipboardUtilsTest.TimestampedHdropFileNamePatternAndContent`，覆盖 8 手柄方向拉伸、圆角约束、手柄优先级防劫持、样条画笔缩放与时间戳落盘验证，74 项 capture 测试与 336 项全量测试 100% 通过。

---

### [2026-09-19] 截图标注 8 方向控制手柄几何引擎统一重构与文字缩放正交防漂移治理
- **背景与痛点**：
  用户反馈在截图标注模式下，文字标注调整大小方框行为异常：“往右拉，它往下；全面检查方框、椭圆等标注工具的拉大放小，做成统一管理、可复用、单一职责、多态的体系”。
- **根因深度分析**：
  1. **文字缩放单向扩展与垂直中心脱节**：`TextHandler::resize` 原逻辑在拉伸右侧手柄（`HitArea::R`）时，根据 `dx` 增加 `fontSize`，但基准点 `startPt`（左上角）保持未动。随着字体变大，文本渲染高度 `bh` 随之剧增，底部向下大幅延伸，导致右侧中点手柄 `(x + w, y + h / 2)` 的 Y 坐标不断向下漂移，形成用户痛陈的“往右拉它往下”现象。同理，左拉、上拉、下拉均存在基准边和中心轴正交错位。
  2. **手柄点击误触发文本编辑态**：在 `CaptureInput.cpp` 的 `WM_LBUTTONDOWN` 中，命中处于激活态的文字元素的手柄时，误将 `isEditing` 设为 `true`，导致用户拖拽缩放手柄时界面触发文本输入闪烁光标与虚线编辑框。
  3. **手柄悬浮光标状态错位**：在 `updateHoverCursor` 中，未激活文字元素只要被鼠标经过，便无条件返回 `IDC_IBEAM`，导致悬浮于文字缩放手柄上方时未能显示正确的双向拉伸光标（`IDC_SIZEWE` 等）。
  4. **各图元手柄管理逻辑重复面条化**：`RectangleHandler`、`EllipseHandler`、`PenHandler` 以及 `DefaultBoxHandler` 各自重复硬编码了 8 个手柄的坐标计算与命中测试循环。
- **架构方案与单一事实源（SSOT）**：
  1. **建立统一单一职责手柄几何引擎 (`HandleGeometry`)**：
     - 单一事实源管理 8 方向手柄（LT, T, RT, R, RB, B, LB, L）的几何拓扑；
     - `getBoxHandles(bbox)`：统一定义并计算 8 个控制点；
     - `hitTestHandles(bbox, pt, hw)`：两阶段判定（4 个角点手柄优先，4 个边中点手柄次之），消除尺寸极端微小时角边重叠的误判；
     - `renderBoxHandles(canvas, bbox)`：统一以微晶高反差白底加主色边框绘制 8 方向控制手柄与包围盒细框；
     - `computeResizedRect(originalRect, dx, dy, handle, minW, minH)`：通用 8 方向拉伸计算，统一内置 `minW`/`minH` 防倒塌与防翻转夹取。
  2. **基类多态复用 (`DefaultBoxHandler`)**：
     - `DefaultBoxHandler` 全面接入 `HandleGeometry`，矩形、椭圆、画笔、高亮、马赛克、模糊、聚光灯、水印、智能消除全面复用统一的手柄几何与缩放管线；
     - `RectangleHandler`、`EllipseHandler`、`PenHandler` 消除各自私有重复的 8 方向控制点数组与循环，通过 `hitTestHandles` / `computeResizedRect` 多态分发。
  3. **文字缩放几何绝对锚定与零漂移算法 (`TextHandler::resize`)**：
     - 引入正交轴几何对称锚定：
       - `HitArea::R`：左边固定，垂直中心绝对锚定（`startPt.y -= dH / 2`），彻底消灭向右拉伸时文字与手柄向下漂移的致命问题；
       - `HitArea::L`：右边固定，垂直中心绝对锚定（`startPt.x -= dW; startPt.y -= dH / 2`）；
       - `HitArea::B`：顶边固定，水平中心绝对锚定（`startPt.x -= dW / 2`），彻底消灭向下拉伸时文字向右漂移；
       - `HitArea::T`：底边固定，水平中心绝对锚定（`startPt.x -= dW / 2; startPt.y -= dH`）；
       - 4 个角手柄（RB, LT, RT, LB）：精确锚定对应对角顶点，文字按几何对角线自由等比缩放；
     - 同步维护 `element.endPt = startPt + (bw, bh)`，保证文本图元在全库交互中形成合法二维包围盒。
  4. **鼠标交互与文本编辑态解耦 (`CaptureInput`)**：
     - 在 `WM_LBUTTONDOWN` 与元素选中逻辑中，严格仅当 `hitArea == HitArea::Body` 时激活文本编辑态（`isEditing = true`）；若命中手柄（`!= HitArea::Body`），则置 `isEditing = false`，仅执行几何尺寸拉伸；
     - 在 `updateHoverCursor` 中，仅在命中文字 Body 时展示 `IDC_IBEAM`，命中 8 方向手柄时准确展示 `IDC_SIZEWE` / `IDC_SIZENS` / `IDC_SIZENWSE` / `IDC_SIZENESW` 双向拉伸光标。
  5. **自动化质量门禁与防回退断言**：
     - 在 `tests/unit/test_capture.inc` 中挂载：
       - `CaptureMarkupResizeTest.HandleGeometryEngineContractAndClamping`：验证 8 方向手柄几何与尺寸夹取契约；
       - `CaptureMarkupResizeTest.TextHandlerEightDirectionZeroDriftAnchoring`：数学级断言文字在 8 方向拉伸下锚点边与对称中心的绝对不变性（`EXPECT_NEAR(centerY, initialCenterY, 1.0)`）；
       - `CaptureMarkupResizeTest.TextElementActiveHandlesHitTestAndNonEditingCaret`：验证手柄命中分类与包围盒同步。
     - 全量 339 项单元测试（原生 C++ `Tools3000Tests.exe`）与前端 6 重门禁（`npm run lint`）100% 满分通过。

---

### [2026-09-19] 卸载全链路卡顿根因排查与毫秒级极速卸载架构方案
- **背景与痛点**：
  用户反馈卸载 Tools3000 时极其缓慢，基准实测高达 26.93 秒（安装仅需 4 秒，卸载却慢 7 倍），严重影响体验。
- **根因深度分析**：
  1. **卸载器自身自锁假死死循环 (耗时 22 秒+，贡献 82% 延迟)**：`installer.iss` 中的 `CheckDirFilesLocked` 遍历 `{app}` 目录下全量 `.dll` 与 `.exe`。由于 `unins000.exe` 为卸载器可执行文件自身（正被系统进程加载并内存映射为 `SEC_IMAGE`），`CreateFile(..., GENERIC_READ or GENERIC_WRITE, 0, ...)` 排他写打开恒定失败（错误码 32 共享冲突），导致 `AreAppFilesLocked()` 恒返回 `True`。`WaitForProcessesAndFilesReleased(3000)` 无论如何都会死等到超时；
  2. **时钟轮询伪递增与 `tasklist` 进程枚举风暴**：`WaitForProcessesAndFilesReleased` 每次循环使用 `Elapsed := Elapsed + IntervalMs (100)` 伪递增，但循环体内针对 `Tools3000.exe` 与 `Tools3000_Service.exe` 反复执行 `cmd.exe /c "tasklist /FI ... | findstr ..."`。在 Windows 下每次启动 `cmd.exe` + `tasklist.exe` + `findstr.exe` 耗时约 350ms，单次循环实际墙上耗时超 700ms，导致原本标称 3 秒的等待被拉长至 11.3 秒，并在 `InitializeUninstall` 与 `CurUninstallStepChanged(usUninstall)` 各重复触发一次（11.3s + 10.7s = 22 秒纯死等）；
  3. **服务存在性与活动态混淆**：`InitializeUninstall` 中错误判断 `ServiceExists()`（服务注册表项是否存在，安装后恒为 `True`），导致即便 Tools3000 与服务完全未启动，也在初始化阶段强行执行停机杀进程与 11 秒轮询；
  4. **`[UninstallRun]` 二次加载宿主 EXE 破坏文件删除**：旧逻辑在 `[UninstallRun]` 中启动 `{app}\Tools3000.exe --unregister-autostart`，导致刚被杀死的宿主程序又被重新拉起并占用 DLL，影响文件删除并增加 1~2 秒冷启动开销；
  5. **CLI 脚本盲等与空转执行**：`uninstall.ps1` 原逻辑无条件循环执行 `Start-Sleep 300ms`，且在毫无活动实例时依旧空转执行 `sc.exe stop` 与 `taskkill`（产生 750ms 空白延迟）。
- **世界级高精度架构治理方案**：
  1. **卸载器自锁排除防御**：在 `CheckDirFilesLocked` 中显式排除 `Pos('unins', Lowercase(FindRec.Name)) = 1`，彻底杜绝卸载器自身锁定误报；
  2. **Win32 原生 SCM 内核直查替代进程风暴**：
     - 引入 `advapi32.dll` 原生 API：`OpenSCManagerW`, `OpenServiceW`, `QueryServiceStatus`, `CloseServiceHandle`，以 0.01ms 极速直查服务真实状态，彻底消灭 `tasklist` 进程创建；
     - 主程序状态收敛为“全局互斥体 `CheckForMutexes` (0.001ms) + 隐藏消息窗口句柄 (0.001ms) + 文件排他锁 (0.05ms)”三重硬件级真相探测；
  3. **真实墙上时钟 (`GetTickCount`) 与瞬时退出**：
     - `WaitForProcessesAndFilesReleased` 接入 `GetTickCount`，在无运行实例与无文件锁定时 0ms 瞬时返回退出；
  4. **计划任务原生解耦注销**：
     - 将 `[UninstallRun]` 中的 `--unregister-autostart` 替换为 `schtasks.exe /delete /tn "Tools3000\Autorun for {username}" /f` 与 `schtasks.exe /delete /tn "Tools3000" /f`，在 `usPostUninstall` 同步清理，彻底杜绝卸载期二次拉起主程序；
  5. **CLI 脚本状态嗅探与弹性退出**：
     - `uninstall.ps1` 与 `install.ps1 -Uninstall` 实施活动进程前置嗅探，采用 20ms 弹性轮询退出，并在卸载完成后彻底净化安装根目录残留。
- **实测成果与基准断言**：
  - `unins000.exe` 耗时由 **23,136 ms** 直降至 **1,482 ms**（提速 **15.6 倍**，下降 **94%**）；
  - `uninstall.ps1` 全生命周期由 **26.93 秒** 直降至 **2.31 秒**（提速 **11.6 倍**，下降 **91%**）；
  - 活动实例强退卸载（Tools3000 正在运行时卸载）实测仅耗时 **2.37 秒**；
  - 344 项原生单元测试全通，前端 7 重多语言、排版、日志、内存门禁 100% 满分通过。

---

### [2026-09-19] 卸载全链路二次审查与零子进程内联清理工程治理
- **背景与痛点**：
  在首轮卸载性能优化后，二次深度审查发现若干隐蔽架构缺陷与边缘暗坑：
  1. Inno Setup 卸载器日志中频繁出现 `Failed to delete directory (145)`（目录非空），导致从 Windows 设置/控制面板卸载时安装根目录遗留残留；
  2. `[UninstallRun]` 与 `CurUninstallStepChanged(usPostUninstall)` 重复拉起外部 `schtasks.exe` 与 `sc.exe` 高达 4 次，其中 `schtasks /delete /tn "Tools3000"` 恒定报错（目录名非任务名）；
  3. `[UninstallRun]` 中异步无等待拉起 `sc.exe delete` 存在 SCM `ERROR_SERVICE_MARKED_FOR_DELETE` 竞态悬空隐患；
  4. `IsFileLocked` 将只读属性等错误码（如 `ERROR_ACCESS_DENIED` 5）误判为文件锁定，且 `IsTools3000Running` 耦合文件锁导致杀软读扫描时误报“程序正在运行”并弹出拦截对话框；
  5. CLI 脚本在托盘无窗模式下对 `CloseMainWindow()` 执行 150ms 盲等超时，且在缺失 `unins000.exe` 时无法深度清理绿色/便携服务与计划任务。
- **根因深度分析与世界级治理方案**：
  1. **安装期动态生成文件补全卸载清单**：
     - 安装期由 Pascal 脚本生成的 `initial_modules.json` 未登记在 Inno Setup `[Files]` 表中，导致卸载器默认跳过该文件并不空目录；
     - 在 `[UninstallDelete]` 中补齐 `Type: files; Name: "{app}\initial_modules.json"` 以及 `dirifempty` 级联清理，实现纯原生 Windows 卸载 100% 干净移除 `{app}` 物理根目录；
  2. **全面废除 `[UninstallRun]` 外部进程风暴，统一接入 Win32 原生 SCM API**：
     - 在 `advapi32.dll` 引入 `ControlService` 与 `DeleteService`，在 `usUninstall` 阶段（物理删文件前）内联以 0.05ms 极速下发服务终止与标记删除，彻底消除异步竞态与文件删除锁定；
     - 彻底清空 `[UninstallRun]` 区域，消灭多余的外部进程创建；
  3. **TaskCache Tree 注册表前置嗅探与单次通配符注销**：
     - 检查 `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Schedule\TaskCache\Tree\Tools3000`，未启用自启动时 0ms 瞬时跳过；
     - 存在任务时，以 `schtasks.exe /delete /tn "Tools3000\*" /f` 单次调用彻底注销该目录下所有用户的所有子任务，并净化 Tree 根项，消灭重复调用与无效任务名错误；
  4. **精确错误码断言 (`ERROR_SHARING_VIOLATION` 32 / `ERROR_LOCK_VIOLATION` 33)**：
     - `IsFileLocked` 仅在 Win32 返回 32 或 33 时判定为活跃锁定，阻断只读文件导致的假死超时；
     - `IsTools3000Running` 严格收敛于“互斥体 + 消息窗口句柄”，彻底解耦文件读句柄扫描引起的误报弹窗；
  5. **CLI 极速强退与缺失卸载器兜底**：
     - 优化 `uninstall.ps1` 进程清理流程，去除无主窗时的 150ms 轮询等待；
     - 增加卸载器未找到时的自愈深度清理（自动拔除服务、任务计划与注册表项）；
     - 加固 `uninstall.cmd` 支持 `pwsh` 缺失时优雅回退至 `powershell.exe`。
- **实测成果与基准断言**：
  - `unins000.exe` 原生卸载实测总耗时压减至 **1,962 ms**，目录完全清空（0 残留，`Removed all? Yes`）；
  - `uninstall.ps1` 运行中实例卸载实测压减至 **2,224 ms**；
  - `install.ps1 -Uninstall` 实测压减至 **2,728 ms**；
  - `uninstall.cmd` 纯 CMD 环境秒级执行通过；
  - 344 项原生单元测试与前端 7 重防护门禁 100% 满分通过，ISCC 安装包编译 100% 零警告通过。

---

### [2026-09-19] 卸载全链路 COM 原生化与计划任务/重启管理器零外部进程治理
- **背景与痛点**：
  在第三轮深度实机审计中，发现前序方案中仍存在若干严重隐蔽缺陷与耗时毛刺：
  1. **计划任务通配符报错与注册表破坏暗坑**：`schtasks.exe` 原生不支持 `/tn "Tools3000\*"` 通配符删除，执行恒定返回 1 并报错 `The system cannot find the file specified`。前序方案随后直接调用 `RegDeleteKeyIncludingSubkeys(HKLM, '...TaskCache\Tree\Tools3000')`，导致磁盘 `C:\Windows\System32\Tasks\Tools3000` 物理 XML 文件与 `TaskCache\Tasks\{GUID}` 孤立悬空，触发 Windows 任务计划管理控制台 (MMC `taskschd.msc`) 报错“任务映像已损坏或已被篡改”；
  2. **已停止服务无条件触发外部 `taskkill` 产生 590ms 进程风暴**：在 `DeleteSearchServiceNative` 中，即便搜索服务已停止或已被标删，末尾仍无条件执行 `taskkill.exe /f /t /im Tools3000_Service.exe`，单次外部进程启动与全系统扫描白白浪费 590ms；
  3. **Windows 重启管理器 (Restart Manager) 启动暗藏 1000ms 延迟**：Inno Setup `CloseApplications=yes` 会在卸载初期拉起 Windows Restart Manager 并对全量登记文件做系统级句柄扫描，引入约 974ms 纯启动等待；由于 Tools3000 已内置原生互斥体与消息窗口精确退出管线，该机制纯属冗余；
  4. **`initial_modules.json` 卸载期阶段错位触发 Error 145 目录非空重试**：安装期由 Pascal 脚本生成的 `initial_modules.json` 在 `[UninstallDelete]` 阶段删除时，落后于 Inno Setup 内部文件删除后的首次目录清理尝试，导致产生 `Failed to delete directory (145)` 并在删除前等待 500ms 重试。
- **世界级高精度架构治理方案**：
  1. **Task Scheduler 2.0 原生 COM 接口全链路接入 (`Schedule.Service`)**：
     - 在 Inno Setup Pascal 脚本与 PowerShell CLI（`uninstall.ps1`、`install.ps1`）中，统一引入 Windows 原生 `Schedule.Service` COM 对象；
     - 递归枚举 `\Tools3000` 目录下全量任务执行 `DeleteTask`，随后调用 `RootFolder.DeleteFolder("Tools3000", 0)` 彻底净化任务目录；
     - 0 外部子进程创建，耗时从数百毫秒暴降至 11ms，且 100% 杜绝手动篡改 `TaskCache\Tree` 注册表带来的系统级数据库损坏风险；COM 异常时优雅回退至单用户精确任务注销；
  2. **服务进程清理前置守卫 (Guarded Process Kills)**：
     - 在 `StopSearchServiceNative` 与 `DeleteSearchServiceNative` 中，仅在 `IsSearchServiceRunning()` 或服务二进制文件被锁定时才执行兜底 `taskkill`，常态下 0 外部进程开销；
  3. **剔除冗余重启管理器开销 (`CloseApplications=no`)**：
     - 显式声明 `CloseApplications=no`，卸载器启动至准备阶段耗时从 1400ms 缩短至 437ms；
  4. **`initial_modules.json` 在 `usUninstall` 阶段前置物理删除**：
     - 在 `usUninstall` 步骤（Inno Setup 执行默认文件删除之前）优先调用 `DeleteFile` 清理动态生成的 `initial_modules.json`，彻底抹平 Inno Setup 内部阶段目录清理竞争与 500ms 重试盲等。
- **实测成果与基准断言**：
  - 运行中实例强退卸载全生命周期实测从 2.37 秒进一步压降至 **1,866 ms**（1.8 秒）；
  - 纯卸载准备与清理阶段（含实例终止、服务标删与计划任务 COM 移除）仅耗时 **437 ms**；
  - 计划任务 COM 移除耗时 **11 ms**，注册表与任务数据库 100% 保持原生一致性；
  - `uninstall.ps1` 全流程实测 **2,910 ms**，`uninstall.cmd` 实测 **2,711 ms**；
  - 全量 344 项原生单元测试与前端 7 重防护门禁 100% 满分通过，零 Emoji 红线 100% 遵从。

---

### [2026-09-19] 桌面文件拖拽原生穿透强保护、VK_F24 中立脉冲修饰键自愈与手势捕获加固
- **背景与痛点**：
  1. 用户在 Windows 桌面、资源管理器（Explorer）及通用文件对话框中，进行图标框选、文件拖拽、右键移动或快速点击时，偶发遭遇左键被手势状态机误拦截、光标捕获悬空或文件拖拽被锁死；
  2. 在手势操作、快捷键模拟或前台焦点切换过程中，由于 Windows 将独立的 Alt 释放事件识别为激活窗口主菜单栏（`SC_KEYMENU` / 菜单模态），导致后续鼠标点击与 OLE DragDrop 拖拽操作全域瘫痪；
  3. 轮盘菜单（`RadialMenuOverlay`）在捕获转移时未能主动感知隐藏，`hide()` 盲目调用 `ReleaseCapture()` 存在误释放其他前台窗口捕获的风险。
- **架构方案与单一事实源治理**：
  1. **桌面与文件管理器原生穿透强保护 (`isDesktopOrFileManagerWindow`)**：
     - 在 `src/gesture/GestureInputPolicy.h` 中形式化收敛 `isDesktopOrFileManagerWindow(cls)`，精准识别系统桌面（Progman/WorkerW/SHELLDLL_DefView/SysListView32）、资源管理器（CabinetWClass/ExploreWClass/DirectUIHWND）、通用文件对话框（#32770）及外壳任务栏窗口；
     - 在 `MouseHook.cpp` 的 `WM_LBUTTONDOWN` 中，若光标落于上述窗口，一律强制阻断左键手势拦截（`canStartGesture = false`），保障人类桌面图标框选、移动与文件拖拽 100% 原生穿透；
     - 细化 `isLeftButtonGestureAllowed` 边缘滑动规则：仅在对应边缘明确配置了边缘滑动（EdgeSlide）或显式全局开启左键手势时才允许左键拦截；
     - 优化常规左键点击投递策略：仅当此前确有触发键处于按下状态（如右键手势追踪过程中按左键取消自愈）时才向状态机投递 LeftDown，常态 Idle 下不再向手势引擎投递常规左键，彻底消灭双脑失步与状态污染。
  2. **VK_F24 中立无害脉冲防御与修饰键安全自愈 (`emergencyFlushInputState`)**：
     - 针对 Windows Alt 释放激活系统菜单栏（`SC_KEYMENU`）并锁死拖拽的底层暗坑，在 `WinUtils::emergencyFlushInputState`、`DialogNavigator::sendKeyChord`、`RemoteMasterEngine::flushModifiers` 及 `ensureModifierReleased` / `ensureAllModifiersReleased` 中引入 **VK_F24 中立脉冲中和机制**；
     - 释放 Alt 键（VK_MENU / VK_LMENU / VK_RMENU）前先行原子注入无害的 `VK_F24 Down + Up` 脉冲，彻底打破 Windows 菜单激活判定逻辑，保障修饰键释放后绝不激活系统菜单栏或锁死拖拽；
     - 在主程序退出（`shutdownSubsystems`、`MessageWindowProc` 销毁）、系统会话切换（`SystemSessionChangedEvent`）、电源事件（`SystemPowerChangedEvent`）及瞬态 UI 取消（`CancelTransientUiEvent`）中全局接入 `emergencyFlushInputState`，强制执行 `ReleaseCapture`、`ClipCursor(nullptr)` 与修饰键安全自愈。
  3. **手势引擎状态机与轮盘菜单捕获加固**：
     - `GestureEngine::cancelTracking` 与 `endTracking` 强化状态复位（重置 `m_activeTriggerDown`、`m_activeTriggerUp`、`m_gestureEdgeZone`、`m_gestureModifiers`、`m_recognizer`）；
     - `RadialMenuOverlay` 响应 `WM_CAPTURECHANGED`，当鼠标捕获转移时立即隐藏自身，`hide()` 时仅在自身持有捕获时才调用 `ReleaseCapture()`；
     - `GestureEngine::reinjectTriggerClick` 支持 `triggerOverride`，在命中黑名单或全屏独占取消手势时精准补发原始触发键点击。
  4. **全套自动化防回退单元测试**：
     - 在 `tests/unit/test_gesture.inc` 挂载 120+ 行单测（7 个专项测试），全量 351 项原生单元测试与前端 7 重门禁 100% 满分通过。

