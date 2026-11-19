# Tools3000 v1.0.7 官方正式版发布说明

欢迎使用 **Tools3000 v1.0.7** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 修复长截图悬浮控制台点击「完成/暂停/贴图/保存/取消」无响应的问题，确保按钮悬停、防手抖与防连点反馈 100% 必达
- 根治深色聊天/AI 对话及分栏界面的长截图复读机式重复拼接，解决触底微动误拼、手动回滚误报与启动 1 秒秒停痛点
- 为录屏演播室时间线新增全域磁性吸附（拆分点/播放头/运镜/标注双缘对齐）、琥珀金对齐激光线、时长浮标与关键点快捷键跳转
- 优化截图原位改字的邻字保护与右尾顺延字距，彻底解决改字削伤紧邻汉字点画或字距过宽不自然的问题
- 大幅提升高像素手机屏摄原图及反光弱对比度画面的 OCR 识字准确率（消除横向拉扁误识）
- 新增企业防泄密双行左对齐结构水印（账号 + 日期时间双排模板）与截图/录屏只读 HUD 锁暗房深色、交互控件跟随主主题的双层美学

### 架构演进与技术重构 (底层实现)
- 移除 ScrollCaptureOverlay::m_controlHwnd 误调的 applyTaskbarSafeOverlayStyle（消除 WS_EX_TRANSPARENT 鼠标穿透），接入 SetCapture/WM_CAPTURECHANGED、按下期冻结重排与 Z-Order 压顶
- 重构 ScrollCapture 引擎为有效纹理行双轨验证（ZNCC + 前景 MAD）、抗混叠 pyrDown 6 峰/5 簇、单次纹理锚定边栏桥接、32 条带静态侧栏剥离、前/后向对称对决及尾部全局防重复循环门禁（豁免周期表格）
- 在 StudioTimeline 与 StudioPreviewWindow 中落地屏幕空间恒定 DIP 容差的全域磁性吸附纯算法核与多轨对齐引导渲染
- 在 TextReplaceEngine 中实现 MECE 三阶连通域主权护栏、原图真实墨水字距保真顺延与水平表格线硬墙校验
- 恢复 DeepOcrEngine 严格等比高度归一化并新增微秒级弱对比度行 LUT 提纯；扩展 WatermarkRenderer 支持多行左对齐排版与斜铺自适应步距
- 补齐 4 项长截图 MECE 回归单测及 OCR/改字/水印/演播室防回退测试，同步更新 I18nLogCatalog 与 PROJECT_MEMORY.md
- 新增 TextReplaceEngine（Render-and-Match 五维物理闭环克隆、内点主导背景复杂度判别 inlierRatio >= 0.64 防紧凑多行 Telea 鬼影、1px 邻字零误伤护栏、先左右后顶底四向墨水安全走廊顺延与独立局部背景平面拟合、12-Slot 擦除底图缓存与 48-Slot 探针缓存）。
- 集成 WinML DirectML GPU 推理管线 AiInpaintEngine（migan_pipeline_v2.onnx 双源容灾下载，遵循每次新建 TensorUInt8Bit 规避 GPU 脏位），未装模型时复杂纹理平滑降级 OpenCV INPAINT_TELEA。
- 解耦 MarkupEngine 纯净底图烘焙与 PinWindow Direct2D GPU 顶层编辑态渲染（彻底解决 importElements 清零 isEditing 导致的无光标问题，WM_TIMER(3) 零 CPU 重合成）。
- 升级 PinPasteCoordinator::computeContentFingerprint 为全像素 64-bit FNV-1a 精确哈希（抹平 BGR/BGRA 通道差异），并在 stepNext 建立新鲜剪贴板显式复制最高优先契约。
- 补齐 TextReplaceEngineTest（5 项）与 PinPasteCoordinatorTest（含薄波浪线指纹与新鲜剪贴板优先防回退断言，10 项），全量 750/750 单元测试、6 大静态门禁与 10 项系统级 E2E 门禁全部通过，并沉淀至 PROJECT_MEMORY.md。

### 稳定性与底层演进
- 上线「方案 B」截图贴图原位无痕改字能力：选字即点 [改字] 原地修改图片中的文字，支持 60FPS 呼吸闪烁光标、鼠标点选定位与拖选、字号/字重/字体族/背景模式精调、双击二次编辑与 Ctrl+Z/Ctrl+Y 零重跑 OCR 无损还原。
- 新增可选按需下载的 MI-GAN 512 本地 AI 纹理修复模型（设置 -> 文字识别），基础安装包 0 KB 膨胀，支持断点进度、取消下载与一键移除。
- 移除荧光笔、波浪线、直线与改字四枚图标上的冗余字母 A，放大为居中高辨识度纯图形矢量图标，贴图悬浮舱与截图二级栏全局同步生效。
- 位置拖拽升级为编辑框正下方居中横向 6 点微晶拖柄（彻底释放左右对齐视野）与 1:1 刚体平移编辑框，拖拽时带原基线轻磁吸、左右对齐参考丝线与原擦除区虚线幽灵轮廓。
- 新增同行右侧原图文字自动顺延让位（支持 [顺延]/[原位] 一键切换），改长新文字不再覆盖同行后字，且顶/底/左/右四向边缘完整搬运无削切残点。
- 修复截图贴图加波浪线后 Ctrl+C / 点复制再按 F3 贴出标注前旧图的问题，确保 F3 贴图与粘贴到微信的内容 100% 一致。


