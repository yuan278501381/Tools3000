# Tools3000 实景文字识别（OCR）回归测试样本集

本目录存放用于端到端自动化回归测试的真实场景截图与实拍图（均经项目负责人确认无泄密风险，直接由 Git 版本管理）。

`tests/unit/test_live_ocr.inc` 中的实景回归测试用例直接加载本目录样本，用于在本地及已安装 PP-OCRv4 离线模型包的测试环境中持续守护 OCR 识别准确率、版面分栏、UI 下拉箭头剥离、结构化 JSON 标点规整、二维码解码与原位改字边界保护不发生回退。

## 样本清单

| 文件名 | 场景类别 | 核心回归断言覆盖点 |
| --- | --- | --- |
| `sap_form_table.jpg` | SAP B1 多栏表单 + 密集明细表 + 右侧属性边栏 | 多栏隔离不串行、表头与编码/金额识别（`G16611`、`JX0200-00680`、`62,376.00 RMB`） |
| `release_notes_dense_doc.png` | 高密度中英混排长文档 + 行内等宽代码 + 表格 | 密集文档高吞吐与中英混排准确性（`本次版本核心亮点与体验升级`、`Direct2D`、`Tools3000-Setup.exe`） |
| `pda_screen_photo.jpg` | 手持 PDA 工业终端拍屏（强摩尔纹与透视倾斜） | 低对比度倾斜屏摄精准识别（`生产箱单-PL202610150024`、`周转箱编码`、`ZZ30003665已被禁`，严防误识为 `乙乙`） |
| `meeting_tv_photo.jpg` | 会议室大屏 PPT 实拍 | 室内透视屏摄 OCR 识别（`SAP待解决问题`、`剩余5%待到期时开票`）及原位改字 `5% -> 10%` 紧邻字 `余` 零抹除保护 |
| `erp_table_screen_photo.jpg` | ERP 屏幕实拍摩尔纹 + 深灰表头密集表格 | 低反差深灰表头增强（`物料名称`/`仓库名称`/`数量`）、100+ 单元格高召回与编码数字（`ZZ00443481`、`自制落料片仓库`、`77`、`400`） |
| `packing_card_photo.jpg` | 工业产品装箱卡（白底贴纸 + QR 码）叠放于复杂背景物料表 | 复杂前景/背景双层表格召回（`瑞鹄浩博产品装箱卡`、`501010567AA`、`右前纵梁后部加强板`、`ZZ30001332`）及二维码 `ZZ00444040` 解码 |
| `apifox_json_pretty.png` | Apifox 格式化 JSON 响应与参数栏 | 单行分桶防串扰（`"DocTotalFC" : 0.0,`、`"Comments" : "",`、`"U_BillType" : "Y",`、`"WddStatus" : "-",`、`$filter`、`$orderby`）及 UI 下拉箭头剥离（`GET` 不含 `v`、`Body` 不含 `~`） |
| `apifox_json_compact.jpg` | Apifox 超宽紧凑单行 JSON + 底部错误响应 | 超宽行重叠滑窗拼接、结构感知 JSON 标点规整（`{"U_PLM_Categ2":"BCP0300"`、`"ItemPrices":[{"Currency":"RMB","PriceList":1}]}`）、字符串值内中文标点保全及底部 `Internal error (-10) occurred` 识别 |
