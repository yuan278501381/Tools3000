# Tools3000 v1.0.4 官方正式版发布说明 (Official Release Notes)

欢迎升级至 **Tools3000 v1.0.4 官方正式版**！本次版本聚焦 **全场景离线实景文字识别（Live Text OCR）的十倍级性能跃迁与零遮挡对照交互重塑**，全面引入 **DirectML 多档固定形状 Session 零重编译架构与后台空闲 PSO 预热**、**近轴对齐零拷贝切片与 FFD 降序最佳条带装箱**、**自适应斜铺半透明溯源水印精准剔除**、**8-Slot 图像内容指纹毫秒级缓存**、**0ms 原位实景贴图与智能侧翼避让对照面板**，并补齐 **录屏演播室与全套浮层窗口的实时主题色/深浅色同步**。

Welcome to **Tools3000 v1.0.4 Official Release**! This release delivers a world-class leap in **Live Text OCR performance and side-by-side workflow UX**, featuring **Zero-Recompile Fixed-Shape DirectML Session Buckets with Background PSO Pre-warming**, **Zero-Copy Axis-Aligned Cropping & First-Fit Decreasing (FFD) Strip Packing**, **Adaptive Faint Diagonal Traceability Watermark Filtering**, **Sub-millisecond 8-Slot Image Content Fingerprint LRU Caching**, **0ms In-Place Live-Text Pinning with Non-Occluding Side-Docked Comparison Panel**, and **Real-Time Live Theme/Accent Synchronization across Studio & Capture Overlays**.

---

## 本次版本核心亮点与体验升级 (Key Highlights)

### 1. [十倍级 OCR 极速架构] DirectML 固定桶零重编译、突发缓存拉取与后台静默预热
- **固定形状多桶 Session 彻底消灭 GPU 动态重编译风暴**：构建 **3 档 DBNet 检测画布桶**（`960×640`、`1280×1024`、`960×1280`）与 **4 档识别微批次桶**（`1×640`、`4×960`、`16×960`、`32×960`），根除任意选区尺寸与动态条带宽度引发的百次级 DirectML / D3D12 算子图推翻重编译；高分屏密集长图文（`2226×1652`，2,700+ 字）端到端耗时由 `18.68s` 压降至 **`1.56s`**（提速 **`12.0x`**），多栏密集网格业务单据（`1024×537`）压降至 **`0.73s`**，日常中小选区 **`90 ~ 220ms`** 瞬时出字；
- **启动期后台低干扰静默预热（`warmupAsync`）**：插件就绪后在后台低优先级线程自动完成模型加载与常用固定桶管线预编译，消灭首次识字 `3.0s` 冷启动等待；
- **D3D12 Readback 整块突发拉取与多核并行编解码**：采用 `cv::parallel_for_` 多核并行归一化输入张量；将 DirectML 非缓存 Readback 输出张量通过单次整块 `std::memcpy` 突发拉取至 CPU L3-Cached 连续缓冲后再并行执行 CTC 解码，彻底消除跨总线标量随机读取停顿。

### 2. [高精度几何装箱与抗水印干扰] 零拷贝切片、FFD 降序装箱、斜铺淡水印剔除与指纹缓存
- **近轴对齐 `O(1)` 零拷贝切片与 FFD 降序最佳装箱**：对常规水平文本行直接采用零拷贝子图切片与无逆透视矩阵的线性字符像素映射（小字行采用 `cv::INTER_CUBIC` 保持紧凑英文间距锐度）；长行采用单趟连续指针列方差切分，配合 **First-Fit Decreasing (FFD)** 降序条带紧凑装箱与语序还原，条带装载率提升至 `92%+`；
- **自适应斜向半透明溯源水印精准过滤**：当页面主体为水平正交排版时，自动对 `14° ~ 68°` 倾角候选框执行内部双线性网格灰度分位数采样，精准剔除低对比度斜铺溯源水印，100% 零误伤灰底行内代码胶囊与表格细字，消除水印乱码穿插与近半无效推理；
- **8-Slot 图像内容指纹 LRU 缓存与备选引擎大图防退化**：基于图像维度、引擎/语种配置与 `16×16` 网格像素采样构建线程安全 64-bit 内容指纹缓存，同图跨入口二次提取耗时降至 **`~1ms`**；同时修复系统 `Windows.Media.Ocr` 备选引擎对 `>= 1280px` 高分屏大图盲目 `2.0x` 超分放大的性能退化。

### 3. [世界级识字交互与全域实时主题] 0ms 原位实景贴图、侧翼避让对照面板与零重复推理
- **0ms 原位贴图 + 侧翼停靠对照双流合一**：截图框选点击 `[提取文字]` 瞬间 `0ms` 在原选区坐标生成实景选字贴图，并将识别结果面板**智能侧翼吸附停靠在原图右侧/左侧边缘**（告别居中弹窗遮挡原图，状态刷新平滑无闪烁）；单次推理结果同时驱动原图直接划词/复制表格与侧翼面板全文精修；
- **零重复推理与最新任务优先接管**：贴图后台预热完成时按 `Ctrl+T` / 右键提取文字 `0ms` 直接消费内存结果，预热进行中自动挂载完成意图复用唯一在途任务，彻底根除双线程排队重复识别；连续框选识字支持最新任务 `0ms` 优先接管，永不弹窗阻断用户操作；
- **全界面主题色/深浅色实时联动与热重载加固**：补齐录屏演播室（`StudioPreviewWindow`）、截图工具栏、录屏悬浮条、贴图与 OCR 面板的主题色及深浅色毫秒级实时同步；修复 `OcrResultWindow` 在插件热卸载重载场景下窗口类反注册生命周期防护。

---

## 发布资产与 SHA-256 校验说明 (Release Assets)

| 文件名 (File Name) | 适用平台 (Platform) | 说明 (Description) |
| :--- | :--- | :--- |
| **`Tools3000-Setup.exe`** | Windows 10/11/Server (x64) | 官方推荐标准数字签名安装程序 (Standard Signed Installer) |
| **`Tools3000-v1.0.4-Setup.exe`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方归档安装程序 (Versioned Signed Installer) |
| **`Tools3000-Portable-x64.zip`** | Windows 10/11/Server (x64) | 免安装绿色便携版压缩包 (Portable Archive) |
| **`Tools3000-v1.0.4-win-x64-portable.zip`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方绿色便携版压缩包 (Versioned Portable Archive) |
| **`SHA256SUMS.txt`** | 全平台产物 | 官方发布二进制资产的 SHA-256 完整性校验哈希值清单 |

---

## 开源许可证与作者署名 (License & Attribution)

本项目基于 **[MIT License](../LICENSE)** 开源协议发布。

* **项目作者 (Author)**：**`Yy1 (yuan278501381)`**（GitHub：[@yuan278501381](https://github.com/yuan278501381)）
* **项目仓库 (Repository)**：[https://github.com/yuan278501381/Tools3000](https://github.com/yuan278501381/Tools3000)
* **法定版权 (Copyright)**：`Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors`
