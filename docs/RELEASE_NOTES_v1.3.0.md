# Tools3000 v1.3.0 官方正式版发布说明

欢迎使用 **Tools3000 v1.3.0** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 搜索界面收敛为纯净表格视图，移除冗余卡片视图与工具栏排序下拉菜单，全列由表头直接手动排序且空值永远沉底，彻底解决修改时间排序无反应问题
- 表头升级为 100% 整格热区（空白处即可点击排序、拖拽调列与右键选列），修复表头与数据行 14px 对齐错位及跨列拖放吞点击
- 搜索窗在后台且用户切到其他软件时，按 Alt+Space 改为直接呼出搜索窗口而不再误触发上一次结果的 QuickLook 预览
- 彻底根治截图/贴图 OCR 识别长达 20~30 秒的卡顿痛点，同时为全盘文档内容搜索增加后台自动暂停、隐藏即停与 20 秒/3 万文档软熔断及「继续深扫」续扫保护，杜绝无人值守扫盘拖慢整机
- 全面修复独立 Rust Picasa 看图器 P0/P1/P2 问题（删非末尾图不僵死、超长图与畸形图防闪退、系统打开方式与资源管理器定位原生化、注册表原值备份还原与 Win10/11 UserChoice 锁定透明提示、幻灯片平滑叠化与 8 语种地道本地化）
- 发布资产精简为清爽 3 件套（安装包、便携包、SHA256SUMS.txt），安装包瘦身剔除 6 个零引用僵尸 DLL，跨账户卸载干净清理关联并自动继承升级开关

### 架构演进与技术重构 (底层实现)
- 重构 SearchTableHeader / VirtualSearchResults / useViewportProperties / searchUtils，启用 scrollbar-gutter: stable、稳定 sourceResults 解耦、不可变属性缓存、云盘脱机占位属性保护（SearchPropertyExtractor）与 F5 后台重建非遮罩轮询
- 修复 OcrWorkerHost 同目录误清 DirectML Session 缺陷，引入两阶段热保活（60s DXGI Trim + 15min 退出归零）、4KB ONNX 头部校验替代 102MB 整文件读盘、ABOVE_NORMAL 优先级与 QR 局部 ROI 快路径，并保留 4 档 SVTR 甜点桶保证 100% 识别精度
- 在 MftParserQuery / PlainTextExtractor / WindowsIFilterExtractor / SearchPlugin / useSearchEngine 实现内核级背景 I/O 降权（THREAD_MODE_BACKGROUND_BEGIN）、半核并发限流、>16MB 纯文本 4MB 滑窗 SIMD 预过滤、COM IFilter 2s 超时、search.cancel 非阻塞快车道、MessageBridge 8 线程异步池与 20s/30k 断点续扫
- 全量加固 src/viewer（解码尺寸硬上限与 try_reserve_exact、GPU 超限盒式降采样、SHOpenFolderAndSelectItems/SHOpenWithDialog/IFileOperation、WM_CAPTURECHANGED、WM_COPYDATA 单实例、D2D 设备丢失自愈、assoc_dialog 多屏 DPI 与焦点环、真 LRU 缩略图与 MsgWaitForMultipleObjects 零轮询）
- 升级 deploy.ps1、installer.iss、scripts/release.ps1、verify-version.ps1、update-package-manifests.ps1 与 generate-third-party-notices.ps1，统一 Rust 看图器版本 SSOT、Authenticode 签名与 61 个 Rust crates SBOM，修复 Scoop 清单二维数组格式
- 基于纯 Rust + Win32 Direct3D11 / DirectComposition / Direct2D 1.1 / DirectWrite 构建无边框半透明合成窗口（monitor_height - 1 规避 DWM 全屏黑底），原生适配 Per-Monitor V2 100%~250%+ 混合多屏缩放
- 封装帧率无关指数阻尼物理引擎（physics.rs / viewport.rs）：落地四态生命周期（Opening 弹入、Viewing、Slideshow Ken Burns + 900ms 双缓冲叠化、Closing 向心坍缩）、光标锚点对数滚轮缩放、惯性甩动滑行与越界橡皮筋弹簧回弹
- 构建多线程异步预解码与预算制 LRU 缓存池（WIC 硬件加速 + gif / image-webp / png APNG / resvg SVG + QOI/TGA/PNM），动画收敛后自动休眠高频定时器实现 0% 静止 CPU 占用
- PE 静态嵌入双图标资源组（主程序图标与文档关联拍立得图标）及编译期强类型 8 语种词典，20/20 单元测试与离屏 GPU 帧回读校验通过
- WebView2 生命周期治理：修复 TrayWindow/SearchWindow 预载挂起失效，SettingsWindow 增加最小化 TrySuspend 与隐藏态事件合批暂存防唤醒，QuickLookWindow 接入 60s 闲置自动销毁；前端全局挂载 html.t3k-page-hidden 暂停无限动画与隐藏轮询，SearchApp 图标缓存升级为 512 条 LRU
- Plugin_Capture.dll 12 个重型多媒体/AI DLL（~47 MB）接入 MSVC /DELAYLOAD 与安全根目录寻址钩子，新增纯 .NET PE 递归导入解析脚本 sync-delayed-dlls.ps1 接入 CMake 与 deploy.ps1 防洁净打包漏拷贝
- 主进程大块驻留清理：补齐 ScrollCapture/ScreenRecorder/StudioPreviewWindow 清理漏项，PinWindow 落地 cv::Mat COW 写时复制单张 4K 省 33 MB，AiInpaintEngine 接入 90s 闲置回收守护，SpotlightOverlay 启用惰性 D3D11 设备与 60s 闲置释放
- 保留 GestureEngine/GestureTrailOverlay 全生命周期 timeBeginPeriod(1) 保障满载高压手势不掉线；升级全进程树资源采集与 test_memory_lifecycle_e2e.ps1，前端 7 门禁、792/792 C++ 单测及全功能 E2E 全部通过

### 稳定性与底层演进
- 新增零外部依赖单文件独立看图器 Tools3000_Picasa_Viewer.exe，支持真实操作系统半透明桌面穿透、17 种常见/现代/矢量/动图格式极速浏览与高分屏 1:1 像素还原
- 致敬经典 Picasa Photo Viewer 交互布局并扩展实用能力：支持底部一体化胶片缩略图流、对称水晶控制栏、顺/逆时针旋转、水平/垂直镜像翻转、电影级幻灯片放映及两级 Esc 退出
- 内嵌与 Tools3000 主程序一致的 8 国语言并自动跟随系统显示语言，内置可视化文件关联面板及经典拍立得相纸关联文档图标
- 显著降低 Tools3000 后台静默待机内存占用与 CPU 唤醒，同时 100% 保证搜索瞬开、设置草稿、截图贴图、录屏演播室及 CPU 100% 满载手势跟手零损伤
- 清理全部 8 语种 README 中不再提及的 QuickLook 对比项，保持多语言文档行数与口径完全对齐



---

### 文件完整性校验与安全查验 (SHA-256 / VirusTotal)

| 发布文件 | SHA-256 | 安全检测 |
| :--- | :--- | :--- |
| `Tools3000-v1.3.0-Setup.exe` | `ef04d081244d13f3477c5c3116cdafdf91eb389d4034f3597100ebea99ade34a` | [VirusTotal 查验](https://www.virustotal.com/gui/file/ef04d081244d13f3477c5c3116cdafdf91eb389d4034f3597100ebea99ade34a) |
| `Tools3000-v1.3.0-win-x64-portable.zip` | `6f3f3263ca0d73750cc0e4caaf0bac7951bb2c93b1a4270641937e1e366e67f5` | [VirusTotal 查验](https://www.virustotal.com/gui/file/6f3f3263ca0d73750cc0e4caaf0bac7951bb2c93b1a4270641937e1e366e67f5) |

下载后可在 PowerShell 中快速核对本地文件哈希：

```powershell
Get-FileHash .\Tools3000-v1.3.0-Setup.exe -Algorithm SHA256
```

> 如遇 Windows Defender SmartScreen 未知发布者提示，核对上述 SHA-256 与 VirusTotal 无误后点击「更多信息 -> 仍要运行」即可；安装包与便携包内均随附 SBOM.spdx.json 软件物料清单。
