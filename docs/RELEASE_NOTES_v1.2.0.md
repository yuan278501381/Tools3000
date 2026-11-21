# Tools3000 v1.2.0 官方正式版发布说明

欢迎使用 **Tools3000 v1.2.0** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 彻底根治录屏、演播室导出与高精度 OCR 后主进程专用私有内存长期飙升至数 GB 的痛点，主进程常驻保持轻盈低耗，杜绝「关于」监控面板与任务管理器虚拟提交高企
- 独立 OCR 子进程闲置 60 秒自动退场、录屏与演播室导出子进程任务完成即刻退场，由操作系统 100% 物理与虚拟内存双归零
- 关闭演播室窗口时彻底释放高清壁纸原图与缩略图缓存，并将鼠标点击特效高频隐藏日志降级为调试日志消除日志刷屏

### 架构演进与技术重构 (底层实现)
- 根目录 LICENSE 写入 FSF 官方逐字 GNU GPL v3.0 原文以确保 GitHub Licensee 精准识别，同级新增 LICENSE-EXCEPTION.md 声明 GPLv3 Section 7 链接豁免并纳入 deploy.ps1 打包白名单
- 全链路同步 8 语种 README、AboutPage.tsx、8 套前端国际化词典、ui/package.json、Scoop bucket、WinGet 1.10.0 多语言清单（通过 winget validate 实机校验）、SPDX SBOM 生成器与 41 份源码/文档头声明
- 通过 CMakeLists.txt TOOLS3000_BUILD_DATE -> Version.h.in -> MessageBridge app.getSystemInfo -> AboutPage 单一事实源链路透传真实编译日期，并通过全量 7 重静态门禁与前端单测构建验证
- 复用主程序入口 Tools3000.exe --worker=<ocr|studio-export|record-encode> 与 Plugin_Capture.dll 策略表分发器，零额外二进制与零包体膨胀，结合宿主 Job Object 与 ParentProcessWatchdog 双重杜绝僵尸进程
- 构建 OcrWorkerClient/OcrWorkerHost（一次性按需共享内存零拷贝传图 + 命名管道二进制协议 + 纯内存档位透传）、RecordEncodeWorker（单帧共享内存 stage/commit 零锁编码 + PCM 音频复用）与 StudioExportWorker（全字段无损 JSON 序列化 + 实时进度管道 + 命名事件毫秒级取消），全部支持异常平滑回退进程内引擎
- 补齐 StudioVideoCompositor::clearCaches/clearGlobalCaches 与关闭演播室 CoFreeUnusedLibrariesEx 清理链路，修复 DeepOcrEngine 并发卸载原子状态发布顺序竞态，并新增 WorkerIsolationTest 端到端跨进程回归测试验证宿主 0 GPU/MFT 驱动加载与极低内存峰值
- 新增单一事实源清单生成与门禁脚本 scripts/update-package-manifests.ps1，支持根据实测发布产物自动生成并验证 bucket/tools3000.json（含 checkver/autoupdate 与 persist: data 便携模式联动）及 packaging/winget 4 份多语言清单（精准绑定 Inno Setup ProductCode: Tools3000_is1），并已通过微软官方 winget validate 实机校验
- 扩展 scripts/release.ps1 发版流水线，发版时自动向 GitHub Release Notes 注入 SHA-256 与可点击 VirusTotal 直链表，并自动刷新校验与提交包管理器清单至主干与标签快照
- 同步更新 README.md、README.zh-TW.md、README.en.md、README.ja.md、README.ko.md、README.ru.md、README.pt-BR.md、README.es.md 共 8 份文档的锚点导航、徽章矩阵与系统要求段落，保持多语种结构 100% 同构对齐
- 建立三层冷雾铝灰与深邃曜石表面深度令牌体系（index.css）并在 routes.ts 沉淀全 14 模块宝石色盘单一事实源（toneStart/toneEnd），由侧边栏 24x24 宝石 Squircle 图标、页面 42x42 Hero 页眉与模块色顶部径向柔光顶衬全域统一驱动
- 完善 UIKit.css Apple Grouped Inset List 全部四种相邻行发丝分隔线、SettingGroup 26x26 微晶图标徽章与去霓虹发光阻尼 Toggle，并清除 5 个子页面残余上跳动效与硬编码旧紫
- 同步补齐 8 语种侧边栏分组词条，100% 通过 7 重前端静态门禁（含 190 项 CSS 变量与排版底线校验）、Vitest 全部 26 套件与 Playwright 浅/深双主题 12 张实拍人工审片
- DirectML SVTR 推理收口至严格满足 B*W^2 < 1.9M 显存甜点律的 4 档按需微批桶（16x160 / 16x320 / 4x640 / 2x960），规避 4GB 独显换页悬崖并消除跨次复用 TensorFloat 引发的 GPU 缓存锁死缺陷
- tier=server 升级为一阶 10.8 MB Mobile 全量推理 + 二阶低置信度疑难行按需单一 2x640 Server 会话单向择优复核；竖排方向检测 cls 按需 CPU 懒加载配 Top-5 面积代表投票；CTC 贪心解码引入概率分布数学提前终止降至 0 ms
- 摒弃永不释放的 WinML 全局单例 DirectXHighPerformance 设备，改由 DXGI 枚举最高独显自建独占 ID3D11Device 绑定 CreateFromDirect3D11Device，配合 120 秒闲置守护线程按确定性逆序 Close + IDXGIDevice3::Trim + clear_factory_cache + CoFreeUnusedLibrariesEx 实现操作系统级真实私有内存归还，并在 DeepOcrIdleUnloadAndCompactSessionBudget 接入 55%+ 增量回收防回退硬断言
- 在 StudioTimeline.h 建立代理牌组单一事实源常量（640px / 112 帧 / JPEG Q=78 / 160x90 灰度探针），录屏热路径降采样至 640px/112 帧，进演播室后由 StudioVideoCompositor::compressPreviewDeck 立即压缩为约 2.5-4 MB 内存 JPEG 字节流与 1.54 MB 单通道灰度差分牌组并释放未压缩 BGR。
- 将 StudioHiResFrameReader 多帧 LRU 预算从 128 MB / 20 帧收敛至 28 MB / 4 帧，顺序播放态传入 cacheInLru=false 仅刷新单帧 lastDecodedBgr，且 tryGetCachedFrame 兜底命中单帧缓存确保空格暂停调参 0ms 响应。
- 在 tests/unit/test_studio_recorder.inc 新增 CompactProxyDeckAndHiResReaderLruMemoryBudget 防回退单测并通过 32/32 演播室回归验证。
- 在 index.css 建立 --well-* / --thumb-* / --ghost-hover-bg / --hairline 物理触感令牌体系，统一改造 UIKit 分段控制器、Tabs、自定义圆珠滑块、立体折射按钮、稳重无跳动卡片及 Linear/macOS 风格侧边栏与圆角主内容区。
- 重构 SearchViewSettingsModal.tsx 与 styles/search/*.css：顶部索引维护卡去蓝砖化、搜索模式卡片统一单色图标托盘与同心圆 Radio 指示器、默认初始分类改为 3 列微晶标签网格、搜索顶栏工具键改为 Ghost 无框呼吸态、搜索结果行增加左侧 3px 圆头主题微光选中指示针并优化路径与 3D 键帽底栏。
- 全部 7 重前端静态门禁（183 CSS 变量 / 排版 / i18n / 日志 / 内存修剪）、Vitest 196/196 单测、Vite 生产构建及 Playwright 浅/深双主题实拍验收 100% 通过。

### 稳定性与底层演进
- 将项目全仓开源协议由 MIT 正式升级为 GNU GPL v3.0（附带标准 Windows 系统库与 Microsoft Edge WebView2 链接豁免），在保障个人与企业终端用户 100% 免费无限制使用的同时有效防止源码换皮闭源收割
- 在「关于」页面版本号旁新增等宽真实编译构建日期展示（如 v1.2.0 · 2026-11-21），方便用户与排障场景一眼确认构建批次
- 上线即刻可用的 Scoop 自托管一键安装（配置持久化于 data 目录、升级零丢失），并同步产出符合微软官方 1.10.0 规范的 Winget 清单（Yy1.Tools3000）随时可提交官方源
- 全球 8 语种 README 统一升级为高转化阅读漏斗：顶部新增 Stars/Downloads 徽章与直达下载入口；开发初衷补齐微软 PowerToys 试用历程；新增克制客观的一软替代清单明确覆盖范围与适用边界；将安装与下载前置并配套 SHA-256、VirusTotal、Windows SmartScreen 透明说明与 SPDX SBOM 零黑盒验证体系
- 补齐仓库级 SECURITY.md 安全披露规范与 Bug Report / Feature Request 模板，承接开源推广后的用户反馈与社区共创
- 在全球 8 语种 README 首屏总述之后新增「开发初衷」章节，真诚交代作者十年 Everything、FastStone/Snipaste/PixPin、WGestures/WGestures 2 与录屏工具的使用轨迹、托盘瘦身与痛点动因、4 个月重度自用渐进式打磨至今托盘仅留 Tools3000 的真实状态，以及 AI 时代欢迎提 Issue 与 PR 共创的开放姿态
- 顶部徽章新增 x64 | ARM64 架构标识，并在系统要求中如实说明源码与构建体系原生支持 ARM64 及当前预编译分发形态，同时移除全部 8 语种中关于标题栏/设置手动切换语言的冗余后半句
- 彻底摆脱原先偏平庸的灰白列表质感，以 macOS Sequoia System Settings 与 Linear 旗舰桌面软件为标杆，统一重塑主设置界面的侧边栏、全部 14 个子菜单页眉、分组列表卡片及核心外观/模块/关于页面，双主题下均呈现层次清晰的高级质感
- 通用设置页的外观主题升级为三联迷你桌面可视化预览卡片（含 50/50 垂直分屏跟随系统卡片），托盘图标风格升级为全宽四格沉降凹槽分段控制器，主题强调色升级为六联双环精工宝石色珠；关于页技术栈与性能监控分别重构为两列 Bento 微卡片网格与四格 Telemetry 仪表盘
- 密集屏幕文字截屏（141 行 / 1,945 字）高精度模式热态耗时由 8,823 ms 降至 1,605 ms（提速 5.5 倍），90 度竖拍手写压字装箱卡降至 484 ms，识别率保持高于微信截图 OCR
- 取消开机自动加载 OCR 模型（开机 0 MB 占用），停止识字 120 秒后自动释放全部 GPU/模型内存，单次识别后专用私有内存由 913 MB 真实回落至 75 MB（全量 44 项测试后净归零至 111.5 MB），彻底终结多 GB 级常驻内存膨胀
- 解决短时录屏进入演播室后进程内存飙升至 500-800 MB 的痛点，在保留 0ms 悬停掠览、原片停稳超清与实时运镜预设切换的前提下，将演播室常驻内存压降至约 100-150 MB。
- 重塑主设置界面、搜索主窗口与「视图与偏好定制」抽屉整体观感：消除高饱和纯蓝块堆叠、右侧重复大写标签噪音、杂色图标与生硬灰框，带来更通透、克制、耐看的高级桌面软件体验。
- 浅色模式升级为温润霜瓷三层层次（霜灰侧边栏 + 圆角嵌入式浅瓷画布 + 白瓷卡片），深色模式升级为中性丝绒曜石炭黑底面，与全部 6 款动态主题色和谐共融。


