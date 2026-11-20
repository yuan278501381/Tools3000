# Tools3000 v1.0.10 官方正式版发布说明

欢迎使用 **Tools3000 v1.0.10** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 在文本识别设置页新增「识别精度档位」选择：日常截图与代码默认使用约 16 MB 的极速轻量版，默认安装包保持零膨胀
- 针对高频识别手写笔记、拍屏与复杂模糊文档的用户，支持一键按需叠加下载 ~90 MB 高精度离线增强模型，下载后自动激活，也可随时一键移除释放磁盘空间
- 未安装高精度增强模型时自动优雅回退极速轻量版，完全不中断日常识别体验。

### 架构演进与技术重构 (底层实现)
- 生成并归档 docs/images/{zh-CN,en-US}/pin_groups_demo.webp（~820 KB），升级 8 卡 2+2+3+1 结构 Bento 总览海报 feature_bento_overview.png 与演播室导出动图 screen_recording_demo.webp，并校验中英双语 README 全部 10 处图像路径与零 Emoji 合规。
- 渲染并归档中英双语共 10 份全新高保真视觉资产（feature_bento_overview.png、ocr_live_text_demo.webp、升级版演播室 screen_recording_demo.webp、content_search_demo.webp、capture_markup_demo.webp），全部动图体积克制在 357~977 KB
- 同步沉淀全场景离线 OCR 进阶、三合一单次切屏手势 E2E 门禁及 README 焕新记录至 PROJECT_MEMORY.md。
- 新增 scripts/test_gesture_suite_e2e.ps1，在单一沙箱 Tools3000 进程中串联执行 Phase 1 基础 E2E、Phase 2 Tier 4 1000Hz 压测与 Phase 3 对抗性极限并发测试，100% 保留全部原始断言与吞吐量校验（实测 14,337 注入事件 / 14,447 处理包 / 555ms 优雅退出全绿）
- 更新 deploy.ps1 调用合并后的单次切屏总门禁，同时为 test_capture_recording_e2e.ps1 补充退出后原前台窗口焦点恢复机制。
- 在 DeepOcrEngine 扩展 mobile / server 双档模型管理、多国内源镜像增量断点下载（复用已装 det/cls/dict 仅补齐 ch_PP-OCRv4_rec_server.onnx）、安全卸载释放 WinML 文件映射句柄与自动回落策略
- 将生效精度档位拼入 OcrEngine 图像指纹缓存 routeTag 杜绝切档命中旧缓存，扩展 ocr.getSettings / ocr.updateSettings / ocr.downloadModel / ocr.deleteServerModel IPC 与双语设置页
- 补充 DeepOcrModelTierFallbackAndServerRemoval 隔离单测，全部 25 项 OCR 单测与 6 重前端静态门禁全绿通过。
- 扩展 OcrQrResult 新增 format 制式字段（默认 QR_CODE，向后兼容），在 OcrEngine::detectQrCodes 接入 OpenCV 4.12 原生 cv::barcode::BarcodeDetector::detectAndDecodeWithType，并为紧贴裁剪小图补齐 <1ms 整图 decodeWithType 直解兜底。
- 扩展 OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine 支持长 URL 与 Windows/UNC 路径末尾分隔符折行续接，严格保护相邻独立链接或独立盘符路径不被误粘。
- 新增 OcrEngineOneDimensionalBarcodeDecodingWithFormat 单测并扩展软折行单测，21/21 实机测试与静态门禁全部通过。
- 在 DeepOcrEnginePostprocess.cpp 实现 suppressRedStampForOcr（高饱和纯红连通域 + 暗文字墨水交叉压覆验证 + R 通道穿透还原）、isShadowedDocumentPhoto / enhanceDocumentPhoto（4x4 亮分位极差检测 + 1/4 降采样形态学闭运算光照场除法归一化）与统一入口 preprocessImageForDetection。
- 在 DeepOcrEngine::extractText 中仅在命中红章穿透时同步更新识别源图，并在 80%+ 竖长框触发 90 度旋转时依据 180 度方向分类器投票直接恢复 90/270 度正向阅读序（0 额外模型推理开销）。
- 新增 DeepOcrRedStampSuppressionShadowNormalizationAndRotatedPageOrder 单测，23/23 OCR 回归与静态门禁全部通过。

### 稳定性与底层演进
- 新增中英双语「桌面贴图与贴图分组管理」高清动图（展示 F3 多图置顶、彩色分组、顶部导览条按组秒切与一键磁吸平铺），让 8 大核心功能全部具备直观动图展示
- 升级演播室动图、Bento 总览图与 README 说明，明确突出录屏支持一键导出 MP4、微信直发 GIF（免点直播表情 / 可双击放大高清大图）与高清动态 WebP
- 将「多会话并发办公」调整为更直观易懂的「多用户并发办公」。
- 将 README 首屏升级为直接自动播放的 2x2 核心场景高清动图画廊，一眼展示极速文件搜索与空格预览、高精度离线文本识别（准确率高于微信截图 OCR）与批注改字、滚动长截图、高清录屏与自动运镜演播室
- 新增「七大核心功能一图速览」Bento Grid 设计海报，并将文档内容搜索、精准截图标注、全局鼠标手势与演示聚光灯全量以直观动图呈现，让访问者打开 GitHub 一秒看懂所有核心能力
- 保持文档克制简洁，中英双语 README 与全套视觉资产 100% 同步。
- 解决发版构建期间 3 套独立鼠标手势测试反复强制切屏、连续多次打断用户全屏工作的痛点
- 现在整个手势端到端与压测门禁仅切换 1 次前台至无副作用的空白安全靶板窗，避免鼠标测试事件误点用户应用，并在测试结束后自动归还原鼠标坐标与用户原始前台窗口焦点。
- 在 OCR 后台扫描中零额外延迟并联识别标准一维条形码（EAN-13 / EAN-8 / UPC-A / UPC-E 等），无论整页单据嵌入条码还是单独紧贴条码截图均可自动解码。
- 复制跨行折断的长 URL（https://...）或 Windows 本地/UNC 文件路径（C:\...、\\host\...）时自动无缝直连还原，避免粘贴后路径或链接断裂。
- 合同、发票、发货单上有红色公章覆盖黑字时，引擎自动穿透消除红章干扰并完整还原底层黑色正文，同时 100% 保护独立红色警示文字与红底白字横幅。
- 手机拍摄手写笔记、白板或纸质单据存在手影与台灯渐变暗角时，自动做大核背景光照归一化漂白以提升暗区检测召回率，对清晰数字截图零干预。
- 整张横排文档/单据被手机拍歪 90 度或 270 度时，自动纠正逻辑行阅读序，避免复制文字错乱。


