# Tools3000 实景文字识别（OCR）回归测试样本集

本目录存放用于端到端自动化回归测试的真实场景截图与实拍图（均经项目负责人确认无泄密风险，直接由 Git 版本管理）。

`tests/unit/test_live_ocr.inc` 中的实景回归测试用例直接加载本目录样本，用于在本地及已安装 PP-OCRv4 离线模型包的测试环境中持续守护 OCR 识别准确率、版面分栏、UI 下拉箭头剥离、结构化 JSON 标点规整、二维码解码与原位改字边界保护不发生回退。同时，设置环境变量 `TOOLS3000_RUN_OCR_18_BENCHMARK=1` 运行 `LiveTextOcrTest.Full18FixturesOcrAccuracyLatencyMemoryBenchmark`，可一次性对 `ocr/`（8 张）与 `translate/`（10 张）全部 18 张样本执行端到端离线 OCR 精度、墙钟/CPU 耗时、内存峰值与卸载回收率全维基准评测。

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

## 全 8 语种跨语种离线识别与按需模型包矩阵

Tools3000 将「软件界面语言」与「屏幕源图语种」彻底解耦，确保任意语种用户（例如简体中文用户截韩文/日文/俄文/西葡文图）在默认的 `scriptMode = "auto"` 下零手动切换即可自动嗅探识别并原位翻译。

- **基础离线包（16 MB，`mobile` 默认）**：`det + cls + ch_PP-OCRv4_rec`，原生高精覆盖简体中文、繁体中文、英文与数字代码，低配机零额外负担；
- **可选精度增强包（+90 MB，`server`）**：`ch_PP-OCRv4_rec_server_infer`，仅在手写/拍屏/模糊场景按需叠加，未安装自动无缝回退 `mobile`；
- **可选 AI 原位修复包（+26.8 MB）**：`migan_pipeline_v2.onnx`，用于复杂底纹/照片背景的原位改字与翻译补全；
- **按需轻量多语种扩展包（复用同一套 `det + cls`，单槽互斥低内存按需加载）**：
  - `japan` (`japan_PP-OCRv4_rec_mobile.onnx`, ~9.3 MB)：平假名 / 片假名 / 日语汉字；
  - `korean` (`korean_PP-OCRv5_rec_mobile.onnx`, ~12.9 MB)：韩文谚文与韩英混排；
  - `cyrillic` (`cyrillic_PP-OCRv5_rec_mobile.onnx`, ~7.7 MB)：俄语及东欧西里尔字母；
  - `latin` (`latin_PP-OCRv5_rec_mobile.onnx`, ~7.5 MB)：西班牙语、葡萄牙语及欧洲拉丁变音符（`ñ/¿/ç/ã` 等）。

### 多语种样本清单（`multilang/`）

`LiveTextOcrTest.MultilingualAutoScriptProbeAndInPlaceTranslateBenchmark` 直接读取以下样本验证全 8 语种零切换自动嗅探准确率与跨语种原位翻译闭环：

| 文件名 | 目标语种体系 | 核心回归断言覆盖点 |
| --- | --- | --- |
| `multilang/zh_tw_settings_card.png` | 繁体中文 + 英文数字 | 命中基础 `cjk` 零额外探针开销，精准识别繁体字形（`繁體中文`、`監控`、`記憶體回收率 95.2%`） |
| `multilang/ja_settings_card.png` | 日语（平/片假名 + 汉字 + 英文） | `auto` 自动检测字符密度腰斩并切至 `japan` 扩展包，精准识别假名与混排（`オフライン文字認識`、`スクリーンショット`、`DirectML GPU`） |
| `multilang/ko_dark_settings_card.png` | 暗色韩语 UI + 版本号 | `auto` 自动检测主包坍塌并切至 `korean` 扩展包，全量召回韩文谚文与 `(v1.4.0)`，支持中文用户直译简体中文 |
| `multilang/ru_settings_card.png` | 俄语（西里尔字母）+ 英文数字 | `auto` 自动检测伪拉丁混排畸变并切至 `cyrillic` 扩展包，一字不差识别西里尔正文与 `DirectML GPU`、`26%` |
| `multilang/es_pt_settings_card.png` | 西班牙语 + 葡萄牙语混排 | `auto` 自动检测变音符缺失并切至 `latin` 扩展包，精准保留 `ñ`、`¿`、`ç`、`ã`、`ê` 等拉丁变音符号 |
