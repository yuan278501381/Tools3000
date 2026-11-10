---
trigger: always_on
---

# Tools3000 智能体工程规范与架构记忆 (Engineering Standards & Core Memory)

> **审计基准说明**：遵循本文第 5 节的「已确认产品决策与审计基线 (Accepted Product Decisions & Audit Baseline)」。2026-09-04 项目负责人已明确接受计划任务 Everyone 完全控制与最高权限、CLI 卸载默认删除个人数据、QuickLook 音视频/PDF 预览格式占位及 Lua 可信本地自动化定位等既定决策，严禁在行为未实质改变时将其重复报告为缺陷。搜索服务严格遵循 `DEMAND_START`，仅在用户首次显式呼出搜索时启动，之后在本次 Windows 会话中跨 Tools3000 退出常驻。

---

## 1. 核心定位与极客体验基准 (Core Philosophy & UX Standards)
1. **世界级品质基准**：严格遵循世界级架构、世界级性能、世界级鲁棒性、世界级 UI 与世界级 UX。
2. **人类易用度首位**：每一个功能都要极致考量人类易用度，以世界级的交互体验为基准，方便日常操作与极客调优（因为这本身就是一款桌面效率软件）。
3. **高分屏与缩放适配 (DPI)**：前端、悬浮层与原生窗口必须全面支持高分屏（High-DPI）与低分屏自适应，毫秒级响应并完美兼容 Windows 系统的屏幕物理缩放比例（Scale Factor, 100% ~ 200%+ 混合多屏）。
4. **主题与个性化体系**：全面支持 Tools3000 核心界面与设置页面的深色/浅色主题（Dark / Light Theme）自适应切换，并支持全局动态主题色（Accent Color）。
5. **零 Emoji 与矢量化设计红线 (Zero Emoji & Vector Iconography Standard)**：
   - **严苛红线**：严禁在 UI 状态徽章、提示标签、状态指示器及正文中内嵌 Unicode 彩色 Emoji 字符（如 `📦`、`💻`、`🟢`）；
   - **矢量一致性**：统一采用高精度 Lucide 矢量 SVG 图标，配合半透明微晶玻璃拟态胶囊（Glass Capsule Badges）与 ClearType 次像素渲染，保障原生桌面软件质感。

---

## 2. C++ 后端与系统工程规范 (C++ Backend & System Engineering)
1. **DLL 导出与内联规范 (DLL Export Conventions)**：
   - 当使用导出宏（如 `TOOLS3000CORE_API`、`PLUGIN_EXPORTS`）标记类时，**严禁在头文件中全内联实现非模板方法或静态单例**；
   - 其实例化与具体实现必须下沉至 `.cpp` 源文件中，确保跨 DLL 边界符号正确导出，彻底杜绝跨模块 `LNK2019`（无法解析的外部符号）链接错误。
2. **Windows 系统宏命名冲突规避 (Windows Macro Collisions)**：
   - 严禁定义名为 `MOD_CTRL`、`MOD_ALT`、`MOD_SHIFT`、`MOD_WIN` 等与 `windows.h` 系统宏同名的常量、枚举或变量；
   - 此类命名会触发预处理器宏冲突并导致 `C2059` 语法错误。必须统一使用自定义前缀（如 `MOUSE_MOD_`、`APP_MOD_`）。
3. **极限轻量与物理内存收缩规范 (Working Set Physical Memory Trim - `trimWorkingSet`)**：
   - **触发原则**：遵循“冷路径退场修剪，热操作期间绝不修剪”铁律。主动内存修剪（`tools3000::core::WinUtils::trimWorkingSet()`）严格仅在重型任务结束（截图完成/取消、录屏停止、长截图拼接完成、OCR 识别结束）、重型组件彻底销毁（WebView2 销毁、插件停用）或闲置超时冷路径调用，及时将无用物理内存归还系统工作集；
   - **严苛红线**：严禁在 1000Hz 鼠标钩子、底层键盘钩子、打字按键连击流、手势实时追踪或 60FPS 渲染循环等热路径中调用，彻底杜绝因软缺页中断（Soft Page Faults）引发的微卡顿；避免在常规高频窗口显隐中频繁触发进程级工作集回收；
   - **惰性重载机制**：修剪必须与重型资源（Direct2D 渲染目标、DIB Section、大尺寸 OpenCV Mat 缓冲区）的惰性按需重新初始化相配合。
4. **全链路键盘加速器管线标准 (Keyboard Accelerator Pipeline - `KeyboardPipeline`)**：
   - **Win32 宿主消息拦截**：所有顶层宿主窗口（`SettingsWindow`、`SearchWindow`、`TrayWindow` 等）必须通过 `KeyboardPipeline::filterWindowMessage` 拦截 `SC_KEYMENU`、`SC_CONTEXTHELP` 与 `WM_HELP`，杜绝用户按下 Alt、F10 或 F1 时激活系统原生菜单抢夺焦点；
   - **WebView2 加速器策略抑制**：必须在 `ICoreWebView2Controller` 上挂载 `KeyboardPipeline::applyWebKeyboardPolicy`，屏蔽 Chromium 默认浏览器快捷键（`Ctrl+P`、`Ctrl+F`、`Ctrl+U`、`Ctrl+J`、`Ctrl+H`、`Ctrl+W`、`Alt+Left/Right` 等），保障所有按键组合 100% 透传至前端 React DOM；
   - **录制态全局热键防误触 (Silent Recording Mode)**：进入快捷键录制状态时，前端必须通过 `hotkey.setPaused(true)` 通知后端，底层全局热键自动静默，防止按键录入时在后台误触发已有工具功能。
5. **浮层视口与专注助手全屏避让标准 (Overlay Viewport & Focus Assist Avoidance - `FocusAssistAvoidance`)**：
   - **局部动态最小包围盒优先**：鼠标点击水波纹、移动流光轨迹、手势划线等局部瞬态特效，严禁创建或调整为虚拟全屏窗口；必须按活跃元素并集动态计算局部最小包围盒（100~300px 微型视口），减少 99% 的内存与 GPU 渲染合成开销，并从底层阻断 Windows Shell 误判全屏独占触发专注助手（Focus Assist `🔔z` 免打扰）；
   - **全屏聚光灯暗角几何避让**：必须覆盖全屏的暗角渐变窗口（如 Spotlight 聚光灯暗角），物理窗口尺寸主动缩减 1 像素（如 `vw - 1, vh - 1`），物理打破 Windows `SHQueryUserNotificationState` 的全屏独占几何匹配逻辑。
6. **跨系统无缝通用圆角双保险架构标准 (Universal Rounded Corners Dual Insurance Pipeline - `UniversalRoundedCornersDualInsurance`)**：
   - **跨系统断层机理**：Windows 11 DWM 硬件圆角（`DWMWCP_ROUND`）在 Windows 10、Windows Server 全系列（Server 2019/2022/2025）、精简版系统及远程桌面 (RDP) 会话下会被系统忽略或关闭，导致无边框弹窗（如搜索中心、托盘菜单）暴露 Win32 直角底衬与粗糙阴影裁切块；
   - **双保险终局标准**：所有无边框浮动弹出窗口（`SearchWindow`、`TrayWindow` 等）在几何尺寸变更时必须统一路由至 `tools3000::core::WinUtils::applyUniversalRoundedCorners(hwnd, width, height, radius)`：
     - *第一道防线（Windows 11）*：设置 `DWMWA_WINDOW_CORNER_PREFERENCE` 为 `DWMWCP_ROUND`，享受 GPU 硬件加速超平滑圆角与系统原生高斯深度阴影；
     - *第二道防线（Windows 10 / Server 2022/2025 / RDP）*：调用 Win32 内核级 `CreateRoundRectRgn` 与 `SetWindowRgn` 直接在物理像素层强行裁剪剔除四个直角尖角，彻底抹平全版本跨系统视觉断层；
     - *前端与 DPI 对齐*：前端外壳容器必须 0 padding 紧密贴合窗口边界，圆角半径 `radius` 严格跟随显示器物理 DPI 缩放毫秒级重算（`scaleMetric(radius, scale)`）。
7. **跨架构原生兼容规范 (Cross-Architecture: x64 & ARM64)**：
   - C++ 原生代码必须保持指令集便携性，原生支持 **x64** 与 **ARM64** 双指令集架构；
   - 严禁未经 `#ifdef _M_ARM64` / `#ifdef _M_X64` 条件编译保护直接内联特定架构专有汇编或 SIMD 内联函数；构建系统必须保持 `build-x64` 与 `build-arm64` 独立隔离校验。
8. **全链路观测与日志国际化规范 (Observability & Log Gate)**：
   - **统一日志类库接入**：必须使用统一的 `LOG_*` 宏家族，严格区分 `TRACE`、`DEBUG`、`INFO`、`WARN`、`ERROR`、`CRITICAL` 六级专业日志；
   - **TraceID 注入与链路透传**：核心 IPC 交互、跨进程调用与异步任务必须注入并透传 `TraceID`，实现微内核架构下的端到端精准追踪；
   - **C++ 中文日志多语言收录 (`I18nLogCatalog`)**：全库源码中所有的中文日志模板必须 100% 在 `src/core/logger/I18nLogCatalog.cpp` 中集中注册并提供无汉字污染的英文映射；中英文模板中的 `{}` 占位符数量必须 1:1 绝对对齐；
   - **CI 门禁约束**：必须通过 `node scripts/check-logger.js` 自动化静态扫描，杜绝未登记日志与占位符错位。

---

## 3. 前端 (React/TypeScript) 工程与排版渲染规范 (Frontend Development)
1. **多语言同构矩阵与 7 重防护门禁 (Internationalization Matrix Gate - i18n)**：
   - **动态键类型断言**：`react-i18next` 的 `t()` 函数使用严格联合类型校验，动态变量传键必须显式断言为 `as any`（如 `t(item.key as any)`）以消除 `TS2345`；
   - **多语言 7 重防护门禁 (`check-i18n.js`)**：
     1. 中英文字典 100% 同构双向对齐（`en.json` 与 `zh.json`）；
     2. 英文包绝对 0 汉字字符污染；
     3. 中文包 0 未翻译英文泄漏（专有名词除外）；
     4. 动态变量插值 `{{param}}` 100% 双向对齐；
     5. 全库源码所有 `labelKey` / `t()` 引用 100% 在字典中真实存在；
     6. 全库源码 0 裸中文硬编码与 100% 英文 Fallback；
     7. 全库 UI 预设与选项实体 100% 接入 `labelKey`/`nameKey` 国际化管线。
2. **字体排版与渲染工程标准 (Typography & Font Rendering Standards - 方案 B & C 黄金准则 & 排版单一事实源)**：
   - **零系统字体污染 (Zero System Font Pollution)**：严禁在安装包中向 Windows `C:\Windows\Fonts` 写入字体或修改系统注册表，杜绝管理员权限受限、DirectWrite 进程锁定导致的卸载残留以及字体分发版权合规风险；
   - **WebFont 选型权衡架构记忆 (WebFont vs Native Font Stack Trade-offs & 方案 B 黄金准则)**：
     - *视觉评估*：思源黑体（Noto Sans SC）在中文字形饱满度、大中宫与现代几何字面表现力上明显优于系统默认微软雅黑；
     - *桌面端旧方案硬伤*：内嵌全量 `@fontsource/noto-sans-sc` 引入 100+ 个 `.woff2` 碎片切片，使安装包由 16MB 暴增至 32.4MB（+100% 膨胀）并在打包时产生海量磁盘 I/O 碎片；
     - *方案 B 架构终局标准*：全项目采用 **单字重极简无损思源黑体架构（500 Medium · 仅 1.10MB 单文件 · 0 碎片 I/O）** 并结合 `@font-face` 中的 `local('Source Han Sans SC')` 本地白嫖机制。既彻底消灭 100+ 切片导致的 20MB 膨胀与打包卡顿，又完美呈现中英数字/括号 100% 同源浑然一体的世界级现代几何字面质感！
   - **应用级单一事实源字体栈体系 (Single Source of Truth Font Stack)**：
     - 界面无衬线统一引用 `--font-sans`（`"Noto Sans SC", "Source Han Sans SC", -apple-system, BlinkMacSystemFont, "Segoe UI Variable Text", "Segoe UI", "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei UI", "Microsoft YaHei", sans-serif;`）；
     - 现代等宽/类名/代码/快捷键统一引用 `--font-mono`（`"Cascadia Code", "Cascadia Mono", "Segoe UI Mono", "Consolas", "PingFang SC", "Microsoft YaHei UI", monospace;`）；
     - 严禁在任何新组件中裸写 `ui-monospace` 或硬编码 `font-family`。
   - **字号底线与字重加权红线 (Legibility & Font Size / Weight Floor - 字号与字重双重底线)**：
     - 界面中所有文本（含次级辅助说明、状态徽章、输入控件、占位符 placeholder 等）字号不得低于 `0.83rem` (`11.8px ~ 12px`)；
     - **字重底线严禁低于 500 Medium**（正文 550，标题 650~700），行高不得低于 `1.4`，保障 ClearType 次像素物理渲染字字饱满锐利；
     - 严禁出现 400/450 细字重导致的笔画发虚与边缘发灰。
   - **技术标识符微晶胶囊标准 (Inline Glass Code Badge Standard)**：所有窗口类名、进程名、文件路径、按键序列等技术元数据严禁以粗糙细文本直接裸露，必须统一使用 `<CodeBadge />` 微晶等宽代码胶囊封装。
   - **自动化排版 CI 门禁 (Automated Typography CI Gate)**：CI 流水线强制执行 `npm run typography-check`，一旦发现孤立字体声明、低于 11.8px 的微小字号或低于 500 的细字重直接阻断构建。
3. **CSS 变量与设计令牌单一事实源 (CSS Variables & Design Tokens Gate)**：
   - 全局界面样式必须基于 CSS 变量（Design Tokens）构建；
   - 严禁使用未声明的悬空 CSS 变量，CI 流水线通过 `node scripts/check-css-variables.js` 实施强校验，发现悬空引用直接阻断。

---

## 4. 质量红线与代码覆盖率规范 (Quality Assurance & Code Coverage)
1. **实用覆盖率基准 (Practical Coverage Baseline)**：原生源码行覆盖率门禁从已测基线 **32%** 起步，后续只升不降。新增或修改的核心业务逻辑、工具算法、数据协议、状态机、安全边界与缺陷修复必须配套针对性测试。全库 100% 语句/分支覆盖并非项目强制要求。
2. **覆盖率单向棘轮 (Coverage Ratchet)**：仅在测得的基线实际稳步提高后方可上调全库覆盖率阈值。分支覆盖率仅在所选工具真实产生分支数据后方可作为阻断指标，`0/0` 分支数据绝不得冒充为 100% 覆盖。
3. **模块化测试架构体系 (`Tools3000Tests.exe`)**：单元测试统一划分至 6 大高内聚子套件（`test_gesture`, `test_core`, `test_capture`, `test_search`, `test_ui_lifecycle`, `test_dialog`），避免巨石测试文件并确保模块快速隔离测试。
4. **零废弃代码原则 (Zero-Dead-Code Principle)**：被证实无法触达的代码路径必须重构或移除；但针对特定平台容错、异常恢复以及特定硬件架构的代码分支，应通过针对性测试或详尽文档记录的人工验证进行合理解释。
5. **自动化门禁与回归守护 (Automated Gate & Verification)**：原生单元测试（`Tools3000Tests.exe`）与前端质量检查（`npm run lint` 包括 `eslint`、`check-i18n.js`、`check-logger.js`、`check-css-variables.js`、`check-typography.js`、`check-trim-workingset.js`）必须在 Release 构建与 CI 流水线中 100% 通过。本地增量构建工具可提供显式跳过选项，但严禁虚假声称测试已执行。

---

## 5. 已确认产品决策与审计基线 (Accepted Product Decisions & Audit Baseline)
以下既定产品行为已于 2026-09-04 / 2026-09-05 由项目负责人明确评审并通过。在后续审计中，除非其实际实现发生实质性改变或引入了新的明确退化，否则**严禁将其重复报告为缺陷**：
1. **最高权限自启动计划任务与 Everyone 权限 (Elevated Auto-Start Task)**：每个用户的 Tools3000 计划任务可以以 `HighestAvailable` 权限运行，并授予 `Everyone` 完全控制权限。这是经过权衡并明确接受的安全折衷。
2. **CLI 卸载默认清理个人数据 (Destructive CLI Uninstall Default)**：`install.ps1 -Uninstall`、`uninstall.ps1` 和 `uninstall.cmd` 在未指定 `-KeepPersonalData` 时默认删除 Tools3000 的配置、缓存、截图与录像文件。该破坏性默认行为符合产品设计意图。
3. **QuickLook 媒体预览格式占位 (QuickLook Format Placeholders)**：QuickLook 允许在视频、音频与 PDF 的内嵌预览功能尚未完全就绪时，先行声明并识别这些文件格式。此行为属于已确认的产品演进范畴，非阻断发布的缺陷。
4. **搜索服务生命周期契约 (Search Service Lifecycle Contract)**：搜索服务生命周期严格遵循 `DEMAND_START`（按需启动）。Tools3000 主程序启动、后台隐藏 WebView 预载、设置页状态轮询、窗口获焦及 Tools3000 主程序退出均绝不允许启动搜索服务。只有当用户产生显式呼出搜索的操作时才允许启动该服务。搜索服务一旦启动，将在本次 Windows 会话中跨搜索窗口隐藏与 Tools3000 主程序退出而持续常驻，直至 Windows 关机/重启或管理员手动终止；下一次 Windows 开机绝不得自启动搜索服务。
5. **Lua 脚本可信本地自动化定位 (Trusted Local Lua Automation)**：Lua 引擎定位为宿主授予的可信本地自动化扩展通道，直接与宿主 API/IPC 交互，非防范恶意外代码的隔离沙箱环境。

---

## 6. 开源版权与原作者署名基准 (Copyright & Open Source Attribution Standards)
1. **作者唯一法定标识 (Author Identity)**：项目官方作者署名标准为 **`Yy1 (yuan278501381)`**（展示格式：`Yy1 (@yuan278501381)`）。
2. **GitHub 官方主页与仓库 (GitHub Repository & Profile)**：
   - 作者官方主页：`https://github.com/yuan278501381`
   - 项目官方仓库：`https://github.com/yuan278501381/tools3000`
3. **开源协议与法定版权声明规范 (License & Notice Enforcement)**：
   - 项目默认采用 **MIT License**；
   - 在代码仓库的 `LICENSE` 文件、UI 关于界面（About Page）、安装包发布者元数据（Inno Setup `AppPublisher`）以及所有相关文档中，均强制统一保留 `Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors` 的法定版权声明。

---

## 7. Git 协作、分支管理与自动化发版标准 (GitFlowReleasePipeline)
1. **Git 提交信息双重视角规范 (Dual-Perspective Commit Convention)**：
   - 必须使用**中文**编写 Git 提交信息；
   - **首行原则**：第一行（第一句）永远是全局概览摘要，确保在查看 Git 日志时能一目了然；
   - **双重视角清单**：主体描述需以清单形式，从以下两个维度进行阐述：
     - 💼 **业务角度**：清晰说明实现了什么功能、解决了什么业务痛点；
     - 🔧 **技术角度**：清晰说明底层的技术路线、重构逻辑或架构模式。
2. **一键 DevOps 发版总控 (`scripts/release.ps1`)**：
   - 发版流水线由 `pwsh scripts/release.ps1`（或带参数 `-Bump Minor/Major`、`-Version X.Y.Z`）全自动统筹编排；
   - 流水线严格按顺序执行：工作区纯净度检查 -> `VERSION` 单一事实源版本号递增 (+1) -> 自动提取生成 Release Notes -> 开发分支提交 -> 向 `main` 主干执行非快进合并（`--no-ff`） -> 全新洁净构建与打包（`deploy.ps1`） -> 全生命周期端到端门禁校验 -> 二进制 `ProductVersion` 强断言 -> 自动化 Authenticode 代码签名校验与安装包/便携包/SHA256SUMS 产物归档 -> 创建不可变带注释 Tag 与 GitHub Release -> 安全切回或创建下一阶段开发特性分支。
3. **双圆环拓扑美学 (Visual Graph Integrity)**：
   - 确保在 Git Graph 中直观呈现出独立的开发支线与清晰的双父节点汇聚圆环（`merge(dev)`）。
