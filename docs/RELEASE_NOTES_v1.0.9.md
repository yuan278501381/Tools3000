# Tools3000 v1.0.9 官方正式版发布说明

欢迎使用 **Tools3000 v1.0.9** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 彻底根治虚拟机及物理机直接运行安装包覆盖升级后自动重启系统的问题，无论交互式点击完成还是静默安装均实现 100% 物理零重启当场生效
- 清除演播室扫描外部第三方软件目录的历史残留，确保任意纯净新机安装后背景数量与开发机 100% 一致且零版权风险
- 演播室背景扩充为 49 款内置预设（21 张原创高清壁纸 + 21 款双级流体丝绸极光渐变 + 7 款极简纯色），支持「全部 / 沉稳 / 轻盈 / 风景 / 渐变 / 自定义」6 分类切换与悬停名称提示
- 升级演播室画面边缘光学合成层：将旧版向内遮挡画面的硬白框重构为包裹在画面外围的磨砂水晶折射外托框（可调 0~16px），搭配三层空气悬浮阴影与平滑圆角高光棱线。

### 架构演进与技术重构 (底层实现)
- 新增 SSOT 判定函数 OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine：基于转义感知双引号状态机与 JSON 上下文检测，结合同栏邻接几何约束及漏引号跨新键 "Key": 防误粘护栏。
- 统一接入 PinWindowOcr::joinAlignedSelectedPieces、OcrEngine::orderTextBlocksWithXYCut、OcrEngine::recognizeToText 与 OcrEngine::smartMergeParagraphs。
- 新增单测 OcrEngineSoftWrappedJsonSeamlessJoinAndGuards，并在实景回归 FixtureApifoxCompactWideJsonRegression 中用 nlohmann::json::parse 端到端硬断言 5 行压缩 JSON 100% 合法解析，22/22 测试与静态门禁全绿。
- 在 DeepOcrEngine 新增静态后处理 DeepOcrEngine::rescueMissedUnderscores：使用局部包围窗背景估计（兼容单行混合底色），在原图上部主体带 [12%..72%] 锁定无墨水空隙并在底部 [72%..100%] 校验扁平连续水平下划线笔画，附带左右邻字脚下反通长下划线护栏。
- 扩展 DeepOcrEngine::normalizeStructuredJsonPunctuation 覆盖行尾孤立 "key": f / 内嵌 "key":f"child": / 数组 [f"key": 的左花括号规整，严格豁免 "flag": false 与 "code": "f"。
- 新增合成像素单测 DeepOcrPixelUnderscoreRescueAndHyperlinkGuard 并强化实景回归 FixtureApifoxCompactWideJsonRegression，LiveTextOcrTest 18/18 与静态门禁全绿。
- 在 tests/unit/test_live_ocr.inc 新增 5 组实景回归用例（Catalog 完整性校验全环境必跑，模型未安装环境优雅 GTEST_SKIP）。
- 增强 OcrEngine::detectQrCodes：在 1x detectAndDecodeMulti 未命中时增加单码强透视 detectAndDecode 与 2x INTER_CUBIC 上采样兜底，并按真实 invScale 映射角点回原图坐标系（于 extractPage 后台并发执行，0ms 额外感知延迟）。
- 同步沉淀全库 9 大巨石文件拆分与实景 OCR 测试决策至 PROJECT_MEMORY.md；全量 Tools3000Tests 775/775 通过（+1 环境变量基准跳过），前端 6 重门禁与生产构建 100% 通过。
- 拆分产出 src/service/MftParser.cpp (1,342 行) 与 src/service/MftParserQuery.cpp (1,713 行)，零公开头文件改动，同步更新 CMakeLists.txt SERVICE_OBJECT_SOURCES。
- 通过 Tools3000_Service 与 Tools3000Tests 编译、51 项搜索/MFT/USN/索引单元测试及静态门禁验证。
- 在 ui/src/styles/search/ 下拆分 base-header.css (322 行)、filters-syntax.css (601 行)、results-footer.css (807 行)、popover-formats.css (1,213 行)、modals-overlays.css (472 行)，ui/src/SearchApp.css 收敛为 10 行顺序 @import 清单。
- 通过 eslint、check-i18n、check-logger、check-css-variables、check-typography、check-trim-workingset、tsc -b、vite build 与 check-production-html 全套前端门禁。
- 新增私有跨单元声明头文件 src/ocr/DeepOcrEngineInternal.h，零改动公开头文件 DeepOcrEngine.h 与外部 ABI。
- 拆分产出 DeepOcrEngine.cpp (1,410 行)、DeepOcrEngineModel.cpp (931 行) 与 DeepOcrEnginePostprocess.cpp (1,334 行)，同步更新 CMakeLists.txt 中 Tools3000Tests 源文件列表。
- 通过 Plugin_Capture 与 Tools3000Tests 编译、39 项 OCR/TextReplace 专项单元测试及 logger-check、trim-workingset-check 静态门禁验证。
- 新建私有内部协作头 src/capture/StudioVideoCompositorInternal.h（47 行，声明 compositor_detail 圆角/Alpha 混合/色值/质量系数与时间轴标注渲染入口），StudioVideoCompositor.h 公开 ABI 零改动
- 纯物理平迁至 StudioVideoCompositor.cpp（1,766 行）、StudioVideoCompositorAnnotations.cpp（1,245 行）、StudioVideoCompositorExport.cpp（2,004 行）并接入 capture_objects
- 完整通过 Plugin_Capture / Tools3000Tests 编译、*Studio*:*Record*:*Watermark* 57/57 单测、logger-check（728 条）与 trim-workingset-check（24 处）
- 新建私有内部协作头 src/capture/MarkupEngineInternal.h（43 行，声明唯一 GDI+ 启动状态访问、平滑样条、花样直线、文本高亮/下划线、统一聚光灯与两套 Handler 工厂注册入口），MarkupEngine.h 公开 ABI 零改动
- 纯物理平迁至 MarkupEngine.cpp（1,390 行）、MarkupEngineShapes.cpp（2,442 行）、MarkupEngineEffects.cpp（2,085 行）并接入 capture_objects
- 完整通过 Plugin_Capture / Tools3000Tests 编译、*Markup*:*TextReplace*:*Spotlight*:*Magnifier* 全量专项单测、logger-check（728 条）与 trim-workingset-check（24 处）
- 新建私有内部协作头 src/capture/CaptureInputInternal.h（34 行，声明 input_detail 比例/取色/光标/保存格式/合并撤销辅助）并在 CaptureInput.h 私有区增加两个非虚消息路由成员（零公开 ABI 变更、零类内存布局变化）
- 纯物理平迁至 CaptureInput.cpp（1,983 行）、CaptureInputCommands.cpp（1,335 行）、CaptureInputMouse.cpp（2,000 行）、CaptureInputKeyboard.cpp（1,669 行）并接入 Plugin_Capture
- 完整通过 Plugin_Capture 编译、截图集成/工具栏/圆角/快捷键/长图单测、logger-check（728 条）与 trim-workingset-check（24 处）
- 新建私有内部协作头 src/capture/CaptureRendererInternal.h（70 行，集中声明色彩格式换算、TextFormatScope RAII 与 renderer_detail::getBeautyColors），CaptureRenderer.h 公开 ABI 零改动
- 纯物理平迁至 CaptureRenderer.cpp（1,562 行）、CaptureRendererToolbar.cpp（1,906 行）、CaptureRendererMarkup.cpp（2,460 行）、CaptureRendererHud.cpp（1,974 行）并接入 Plugin_Capture
- 完整通过 Plugin_Capture 编译、CaptureRenderer/Markup/LongImage 专项单测、logger-check（728 条）与 trim-workingset-check（24 处）
- 新建私有内部协作头 src/capture/StudioPreviewWindowInternal.h（276 行，集中管理 HitId、调色板常量与 studio_detail 辅助声明），StudioPreviewWindow.h 公开 ABI 零改动
- 纯物理平迁至 StudioPreviewWindow.cpp（1,881 行）、StudioPreviewWindowExport.cpp（1,447 行）、StudioPreviewWindowDrawers.cpp（1,447 行）、StudioPreviewWindowTimeline.cpp（1,925 行）、StudioPreviewWindowRender.cpp（1,498 行）、StudioPreviewWindowInput.cpp（2,350 行）并接入 capture_objects
- 完整通过 Plugin_Capture / Tools3000Tests 编译、*Studio*:*Record*:*Coach*:*Timeline*:*Watermark* 57/57 单测、logger-check（728 条）与 trim-workingset-check（24 处）
- 废除 DeepOcrEngine 多行横向 FFD 拼接（消除 SVTR Transformer 全局自注意力跨行串扰、128 暗灰隔断墙及 col(rw-1) 边列墨水涂抹），升级为单行隔离 3 档宽度分桶（W=320/640/960）微批次推理 + 局部洁净背景呼吸边距
- 新增 coalesceSameLineTinyFragments、refineAxisAlignedLineBox（防碰撞首尾标点墨水挽救）、超宽行 28~52px 重叠滑窗洁净单列谷底拼接、isUiDropdownChevronPatch / stripUiDropdownChevrons 及 normalizeStructuredJsonPunctuation（严格 1:1 保持 text 与 charBoxes 对齐）
- 在 tests/unit/test_live_ocr.inc 新增 3 项防回退单元测试，*Ocr*:*Pin*:*TextReplace* 85/85 通过，logger-check（728 条）与 trim-workingset-check（24 处）全部通过
- 将 pin_detail::buildPinContextMenuSnapshot/executePinContextMenuCommand、showGroupPopupMenu、forceHoverStateForTest 至 executeInteractiveButton 与完整 pinWndProc 消息循环（2,013 行）按原语义平迁至 src/capture/PinWindowInput.cpp
- 完整编译 Plugin_Capture / Tools3000 / Tools3000Tests，全量运行 Tools3000Tests.exe 767/767 通过（1 项需环境变量基准跳过），logger-check（728 条）与 trim-workingset-check（24 处）全部通过
- 将 PinWindow::ensureTextResources 至 drawHoverToolbar 及 createRenderResources 至 render（1,734 行）按原语义平迁至 src/capture/PinWindowRender.cpp
- 接入 Plugin_Capture 编译目标，完整通过 60 项 *Pin*:*TextReplace* 真机回归单测及 logger-check / trim-workingset-check 静态门禁
- 将 pin_detail::cleanupOcrWorkingSet、normalizePinOcrBlocksAndTableState、assembleSelectedSpansText 等辅助函数与 PinWindow::isLiveTextEnabled 至 redoPinMarkup（1,249 行）按原语义平迁至 src/capture/PinWindowOcr.cpp
- 接入 Plugin_Capture 编译目标，完整通过 60 项 *Pin*:*TextReplace* 真机回归单测及 6 重日志与 trimWorkingSet 静态门禁
- 新增私有内部协作头 src/capture/PinWindowInternal.h，集中声明跨编译单元共享枚举、纯内联辅助及 ODR 单一事实源 extern 原子量，避免多文件 static 状态分裂
- 将 PinWindow 原位改字全套实现（486 行）按原语义平移至 src/capture/PinWindowTextReplace.cpp 并纳入 Plugin_Capture 构建目标
- 通过全部 60 项 *Pin*:*TextReplace* 单元测试及日志/内存修剪静态门禁验证
- 在 DeepOcrEngine 新增 isLowContrastScreenPhoto 快采样判据与 enhanceLowContrastForDetection（LAB 色度通道高斯平滑去彩纹 + L 通道温和 CLAHE 融合），实行检测/识别双分支解耦并将识别切片低反差拉伸阈值放宽至 132
- 在 PinWindow 跨版面分区流式选字分支中引入 sideBySideColumns 几何判别，仅真正左右并排双栏走带可见引导框（m_showCrossRegionRectGuide）的二维围选，上下堆叠分区维持完整阅读序流式选字
- 新增 DeepOcrLowContrastScreenPhotoEnhancementTest 与真窗口回归测试 PinWindowCrossRegionVerticalFlowSelectionTest，全套单测及日志/内存门禁通过
- 调用项目原生 MarkupEngine、FloatingGlassBar、CaptureVectorIcons 与 Direct2D/DirectWrite 离屏渲染管线生成 1.5x 高清中英双语实际效果截图（docs/images/{zh-CN,en-US}/scene_pin_live_text_wavy.png）
- 校验 README.md、README.en.md 与 docs/DEVELOPER.md 全部相对路径资源与链接有效性，确保零 Emoji、零硬编码旧版本号及零内部实现符号泄漏
- 新增 SessionIsolation 会话隔离模块，单实例互斥体切换为 Local\Tools3000_SingleInstance_Mutex，并基于 dwShareMode=0 与 FILE_FLAG_DELETE_ON_CLOSE 内核独占锁实现 WebView2 Slot 0 零迁移抢占、Slot 1..15 并发复用及 7 天过期副槽位冷清理
- 为 AtomicFile::atomicWriteFileWithFlush 补齐 MoveFileExW 共享冲突短退避重试，将 ConfigManager/StatsManager 导出落盘统一收口至单一原子写入事实源，并同步更新 installer.iss 多前缀运行检测
- 在 WinUtils 新增 isRemoteDesktopSession（覆盖 SM_REMOTESESSION/SM_REMOTECONTROL 及 NtQueryInformationProcess 命令行识别远控活跃被控会话）与 canSafelyExcludeWindowFromCapture，自动规避远控场景下 WDA_EXCLUDEFROMCAPTURE 导致的黑屏/隐身
- 重构 scripts/sign-artifacts.ps1 为多策略分发架构（azure-trusted-signing/pfx/cert-store 批量签名），内置多 RFC 3161 TSA 容灾、-VerifyOnly 审计模式与 -RequirePublicTrust 公网根 CA 强门禁，联动 deploy.ps1 与 release.yml 实现双轮云端签名，配套全套 C++ 防回退单测
- installer.iss 全量移除 [Files] 段中的 restartreplace 标志，升级覆盖安装无条件进程树清场与递归文件锁扫描，并在 CurStepChanged(ssInstall) 引入基于 MoveFileExW 的 NTFS 原地改名腾位引擎（EvacuateLockedFilesInTree）及 wpFinished 完成页 NoRadio 防呆锁
- 新增 StudioBackdropCatalog.{h,cpp} 与 ui/src/constants/studioBackdrops.ts 构建前后端 49 款背景单一事实源及历史主题键兼容映射，在 PluginEntry.cpp 接入统一白名单校验与一次性 Schema 平滑迁移
- StudioVideoCompositor 重构 ensureBackdropCache 与 renderArtisticBackdropBgr：实现 1/8 DCT 低内存缩略图缓存、双级流体域扭曲（2-Octave Domain Warping）丝绸着色器、31px 高斯磨砂折射外托框、1/4 低分三层 152px 漫射深影及解析式圆角矩形 SDF 亚像素方向性高光棱线
- 在 test_core.inc 与 test_studio_recorder.inc 补充禁用 restartreplace、背景目录完整性、第三方路径清零及水晶外托框折射渲染防回退单测，并同步沉淀 PROJECT_MEMORY.md。

### 稳定性与底层演进
- 解决压缩单行 JSON 或长 URL 在编辑器自动软折行（例如中文字"浇/封"、"电/子"、"产/品"跨行）时，贴图划选复制或对照面板复制会在字符串内部插入硬换行符 \r\n 导致粘贴到 Postman/Apifox/VS Code 报 JSON 语法错误的问题。
- 复制整段软折行紧凑 JSON 后可直接通过 Postman Beautify 一键格式化，无需手工删除中间换行。
- 修复 Apifox 紧凑 JSON 底部 "message": { 单个左花括号在 JPEG 压缩小字号下被误识为 "message": f 的问题。
- 修复浅蓝选中高亮背景与紧凑行距下 "bom_Make"、"User_Text"、"U_financial_type" 等 1px 贴底下划线被识别为空格的细节瑕疵，且对普通英文空格与带通长下划线的超链接文字零误伤。
- 将用户提供的全部 8 张真实文字识别截图纳入仓库版本管理（tests/fixtures/ocr/，涵盖 SAP 多栏表单、密集技术文档、PDA 摩尔纹拍屏、会议室大屏 PPT、ERP 深灰表头拍屏、工业产品装箱卡与二维码、Apifox 格式化及超宽紧凑 JSON）。
- 修复工业装箱卡等复杂背景中占画面极小（约 75x75px）的倾斜二维码在默认 1x 多码检测下漏检的问题，实现文本识别与二维码 ZZ00444040 同步稳定提取。
- 在每次单元测试中持续守护 OCR 识别准确率、JSON 标点规整、UI 下拉箭头剥离、多栏防串行、二维码解码以及真实 PPT 屏摄 5% -> 10% 原位改字零抹除防回退。
- 将原 3,029 行 NTFS MFT/USN 引擎拆分为卷索引监听管理与多维度搜索查询两大高内聚模块，达成全仓库业务源码文件全部低于 3,000 行的清洁治理目标。
- 保持文件实时索引、USN 追赶、拼音/正则/通配符匹配与内容检索候选集筛选行为 100% 等价无回退。
- 将原 3,399 行全局搜索界面样式拆分为基础输入栏、筛选排序与语法速查、结果列表与底部状态栏、偏好气泡与格式定制、右键菜单与模态遮罩五个高内聚模块，方便搜索界面的主题与交互增量维护。
- 严格保持原 CSS 选择器顺序与层叠优先级 100% 等价，零视觉样式漂移。
- 将原 3,592 行 PP-OCRv4 深度推理引擎源文件拆分为核心推理运行时、模型包管理下载校验、图像预处理与后处理规整三大模块，大幅提升离线 OCR 管线后续迭代与定位效率。
- 保持检测、识别、JSON 标点规整、UI 下拉箭头剥离与多档宽度微批次推理行为 100% 等价无回退。
- 将演播室视频合成巨石 StudioVideoCompositor.cpp（4,883 行）无损拆解为视觉舞台/缩略图/单帧合成核心、时间轴矢量标注渲染、FFmpeg 视频/GIF/WebP 导出与高分帧读取器 3 个高内聚单元，100% 维持渲染画质、导出质量与性能零回退
- 将截图标注巨石 MarkupEngine.cpp（5,831 行）无损拆解为标注引擎核心/撤销栈/注册表、几何线条与手绘高亮处理器、马赛克/模糊/文本/序号/放大镜/聚光灯/水印/消除/原位改字处理器 3 个高内聚单元，100% 保持全部标注工具与撤销重做零回退
- 同步规范化前序拆分文件的标准 CRLF 行尾编码
- 将截图输入控制巨石 CaptureInput.cpp（6,790 行）无损拆解为光标/选区/标注核心、工具栏命令执行、鼠标消息交互、键盘/IME/滚轮/定时器交互 4 个高内聚单元，100% 维持截图全套交互与热键零回退
- 将截图渲染巨石 CaptureRenderer.cpp（7,805 行）无损拆解为资源/DPI/选区基础、工具栏/浮窗/美化套壳、矢量图标与标注预览、放大镜/取色/二维码/长图工作台与主合成 4 个高内聚单元，100% 保持既有截图视觉与交互零回退
- 将全库最大巨石文件 StudioPreviewWindow.cpp（10,506 行）无损拆解为生命周期核心、异步导出/模态窗、左侧抽屉、时间轴与动效教学卡、主舞台渲染、输入交互消息循环 6 个高内聚单元，保持演播室全部功能与性能 100% 零回退
- 根治 API/JSON 截图中的典型误识（"DocTota1FC" : O.0, -> "DocTotalFC" : 0.0,、"Comments" : "- -> "Comments" : "",、"U_Bi11Type" : "Y"_ -> "U_BillType" : "Y",、恢复整段 "WddStatus" : "-",、Sfilter/Sorderby -> $filter/$orderby，以及超宽紧凑单行 JSON 下划线/花括号与结构位全角标点污染）
- 像素级精准剥离界面按钮旁的 UI 下拉框箭头图标（GET v -> GET、Body~ -> Body、独立下拉箭头 √ 清除），且对正文真实字母 v/V 与非对称勾选符零误伤
- DirectML GPU 实测 896x632 JSON 截图热运行耗时由 1,273ms 降至 892ms（提速 30%）
- 完成 PinWindow.cpp 巨石文件纯物理 5 分片零回归试点重构（原 7,185 行 -> 主文件瘦身至 1,812 行），贴图右键菜单、热区命中、鼠标/键盘/IME 交互与窗口生命周期边界彻底清晰且 100% 保持既有功能零回退
- 同步在 PROJECT_MEMORY.md 沉淀零回归纯物理拆分四条铁律，供后续大文件重构复用
- 将贴图 Direct2D/DirectWrite 渲染管线、实景文字选区/悬浮工具栏/原位改字与批注绘制剥离为独立编译单元，PinWindow.cpp 瘦身至 3,781 行且 100% 维持渲染表现与交互行为不变
- 将贴图后台异步 OCR、字符级选区拼装、表格/二维码提取与荧光笔/波浪线/直线文字批注剥离为独立高内聚编译单元，PinWindow.cpp 进一步瘦身 1,213 行且 100% 维持既有行为不变
- 启动 PinWindow.cpp 万行巨石纯物理拆分试点，优先剥离高内聚的原位无痕改字编辑模块，提升代码可读性与增量编译效率，100% 保持既有功能与交互手感不变
- 提升手机拍屏 ERP 表格等低反差深灰表头与强摩尔纹场景下的离线 OCR 召回率与字准（补齐深灰表头文字及紧贴黑线的短数字），原生清晰桌面截图保持零耗时直通
- 修复贴图窗口在上下堆叠多版面图片上从上往下流式拖选时右侧文字被隐形竖线截断的体验问题，确保拖选所见即所得
- 更新中英双语 README 首屏展示图，以“新版本发[不|]前核对清单”原位编辑错别字直观呈现原位无痕改字能力
- 将 README.md 与 README.en.md 全面改版为极简极客名片风（~118 行），去除内部研发术语、过时版本号、拉踩对比表与冗余模块介绍，让普通用户第一屏即看懂产品亮点并快速下载上手
- 重点突出滚动长截图及贴图直接复制文字、画波浪线/荧光笔/直线下划线与原位改字等核心卖点，并在首屏直接展示中英双语真实效果截图
- 将源码编译、系统架构与进阶开发文档独立收纳至 docs/DEVELOPER.md，实现用户门面与开发者文档清晰解耦
- 解决 Windows Server 多管理员并发桌面会话（Console + 多路 RDP，含同账号）无法并行运行 Tools3000、WebView2 锁死与共享配置/统计写冲突问题
- 解决通过 RustDesk/向日葵/ToDesk/RDP 远程连接 Windows Server 录屏时控制条与浮层隐身、用户误判录屏无响应的痛点
- 升级代码签名体系以原生支持微软 Azure Trusted Signing（OIDC 无静态密钥）、SignPath 开源计划与商业证书，打通零 SmartScreen 拦截公网发版链路并补齐实操文档


