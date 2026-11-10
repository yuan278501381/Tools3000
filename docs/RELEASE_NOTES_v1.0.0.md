# Tools3000 v1.0.0 官方正式版发布说明 (Official Release Notes)

欢迎体验 **Tools3000 v1.0.0 官方正式版**！这是 Tools3000 历经全链路深度打磨与系统工程精细演进后的首个官方正式里程碑版本。

Tools3000 是一款专为 Windows 10/11 与 Windows Server 平台日常办公、设计创作、开发运维与极客生产力打造的**全场景高性能原生桌面效率工具集**。依托 **C++20 原生微内核 + Rust SIMD 全文检索引擎 + Direct2D/DirectComposition 硬件加速合成**，Tools3000 将**文件与全文搜索、全局鼠标手势、屏幕四角触发角、全能截图与录屏、按键回显与统计、鼠标演示与特效、文件对话框助手、远程协助增强**深度熔铸为一体，提供轻盈纯净、随叫随到、无打扰的世界级桌面交互体验。

Welcome to **Tools3000 v1.0.0 Official Release**! Engineered with a native C++20 microkernel, a zero-dependency Rust SIMD full-text engine (`tools3000_content_rs`), and GPU-accelerated Direct2D/DirectComposition rendering, Tools3000 unifies **File & Content Search, Global Mouse Gestures, Screen Hot Corners, Screen Capture & Recording, Keycast Overlay & Analytics, Cursor Effects & Presentation, File Dialog Enhancer, and Remote Assistance Boost** into an ultra-lightweight desktop productivity suite.

---

## 核心功能矩阵深度解构（按客户端导航顺序）

### 1. [01] 文件与全文搜索 (File & Content Search) — 275万文件秒查与正文穿透
- **百万级文件毫秒响应与跨目录秒搜**：随时按下 `Alt + Space` 零延迟唤起搜索中心。基于底层 NTFS MFT 元数据直读、USN 实时增量日志与**可分离目录 64 位位掩码 (`FileRecord::dirMask`) + 极大反链预筛算法**，在 275 万真实文件规模下，跨目录多词查询与拼音首字母混搜（如 `sztx 账号密码`）耗时稳定在 **20~27 ms**；
- **最匹配优先置顶与无锁 Bloom Frecency 加权**：多关键词与多扩展名并集（如 `*.log *.txt log`）自动隔离扩展名假命中，按文件名主干（Stem）命中覆盖度分层置顶，并结合 **64 位双哈希 Bloom 运行历史无锁快照 (`RunHistorySnapshot`)** 将高频打开的文件（`[打开 N 次]`）智能排在最前；
- **Windows 资源管理器原生真实图标还原**：针对 `.exe`、`.lnk`、`.ico`、`.cur`、`.ani`、`.scr`、`.cpl`、`.msc` 等文件直接提取每个文件真实内嵌的 Windows 高清图标，普通文档提取系统关联图标，大幅降低视觉认知负荷；
- **C++ SEH + Rust SIMD 深度正文穿透检索 (`content:`)**：内置零外网依赖的 Rust SIMD 检索引擎（`tools3000_content_rs`）与 MSVC `__try/__except` 页错误保护，直接穿透搜索 Word、Excel、PowerPoint、PDF（含 `ToUnicode` CMap）、AutoCAD DXF 及代码源文件。采用 220ms 时间片流式上屏，跨分片锁定行高**彻底杜绝列表闪烁**，支持实时扫描百分比、一键停止扫描及点击行号胶囊（如 `L66`）**单行代码一键复制**；
- **完备高级语法与 QuickLook 空格预览**：支持 `*.log *.txt`、`ext:jpg;png`、`size:1mb..50mb`、`size:empty/tiny/small/medium/large/huge`、`dm:today/thisweek`、`!exclude` 与 `regex:`；选中任意结果按 `Space` 空格键即刻原地预览；严格遵循 `DEMAND_START` 按需启动契约。

### 2. [02] 全局鼠标手势 (Global Mouse Gestures) — 轨迹、径向轮盘与即时触发
- **右键/中键轨迹、滚轮联动与屏幕边缘滑动**：按住右键（或中键）一划即可执行后退、前进、关闭标签页、恢复标签页、窗口置顶等操作；支持在桌面或任务栏边缘滑动滚轮调节系统音量或切换虚拟桌面；
- **8~16 扇区单行径向轮盘与立即执行 (`instantExecute`)**：支持呼出高质感 Direct2D 径向轮盘菜单与飞行方向罗盘；支持轨迹达标刹那微秒级立即触发而无需等待松开按键，以及应用级专属作用域规则与全屏游戏自动避让。

### 3. [03] 屏幕四角触发角 (Screen Hot Corners) — 零瞄准四角盲甩调度
- **四角物理盲操作**：将光标推入屏幕四个角落即可一键显示桌面、打开多任务视图或立即锁屏息屏；
- **防抖驻留滤波**：内置驻留延迟滤波，日常快速划过绝不误触，全屏游戏与观影时自动静默。

### 4. [04] 全能截图、长截图、贴图与高清录屏 (Capture, Scroll Capture, Pin & Recording)
- **DirectX 零黑屏定格与对标 PixPin/Figma 精细标注**：
  - 按 `F1` 瞬间零黑屏定格并磁吸窗口边界；内置 **Figma 式近场平滑渐显 (`smoothstep`) 圆角手柄**与抗锯齿曲率光标；
  - 支持**正圆/椭圆局部放大镜**与 **4 种引出线**（双切线 `Tangent`、折线 `Elbow`、虚线 `Dashed`、单线 `Single`）；
  - 支持 **6 种箭头样式**（含立体箭头 `CampaignMarch`、路线箭头 `MarchDoubleTrack`）、折线/贝塞尔平滑曲线切换、**10 种首尾独立端点**，以及矩形/椭圆/箭头/文字的**「填充色 + 独立边框色 + `0~12px` 边框粗细」双通道融合控制**；
  - 支持微晶胶囊字签、删除中间步骤后自动重排的**自愈序号**，以及 5 大主题美化分享外壳。
- **Smartisan 式滚动长截图首尾无损裁切与 Minimap 工作台**：
  - 自动平滑滚动拼接网页、文档与聊天记录，完成后无缝进入基于 `4096px` 纹理瓦片流的全屏暗房精修工作台；
  - 提供**非破坏性首尾/左右推拉裁切标尺**（已裁除区域半透暗罩保留，随时往回拖动可 100% 无损恢复）、近边缘自动滚屏、**右侧全景缩略图 (Minimap) 联动拖拽裁切**与长图原位标注。
- **32 组宝石桌面贴图与离线 OCR**：
  - 一键将截图、剪贴板图像或文本钉在桌面顶层（`F3`），支持 **32 个自定义贴图分组**（12 色宝石色相环、顶部雪瓷白分组导览栏、托盘 Bento 抽屉一键归组）；内置 100% 离线 Windows OCR 跨行文字框选复制。
- **镂空呼吸边框高清录屏与 2-Pass GIF**：
  - 镂空呼吸微光边框与自动避让控制条彻底告别盲录焦虑；支持系统音/麦克风独立静音、视频帧抓拍、断电安全分片 MP4 封装与高保真 2-Pass 调色板 GIF 导出。

### 5. [05] 按键回显与本地统计 (Keycast & Keystroke Analytics)
- **DirectWrite 真实字宽测量与高对比微晶键帽**：实时渲染半透明微晶键帽与连击角标（`Ctrl + C x3`），长组合键自适应宽度绝不截断；
- **智能上档符号折叠与打字静默**：`Shift + 8` 自动折叠为 `*`；连续打字与拼音输入自动静默，仅在按下功能组合键时浮现；
- **纯本地键鼠统计与热力图**：记录今日按键数、鼠标点击数、移动里程与全键盘热力图，100% 留存本地 SQLite 数据库。

### 6. [06] 鼠标演示与特效 (Cursor Effects & Presentation)
- **双击 Ctrl 聚光灯焦点**：柔和压暗背景并高亮跟随光标，物理窗口主动缩减 1 像素（`vw - 1, vh - 1`）从底层阻断误触发 Windows 专注助手；
- **局部微视口点击水波纹与流光尾迹**：采用 100~300px 动态包围盒与 256 阶 LUT 查表渲染，GPU 开销降低 99%。

### 7. [07] 文件对话框助手 (File Dialog Enhancer)
- **顶部微晶导览胶囊条**：在标准打开/保存对话框（`#32770`）顶部悬浮微晶胶囊，一键秒切至当前已打开的 Explorer 目录或收藏路径，并为不同宿主应用独立记忆工作目录。

### 8. [08] 远程协助增强 (Remote Assistance Boost)
- **系统热键直通与 RustDesk 触控板四指横滑守护**：远控窗口内 `Alt + Tab`、`Win + E`、`Win + R` 直接透传至远端，同时智能放行本地虚拟桌面切换；内置 `MouseHook` 物理按键掩码追踪与手势延迟按下配对自愈机制，彻底根除 RustDesk/ToDesk 下精密触控板四指横滑间歇性失效；
- **11 通道修饰键粘滞急救与输入法智能脱敏**：快速双击右侧 `Ctrl`（或按 `Ctrl + Alt + Backspace`）无抢焦冲刷释放所有粘滞修饰键；进出远控窗口自动切换英文键盘（`ENG 0409`）并无感恢复本机输入法。

---

## 物理内存占用横向对比 (Memory Consumption Benchmark)

```text
物理内存常驻占用横向对比 (越低越好)：
┌─────────────────────────────────────────────────────────────────────────────┐
│ [高开销] 常见 Electron 架构效率工具 (300 MB ~ 800 MB)                       │
│          ██████████████████████████████████████████████████████████ 600 MB  │
├─────────────────────────────────────────────────────────────────────────────┤
│ [中开销] 传统 C# / .NET 架构桌面工具 (120 MB ~ 200 MB)                      │
│          ████████████████ 160 MB                                            │
├─────────────────────────────────────────────────────────────────────────────┤
│ [轻量级] Tools3000 一体化微内核 (八大模块并发活跃常驻仅 20 MB ~ 40 MB)      │
│          ███ 28 MB                                                          │
├─────────────────────────────────────────────────────────────────────────────┤
│ [静默态] Tools3000 后台日常托盘待机 (日常仅 ~11 MB，待机 CPU 占用 0.0%)     │
│          █ 11 MB                                                            │
└─────────────────────────────────────────────────────────────────────────────┘
```

- **日常静默待机仅 `~11 MB`**：原生编译零虚拟机包袱，窗口隐藏时自动挂载 `TrySuspend()` 冻结 WebView2 JS 堆；
- **全模块并发运转仅 `20 ~ 40 MB`**：八大核心模块并发常驻仅占传统 Electron 工具的 1/20；
- **冷路径 `< 0.5 秒` 极速收缩 (`trimWorkingSet`)**：截图、录屏、长截图拼接、OCR 完成后立即回收临时物理内存归还系统，1000Hz 键鼠热路径零缺页中断。

---

## 发布资产与 SHA-256 校验说明 (Release Assets)

| 文件名 (File Name) | 适用平台 (Platform) | 说明 (Description) |
| :--- | :--- | :--- |
| **`Tools3000-Setup.exe`** | Windows 10/11/Server (x64) | 官方推荐标准安装程序 (Standard Signed Installer) |
| **`Tools3000-v1.0.0-Setup.exe`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方归档安装程序 (Versioned Installer) |
| **`Tools3000-Portable-x64.zip`** | Windows 10/11/Server (x64) | 免安装绿色便携版压缩包 (Portable Archive) |
| **`Tools3000-v1.0.0-win-x64-portable.zip`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方绿色便携版压缩包 (Versioned Portable Archive) |
| **`SHA256SUMS.txt`** | 全平台产物 | 官方发布二进制资产的 SHA-256 完整性校验哈希值清单 |

---

## 开源许可证与作者署名 (License & Attribution)

本项目基于 **[MIT License](../LICENSE)** 开源协议发布。

* **项目作者 (Author)**：**`Yy1 (yuan278501381)`**（GitHub：[@yuan278501381](https://github.com/yuan278501381)）
* **项目仓库 (Repository)**：[https://github.com/yuan278501381/Tools3000](https://github.com/yuan278501381/Tools3000)
* **法定版权 (Copyright)**：`Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors`
