# Tools3000 v1.0.3 官方正式版发布说明 (Official Release Notes)

欢迎升级至 **Tools3000 v1.0.3 官方正式版**！本次版本全面重塑了录屏演播室的视听与操控质感，带来 **双层视网膜超清预览引擎（`StudioHiResFrameReader`）**、**电影胶片画廊 + 三层雕刻声学山峦曲线与静音气口引导主轨**、**黄金三段式走带控制栏与三级长·中·短精密游标卡尺时间刻度（支持 `1x ~ 24x` 无级局部精修缩放与 4 小时超长录屏）**、**深浅背景自动适应的企业溯源与品牌双模水印全链路系统**，以及 **WinML/DirectML 硬件加速与版面感知严苛基线对齐的秒级实景选字 OCR**。

Welcome to **Tools3000 v1.0.3 Official Release**! This release elevates the Studio screen recording, capture watermarking, and Live Text OCR engines to a new level of precision: introducing a **Dual-Tier Retina Native-Resolution Preview Engine**, **Cinema Filmstrip + Sculpted Acoustic Mountain Waveform with Smart Silence Guides**, **Symmetric Three-Zone Transport Bar & Three-Tier Vernier Timecode Ruler (with `1x ~ 24x` zoom and 4-hour long-session architecture)**, **Light/Dark Auto-Adaptive Brand & Traceability Watermarks**, and **WinML/DirectML Accelerated Layout-Aware Live Text OCR**.

---

## 本次版本核心亮点与体验升级 (Key Highlights)

### 1. [演播室视听与时间轴重塑] 双层视网膜超清舞台、电影胶片山峦音轨、黄金三段式走带与三级精密刻度
- **标准 Windows 自由缩放窗口与双层视网膜超清预览引擎**：演播室全面采用原生 `WS_OVERLAPPEDWINDOW` 架构，支持四边与四角丝滑拉伸大小、Aero Snap 边缘贴靠分屏、双击最大化与尺寸记忆；内置 `1280px` 瞬时代理帧牌组 + 驻留式原片全分辨率解码器 `StudioHiResFrameReader`（含 LRU 帧缓存与 `HIGH_QUALITY_CUBIC` 视口像素级对齐），无论窗口拉得多大或叠加高倍特写运镜，代码与表格文字始终刀锋般锐利；
- **黄金三段式对称控制栏与 `1.0x ~ 24.0x` 时间轴无级精修缩放**：左翼聚合 `[拆分(S) | 删除 | 倍速]`、`[撤销 | 重做]`、`[标注 | 运镜]` 创作三舱；中枢居中锚定一体式微晶走带控制台 `[ |< 跳回片头 ] [ < 上一帧 ] [ 立体主播放/暂停 ] [ > 下一帧 ] | 双色防抖等宽时间码`（支持 `Home` 与 `Left / Right` 单帧微调）；右翼配备 `[ 全览/倍率 | - ──o── + ]` 视野缩放舱，支持悬停时间轴直接滚轮光标中心锚定缩放、`Shift+滚轮` 或底部微晶滑块平滑平移；
- **瑞士腕表级三级长·中·短精密游标卡尺刻度（Vernier Timecode Ruler）**：采用偶数细分格制式，主刻度长竖线右侧内嵌等宽时间码，相邻整秒/主刻度正中央呈现挺拔无数字半格中点线（如 `0.5s`、`1.5s`），两侧铺设十分位/四分位无数字精密短齿，卡点剪辑一目了然；
- **「电影胶片画廊 + 三层雕刻声学山峦曲线」主轨**：上舱连续铺展真实录屏胶片缩略图与吸左磨砂元数据微胶囊；下舱独立声学视窗自动剥离压限音频底座，绘制瞬态柔雾外峰、垂直渐变主体山脉与抗锯齿高光山脊线，自动标出 `>= 0.45s` 连续空白停顿段（`静音 X.Xs`）引导一键拆分剪除；
- **全景光学取景框运镜拖拽、4 小时超长录屏架构与 0ms 瞬时准备界面**：拖拽运镜焦点时自动展开为 100% 全景无裁切底图配 Direct2D 0ms 贴手准星与徕卡四角取景框；底层升级 `O(F + C)` 线性游标烘焙与轨迹保拐点流式抽稀，轻松承载 3~4 小时超长录屏；点击停止录制瞬间 `0ms` 弹出 60FPS 光学快门准备界面，非破坏性原位交付结果卡支持同一母带连续导出 MP4 / GIF / WebP 多格式。

### 2. [全链路企业溯源与品牌水印] 图片 Logo / 文字角标 / 斜铺溯源三模合一与深浅背景自动显影
- **演播室成片与静态截图/贴图共享单一事实源水印引擎**：全面支持透明 PNG/WebP 品牌 Logo（记忆最近 6 张常用徽标，支持直接在预览画面拖拽自由定位与右下角手柄无级缩放）、微晶胶囊文字角标与 `-28°` 斜向交错网格平铺水印；
- **深浅与中间调背景自动适应显影**：平铺水印支持 `{user}`、`{host}`、`{date}`、`{time}` 智能变量双向点选开关、`35% ~ 280%` 全程线性字号缩放，以及平滑亮度场双极自适应混合——无论浅色文档表格、深色代码窗口还是中间灰界面均自动呈现清晰柔和的对比度，且固定在画面顶层不随镜头推拉晃动；支持企业策略注册表强制锁定，并与主设置界面双向实时同步。

### 3. [录屏画质、音频与系统级细节] 硬件高码率防噪点、全局锁色板纯净 GIF 与一体化麦克风选择
- **缩放运动零马赛克与纯净无偏色双档 GIF**：激活硬件编码质量优先模式与高动态码率根除全屏缩放帧宏块噪点；GIF 导出跨全片均匀预采样构建锁定全局 256 色调色板（内置 12 级无偏色中性黑白灰护栏与平坦纯色零抖动死区锁），彻底消除深色背景斜纹噪点与中途切入浅白界面的偏黄染色，清晰区分 `GIF (微信表情 · 免点直播)` 与 `GIF (高清大图 · 可双击放大)`；
- **一体化麦克风分体胶囊与纯净系统原声**：录屏工具栏提供 `[ 图标开关 | 选择来源 ]` 分体胶囊，默认对齐 Windows 控制台默认输入设备并支持一键切换外接/内置麦克风；人声降噪严格仅作用于麦克风通道，100% 原样保留电脑系统背景声音；补齐跨现代编辑器（VS Code / 浏览器 / 办公软件）打字自动运镜以及录制期自动隐藏任务栏与桌面图标。

### 4. [微信级离线实景选字 OCR] WinML/DirectML 硬件加速、秒级静默预热与版面感知严苛基线对齐
- **WinML/DirectML 加速与贴图 0ms 静默预热**：接入系统原生 `Windows.AI.MachineLearning` 零拷贝张量与宽幅条带合批推理（优先启用 DirectML GPU 加速），贴图创建瞬间后台低干扰静默完成预热，鼠标移入文字即刻划选；
- **版面感知分栏隔离与严苛水平行对齐**：自动检测物理竖向窗格分割线与半屏空白鸿沟，隔离异构侧边栏与多区域表单，杜绝跨栏串行与超长幽灵框。

---

## 发布资产与 SHA-256 校验说明 (Release Assets)

| 文件名 (File Name) | 适用平台 (Platform) | 说明 (Description) |
| :--- | :--- | :--- |
| **`Tools3000-Setup.exe`** | Windows 10/11/Server (x64) | 官方推荐标准数字签名安装程序 (Standard Signed Installer) |
| **`Tools3000-v1.0.3-Setup.exe`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方归档安装程序 (Versioned Signed Installer) |
| **`Tools3000-Portable-x64.zip`** | Windows 10/11/Server (x64) | 免安装绿色便携版压缩包 (Portable Archive) |
| **`Tools3000-v1.0.3-win-x64-portable.zip`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方绿色便携版压缩包 (Versioned Portable Archive) |
| **`SHA256SUMS.txt`** | 全平台产物 | 官方发布二进制资产的 SHA-256 完整性校验哈希值清单 |

---

## 开源许可证与作者署名 (License & Attribution)

本项目基于 **[MIT License](../LICENSE)** 开源协议发布。

* **项目作者 (Author)**：**`Yy1 (yuan278501381)`**（GitHub：[@yuan278501381](https://github.com/yuan278501381)）
* **项目仓库 (Repository)**：[https://github.com/yuan278501381/Tools3000](https://github.com/yuan278501381/Tools3000)
* **法定版权 (Copyright)**：`Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors`
