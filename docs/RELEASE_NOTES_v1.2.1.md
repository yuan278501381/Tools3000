# Tools3000 v1.2.1 官方正式版发布说明

欢迎使用 **Tools3000 v1.2.1** 官方正式版。以下为本次版本的核心功能演进、问题修复与底层架构改进摘要。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 全面升级全部 14 个设置页、独立浮窗与抽屉至温润精工视觉体系，采用流体双阶自适应画布（Reading 960px / Workspace 1280px）根治窄栏挤压手势表操作列与 104 键热力图截断问题
- 重塑全站状态徽章与通知弹窗（AppToaster / SavedToast）为无噪微点与自适应紧凑毛玻璃浮层，彻底告别高饱和大绿块；标题栏左上角默认展示立体彩色 3D Logo
- 数据统计「近期活动图表」升级为 1:1 铺满卡片宽度的四阶流光渐变面积图，支持「近 7 天 / 本月 / 本年」一键切换与雕塑毛玻璃悬浮详情卡
- 根治搜文件 F1 语法速查「通配扩展」卡片垂直拉伸变形、排序菜单单选样式、默认搜索模式与默认初始筛选分类重叠冲突，以及多处前后端持久化断链与冗余设置项
- 截图自动捕捉全面穿透现代桌面应用（浏览器、VS Code、Electron、WinUI3、微信、资源管理器），鼠标滑过即精准吸附按钮、卡片、输入区、客户区与无阴影窗口，滚轮或 Tab 多级穿透丝滑跟手
- 对纯图片、Canvas、远控画面等无障碍盲区智能补全高置信度视觉闭合卡片边界，大幅提升截图框选命中率
- 补齐 airy_glacier、dusk_mountains、misty_lake_dawn 三款截图美壳主题后端映射，消除前端切换无响应问题

### 架构演进与技术重构 (底层实现)
- 在 PROJECT_MEMORY.md 增量沉淀 Warm Precision 视觉体系、流体双阶画布、MECE 四维 WindowElementDetector 引擎、统一 AppToaster 与搜索模式正交解耦架构决策
- 在 routes.ts 建立 canvasWidth 单一事实源并全面接入 @container app-canvas 容器查询；新建 AppToaster 统一接管主设置窗与搜文件浮窗全部 sonner 通知并关闭 richColors
- 新增 keyStatsModel 纯数据内核与 ResizeObserver 物理像素级 SVG 坐标系，实现 Fritsch-Carlson 单调三次样条、自然月聚合与防重叠 X 轴刻度，并配套 keyStatsModel.test.ts 回归测试
- 对搜索模式与默认初始筛选分类实施 MECE 正交解耦，修复 SearchApp/useSearchEngine config.get/set 键名错传、SearchPlugin.cpp 旧键覆盖与 excludeSystem 兜底，同步全 8 套多语言词典与 7 重静态门禁
- 新增 WindowElementDetector 实现 MECE 四维探测架构：冻屏瞬间单次 Z-Order 拓扑快照与 DWM 无阴影外框采集、免疫遮罩的后台 MTA 线程懒缓存空间剪枝 UIA2 引擎（40ms/60ms 硬超时防卡死）、冻结画布局部 ROI OpenCV 视觉卡片兜底、以及 IoU/边差语义仲裁去重与自适应圆角归一化
- 通过代际版本号 generation 与 WM_CAPTURE_ELEMENT_REFINED 异步零撕裂回传主线程，优化选框弹簧阻尼系数（k=380, d=36）并补齐 4 项 WindowElementDetectorTest 回归单测

### 稳定性与底层演进
- 在全球 8 语种 README 中完整呈现「QQ/微信截图 -> 标注方便的 FastStone Capture -> 可贴图的 Snipaste -> 滚动长截图与 OCR 的 PixPin」真实工具演进轨迹


