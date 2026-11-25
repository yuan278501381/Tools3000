# Tools3000 v1.4.9 Release Notes | 官方正式版发布说明

**[English](#english-release-highlights)** · **[简体中文说明](#本次重点修复与安全加固)** · **[Upgrade / 升级方式](#升级方式)** · **[SHA-256 & VirusTotal](#文件完整性校验与安全查验-sha-256--virustotal)**

---

## English Release Highlights

Welcome to **Tools3000 v1.4.9** (compared against **v1.4.8**). This rapid maintenance release fixes PP-OCRv6 offline model download verification, hardens integrity checks across all four downloadable AI model packs, and prevents security software false positives on browser profile folders:

1. **PP-OCRv6 & AI Model Pack Download Verification Fix**
   - Fixed a verification bug in `v1.4.8` where `PP-OCRv6_rec_small.onnx` and `PP-OCRv6_rec_medium.onnx` failed integrity checks right after downloading (`Failed to download or verify PP-OCRv6_rec_small.onnx`).
   - Replaced the legacy OpenCV DNN probe with full ONNX `ModelProto` structural validation, embedded dictionary verification, and exact HTTP `Content-Length` truncation checks across all four downloadable packs (**PP-OCRv6 Small**, **PP-OCRv6 Rec Medium**, **4 Multilingual Extension Packs**, and **MI-GAN 512 AI Inpaint**).
   - Fixed a UI feedback gap where a failed upgrade from legacy `PP-OCRv4` to `PP-OCRv6` silently hid the error toast.
   - Improved red-stamp inverse alpha unblending (preventing black text under translucent stamps from splitting) and isolated standalone code-editor line-number gutters so copying wrapped JSON/code never pulls leading line numbers into the selection.
2. **Zero Handle Touch on Browser Credential Files (`#28`, `@aungkokomm`)**
   - Hardened `Tools3000_Service.exe` (local NTFS filename indexing service) to explicitly skip metadata handles (`OpenFileById` / `GetFileAttributesExW`) and full-text content scanning for browser credential/session files (`Login Data`, `Web Data`, `Cookies`, `Local State`, `logins.json`, `key4.db`, `cookies.sqlite`, etc.) and browser profile / Windows credential directories across Chrome, Edge, Brave, Opera, Vivaldi, Firefox, and derivatives.
   - Completely eliminates antivirus "browser password shield" false-positive alerts while keeping normal file search unaffected.

**Upgrade**: In-app via `Settings -> Insights & About -> About -> Check for Updates` (works for both standard installer and portable builds), overwrite with `Tools3000-v1.4.9-Setup.exe` / `Tools3000-v1.4.9-win-x64-portable.zip`, or run `scoop update tools3000`.

---

## 本次重点修复与安全加固

欢迎升级至 **Tools3000 v1.4.9**。本版本以已正式发布的 **v1.4.8** 为对比基线，紧急修复 PP-OCRv6 离线模型包下载校验失败问题，加固全部 4 类离线 AI 模型下载完整性，并从内核 I/O 层消除搜索服务触发安全软件「浏览器密码保护盾」误报：

### 1. 彻底修复 `PP-OCRv6` 离线模型下载校验失败并加固全部 4 类模型包下载
- **修复 `PP-OCRv6_rec_small.onnx` / `PP-OCRv6_rec_medium.onnx` 下载后误判损坏**：根治 `v1.4.8` 下载完成识别模型后误调用旧版 OpenCV DNN 试解析导致报错 `Failed to download or verify PP-OCRv6_rec_small.onnx` 的问题，全新安装、从旧版 `v4` 一键升级至 `PP-OCRv6` 以及下载高精度增强包均可在国内直连秒速稳定完成；
- **全 4 类模型包完整防截断校验**：为 **PP-OCRv6 核心包**、**+73 MB 高精度增强包**、**韩/俄/阿/泰 4 国扩展包** 与 **MI-GAN 512 原位改字修复模型** 全面补齐 HTTP `Content-Length` 精确字节防截断断言与完整 ONNX `ModelProto` 结构校验（同时消除校验瞬间约 200 MB 内存峰值）；
- **修复升级失败静默吞错提示**：修复老版 `v4` 用户点击「升级至 PP-OCRv6」遇到异常时错误 Toast 未弹出的问题；
- **红章压字与代码行号槽识别优化**：升级红章穿透为光学反混合还原（避免半透明红章覆盖的黑字被漂淡断行），并新增代码编辑器左侧孤立行号槽隔离，拖选折行 JSON/代码复制不再夹带行号。

### 2. 搜索服务对浏览器敏感凭据文件零句柄触碰（`#28`, `@aungkokomm`）
- 在本地 NTFS 索引服务 `Tools3000_Service.exe` 的 USN 实时属性刷新、后台目录水合、搜索结果属性查询与全文内容搜索 4 条全链路中，显式跳过主流浏览器（Chrome / Edge / Brave / Opera / Vivaldi / Firefox 等）的密码、Cookie、密钥库文件（`Login Data`、`Web Data`、`Cookies`、`Local State`、`logins.json`、`key4.db`、`cookies.sqlite` 等）及 Windows 凭据目录；
- 从系统文件句柄层面彻底做到“连属性都不触碰”，消除部分安全软件「浏览器密码保护盾」误报弹窗，同时不影响正常工作目录与文档搜索。

---

## 升级方式

- **应用内一键升级（安装版与绿色便携版均支持）**：打开 `设置 -> 洞察与关于 -> 关于 -> 检查更新`（或点击系统托盘更新提示条）一键下载并自动完成原地升级，完整保留全部配置、剪贴板历史、固定选区与离线模型；
- **标准安装版手动覆盖**：下载下方 `Tools3000-v1.4.9-Setup.exe` 直接运行覆盖安装；
- **绿色便携版手动覆盖**：下载 `Tools3000-v1.4.9-win-x64-portable.zip` 解压覆盖程序目录（保留 `data/` 目录配置与数据）；
- **Scoop 用户**：在终端执行 `scoop update tools3000`。
