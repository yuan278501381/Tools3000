# Tools3000 截图原位翻译回归测试样本集

本目录存放用于端到端自动化回归测试与视觉审片验收的真实场景原图（已严格剔除含账号/密码/服务器凭据的 Postman 等敏感截图，仅保留经项目负责人确认合规的测试原图，由 Git 版本管理）。

配合 `tests/unit/test_capture.inc` 中的 `InPlaceTranslateEngineTest.LiveImageVisualQaWhenEnvSet`（设置 `TOOLS3000_LIVE_TRANSLATE_QA_IMG` 与 `TOOLS3000_LIVE_TRANSLATE_QA_OUT` 环境变量运行，可选 `TOOLS3000_LIVE_TRANSLATE_QA_VERBOSE=1` 输出逐行几何与样式诊断），可持续守护以下核心排版质量红线永不回退：

- 4 向像素级墨水连通封顶擦除（消除 OCR 紧包框遗漏首尾字母导致的“毛边/残渣”，同时保护下拉箭头、图标与表格线）
- 格式塔左对齐列守恒 + 满宽行禁居中 + 独立居中按钮/大灰框广告/双行居中标题精准居中
- 多栏/并排卡片/图标插队回溯段落聚类与全大写短眉标防火墙
- 多行段落逐行视觉 DNA 采样与物理字号防爆护栏 + DirectWrite `UNIFORM` 紧凑行高 + 单行真实墨水垂直光学居中
- 深/浅色模式次级柔和灰字无损保色与尾部独立紧凑时间戳列剥离

## 样本清单

| 文件名 | 翻译方向 | 场景类别 | 核心回归验证点 |
| --- | --- | --- | --- |
| `dark_sidebar_list.png` | 英 -> 中 | 暗色 IDE/对话侧边栏双语列表 + 右对齐时间戳列 | 满宽长行严守左对齐不居中、`8d` 尾部时间戳剥离防粘连、译文字号与同屏原生中文等大齐平、深色柔和次级灰字不跳刺眼纯白、`Tools3000` 产品名原样保留 |
| `nytimes_homepage.jpg` | 英 -> 中 | 现代多栏英文报纸主页 + 顶栏下拉菜单 + 柱状图 | 顶栏导航零毛边零切底水平齐平、多行粗体大标题字号不腰斩、`OPINION` 眉标不吞并标题、`Seat margin in the / U.S. House of Representatives` 居中双行合并、`257 D, 178 R` 数值坐标原样保留 |
| `yahoo_portal_cards.png` | 英 -> 中 | 现代卡片流门户 + 复杂照片大图多行白字 + 低对比度广告灰框 | 并排双卡片回溯合并不断裂、照片大图 3 行白字合并 + 紧凑圆角托底、低反差纯灰广告框居中、双行紧凑标题 `UNIFORM` 行距不丢字省略、放大镜 `Q` 噪声拦截 |
| `sap_b1_form_designer.jpg` | 中 -> 英 | SAP B1 高密度经典桌面 ERP 窗体与 3D 黄色按钮 | 左侧树形菜单字号齐整无锯齿跳崖、低对比度 1px 黄色按钮边框精准刹车零穿框、图标不误涂 |
| `hackernews_nested_comments.png` | 英 -> 中 | Hacker News 多级嵌套缩进评论流 | 嵌套缩进评论严守原左边缘零向右漂移、作者/时间浅灰 meta 行与黑色正文不误合并、`reply` 独立保留 |
| `alternativeto_web_grid.jpg` | 英 -> 中 | AlternativeTo 网页网格卡片 + 居中搜索横幅 | 居中主标语与底部居中按钮精准居中、卡片标题与浅灰分类行分层独立、`AlternativeTo`/`NGINX`/`PostgreSQL` 品牌名保留 |
| `dark_markdown_codebadges.jpg` | 中 -> 英 | 暗色中文技术文档 + 悬挂缩进列表 + 行内代码胶囊 | 悬挂缩进多行列表段合并、行内代码徽章残影擦净、多行正文统一字号平滑归一 |
| `gnway_admin_dialog.jpg` | 中 -> 英 | 经典 Win32 管理端树菜单 + 表格 + 模态错误弹窗 | `AS0002` 编码与 `V8.6.0.724` 版本号原样保留、表格短单元格 `是/是/是` 独立分离不合并、弹窗 `确定` 居中按钮 |
| `packing_card_handheld.jpg` | 中 -> 英 | 工业产品装箱卡表格 + 二维码 + 料号编码 | 二维码区域零侵入、工业料号 `ZZ00445707`/`501023856AA`/`HB0502000441` 保留原样、单元格译文严守表格线不跨行踩线 |
