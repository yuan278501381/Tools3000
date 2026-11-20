# Tools3000 v1.0.8 官方正式版发布说明

欢迎使用 **Tools3000 v1.0.8** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 演播室自动运镜全面升级为认知友好型体验：新增「柔和 / 标准 / 动感」三档预置与倍率（低至 1.00x）、节奏、触发频率、聚焦暗角四维无级微调，支持「放大时锁定视角」防文字漂移与视觉因果自动纠偏，告别打断观看思路的频繁晃动
- 长截图编辑器支持鼠标悬停在已选中的标注图元（箭头、矩形、椭圆、线条、文字、序号、马赛克等）上滚动滚轮直接调节大小，空白处及未选中区域保持丝滑翻页
- 全新安装现默认开启「截图贴图与录屏」核心模块（同时离线高精度 OCR 模型包仍保持按需可选，不强制捆绑）
- 根治安装向导误报「先前程序的安装/删除未完成，必须重启计算机」的烦人拦截，即使旧版卸载/升级留下待重启残留也能零重启自动自愈秒装，并修复中文重启选项错显为「我同意此协议」的语言包乌龙
- 优化侧边栏与手势页搜索框视觉细节，消除聚焦时内部刺眼的直角蓝框，统一呈现圆润外层柔光焦点环。

### 架构演进与技术重构 (底层实现)
- StudioTimeline / StudioVideoCompositor / StudioPreviewWindow 实现三档预置 + 四维无级参数 SSOT 持久化、非对称进出镜临界阻尼弹簧、84% 三脚架死区锁镜、160x90 代理帧视觉差分因果校正与微秒级低分高斯羽化聚光暗角
- CaptureInteractionPolicy.h 建立长截图滚轮路由单一事实源（resolveLongImageWheelRoute），引入非钳制文档坐标命中与随 DPI/缩放自适应的 11 DIP 恒定视觉容差带
- installer.iss 为 capture 父组件补齐 Flags: checkablealone 并同步 PluginManager::reconcilePluginConfigs 空白配置 7 模块同构初始化
- installer.iss 在 NextButtonClick(wpReady) 抢先于 wpPreparing 门禁之前挂载 Win32 原生字节级 REG_MULTI_SZ 自愈引擎（覆盖 PendingFileRenameOperations 与 PendingFileRenameOperations2，杜绝 RegWriteMultiStringValue 截断末尾空串破坏系统奇偶配对），修复 FilesLocked 漏杀缺陷并将释放等待窗扩展至 3500ms；同步修正 ChineseSimplified.isl 重启单选词条
- Sidebar.css / GesturePage.css 清除被全局 :focus-visible 覆盖的内层输入框轮廓，补充全套 C++ 防回退单测并沉淀 PROJECT_MEMORY.md。


