# Tools3000 v1.0.1 官方正式版发布说明 (Official Release Notes)

欢迎升级至 **Tools3000 v1.0.1 官方正式版**！本次发布围绕 **桌面贴图与截图标注心流重塑、跨表面原生/Web 主题大一统、百万级搜索引擎内存瘦身与并发提速、底层键鼠管线收敛** 进行了全栈深度进化，累计合入 37 项体验打磨与工程加固。

Welcome to **Tools3000 v1.0.1 Official Release**! This release delivers a comprehensive Direct2D crystal context menu overhaul for desktop pins, multi-point polyline & centripetal smooth curve markup, 50-step full-transaction vector Undo/Redo, cross-surface dark/light & accent theme unification, and a ~50.3 MB memory reduction across the multi-million-file search index.

---

## 本次版本核心亮点与体验升级 (Key Highlights)

### 1. [贴图与交互] Direct2D 微晶自绘右键复合面板与实时无级不透明度滑块
- **三段式减负信息架构**：彻底告别旧版平铺 23~25 行的系统灰白长菜单，重构为 **「顶栏 6 枚图文快捷格 + 实时无级不透明度滑块 + 6~7 行精炼主干」**，一级扫视认知负荷降低 65%，且 100% 保留全部既有功能与键盘快捷键；
- **高频动作抬手即达**：顶栏等宽排布 `复制`、`标注`、`识字`、`旋转`（支持 `Shift + 点击` 直接逆时针旋转 90°）、`反色`（激活态高亮微晶胶囊）、`保存`，大幅缩短鼠标移动距离；
- **连续无级不透明度预览**：支持 `10% ~ 100%` 丝滑拖拽与悬停滚轮 `±5%` 步进实时预览（内置 `50%` / `75%` / `100%` 柔和磁吸与点击百分比胶囊一键复位 `100%`），调完不强制关闭面板，方便连续勾选鼠标穿透等状态；
- **动态上下文感知与宝石色分组级联**：局部划选 OCR 文字时首行自动置顶 `复制所选文字`；单张贴图时自动隐藏全局批处理噪音，多贴图时提供 `关闭其他贴图` 一键清理；缩放非 `100%`、反色、灰度及所属分组通过微晶徽章与真实宝石色圆点在一级表面直观回显，并支持三级分组换色、多屏边缘自动翻转避让与超多分组滚轮视口。

### 2. [截图标注与 OCR] 多点折线/美感向心曲线连接、文字交叠防挖空与无损编辑栈
- **直线工具双模态升级（多点折线 & 美感平滑曲线）**：保留按住拖拽秒画单段直线的直觉手感；轻点画布即进入多点连续连线模式，二级属性栏支持一键互切 **「多点折线连接」** 与 **「美感向心样条曲线 ($\alpha = 0.5$)」**（严格穿过每个点击锚点、数学保证零自交打结，落盘采用 `1/16 px` 亚像素精度消除斜向锯齿），支持 `Backspace` 撤回锚点、靠近起点磁吸闭合成环及独立顶点微调手柄；
- **默认燕尾箭头与思源黑体字芯杂色根治**：箭头工具初始默认升级为现代利落的**燕尾箭头**；根治 `Noto Sans SC` / `Source Han Sans SC` 汉字交叠笔画在 `FillModeAlternate` 奇偶规则下产生的交叉处白色挖空虚线杂色，切换为非零环绕规则 (`FillModeWinding`) 与离屏满不透明度合成；
- **50 步全事务矢量 Undo/Redo 栈与非 `1.0x` 贴图无损二次标注**：标注引擎支持改色、平移、`Delete` 与清空的 100% 可逆撤销重做；`PinWindow` 分离保存 1:1 原始无损底图与矢量图元列表，根治缩放态二次标注引起的尺寸膨胀或糊化，并在 `>= 300%` 放大时自动呈现像素级网格；
- **双栏文档 XY-Cut 智能分栏 OCR**：离线 OCR 引擎引入递归垂直空白走廊投影分栏算法，彻底解决双栏论文、并排对比代码框选提取时的左右串行问题。

### 3. [文件与正文搜索] 百万级索引立省 ~50 MB 内存与无锁并发加速
- **索引结构精简与零锁阻塞水合**：移除 `FileIndexStore` 冗余平行键表，在 275 万文件规模下立省 **`~50.3 MB`** 常驻物理内存；引入支持嵌套工作窃取的常驻查询线程池，消除持锁期间的磁盘属性阻塞；桌面/开始菜单快捷方式与 `FOLDERID_AppsFolder` UWP 应用提权排序；
- **斜杠路径双级预筛与 Excel 行号精确还原**：支持形如 `service\main.cpp` 的目录位掩码 + 末段文件名 $O(1)$ 预筛；`ZipXmlExtractor` 升级 65KB EOCD 按需流式解压并解析 `.xlsx` `sharedStrings.xml` 按行精确还原单元格文本；`rust_content` 补齐运行时 AVX2 动态派发与 ARM64 NEON 加速。

### 4. [全局视觉、设置中心与系统底座] 跨表面主题大一统与零空转省电
- **原生 Direct2D 浮层与 WebView2 主题/强调色全同步**：建立 `NativeThemePalette` 单一事实源，截图工具栏、贴图悬浮栏/右键面板/分组栏、长图控制台、录屏灵动岛、离线 OCR 结果窗、手势轨迹胶囊与文件对话框导览条全面联动深浅色模式与自定义主题强调色，WebView2 首帧同步挂载 `data-theme` 消除亮色冷启动闪屏；
- **设置中心 `Ctrl + K` 细粒度全文检索与纯矢量双回车热力图**：侧边栏搜索支持直达任意具体开关配置项；`KeyboardHook` 区分 `LLKHF_EXTENDED` 独立统计主键盘回车与小键盘回车热力分布；
- **后台零唤醒休眠与任务栏图标防污染**：合并复用单一底层键盘钩子，触发角与异步旋转日志空闲态零唤醒休眠；修复快捷方式元数据同步跨路径污染外部固定任务栏图标的问题。

---

## 发布资产与 SHA-256 校验说明 (Release Assets)

| 文件名 (File Name) | 适用平台 (Platform) | 说明 (Description) |
| :--- | :--- | :--- |
| **`Tools3000-Setup.exe`** | Windows 10/11/Server (x64) | 官方推荐标准数字签名安装程序 (Standard Signed Installer) |
| **`Tools3000-v1.0.1-Setup.exe`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方归档安装程序 (Versioned Signed Installer) |
| **`Tools3000-Portable-x64.zip`** | Windows 10/11/Server (x64) | 免安装绿色便携版压缩包 (Portable Archive) |
| **`Tools3000-v1.0.1-win-x64-portable.zip`** | Windows 10/11/Server (x64) | 带明确版本号标识的官方绿色便携版压缩包 (Versioned Portable Archive) |
| **`SHA256SUMS.txt`** | 全平台产物 | 官方发布二进制资产的 SHA-256 完整性校验哈希值清单 |

---

## 开源许可证与作者署名 (License & Attribution)

本项目基于 **[MIT License](../LICENSE)** 开源协议发布。

* **项目作者 (Author)**：**`Yy1 (yuan278501381)`**（GitHub：[@yuan278501381](https://github.com/yuan278501381)）
* **项目仓库 (Repository)**：[https://github.com/yuan278501381/Tools3000](https://github.com/yuan278501381/Tools3000)
* **法定版权 (Copyright)**：`Copyright (c) 2026 Yy1 (yuan278501381) & Tools3000 contributors`
