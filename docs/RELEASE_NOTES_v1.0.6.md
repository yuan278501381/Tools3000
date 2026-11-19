# Tools3000 v1.0.6 官方正式版发布说明

欢迎使用 **Tools3000 v1.0.6** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- **任意行距零压字文字批注**：针对代码编辑器、终端日志、密集表格等窄行距场景，波浪线与直线自动探测两行真实缝隙并精准停泊居中，绝不再冲入下一行盖住文字；同时在遇到任意文字笔画（如 `g/p/q/y` 降部或极窄行距字头）时自动退居文字底层，保证字迹 100% 刀锋清晰，宽松网页文档则继续保持舒展大气的斜角书法丝带质感；
- **标注后 `Ctrl+C` 与工具栏复制 100% 必达**：修复在贴图上给选中文字画完波浪线/直线/荧光笔后按 `Ctrl+C` 仍误复制为纯文本的问题；清透批注态下按 `Ctrl+C` 或点击悬浮舱自动切显的「复制图」按钮，均可直接复制带全部最新标注的高清图片；
- **工具栏按钮防抖加固与剪贴板高可用**：放宽贴图右上角与悬浮舱按钮的手抖防误触阈值并增加抬手容差，消除手轻晃 `4px` 被误吞点击及工具栏淡出竞态导致的复制偶发失效，并诚实反馈剪贴板写入结果；
- **OCR 识别耗时全阶段可观测**：日志直接输出每张图的 DBNet 检测、装箱与 Rec 推理逐阶段毫秒耗时及识别行数。

### 架构演进与技术重构 (底层实现)
- `TextAnnotationHelper.h` 新增自适应行距几何求解器（`resolveAdaptiveWavyPlacement` / `resolveAdaptiveStraightPlacement` / `findNearestBelowSpanGap`），随真实行间走廊宽度连续平滑收束振幅、笔宽与直线弓弧；
- `MarkupEngine.cpp` 新增外围纯背景环带采样（`estimateLocalBgBgrFromCanvas`）、像素级真实行间走廊探测（`probeTextRowCorridorFromCanvas`）与对比度自适应文字保护穿底混合（`blendHandDrawnInkBehindTextOntoCanvas`），并补齐冷启动首次绘制荧光笔/波浪线前的 `ensureGdiplusStartup()` 防护；
- `PinWindow.{h,cpp}` 修复清透批注态 `Ctrl+C` 与悬浮舱首钮语义路由、新增 `isInteractiveButtonHitWithSlop`（`8 DIP` 抬手容差）与 `10 DIP` 防抖取消阈值；`ClipboardUtils.cpp` 升级 `OpenClipboard` 双宿主 24 次退避重试；
- `DeepOcrEngine.cpp` 接入 `det` / `pack` / `rec` / `total` 逐阶段毫秒日志。
