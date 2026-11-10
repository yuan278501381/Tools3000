# Tools3000 — 开发任务清单

## Phase 1 — 基础设施 + 鼠标手势 MVP

### 1.1 项目骨架搭建
- [x] 顶层 CMakeLists.txt + vcpkg.json
- [x] .clang-format / .clang-tidy 配置
- [x] src/main.cpp 入口点
- [x] WebView2 集成（Evergreen Runtime）
- [x] 前端项目 ui/ 初始化（Vite + React + TypeScript）

### 1.2 核心基础设施
- [x] 统一日志系统（Logger + TraceID）
- [x] 配置管理器（JSON 读写 + 热加载）
- [x] 全局快捷键管理器
- [x] 系统托盘图标
- [x] WebView2 ↔ C++ IPC 桥接
- [x] 崩溃收集（MiniDump）

### 1.3 鼠标手势核心
- [x] 低级鼠标钩子（WH_MOUSE_LL）
- [x] 方向编码手势识别算法
- [x] 手势动作分发系统
- [x] 默认手势集（14个全局 + 2个浏览器专用）
- [x] 作用域规则引擎（进程/类名/句柄）
- [x] 手势轨迹可视化（Direct2D 透明覆盖层 + 淡出动画）
- [x] 暂停/恢复快捷键（含 UI 状态反馈与失败回滚）

### 1.4 WebView2 设置界面
- [x] 设置界面骨架（侧边栏导航 + 内容区）
- [x] 手势设置页（手势映射表 + 全局开关 + 轨迹/触发配置）
- [x] 截图录屏设置页
- [x] OCR 设置页
- [x] 通用设置页（启动/语言/日志级别）
- [x] 关于页（产品信息 + 技术栈）
- [x] 暗色/亮色主题切换
- [x] IPC 通信 Hook（useBridge + Mock 数据）

## Phase 2 — 截图 MVP

### 2.1 截图核心引擎
- [x] BitBlt GDI 全屏/区域/窗口截图
- [x] OpenCV 多格式编码（PNG/JPEG/WebP/BMP）
- [x] 剪贴板输出（CF_DIB 格式）
- [x] 文件保存（时间戳自动命名）
- [x] 多显示器支持

### 2.2 截图标注
- [x] 标注引擎（9 种工具: 矩形/箭头/椭圆/画笔/高亮/马赛克/文本/放大镜/序号）
- [x] 撤销/重做操作
- [x] 截图区域选择覆盖层（冻结屏幕 + D2D 选区 + 窗口检测自动吸附）
- [x] 标注工具栏可交互按钮（替代占位文字）
- [x] 贴图功能（PinWindow: 拖拽/缩放/透明度/多实例）

### 2.3 屏幕录制
- [x] FFmpeg 编码管道（H.264/H.265/VP9/GIF）
- [x] 帧率控制 + 暂停/恢复
- [x] 自动文件命名与保存
- [x] 录制区域选择 UI（复用 CaptureOverlay）
- [x] 录制状态指示器（RecordingIndicator: 红点闪烁/时间/帧数/暂停停止按钮）

### 2.4 长截图
- [x] OpenCV 图像拼接（cv::matchTemplate 特征匹配）
- [x] 自动滚动检测（SendInput 模拟 + 底部检测）

## Phase 3 — 高级功能

### 3.1 OCR（文字识别）
- [x] OCR 引擎实现（改用 **Windows.Media.Ocr** 离线引擎，免打包模型，详见 docs/api 决策）
- [x] 截图区域 OCR 文字提取与剪贴板复制（Ctrl+Shift+O / 覆盖层 OCR 回调）
- [x] 托盘气泡通知（后台线程推理，避免阻塞消息循环）与 IPC 联动（ocr.getStatus / ocr.recognizeImageFile）

### 3.2 录屏选区 UI 与标注工具栏交互
- [x] 标注工具栏按钮实际点击响应（工具切换高亮 + 撤销/重做/清除）
- [x] CaptureOverlay 增加 Screenshot/RecordRegion 模式区分
- [x] 通过 CaptureOverlay 选取区域后触发录屏
- [x] 文本工具真正可输入（WM_CHAR 累积 + 实时预览 + 回车提交）
- [x] 颜色色板（红/黄/绿/蓝/白），高亮工具改用当前色；修正工具栏宽度计算

### 3.3 Lua 脚本扩展
- [x] LuaEngine 初始化与 sol2 绑定（沙箱 + 线程安全 + executeFile）
- [x] C++ API 注入（完整 easy.* 命名空间: keyboard/mouse/clipboard/shell/window/screen/fs/http/ui/url/log）
- [x] GestureAction Script 类型支持（LuaScript 动作经引擎执行）
- [x] 内置命令分发（BuiltinCommandDispatcher: 窗口管理 + 应用级命令回调）

### 3.4 IPC 持久化与完善
- [x] 手势设置 IPC Handler（增删改手势映射）+ 前端 CRUD 编辑器（动作类型选择 / 快捷键捕获 / 冲突检测）
- [x] 截图/录屏设置 IPC Handler
- [x] 通用设置 IPC Handler（开机启动/语言/日志级别）
- [x] 前端 useBridge 从 Mock 切换到真实 C++ 通信

## Phase 4 — 打磨 & 发布
- [x] UI/UX、响应式布局、主题、键盘操作与错误反馈优化
- [x] 并发、生命周期、配置原子写入、插件卸载与热路径性能优化
- [x] 单元测试、前端静态检查、Release 构建与运行时冒烟测试
- [x] 便携版与 Inno Setup 安装包发布链路
