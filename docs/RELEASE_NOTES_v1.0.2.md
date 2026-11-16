# Tools3000 v1.0.2 官方正式版发布说明 (Official Release Notes)

欢迎升级至 **Tools3000 v1.0.2 官方正式版**！本次版本迎来了三大生产力引擎的重磅跃升：**演播室自动运镜录屏与多格式高定背景导出台（含微信直发自动播放 GIF 与网页动态 WebP）**、**微信级原图实景选字双引擎高精度离线 OCR（PP-OCRv4 + Excel 零空列表格直贴）** 以及 **工程制图与办公文档零文件锁空格秒看引擎（CAD/天正/DXF/CDR/PSD/AI/3D/Office/PDF）**，同时根治了 4K 高分屏下第三方中文输入法候选框过小与全文检索大体积字体流耗时等深水区痛点。

Welcome to **Tools3000 v1.0.2 Official Release**! This release introduces three flagship productivity engines: **Studio Auto-Zoom Screen Recorder & Multi-Format Export Suite** (featuring WeChat inline auto-play GIFs, 24-bit animated WebP, 4K GPU hardware encoding, and 8 curated Calm/Airy/Landscape backdrops), **Dual-Engine Offline Live Text OCR** (powered by PP-OCRv4 ONNX with zero-column-drift Excel table reconstruction and QR decoding), and **Zero-Lock Industry CAD/Prepress/3D & Office/PDF QuickLook Preview**, alongside PerMonitorV2 IME scaling and ultra-fast PDF full-text indexing.

---

## 本次版本核心亮点与体验升级 (Key Highlights)

### 1. [演播室录屏与导出] 自动点击放大运镜、8 款高定背景与微信自动播放 GIF / 动态 WebP 导出台
- **物理弹簧自动运镜与超清矢量光标重绘**：开启演播室模式后（录制倒计时卡片实时呈现自动放大运镜提示，正式开录自动隐藏左下角快捷键浮层零穿帮），每次鼠标点击与输入焦点切换均通过二阶临界阻尼弹簧（零超调）平滑推进特写镜头，配合 `OneEuroFilter2D` 滤除手部微抖动并在放大画面顶层以亚像素精度重绘超清矢量指针与点击波纹；停止录制即刻弹出 30fps 实时演播室预览窗，支持分段独立开关运镜或 0 秒一键保留原片；
- **沉稳 · 轻盈 · 风景 三大系列 8 款高定背景**：摒弃传统生硬线性渐变，采用多光源高斯弥散网格、四层大气透视远山剪影与湖面晨光程序化合成及 `4x4 Bayer` 抗色带抖动，内置 **沉稳系列**（`深海曜石`、`暮夜星云`、`松烟墨翠`）、**轻盈系列**（`晨雾云母`、`暖杏羊绒`、`晴空薄荷`）与 **风景系列**（`暮色苍山`、`拂晓湖山`），演播室右侧提供 `4x2` 实时缩略图卡片一键直选；
- **全参数多格式导出模态窗（微信聊天直发自动播放 GIF & 网页动态 WebP）**：支持自定义文件名称、选择保存目录、调节 `格式 / 分辨率 / 帧率 / 质量`、勾选 `保存当前设置` 及实时显示预计文件大小：
  - **`GIF (微信自动播放)`**：专为微信聊天窗口内联自动播放设计，自动钳制长边 `<= 800px`、对齐 `{1, 100}` 厘秒时间基消除慢放惩罚、启用帧间透明差分与全局调色板量化，并内置 `<= 4.85 MB` 闭环自动重压护栏，确保发入微信直接循环动起来而非退化为文件附件；
  - **`WebP (网页动图)`**：基于 `libwebp / libwebpmux` 实现 24-bit 真彩无调色板噪点帧间混合差分动图，适配现代网页与在线文档；
  - **`MP4 (H.264 / HEVC)` & `WebM (VP9)`**：两阶段硬件编码直通（优先激活 Intel QSV / NVIDIA NVENC / AMD AMF 硬件 MFT 并预提取异步序列头，自适应保障 4K 高码率与零跳帧防灰块花屏），完整保持 VFR 至 CFR 真实时长与音轨/字幕对齐。

### 2. [高精度离线文字识别] 微信级原图实景选字 (`Live Text`)、PP-OCRv4 引擎与纯净表格直贴
- **零门槛贴图实景选字与顺手扫二维码**：贴图空白处保持左键拖拽移动，鼠标移入图内文字自动切换 `I` 型光标逐字流式划选、双击选词，松手即现 `[复制]`、`[复制为表格]`、`[对照面板]` 微晶胶囊，并自动识别正色与深底反色二维码；
- **零依赖膨胀的 PP-OCRv4 离线模型包与按需安装**：复用内置 `OpenCV DNN` 驱动 DBNet 倾斜检测 + 方向矫正 + SVTR/CRNN 双宽度分桶推理（内置 Protobuf 动态符号维度固化重写器与模型内嵌 6,623 字词典自解析，国内镜像高速直连下载），安装包同步提供默认不勾选的可选离线引擎子项；
- **Excel 原生分列表格零空列重建**：融合跨行垂直空白走廊投影、左/中/右三锚点对齐、分割线标点噪点剔除与稀疏微偏胶囊智能归并，支持一键复制 `CF_HTML + TSV` 双格式直接粘贴至 Excel / WPS / 飞书还原纯净单元格矩阵。

### 3. [QuickLook 空格预览] 工程制图、印前设计、3D 模型与办公文档 100% 零文件锁秒看
- **工业与设计专业格式全覆盖**：支持 AutoCAD / 天正建筑 `.dwg`、SolidWorks `.sldprt/.sldasm`、SketchUp `.skp` 高清预览，`.dxf` 多图层交互式矢量看图（10%~5000% 光标中心缩放、底色自适应反色与中文图层开关），CorelDRAW `.cdr` 多页预览，Photoshop `.psd/.psb` 流式合成大图，Illustrator `.ai/.eps` 矢量渲染，以及 `.stl/.obj` 零依赖 WebGL 双光源 + 线框 360° 3D 检视器；
- **办公文档零乱码无文件锁引擎**：全链路采用共享内存瞬时读取并即刻释放句柄（不阻塞外部软件保存或重命名），支持 `.xlsx/.csv/.tsv` 标准字母列标与多工作表 0ms 切换、`.docx` 段落表格与内嵌截图混排渲染、`.pptx` 幻灯片大纲及 `.pdf` 内存高清页面光栅化。

### 4. [高分屏输入法与全文检索] 原生 `PerMonitorV2` 清单适配与 PDF 检索加速
- **4K 高分屏第三方中文输入法完美缩放**：为 `Tools3000.exe` 静态嵌入 `PerMonitorV2` 高分屏感知清单与 Windows 10/11 兼容 GUID，联动 WebView2 `NotifyParentWindowPositionChanged` 实时同步输入焦点坐标，彻底解决高分屏缩放环境下搜索窗内搜狗等中文输入法候选框显示过小的问题；
- **全文检索与百万索引冷启动提速**：`PdfExtractor` 精准识别并免解压秒跳 PDF 内嵌数 MB 二进制字库流与图像流，大幅缩短海量文档 `content:` 检索耗时；300 万级文件快照升级单趟流式加载与后台低优先级异步预热，确保首次搜索流畅响应。

---

## 发布资产与 SHA-256 校验说明 (Release Assets)

| 文件名 (File Name) | 适用平台 (Platform) | 说明 (Description) |
| :--- | :--- | :--- |
| **`Tools3000-Setup.exe`** | Windows 10/11/Server (x64) | 官方推荐标准数字签名安装程序 (Standard Signed Installer) |
| **`Tools3000-v1.0.2-Setup.exe`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方归档安装程序 (Versioned Signed Installer) |
| **`Tools3000-Portable-x64.zip`** | Windows 10/11/Server (x64) | 免安装绿色便携版压缩包 (Portable Archive) |
| **`Tools3000-v1.0.2-win-x64-portable.zip`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方绿色便携版压缩包 (Versioned Portable Archive) |
| **`SHA256SUMS.txt`** | 全平台产物 | 官方发布二进制资产的 SHA-256 完整性校验哈希值清单 |

---

## 开源许可证与作者署名 (License & Attribution)

本项目基于 **[MIT License](../LICENSE)** 开源协议发布。

* **项目作者 (Author)**：**`Yy1 (yuan278501381)`**（GitHub：[@yuan278501381](https://github.com/yuan278501381)）
* **项目仓库 (Repository)**：[https://github.com/yuan278501381/Tools3000](https://github.com/yuan278501381/Tools3000)
* **法定版权 (Copyright)**：`Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors`
