# Tools3000 项目审计报告

审计日期：2026-09-05；代码基线：`961bf7032c1fd3f4bc5acd81e68e7c2902410f7e`。审计开始时 Git 工作区干净。

项目能通过现有构建与回归检查，但仍存在会造成搜索服务崩溃、资源耗尽、保存失败被误报成功和搜索状态错误的问题。本轮列出 **14 项发现：5 项 P1、8 项 P2、1 项 P3**。建议优先处理 P1，再补齐服务生命周期与索引行为测试。

P1 表示应优先修复的崩溃、数据丢失或权限边界问题；P2 表示有明确触发条件的功能、稳定性或质量门禁缺陷；P3 表示当前暴露面较低的维护问题。这里的优先级针对本项目，不等同于第三方 CVSS 等级。

## 审计范围与证据边界

审查覆盖项目结构、原生核心与插件生命周期、WebView 来源和桥接策略、搜索服务与索引/文档解析、配置与历史持久化、截图录屏保存路径、前端请求调度，以及构建、安装、发布和依赖配置。对高风险调用链进行深入阅读和隔离复现；没有声称逐行验证所有平台与硬件分支。

遵循 `.agents/rules/agent.md` 和已有审计记录：最高权限自启动任务及其 Everyone ACL、CLI 卸载默认清理个人数据、QuickLook 音视频/PDF 占位范围不重复计为缺陷。首次显式搜索后服务跨主程序退出常驻，也按既定产品契约审查。Lua 的可信本地自动化定位保持原结论。

本轮没有修改产品源码、锁文件和已有测试，没有安装、发布或修改已安装服务。新增审计报告；隔离复现代码和临时产物保留在 [审计探针目录](C:/repo/tools3000/build/audit-probes/CMakeLists.txt)，位于 Git 忽略的构建目录中。

## 验证结果

| 检查 | 本轮实际结果 |
| --- | --- |
| Windows x64 Release 原生增量构建 | 通过 |
| CTest / Tools3000Tests | 159 项测试通过，88 个测试套件 |
| 前端 lint 与其附带规范检查 | 通过 |
| 前端 Vitest | 6 个测试文件，40 项测试通过 |
| 前端 TypeScript 与 Vite 生产构建 | 通过，生产 HTML 检查通过 |
| npm audit | 非零退出；1 个受影响包，聚合等级 high，属于开发依赖 |
| 有针对性的隔离探针 | 确认监听重复启动终止进程、缓存漏结果、管道停机阻塞、处理器注销等待、配置令牌被清除、ZIP 总量缺少约束 |

原生证据：[构建日志](C:/repo/tools3000/build/audit-native-build.log)、[CTest 日志](C:/repo/tools3000/build/Testing/Temporary/LastTest.log)。依赖证据：[npm audit JSON](C:/repo/tools3000/build/audit-npm.json)。探针证据：[源码与构建配置](C:/repo/tools3000/build/audit-probes/CMakeLists.txt)、[管道与缓存结果](C:/repo/tools3000/build/audit-probes/pipe-results.txt)、[监听重启结果](C:/repo/tools3000/build/audit-probes/mft-results.txt)。

没有重新测量原生覆盖率，因此不把历史报告的 34.38% 当作本轮测量值。没有完成 ARM64 编译/目标机测试、真实 SCM 多账户测试、真实磁盘写满故障注入、录屏硬件矩阵、安装升级卸载和正式签名验证；以下相应发现明确标记为静态调用链结论。原生第三方库没有完成全量漏洞数据库比对。

## 发现

### A01 · P1 · 重建索引会重复启动监听线程，触发进程终止

位置：[MftParser.cpp:130](C:/repo/tools3000/src/service/MftParser.cpp:130)、[service/main.cpp:156](C:/repo/tools3000/src/service/main.cpp:156)。

首次索引完成后 `StartListening()` 已创建 USN 监听线程。用户执行重建时，`scheduleRebuild()` 在 `EnumerateFiles()` 后再次调用 `StartListening()`，整条路径没有先停止并 join 原监听线程。赋值给 `m_ListenerThread` 会销毁旧的、仍可 join 的 `std::thread`，从而触发 `std::terminate`，外围 `catch` 无法恢复。

**验证：**探针链接本轮构建的生产 `MftParser` 对象，用无害事件句柄代替卷句柄，连续调用两次实际 `StartListening()`。自定义终止处理器捕获到 `SECOND_START_LISTENING_CALLED_TERMINATE=1`，退出码 86。没有打开磁盘卷或重建用户索引。

**修复方向：**统一监听生命周期；重建先请求停止并 join，再重新取日志边界、枚举、启动监听。增加“已有监听时重建”回归，验证重建后仍能收到新增/删除事件。

### A02 · P1 · Office/ZIP 内容解析没有文档级解压总量上限

位置：[ZipXmlExtractor.cpp:133](C:/repo/tools3000/src/service/content/ZipXmlExtractor.cpp:133)、[ZipXmlExtractor.cpp:276](C:/repo/tools3000/src/service/content/ZipXmlExtractor.cpp:276)、[service/main.cpp:739](C:/repo/tools3000/src/service/main.cpp:739)。

30 MiB 限制只针对单个 deflate 条目；所有匹配条目会不断追加到 `outExtractedText`，没有文档级累计预算、条目数量预算或解析中的取消检查。100 MiB 压缩文件限制不能限制解压后体积。服务还会并行提取多个文件，工作线程中缺少异常兜底，内存分配失败可能终止服务。

**验证：**有界测试文件仅 164,988 字节，包含 3 个各约 12 MiB 的 XML 条目，实际解压文本总量 37,748,838 字节，三个条目全部进入搜索并产生匹配。没有制造 OOM；更大输入导致资源耗尽是依据无累计限制代码作出的推断。

**修复方向：**在所有 ZIP 解析分支共用文档级累计预算；限制条目数及服务并发内存；提取中支持取消；在线程入口捕获异常并返回失败。

### A03 · P1 · SCM 内容搜索按 SYSTEM 权限读取文件，没有客户端文件授权检查

位置：[installer.iss:138](C:/repo/tools3000/installer.iss:138)、[service/main.cpp:901](C:/repo/tools3000/src/service/main.cpp:901)、[service/main.cpp:748](C:/repo/tools3000/src/service/main.cpp:748)。

安装器创建服务时没有指定运行账户，使用默认 LocalSystem。管道 DACL 限制谁能连接，但进入 `ProcessSearchQuery()` 后没有客户端模拟或等价文件访问检查；内容提取线程直接打开文件并把片段返回客户端。因此，能合法连接端点的客户端可以请求搜索 SYSTEM 可读、但其自身令牌未必可读的文件内容。随机管道名和 SID DACL 没有解决这层授权问题。

**证据等级：静态调用链确认；未执行跨账户读取实验。**此发现针对文件内容读取权限，独立于已接受的计划任务权限取舍。微软说明服务未模拟客户端时操作使用自身安全上下文；模拟也必须检查成功，而且线程池线程需要显式传递适当令牌。[ImpersonateNamedPipeClient 官方说明](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-impersonatenamedpipeclient)。

**修复方向：**高权限服务保留必要的卷索引能力；全文读取放在用户权限进程中，或在每个实际读取线程按客户端令牌授权/模拟，并在失败时拒绝读取。回归用一份 SYSTEM 可读、客户端不可读的临时文件验证不会返回片段。

### A04 · P1 · 截图写入失败仍返回保存成功，覆盖保存还会先截断原文件

位置：[ScreenCapture.cpp:575](C:/repo/tools3000/src/capture/ScreenCapture.cpp:575)。

`saveToFile()` 只检查 `is_open()`，随后 `write()` 完直接返回路径，没有检查流状态或显式完成关闭。默认 `ofstream` 不会因写满、配额耗尽等写入错误自动抛异常；因此 `catch` 不能保证捕获这些失败。返回非空路径后上层会显示已保存。直接打开目标文件也会先截断旧文件，写失败时旧截图无法保留。

**证据等级：静态确认；未填满用户磁盘。**触发条件是文件成功打开后发生写入或关闭失败。

**修复方向：**采用唯一临时文件，检查写入、刷新、关闭结果，完整成功后再原子替换目标；失败返回结构化错误并保留原文件。用可注入失败的存储层回归中途失败和覆盖保存。

### A05 · P1 · 录屏封装/关闭失败被丢弃，损坏文件仍可提交并提示已保存

位置：[ScreenRecorder.cpp:683](C:/repo/tools3000/src/capture/ScreenRecorder.cpp:683)、[ScreenRecorder.cpp:387](C:/repo/tools3000/src/capture/ScreenRecorder.cpp:387)、[ScreenRecorder.cpp:1281](C:/repo/tools3000/src/capture/ScreenRecorder.cpp:1281)、[PluginEntry.cpp:188](C:/repo/tools3000/src/capture/PluginEntry.cpp:188)。

`finalizeEncoder()` 忽略 flush packet、`av_write_trailer()` 的失败；清理时也丢弃 `avio_closep()` 的返回值。`stopRecording()` 只依据 `frameCount > 0` 决定提交文件，不知道尾部封装是否成功。此外，原子改名失败时会返回 `.partial` 路径，上层对任意非空路径都提示“录屏已保存”。磁盘/存储错误发生在结尾时，用户会得到错误的成功反馈，且已有目标可能被损坏输出替换。

**证据等级：静态确认；未执行真实存储故障注入。**周期性剩余空间检查无法保证之后的封装与关闭一定成功。

**修复方向：**让 finalize、close、commit 的结果一路返回；区分完整保存、可恢复临时文件和失败。只有完整成功才能替换目标并提示已保存；保留失败临时文件供恢复。

### A06 · P2 · 恢复默认设置或导入其他配置会破坏常驻搜索端点身份

位置：[ConfigManager.cpp:333](C:/repo/tools3000/src/core/config/ConfigManager.cpp:333)、[SearchPlugin.cpp:115](C:/repo/tools3000/src/search/SearchPlugin.cpp:115)、[SearchPlugin.cpp:319](C:/repo/tools3000/src/search/SearchPlugin.cpp:319)。

`reset()` 清空整个 JSON，删除 `/search/pipeToken`；导入也允许覆盖该字段。当前主程序与常驻服务继续使用旧令牌，下次主程序启动却生成/读取另一个令牌。SCM 服务仍在运行时，客户端等待新端点最多两分钟，然后拒绝再启动实例；便携模式也受原服务单实例锁影响。用户恢复默认设置后重启应用即可遭遇搜索失联，直到旧服务被停止或系统重启。

**验证：**实际 `ConfigManager` 隔离探针得到 `RESET_PRESERVES_PIPE_TOKEN=0`；失联后续由服务常驻及端点初始化调用链确认，未对已安装服务进行实验。

**修复方向：**把端点身份从可导入、导出和重置的偏好配置中分离；或者提供明确的身份迁移/重连协议。补齐“首次搜索→恢复默认→退出应用→重开搜索”测试。

### A07 · P2 · 原生候选缓存不包含排除条件，放宽过滤后持续漏结果

位置：[MftParser.cpp:427](C:/repo/tools3000/src/service/MftParser.cpp:427)。

候选缓存只匹配索引代次与查询前缀，缓存的却是已经通过 `excludeHidden`、`excludeSystem` 和路径排除规则的记录。保持查询词不变、放宽过滤条件时，从旧候选集合再次过滤无法找回被排除的文件。前端缓存包含这些设置，不能修复原生缓存的问题。

**验证：**实际 `MftParser::Search()` 隔离测试：一个隐藏文件，先开启隐藏过滤，再关闭，结果为 `HIDDEN=0 RELAXED=0`；使用同样记录的新解析器则 `FRESH=1`。

**修复方向：**将所有影响候选集合的条件纳入缓存键，或者缓存未应用可变排除条件的词匹配集合；覆盖条件放宽、收紧及相同词重查。

### A08 · P2 · 关闭应用仍会同步等待在途搜索，绕过有界关闭设计

位置：[SearchPlugin.cpp:983](C:/repo/tools3000/src/search/SearchPlugin.cpp:983)、[MessageBridge.cpp:544](C:/repo/tools3000/src/core/ipc/MessageBridge.cpp:544)。

搜索插件关闭先调用 `unregisterHandlersByPrefix("search.")`，该路径使用没有截止时间的 `retireSlots()`。有界线程池关闭和处理器隔离发生在后面的 `clearHandlers()`，到不了这一步就可能卡住。实际搜索管道读等待可达 120 秒，SCM 端点等待也可达两分钟；关窗口/点退出不能取消这些工作。

**验证：**在独立进程注册受控阻塞处理器，实际注销前缀超过 2.5 秒仍不返回，释放处理器后才结束。没有把这项机制测试误写成真实应用退出耗时测试。

**修复方向：**退出先拒绝新请求并取消在途 I/O，再协调处理器与插件生命周期；无法退出的代码所在 DLL 必须保留到进程退出，不能简单超时后卸载。补齐“内容搜索中退出”和“服务端点等待中退出”。

### A09 · P2 · 空闲管道客户端可以阻塞服务停止，并耗尽服务工作线程

位置：[service/main.cpp:890](C:/repo/tools3000/src/service/main.cpp:890)、[service/main.cpp:916](C:/repo/tools3000/src/service/main.cpp:916)。

服务使用同步 `ConnectNamedPipe`/`ReadFile`/`WriteFile`，没有请求或空闲截止时间。停机只改变标志并用新连接唤醒等待连接的线程，无法唤醒已经阻塞在旧连接读请求的线程。4 个这样的连接可占满全部工作线程；末尾 `FlushFileBuffers` 也会等待客户端消费缓冲数据。[微软命名管道刷新行为说明](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)。

**验证：**直接运行生产 `PipeWorkerThread`，连接后不发送帧，调用实际 `RequestServiceShutdown()`；3.5 秒后 worker 仍未退出，关闭客户端后立即完成。探针没有初始化数据库或磁盘索引。

**修复方向：**使用可取消的重叠 I/O，维护活动连接并在停机取消；设置握手/帧/空闲预算，避免关停时无界 flush。测试空帧、半帧、只写不读和四连接占满。

### A10 · P2 · 内容查询仍被底层管道的两分钟超时截断

位置：[SearchPlugin.cpp:513](C:/repo/tools3000/src/search/SearchPlugin.cpp:513)、[MessageBridge.cpp:674](C:/repo/tools3000/src/core/ipc/MessageBridge.cpp:674)、[useBridge.ts:138](C:/repo/tools3000/ui/src/hooks/useBridge.ts:138)。

前端和异步桥接层为内容查询保留 30 分钟，服务也设计为持续扫描至完成，但客户端等待响应帧头固定为 120,000 毫秒。服务直到整次查询完成才返回帧头，因此超过两分钟的查询仍会失败；服务扫描也未因客户端放弃而在此处取消。这是实际生效的超时约束不一致，而非注释差异。

**证据等级：静态调用链确认。**未运行超过两分钟的全盘查询。

**修复方向：**贯通查询截止时间与取消协议，或通过进度帧维持有界的空闲超时；用延迟响应的假服务测试超过 120 秒的有效查询与主动取消。

### A11 · P2 · SCM 搜索历史按服务账户存储，没有按客户端 SID 隔离

位置：[SearchHistoryManager.cpp:16](C:/repo/tools3000/src/service/db/SearchHistoryManager.cpp:16)、[RunHistoryManager.cpp:16](C:/repo/tools3000/src/service/db/RunHistoryManager.cpp:16)、[service/main.cpp:325](C:/repo/tools3000/src/service/main.cpp:325)。

两类历史均使用 `SHGetFolderPathW(..., CSIDL_APPDATA, NULL, ...)` 获取当前服务账户目录；启动服务没有把客户端 SID 对应的历史目录传给管理器。在 SCM 模式中，不同用户先后启动服务仍访问同一份 SYSTEM 账户历史：A 搜索后服务停止，B 之后启动服务可通过正常历史 API 取到 A 的记录。端点按用户隔离，持久化数据却没有同样隔离。便携模式和 SCM 模式之间还会呈现不同历史。

**证据等级：静态确认，未创建第二个 Windows 账户验证。**`NULL` 令牌代表当前用户；LocalSystem 不代表当前交互登录用户。[SHGetFolderPathW](https://learn.microsoft.com/en-us/windows/win32/api/shlobj_core/nf-shlobj_core-shgetfolderpathw)、[LocalSystem 账户说明](https://learn.microsoft.com/en-us/windows/win32/services/localsystem-account)。

**修复方向：**运行历史和搜索历史按经过验证的 SID 分区，并配置对应 ACL；卷索引可共享，但用户行为记录不能共享。测试两个 SID 的先后会话和数据清理边界。

### A12 · P2 · 索引快照忽略卷身份，盘符复用后可能导入另一块卷的记录

位置：[MftParser.cpp:675](C:/repo/tools3000/src/service/MftParser.cpp:675)、[DatabaseManager.cpp:320](C:/repo/tools3000/src/service/db/DatabaseManager.cpp:320)。

快照记录了 `volumeSerial`，导入函数却显式忽略；载入器只按盘符寻找解析器。换盘、重新格式化或重新分配盘符后，旧文件记录可被注入新卷索引。后续只有 USN 数值区间校验；新卷游标恰好落在允许区间时不会保证触发全量重建，旧目录树和文件引用号可能残留。

**证据等级：静态确认；没有修改卷或盘符。**触发条件是卷身份改变且旧游标没有被后续区间检查拒绝。

**修复方向：**导入前校验卷身份与日志身份，不匹配立即丢弃该卷快照并重建。新增“同盘符不同卷序列号”和“日志重建但数值重叠”测试。

### A13 · P2 · `scripts/build.ps1 -Package` 能绕过前后端测试门禁

位置：[build.ps1:243](C:/repo/tools3000/scripts/build.ps1:243)、[build.ps1:405](C:/repo/tools3000/scripts/build.ps1:405)。

该入口前端只运行 `npm run build`，原生只编译并检查产物架构，就能进入发布 ZIP 打包；没有执行 ESLint、Vitest 或 CTest。编译测试可执行文件不等于运行测试。正常 `deploy.ps1` 路径已经运行这些检查，但另一个明确支持 `-Package` 的入口仍会产出未经门禁验证的发布包，违反仓库发布构建约定。

**证据等级：完整静态检查该脚本控制流；没有生成或发布新发行包。**

**修复方向：**让打包入口复用统一质量门禁；跨架构场景要求目标机验证记录，避免把仅交叉编译成功当作可发布状态。用故意失败的测试验证打包不能继续。

### A14 · P3 · 锁文件包含有公开漏洞公告的开发依赖 Browserslist

位置：[package-lock.json:1770](C:/repo/tools3000/ui/package-lock.json:1770)。

锁定版本为 `browserslist@4.28.2`，`npm audit` 返回一个受影响包、两条公告，聚合等级为 high，修复可用。维护者给出的受影响范围为 `<=4.28.6`，修复版本为 `4.28.7`。两条公告分别涉及变化查询缓存无界增长，以及不可信自定义统计输入导致崩溃/原型写入。[缓存公告](https://github.com/browserslist/browserslist/security/advisories/GHSA-c83g-rgw3-j3cx)、[自定义统计公告](https://github.com/browserslist/browserslist/security/advisories/GHSA-73wf-gq98-2v4g)。

**证据等级：锁文件、在线审计与上游公告交叉确认。**它在本项目是开发依赖，未发现桌面运行时接受外部 Browserslist 查询的入口，所以本项目修复优先级列为 P3，不宣称桌面应用存在远程高危漏洞。上游缓存公告为 Moderate，和 npm 聚合 High 的分级也应区分。

**修复方向：**更新兼容补丁并提交锁文件，复跑审计和现有前端检查。此次没有执行自动修复或升级依赖。

## 文档与实现一致性

这些问题另列，不计入上面的 14 项运行与工程发现：

- [README.md:48](C:/repo/tools3000/README.md:48) 宣称支持 `size:>100mb`，但当前 `SearchFilterType` 没有大小过滤类型，解析器按普通文本处理。实际探针对 200 MiB 文件执行该表达式也不匹配。应补实现或修正文档与展示语法。
- [README.md:160](C:/repo/tools3000/README.md:160) 的“严格权限受限沙箱”描述与 [Lua 安全模型](C:/repo/tools3000/docs/lua-security-model.md) 不一致。应同步可信自动化定位；这里不要求改变已接受的 Lua 运行时能力。
- 交接文档中的 SQLite FTS5、单 HTML 打包等描述与当前 CSV/二进制快照和 Vite 分块输出不一致，会误导维护者，应按当前架构更新。

## 建议修复顺序与回归重点

先修 A01–A05：确保重建不会杀死服务、文档提取资源有界、文件读取尊重客户端权限、保存成功有可靠依据。

随后成组处理 A06–A12：将端点身份、取消、管道超时、服务关闭和索引缓存设计成可共同验证的生命周期；补充现有测试没有覆盖的重置后重连、搜索中退出、半帧连接、过滤放宽、跨用户历史和换卷快照场景。

最后统一发布门禁并更新开发依赖。新增回归应针对上述实际行为，不以提高测试数量或无条件追求全库覆盖率代替故障验证。
