# Tools3000 v1.0.1 官方正式版发布说明

欢迎体验 **Tools3000 v1.0.1**！本次更新专注于解决日常使用中的体感痛点，全面重构了截图与录屏全链路、大幅升级标注与离线 OCR 能力，并从底层彻底根治系统启动与高并发渲染的稳定性隐患。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- 彻底解决打开软件后右键滑手势轨迹时偶现背后窗体局部露出的视觉残影问题，同时保障 1000Hz 鼠标轨迹零卡顿极致性能
- 彻底修复复制纯文本、代码片段及 Word/WPS/Excel 富文本后按贴图快捷键或手势无法生成文字贴图卡片的问题，并支持贴图焦点态直接按 Ctrl+V 连续贴图
- 修复启动时 syncApplicationShortcuts 与图标缓存刷新脚本误删桌面及开始菜单 Tools3000.lnk 快捷方式的历史隐患
- 捕获与录像设置页新增「录屏实时抓拍快捷键 (Record Snapshot)」可视化录制器
- 历史记录页新增手动刷新、文件名微晶胶囊展示、一键「定位文件」按钮及键盘回车/空格无障碍贴图
- 按键统计页补齐「今日按键」指标卡片，新增手动刷新与「重置今日统计」功能，全量启用等宽数字防抖排版
- 文件对话框增强页新增常用路径原生文件夹选择器，通用设置页新增数据目录展示与一键打开按钮，插件市场与内置扩展指南全面完善交互反馈。

### 架构演进与技术重构 (底层实现)
- 重构 GestureTrailOverlay 与 MouseFeedbackOverlayBase 的 DirectComposition 表面生命周期，预分配时全量清零为 rgba(0,0,0,0)，帧间仅按累积脏矩形局部擦除，切换至持久化 ID2D1DeviceContext 渲染管线并严格遵循先 Commit() 后 SWP_SHOWWINDOW 时序
- 将 beginTrail() 的表面重置下沉至渲染线程原子标志 m_strokeResetPending，彻底消除 WH_MOUSE_LL 钩子线程锁争用与跨线程 DComp 调用，并增加 D3D11 设备丢失自愈重建
- 下沉文本/代码/色卡贴图渲染至 PinPasteCoordinator 单一事实源，结合 EnumClipboardFormats(0) 探测富文本剪贴板格式序优先级，并在 ClipboardUtils::readImageFromClipboard 中自动将全 0 Alpha 的 32 位 CF_DIBV5 转换为不透明 BGR
- 移除构建与安装脚本中对 .LastWriteTime / .CreationTime 的冗余写入，根除 Windows Defender AMSI Timestomp 误报
- 补齐 C++ 与前端 Vitest 回归测试套件（530 项 C++ 单元测试、126 项前端测试及 6 重静态质量门禁 100% 通过）。


