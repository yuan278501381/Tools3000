# Tools3000 v1.4.14 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点更新与体验改进)** · **[Upgrade / 升级方式](#升级方式)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.14** (compared against **v1.4.13**). This maintenance release hardens in-place AI screenshot translation across technical documents and large pages, fixes the Bilibili Index-Translate rejection on SQL-heavy pages, and aligns the streamlined Memory Policy UI and documentation across all 8 languages:

1. **Fixed Bilibili Index-Translate Rejection & End-to-End AI Translation Hardening**
   - Resolved the issue where translating long or technical screenshots with the built-in keyless **Bilibili Index-Translate** preset could fail with an upstream request rejection.
   - Added model-aware batch sizing, smarter timeout floors, and safe concurrency across `Index-Translate-2B / 9B / 35B`, local Ollama/vLLM servers (single-stream protection against GPU queue timeouts), and cloud OpenAI-compatible APIs.
   - Added automatic one-time retry on transient network/gateway errors and automatic split-batch recovery: if a large page hits a timeout, content filter, or truncated reply, Tools3000 automatically splits and retries smaller chunks — isolating any single rejected line verbatim instead of failing the entire page.
   - Prevented partial or malformed AI responses from silently leaving blank translated regions, and ensured **Test Connection** in Settings always performs a live probe rather than reusing cached results.
2. **Preserve Raw SQL Queries in Technical Document Screenshots**
   - Pure SQL queries (`SELECT / UPDATE / DELETE / INSERT` and wrapped multi-line `WHERE / AND` clauses) inside technical notes or database screenshots are now kept verbatim during in-place translation so runnable statements stay intact, while normal English sentences (such as `"Select an item from the list."`) continue translating naturally.
3. **Clearer Translation Error Messages (8 Languages)**
   - Keyless engines (Bilibili Index-Translate, Edge, Google) and rate-limited requests now show accurate retry/switch-engine guidance instead of misleadingly asking you to check API key quota.
4. **Aligned "Memory Policy" Naming, Iconography & 8-Language Polish**
   - Renamed the streamlined read-only schedule page from **Performance & Memory** to **Memory Policy** with a dedicated memory icon across the sidebar, page header, **About** / **General** shortcuts, settings search, and all 8 language packs and README guides.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (supports both installer and portable builds), overwrite with `Tools3000-v1.4.14-Setup.exe` / `Tools3000-v1.4.14-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点更新与体验改进

欢迎升级至 **Tools3000 v1.4.14**。本版本以已正式发布的 **v1.4.13** 为对比基线，重点解决 Bilibili Index-Translate 翻译技术文档长图报错问题、全面增强 AI 原位翻译容错能力，并完成「内存策略」页面的全库命名与多语言一致性打磨：

### 1. 修复 Bilibili Index-Translate 翻译报错并全面加固 AI 翻译链路
- **彻底修复技术文档/长图触发上游拦截报错**：修复使用内置免密钥 **Bilibili Index-Translate** 翻译整页英文技术文档或含查询代码截图时偶发响应异常的问题。
- **按模型档位自适应分批与超时保护**：针对 `Index-Translate-2B / 9B / 35B`、本机 Ollama/vLLM（自动单路防显存排队超时）及通用云端 OpenAI 兼容接口自动匹配最优批次大小与超时下限，整页多段翻译更稳更快。
- **瞬态自动重试 + 异常二分自愈**：遇到临时超时、限流、网关波动或模型回复截断时，自动执行可瞬时取消的重试与自动二分拆批补译；即使单条离群内容仍被上游拒绝，也仅原样保留该条而不影响整页其他段落的翻译结果，彻底杜绝半页留白。
- **连通性测试真实穿透**：在设置页点击「测试连通性」时始终发起真实网络探测，不再因命中本地会话缓存而出现换密钥或断网后的假通过。

### 2. 技术文档截图原位翻译自动保护纯 SQL 语句
- 在翻译技术文档、运维手册或数据库故障排查截图时，自动识别并原样保留可执行 SQL 查询语句及跨行条件续行，既避免代码被翻坏，又减少无效请求；同时确保普通英文句子正常翻译不被误拦。

### 3. 翻译失败提示更准确（8 国语言全覆盖）
- 细化免密钥通道临时异常、频率限制与 API 密钥鉴权失败三类提示文案；使用免密钥引擎遇到临时波动时不再误提示「检查密钥额度」，而是清晰引导稍后重试或切换其他翻译引擎。

### 4. 「内存策略」全库命名、图标与 8 语种一致性收口
- 配合上个版本已精简为只读回收时间表的页面定位，将侧边栏菜单、页面主副标题、「关于 / 通用设置」快捷入口、设置全文搜索及 8 套语言包和 README 文档统一对齐为 **「内存策略」** 与内存条图标，同步清理冗余旧文案并补齐多语种入口翻译。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.14-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.14-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
