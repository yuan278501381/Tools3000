# Tools3000 v1.0.11 官方正式版发布说明

欢迎使用 **Tools3000 v1.0.11** 官方正式版。本版本面向全球用户全面升级了 **8 种界面语言全栈支持**（默认自动跟随系统语言并支持随时手动切换）、**8 语种 Windows 安装向导**，以及 **8 套多语种 README 与 72 份本地化导演级交互演示资产**。

---

## 本次重点更新与体验改进

### 业务与功能体验进化 (用户视角)
- **软件全界面原生支持 8 种语言**：全面覆盖 **简体中文 (`zh-CN`)、繁體中文 (`zh-TW`)、English (`en-US`)、日本語 (`ja-JP`)、한국어 (`ko-KR`)、Русский (`ru-RU`)、Português (Brasil) (`pt-BR`)、Español (`es-ES`)**，贯通设置中心、极速搜索、托盘菜单、QuickLook 预览、截图标注工具栏、贴图与贴图组、滚动长截图、高清录屏与自动运镜演播室、全局鼠标手势、键鼠回显、OCR 对照窗口与系统右键菜单。
- **默认自动跟随系统语言 + 一键防误切手动切换**：首次启动默认自动匹配当前 Windows 显示语言（未知语种自动回退 English）；随时可通过窗口标题栏快捷语言胶囊或「设置 -> 通用 -> 界面语言」即时切换，下拉选项展示各语种原生母语名称，杜绝误切外语后找不到切回入口。
- **8 语种安装向导**：Windows 标准安装程序 (`Tools3000-Setup.exe`) 完整支持全部 8 种安装语言界面与自定义向导提示。
- **8 语种 README 与 72 份本地化交互演示资产**：为全部 8 种语言配备对应母语的八大能力 Bento 总览海报与 8 组「导演级四拍因果交互节拍」高清动态 WebP（清晰呈现光标移动、悬停亮起、物理按压脉冲波纹与结果展开），打开任意语言主页即可一眼看懂每一步鼠标操作与核心功能。

### 架构演进与技术重构 (底层实现)
- 构建 C++ 微内核 8 语种运行时 `UiI18n`（`TOOLS3000CORE_API` 跨 DLL 导出），通过 `GetUserDefaultUILanguage` + `LCIDToLocaleName` 精确区分简繁与海外系统语言；基于 O(1) 线程安全不可变词典快照加载 `resources/locales/native/<tag>.json`，提供 `noexcept` 常驻指针与异常安全 `{0}/{1}` 占位符格式化 API。
- 完成全库 55 个 C++ 原生 UI 源文件共 855 个原生词条迁移与 6 套海外/繁体原生词典精译；同步升级 `MessageBridge` 透传 `systemLanguage` / `resolvedLanguage` 并修正 `Logger` 海外语种日志路由至英文模板。
- 建立 React 前端 8 语种单一事实源 `ui/src/i18n/languages.ts` 与 8 套 2,311 键全量同构语言包；优先采信 C++ 探测的真实 Windows 显示语言并同步 `document.documentElement.lang`。
- 采用零安装包体积膨胀的多语种字体架构：继续仅内嵌单字重 1.10 MB `Noto Sans SC`，非简中语种在 CSS（`:root:lang(...)`）与 DirectWrite（`DWriteFontUtils.h`）按语种动态优选 Windows 原生字体（`Microsoft JhengHei UI`、`Yu Gothic UI`、`Malgun Gothic`、`Segoe UI Variable Text`），新增 6 种语言对安装包字体体积增加 0 KB。
- 升级 Inno Setup 安装脚本（`installer.iss` + `resources/installer/ChineseTraditional.isl`）注册 8 语种界面、28×8 条 `[CustomMessages]` 及卸载目录清理；在 CI 质量门禁扩展 8 语言同构校验并新增 `ui/scripts/check-native-i18n.js` 校验全量 855 个 C++ 原生词条。

### 质量门禁与稳定性保障
- 新增 C++ 单元测试 `UiI18nTest.EightLanguageResolutionCatalogLoadingAndInstallerContract` 与前端单测 `ui/src/i18n/languages.test.ts`，验证全栈 8 语种解析、词典完整加载与安装器契约。
- 全量通过前端 7 重静态门禁、Vitest 196/196 单测、C++ 全量 781/781 单测、三合一单次切屏鼠标手势 E2E、截图录屏演播室 E2E、文件与内容搜索跨进程 E2E、按键回显硬断言 E2E 以及 Authenticode 代码签名强校验。
