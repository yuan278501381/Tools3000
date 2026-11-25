# Tools3000 统一文字识别（OCR）与原位翻译回归测试样本库

本目录为 Tools3000 **离线 OCR 识别**与**原位翻译排版**共享的单一事实源测试样本库（原 `tests/fixtures/translate/` 已全面合并至此，均已脱敏并确认零隐私/零版权隐患）。

`tests/unit/test_live_ocr.inc` 与 `tests/unit/test_capture.inc` 直接加载本目录样本，用于在本地及已安装 PP-OCRv6 / PP-OCRv4 离线模型包的测试环境中持续守护：
- 中/英/日/韩/俄/法/德/西/葡/阿/泰多语种识别准确率、自动语种路由与阿拉伯语 RTL 逻辑序还原；
- 表格与 ERP 多栏隔离不串行、Excel 网格线与负数/千分位金额识别、代码下划线/JSON 标点规整；
- 二维码/条形码解码、红章穿透、拍屏摩尔纹增强、原位改字边界保护与原位翻译排版防回退。

---

## 目录分层（MECE）

```text
tests/fixtures/ocr/
├── *.png / *.jpg         # 18 张公开脱敏真实业务截图与实拍样本（原 OCR 8 张 + 原位翻译 10 张合并）
├── desktop_office/       # 10 张高保真桌面办公软件 / ERP 表单 / 深色 IDE / 多语种网页真实渲染截屏
├── multilang/            # 5 张轻量多语种设置卡片样本
├── local_private/        # (.gitignore 忽略，不入 Git) 本地私有企业真实 ERP/SAP/PDA/工控实测截图（含敏感业务信息，严禁提交）
└── web_benchmark/        # (.gitignore 忽略，不入 Git) 本地缓存的 80 张 Wikimedia 实景拍照 + 5 张尺寸压测图
```

> **本地私有语料与外部实景照片大考说明（Git 仅保留路径与 URL 清单，不入库图片二进制）**：
> - `local_private/` 已被 `.gitignore` 严格排除，用于在本地离线验证真实企业 ERP/SAP/Excel/PDA 生产截图的 OCR 与原位翻译端到端质量，绝不进入公开 Git 仓库；
> - `web_benchmark/` 已被 `.gitignore` 忽略（不入库 40+ MB 图片二进制），仓库内仅保留精简纯文本 URL 清单 `tests/benchmark/web_corpus_urls.json` 与拉取脚本 `tests/benchmark/fetch_ocr_web_corpus.py`，任意机器可一键确定性还原全部 80 张 Wikimedia 实景照片 + 8 张多语种真值卡 + 5 张分辨率压测图，并同时用于 OCR (`Tools3000OcrBench.exe`) 与原位翻译全目录回归；
> - 全目录（根目录 + 所有子目录）端到端 OCR + 原位翻译回归可通过 `TOOLS3000_RUN_ALL_TRANSLATE_AUDIT=1` 运行 `InPlaceTranslateEngineTest.AllOcrFixturesEndToEndTranslateAudit` 一键验证。

---

## 一、核心真实截图与实拍回归样本（18 张，位于根目录）

| 文件名 | 场景类别 | 核心回归断言覆盖点 |
| --- | --- | --- |
| `sap_form_table.jpg` | SAP B1 多栏表单 + 密集明细表 + 右侧属性边栏 | 多栏隔离不串行、表头与编码/金额识别（`G16611`、`JX0200-00680`、`62,376.00 RMB`） |
| `sap_b1_form_designer.png` | SAP B1 1080p 经典桌面 ERP 窗体与黄色按钮 | 左侧树形菜单字号齐整、1px 按钮边框刹车零穿框、小字 ERP 识别 |
| `erp_table_screen_photo.jpg` | ERP 屏幕实拍摩尔纹 + 深灰表头密集表格 | 低反差深灰表头增强、100+ 单元格高召回与编码数字（`ZZ00443481`、`自制落料片仓库`） |
| `gnway_admin_dialog.jpg` | Win32 管理端树菜单 + 表格 + 模态错误弹窗 | `AS0002` / `V8.6.0.724` 编码保留、表格短单元格独立分离、居中弹窗按钮 |
| `release_notes_dense_doc.png` | 高密度中英混排长文档 + 行内等宽代码 + 表格 | 密集文档高吞吐与中英混排准确性（`Direct2D`、`Tools3000-Setup.exe`） |
| `dark_markdown_codebadges.png` | 暗色中文技术文档 + 悬挂缩进 + 行内代码胶囊 | 悬挂缩进段落合并、代码徽章擦净与字号平滑归一 |
| `apifox_json_pretty.png` | Apifox 格式化 JSON 响应与参数栏 | 单行分桶防串扰（`"DocTotalFC"`、`$filter`）及 UI 下拉箭头剥离 |
| `apifox_json_compact.jpg` | Apifox 超宽紧凑单行 JSON + 错误响应 | 超宽行重叠滑窗拼接、JSON 标点规整、未闭合长字符串无缝续接 |
| `postman_api_dark_sanitized.png` | 暗色 Postman API 调试界面（全面 RFC 5737 脱敏） | 代码语义防火墙：保护 `POST`/`GET`/`BaseUrl`/`{{BaseUrl}}/Login` 与 JSON 体 |
| `dark_sidebar_list.png` | 暗色双语侧边栏列表 + 右对齐时间戳列 | 满宽行左对齐守恒、尾部时间戳剥离、次级灰字保色 |
| `nytimes_homepage.png` | 现代多栏英文新闻门户 + 顶栏导航 + 柱状图 | 多栏标题层级、眉标防火墙、数值坐标保留 |
| `yahoo_portal_cards.png` | 现代卡片流门户 + 大图白字 + 低对比度灰框 | 并排卡片回溯合并、大图多行白字、紧凑行距不截断 |
| `hackernews_nested_comments.png` | 多级嵌套缩进英文技术评论流 | 嵌套缩进严守原左边缘、meta 行与正文隔离 |
| `alternativeto_web_grid.png` | 现代网页网格卡片 + 居中横幅 | 居中标语/按钮定位、品牌名（`NGINX`/`PostgreSQL`）保护 |
| `pda_screen_photo.jpg` | 手持 PDA 工业终端拍屏（强摩尔纹 + 倾斜） | 低对比度倾斜屏摄精准识别（`PL202610150024`、`ZZ30003665`） |
| `meeting_tv_photo.jpg` | 会议室大屏 PPT 实拍 | 室内透视屏摄 OCR 与原位改字 `5% -> 10%` 紧邻字保护 |
| `packing_card_photo.jpg` | 工业装箱卡白底贴纸 + QR 码叠放复杂背景 | 双层表格召回与小型倾斜二维码 `ZZ00444040` 解码 |
| `packing_card_handheld.jpg` | 手持工业装箱卡强反光实拍 | 料号（`ZZ00445707`/`501023856AA`）保护、二维码区零侵入 |

---

## 二、桌面办公软件 / ERP / 深色 IDE / 多语种网页截屏（10 张，位于 `desktop_office/`）

专门弥补自然街景拍照对桌面高频场景的分布偏差，全部通过 Windows 原生 Chromium/DirectWrite 渲染截取（100% 原创、精确 Ground Truth）：

| 文件名 | 场景类别 | 语种 | 核心校验维度 |
| --- | --- | --- | --- |
| `excel_finance_sheet_zh.png` | Excel 财务采购对账表 | 简中 + 数值 | 细灰网格线、`=SUM(G2:G9)` 公式、千分位 `12,480.50`、红字负数 `(1,250.00)`、料号与税率 |
| `erp_purchase_order_dense.png` | ERP 采购入库单多栏表单 + 明细子表 | 简中 + 编码 | 深色侧栏 + 多列输入框 + 深灰表头明细行 + 批次号 `LOT-2611A09` / 仓库 `WH-SZ-02` |
| `office_doc_bilingual_review.png` | Word / 协作文档双栏评审 + 批注 | 中英双语 | 双栏分栏隔离、行内等宽代码 `DeepOcrEngine::extractText`、对比表格、小字脚注与侧边批注 |
| `ide_terminal_logs_dark.png` | 深色 VS Code 编辑器 + PowerShell 终端 | 英文 + 代码 | 等宽代码、`snake_case` 下划线、Windows 反斜杠路径、`https://` URL 与日志 TraceID |
| `web_docs_fr_de_es.png` | 欧陆 SaaS 计费与系统设置网页 | 法/德/西 | `PP-OCRv6` 50 语种原生重音变音符（`É`/`é`/`ü`/`ö`/`ß`/`ñ`/`¿`）零切包直出 |
| `web_erp_ja_dashboard.png` | 日语基幹業務 ERP 在库看板网页 | 日语 | `PP-OCRv6` 原生平假名/片假名/汉字 + KPI 卡片与日元金额 |
| `web_erp_ko_dashboard.png` | 韩语电商物流后台管理网页 | 韩语 | 自动语种路由命中 `korean` 扩展包、谚文 + 韩元金额表 |
| `web_portal_ru_cyrillic.png` | 俄语企业工单与财务对账网页 | 俄语 + 拉丁 | 自动语种路由命中 `cyrillic` 扩展包 + 西里尔/拉丁同形字防污染（`PostgreSQL 16` / `DirectML GPU`） |
| `web_portal_ar_rtl.png` | 阿拉伯语 RTL 企业订单看板网页 | 阿拉伯语 | 自动语种路由命中 `arabic` 扩展包 + RTL 逻辑阅读序还原与内嵌编号保序 |
| `web_portal_th_erp.png` | 泰语企业订单与仓储后台网页 | 泰语 | 自动语种路由命中 `thai` 扩展包（防预算饿死）+ 上下声调完整识别 |

---

## 三、PP-OCRv6 离线多语种引擎与按需扩展包矩阵

- **默认核心包（`PP-OCRv6 Small`，~30.3 MB）**：`PP-OCRv6_det_small.onnx` + `PP-OCRv6_rec_small.onnx` + `ch_ppocr_mobile_v2.0_cls.onnx`，内置 18,708 词表原生覆盖简/繁/英/日/法/德/西/葡等 **50 语种**；
- **可选高精度精修包（`PP-OCRv6 Rec Medium`，+73.1 MB）**：仅对低置信度疑难行按需二阶复核，常规高置信度截图零额外开销；
- **可选按需语种扩展包（纯 Apache-2.0，单槽低内存互斥加载）**：`korean` (~12.9 MB)、`cyrillic` (~7.7 MB)、`arabic` (~7.7 MB)、`thai` (~7.6 MB)；越南语等其余语种可通过 Windows OCR 语言包通道支持。
