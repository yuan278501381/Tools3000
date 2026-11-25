import os
import sys
import json
import math
import time
import random
import urllib.request
import urllib.parse
from concurrent.futures import ThreadPoolExecutor, as_completed

import cv2
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
CORPUS_ROOT = str(_REPO_ROOT / "tests" / "fixtures" / "ocr" / "web_benchmark")
REPO_IMAGES = str(_REPO_ROOT / "docs" / "images")
FONTS_DIR = os.path.join(os.environ.get("WINDIR", r"C:\Windows"), "Fonts")

os.makedirs(CORPUS_ROOT, exist_ok=True)
WEB_DIR = os.path.join(CORPUS_ROOT, "web")
GT_DIR = os.path.join(CORPUS_ROOT, "gt")
SIZE_DIR = os.path.join(CORPUS_ROOT, "sizes")
for d in (WEB_DIR, GT_DIR, SIZE_DIR):
    os.makedirs(d, exist_ok=True)

# ─────────────────────────────────────────────────────────────────────────────
# Suite A: 15 Real Project Images from Tools3000 repo
# ─────────────────────────────────────────────────────────────────────────────
PROJECT_SPECS = [
    ("proj_zh_bento", "zh", "ui_poster", os.path.join(REPO_IMAGES, "zh-CN", "feature_bento_overview.png")),
    ("proj_zh_pin_ocr", "zh", "ui_pin_ocr", os.path.join(REPO_IMAGES, "zh-CN", "scene_pin_and_ocr.png")),
    ("proj_zh_content_search", "zh", "ui_dense_search", os.path.join(REPO_IMAGES, "zh-CN", "scene_content_search.png")),
    ("proj_zh_keycast_stats", "zh", "ui_stats_table", os.path.join(REPO_IMAGES, "zh-CN", "scene_keycast_and_stats.png")),
    ("proj_zh_shortcuts", "zh", "ui_settings_form", os.path.join(REPO_IMAGES, "settings_shortcuts.png")),
    ("proj_zh_tray", "zh", "ui_small_menu", os.path.join(REPO_IMAGES, "tray_quick_menu.png")),
    ("proj_zh_tw_bento", "zh_tw", "ui_poster", os.path.join(REPO_IMAGES, "zh-TW", "feature_bento_overview.png")),
    ("proj_en_bento", "en", "ui_poster", os.path.join(REPO_IMAGES, "en-US", "feature_bento_overview.png")),
    ("proj_en_dialog", "en", "ui_dialog_paths", os.path.join(REPO_IMAGES, "en-US", "scene_dialog_enhancer.png")),
    ("proj_en_pin_wavy", "en", "ui_small_pin", os.path.join(REPO_IMAGES, "en-US", "scene_pin_live_text_wavy.png")),
    ("proj_ja_bento", "ja", "ui_poster", os.path.join(REPO_IMAGES, "ja-JP", "feature_bento_overview.png")),
    ("proj_ko_bento", "ko", "ui_poster", os.path.join(REPO_IMAGES, "ko-KR", "feature_bento_overview.png")),
    ("proj_ru_bento", "ru", "ui_poster", os.path.join(REPO_IMAGES, "ru-RU", "feature_bento_overview.png")),
    ("proj_es_bento", "es", "ui_poster", os.path.join(REPO_IMAGES, "es-ES", "feature_bento_overview.png")),
    ("proj_pt_bento", "pt", "ui_poster", os.path.join(REPO_IMAGES, "pt-BR", "feature_bento_overview.png")),
]

project_entries = []
for pid, lang, scene, path in PROJECT_SPECS:
    if os.path.isfile(path):
        project_entries.append({"id": pid, "lang": lang, "scene": scene, "path": path})
    else:
        print(f"[WARN] Missing project image: {path}")

print(f"[OK] Suite A (Project Real Images): {len(project_entries)} images")

# ─────────────────────────────────────────────────────────────────────────────
# Suite C: Exact Ground-Truth Multilingual Cards + Size Scaling Stress Images
# ─────────────────────────────────────────────────────────────────────────────
def load_font(name, size):
    p = os.path.join(FONTS_DIR, name)
    return ImageFont.truetype(p, size)

def render_card(out_path, width, bg_color, fg_color, font_name, font_size, lines,
                padding=36, line_spacing=14, header_bar=None, rotate_deg=0.0, add_noise=False, add_shadow=False):
    font = load_font(font_name, font_size)
    # Calculate required height
    tmp = Image.new("RGB", (width, 100), bg_color)
    d_tmp = ImageDraw.Draw(tmp)
    line_heights = []
    for ln in lines:
        bbox = d_tmp.textbbox((0, 0), ln if ln else "Ag", font=font)
        line_heights.append(max(font_size, bbox[3] - bbox[1]))
    total_h = padding * 2 + (54 if header_bar else 0) + sum(line_heights) + line_spacing * max(0, len(lines) - 1)
    img = Image.new("RGB", (width, total_h), bg_color)
    draw = ImageDraw.Draw(img)
    y = padding
    if header_bar:
        draw.rectangle([0, 0, width, 46], fill=(32, 42, 58) if sum(bg_color) < 300 else (235, 240, 248))
        hfont = load_font(font_name, max(16, int(font_size * 0.9)))
        draw.text((padding, 10), header_bar, font=hfont, fill=fg_color)
        y += 54
    for ln, lh in zip(lines, line_heights):
        draw.text((padding, y), ln, font=font, fill=fg_color)
        y += lh + line_spacing

    if add_shadow:
        arr = np.array(img, dtype=np.float32)
        h, w = arr.shape[:2]
        yy, xx = np.mgrid[0:h, 0:w]
        grad = 1.0 - 0.28 * np.exp(-(((xx - w * 0.72) / (w * 0.35)) ** 2 + ((yy - h * 0.65) / (h * 0.35)) ** 2))
        arr = np.clip(arr * grad[:, :, None], 0, 255).astype(np.uint8)
        img = Image.fromarray(arr)
    if rotate_deg != 0.0:
        img = img.rotate(rotate_deg, resample=Image.BICUBIC, expand=True, fillcolor=bg_color)
    if add_noise:
        rng = np.random.default_rng(20261125)
        arr = np.array(img, dtype=np.float32)
        noise = rng.normal(0, 7.5, arr.shape)
        arr = np.clip(arr + noise, 0, 255).astype(np.uint8)
        img = Image.fromarray(arr).filter(ImageFilter.GaussianBlur(radius=0.5))

    img.save(out_path)
    gt_lines = ([header_bar] if header_bar else []) + [ln for ln in lines if ln.strip()]
    return "\n".join(gt_lines), img.size

GT_SPECS = [
    ("gt_zh_simp", "zh", "gt_clean_doc", "msyh.ttc", 26, (252, 252, 252), (24, 24, 28), 1160, "Tools3000 离线高精度文字识别基准测试（简体中文）", [
        "微内核架构设计原则：高内聚、低耦合、单一职责与单一事实源。",
        "2026年11月25日实测：冷启动专用工作集 4.79 MB，总内存占用 12.84 MB。",
        "支持 100%~200% 混合多屏高分屏缩放，支持截图贴图、长截图与全局秒搜。",
        "生僻字与易混词校验：饕餮、尴尬、璀璨、氤氲、魑魅魍魉、己巳已、戍戌戊戎。",
        "订单编号：ORD-20261125-889402 | 实付金额：¥1,298.50 | 状态：已完成。",
    ], 0.0, False, False),
    ("gt_zh_trad", "zh_tw", "gt_clean_doc", "msyh.ttc", 26, (250, 250, 252), (22, 24, 30), 1160, "繁體中文與港澳臺常用字詞識別精度基準測試", [
        "高效能桌面工具軟體支援繁體中文、簡體中文、英文、日文與多國語系。",
        "即時擷取螢幕畫面、自動辨識表格結構、快速翻譯並保留原始版面排版。",
        "常用繁體字校驗：臺灣、軟體、硬體、網路、螢幕、檔案、視窗、滑鼠、鍵盤。",
        "複雜筆畫測試：鬱鬱蔥蔥、憂鬱的臺灣烏龜、龍騰虎躍、驚鴻一瞥、鶴立雞群。",
        "統一編號：84291053 | 臺北市信義區松仁路100號 | 聯絡電話：02-2345-6789。",
    ], 0.0, False, False),
    ("gt_en_code", "en", "gt_code_dark", "consola.ttf", 22, (24, 27, 34), (226, 232, 240), 1240, "DeepOcrEngine.cpp - Native WinML DirectML Pipeline", [
        "constexpr float kDetBinaryThresh = 0.28f; // DBNet threshold",
        "std::vector<OcrResult> results = engine.extractText(bgrImage, \"auto\");",
        "if (priv_ws_mb <= 15.0 && peak_ws_mb < 420.0) { return Status::OK; }",
        "URL: https://github.com/yuan278501381/Tools3000/releases/tag/v1.4.7",
        "JSON: {\"engine\": \"PP-OCRv6\", \"tier\": \"medium\", \"size_mb\": 132.32, \"ok\": true}",
        "Symbols: !@#$%^&*()_+-=[]{}|;':\",./<>? ~` 0O1lI 5S8B 2Z",
    ], 0.0, False, False),
    ("gt_fr_accents", "fr", "gt_clean_doc", "segoeui.ttf", 26, (255, 255, 255), (25, 28, 35), 1180, "Évaluation OCR en français : accents, ligatures et ponctuation", [
        "L'élève français étudie à l'école près du château d'été où brûle l'âtre.",
        "Garçon, un café crème, une crêpe brûlée et un cœur d'artichaut, s'il vous plaît !",
        "Caractères spéciaux : à â ä æ ç é è ê ë î ï ô œ ù û ü ÿ.",
        "Majuscules accentuées : À Â Ä Æ Ç É È Ê Ë Î Ï Ô Œ Ù Û Ü Ÿ — 1 250,75 €.",
        "Dès Noël, où un zéphyr haï me vêt de glaçons würmiens, je dîne d'exquis rôti de bœuf.",
    ], 0.0, False, False),
    ("gt_vi_tones", "vi", "gt_clean_doc", "segoeui.ttf", 26, (255, 255, 255), (22, 26, 32), 1220, "Kiểm thử nhận dạng ký tự tiếng Việt đầy đủ 6 thanh điệu", [
        "Cộng hòa Xã hội Chủ nghĩa Việt Nam — Độc lập, Tự do, Hạnh phúc.",
        "Hà Nội, Thành phố Hồ Chí Minh, Đà Nẵng, Huế, Cần Thơ, Hải Phòng.",
        "Công nghệ nhận dạng chữ viết tiếng Việt yêu cầu chính xác mọi dấu thanh.",
        "Kiểm tra nguyên âm: ă â đ ê ô ơ ư | Ă Â Đ Ê Ô Ơ Ư.",
        "Từ khó nhiều dấu: người Việt, tiếng Việt, quyền lợi, chuyển đổi, khởi nghiệp, thực hiện.",
        "Đầy đủ thanh điệu: à á ả ã ạ | ằ ắ ẳ ẵ ặ | ầ ấ ẩ ẫ ậ | ề ế ể ễ ệ | ờ ớ ở ỡ ợ | ừ ứ ử ữ ự.",
    ], 0.0, False, False),
    ("gt_ja_mixed", "ja", "gt_clean_doc", "YuGothM.ttc", 26, (254, 254, 254), (24, 24, 28), 1180, "日本語オフラインOCR精度評価テスト（漢字・ひらがな・カタカナ）", [
        "画面の任意の範囲をキャプチャし、文字認識や翻訳を瞬時に実行できます。",
        "東京都渋谷区道玄坂1-2-3 電話番号：03-5555-0199 営業時間 09:30〜21:00。",
        "カタカナ技術用語：マイクロカーネル、クリップボード、スクリーンショット、パフォーマンス。",
        "記号と混在文：Tools3000 v1.4.7（Windows 11 / ARM64・x64 完全対応）無償公開中！",
        "濁音・半濁音確認：ぱぴぷぺぽ、ばびぶべぼ、がぎぐげご、ざじずぜぞ、だぢづでど。",
    ], 0.0, False, False),
    ("gt_ko_hangul", "ko", "gt_clean_doc", "malgun.ttf", 26, (254, 254, 254), (24, 24, 28), 1180, "한국어 오프라인 OCR 정확도 벤치마크 테스트", [
        "대한민국 서울특별시 강남구 테헤란로 152 파이낸스센터 18층.",
        "화면 캡처, 텍스트 인식, 클립보드 기록 및 빠른 파일 검색을 지원합니다.",
        "다람쥐 헌 쳇바퀴에 타고파 — 한글 자음과 모음 복합 받침 정확도 검증.",
        "가격 안내: 기본 모델 무료(0원), 고속 GPU 가속 지원, 메모리 사용량 12.84 MB.",
        "버전 정보: Tools3000 v1.4.7 (2026년 11월 25일 릴리스, x64 및 ARM64 지원).",
    ], 0.0, False, False),
    ("gt_ru_cyrillic", "ru", "gt_clean_doc", "segoeui.ttf", 26, (254, 254, 254), (24, 24, 28), 1200, "Тестирование точности офлайн-распознавания русского текста", [
        "Съешь ещё этих мягких французских булок, да выпей же чаю!",
        "Российская Федерация, г. Москва, ул. Тверская, д. 12, стр. 1, офис 405.",
        "Быстрый поиск файлов, захват экрана, распознавание текста и перевод на лету.",
        "Проверка кириллицы: АБВГДЕЁЖЗИЙКЛМНОПРСТУФХЦЧШЩЪЫЬЭЮЯ.",
        "Версия Tools3000 v1.4.7 — потребление памяти в режиме ожидания: 12,84 МБ.",
    ], 0.0, False, False),
    ("gt_eu_de_es_pt", "eu_latin", "gt_clean_doc", "segoeui.ttf", 26, (254, 254, 254), (24, 24, 28), 1200, "Deutsch / Español / Português — Sonderzeichen & Diakritika", [
        "Deutsch: Zwölf Boxkämpfer jagen Viktor quer über den großen Sylter Deich. Straße, Öl, Übung, Äpfel.",
        "Español: ¡Atención! ¿Dónde está la configuración rápida del niño español y la pingüino?",
        "Português: Configuração automática, tradução instantânea, ação, coração, órgão, são 1.450,00 R$.",
        "Polski / Türkçe / Čeština: Zażółć gęślą jaźń | Şeker, ğ, ı, İ, ö, ü, ç | Příliš žluťoučký kůň.",
    ], 0.0, False, False),
    ("gt_hard_receipt_skew", "zh", "gt_hard_noisy_skew", "msyh.ttc", 22, (240, 238, 232), (55, 55, 60), 900, None, [
        "电子普通发票（实拍阴影与倾斜抗干扰测试）",
        "发票代码：011002600111   发票号码：88492017",
        "购买方名称：星辰智能科技（深圳）有限公司",
        "货物或应税劳务名称       数量    单价      金额",
        "云计算资源订阅服务(年度)   1     2400.00   2400.00",
        "税率：6%   税额：144.00   价税合计：¥2,544.00",
        "开票日期：2026年11月24日   收款人：林知行",
    ], -2.8, True, True),
]

gt_entries = []
for gid, lang, scene, font_name, fsize, bg, fg, w, header, lines, rot, noisy, shadow in GT_SPECS:
    out_p = os.path.join(GT_DIR, f"{gid}.png")
    gt_text, dims = render_card(out_p, w, bg, fg, font_name, fsize, lines,
                                header_bar=header, rotate_deg=rot, add_noise=noisy, add_shadow=shadow)
    gt_entries.append({
        "id": gid, "lang": lang, "scene": scene, "path": out_p,
        "width": dims[0], "height": dims[1], "ground_truth": gt_text
    })

print(f"[OK] Suite C1 (Exact GT Multilingual Cards): {len(gt_entries)} images")

# ─────────────────────────────────────────────────────────────────────────────
# Suite C2: 5-Tier Resolution & Aspect Ratio Stress Images (for Memory/Latency Scaling)
# ─────────────────────────────────────────────────────────────────────────────
def render_fixed_size_stress(out_path, width, height, font_size, cols, rows, title):
    img = Image.new("RGB", (width, height), (250, 251, 253))
    draw = ImageDraw.Draw(img)
    font = load_font("msyh.ttc", font_size)
    mono = load_font("consola.ttf", max(12, font_size - 2))
    draw.rectangle([0, 0, width, max(32, font_size + 18)], fill=(30, 41, 59))
    draw.text((16, 8), title, font=font, fill=(248, 250, 252))
    gt_lines = [title]
    sample_phrases = [
        "Tools3000 高性能离线 OCR 引擎内存与延迟阶梯压测样本",
        "L'évaluation française : café, château, œuvre, résumé №",
        "Latency=18.4ms | PrivWS=12.84MB | Commit=48.20MB | OK",
        "日本語テキスト認識・レイアウト復元および高速検索エンジン",
        "order_id=ORD-2026-99182 | amount=1298.50 | status=PAID",
    ]
    y0 = max(44, font_size + 26)
    usable_h = height - y0 - 16
    row_h = max(font_size + 10, usable_h // max(1, rows))
    col_w = (width - 32) // max(1, cols)
    for r in range(rows):
        y = y0 + r * row_h
        if y + font_size > height - 8:
            break
        for c in range(cols):
            x = 16 + c * col_w
            phrase = sample_phrases[(r + c) % len(sample_phrases)]
            txt = f"[{r+1:02d},{c+1}] {phrase}"
            # trim to fit column
            while len(txt) > 12 and draw.textlength(txt, font=font) > col_w - 18:
                txt = txt[:-2]
            draw.text((x, y), txt, font=mono if (r + c) % 2 == 0 else font, fill=(25, 30, 40))
            gt_lines.append(txt)
    img.save(out_path)
    return "\n".join(gt_lines)

SIZE_SPECS = [
    ("size_01_small_400x120", 400, 120, 18, 1, 2, "Tools3000 极小截图 400x120"),
    ("size_02_hd_1280x720", 1280, 720, 20, 2, 16, "Tools3000 常规窗口截图 1280x720 (HD)"),
    ("size_03_2k_2560x1440", 2560, 1440, 22, 3, 30, "Tools3000 2K 全屏高密文档 2560x1440 (QHD)"),
    ("size_04_4k_3840x2160", 3840, 2160, 24, 4, 42, "Tools3000 4K 超清密集文档 3840x2160 (UHD)"),
    ("size_05_tall_1440x4800", 1440, 4800, 22, 2, 95, "Tools3000 超长滚动截屏 1440x4800 (Tall 3.33:1)"),
]

size_entries = []
for sid, w, h, fsize, cols, rows, title in SIZE_SPECS:
    out_p = os.path.join(SIZE_DIR, f"{sid}.png")
    gt = render_fixed_size_stress(out_p, w, h, fsize, cols, rows, title)
    size_entries.append({
        "id": sid, "lang": "mixed", "scene": "size_stress", "path": out_p,
        "width": w, "height": h, "pixels_mp": round((w * h) / 1_000_000, 2),
        "ground_truth": gt
    })

print(f"[OK] Suite C2 (5-Tier Size Scaling Stress Images): {len(size_entries)} images")

# ─────────────────────────────────────────────────────────────────────────────
# Suite B: 80 Real-World Internet Images (8 Languages x 10 Diverse Scenes)
# via Wikimedia Commons API (open-licensed real photographs, signs, menus, docs)
# ─────────────────────────────────────────────────────────────────────────────
UA = "Tools3000-OCR-Benchmark/1.0 (https://github.com/yuan278501381/Tools3000; open-source research)"

WEB_LANG_QUERIES = {
    "zh": [
        ("sign", "Chinese street sign shop sign"),
        ("menu", "Chinese restaurant menu"),
        ("notice", "Chinese notice board sign"),
        ("book", "Chinese book page text"),
        ("ticket", "China train ticket"),
        ("poster", "Chinese poster text"),
        ("station", "China metro station sign"),
        ("plaque", "Chinese plaque museum"),
    ],
    "en": [
        ("sign", "English street information sign"),
        ("menu", "English restaurant menu board"),
        ("book", "English book page scan"),
        ("receipt", "store receipt paper"),
        ("warning", "warning sign text English"),
        ("plaque", "historical plaque English text"),
        ("poster", "English poster notice"),
        ("newspaper", "English newspaper page"),
    ],
    "fr": [
        ("sign", "panneau rue France texte"),
        ("menu", "menu restaurant français tableau"),
        ("plaque", "plaque commémorative Paris"),
        ("notice", "panneau information musée France"),
        ("book", "page livre français"),
        ("poster", "affiche française texte"),
        ("shop", "devanture boutique boulangerie France"),
        ("station", "panneau gare SNCF métro Paris"),
    ],
    "vi": [
        ("sign", "biển hiệu đường phố Việt Nam"),
        ("street", "Vietnam street sign Hanoi"),
        ("shop", "Vietnam shop sign Ho Chi Minh"),
        ("menu", "Vietnamese menu restaurant"),
        ("notice", "bảng thông báo tiếng Việt"),
        ("book", "sách tiếng Việt trang"),
        ("plaque", "bia di tích Việt Nam"),
        ("poster", "Vietnam banner sign text"),
    ],
    "ja": [
        ("sign", "Japanese street sign Tokyo"),
        ("menu", "Japanese restaurant menu"),
        ("station", "Japan railway station sign"),
        ("notice", "Japanese notice sign board"),
        ("book", "Japanese book page text"),
        ("shop", "Japanese shop sign Kyoto"),
        ("poster", "Japanese poster notice"),
        ("plaque", "Japanese information board shrine"),
    ],
    "ko": [
        ("sign", "Korean street sign Seoul"),
        ("menu", "Korean restaurant menu"),
        ("station", "Seoul subway station sign"),
        ("notice", "Korean information sign board"),
        ("shop", "Korean shop sign Myeongdong"),
        ("book", "Korean book page text"),
        ("plaque", "Korean monument plaque text"),
        ("poster", "Korean poster sign"),
    ],
    "ru": [
        ("sign", "Russian street sign Moscow"),
        ("station", "Moscow metro station sign"),
        ("menu", "Russian menu restaurant"),
        ("plaque", "мемориальная доска Москва"),
        ("notice", "информационный стенд Россия"),
        ("book", "страница книги русский текст"),
        ("shop", "вывеска магазин Россия"),
        ("poster", "плакат русский текст"),
    ],
    "eu_latin": [
        ("de_sign", "Hinweisschild Deutsch Text"),
        ("de_menu", "Speisekarte Tafel Deutsch"),
        ("es_sign", "señal cartel español texto"),
        ("es_plaque", "placa conmemorativa España"),
        ("pt_sign", "placa aviso português Brasil"),
        ("pt_menu", "cardápio restaurante português"),
        ("de_plaque", "Gedenktafel Berlin Text"),
        ("es_menu", "menú restaurante español"),
    ],
}

def commons_search(query, limit=18):
    params = {
        "action": "query",
        "format": "json",
        "generator": "search",
        "gsrnamespace": "6",
        "gsrsearch": f"{query} filetype:bitmap",
        "gsrlimit": str(limit),
        "prop": "imageinfo",
        "iiprop": "url|size|mime",
        "iiurlwidth": "1280",
    }
    url = "https://commons.wikimedia.org/w/api.php?" + urllib.parse.urlencode(params)
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=15) as r:
        data = json.loads(r.read().decode("utf-8", "replace"))
    pages = data.get("query", {}).get("pages", {})
    out = []
    for p in pages.values():
        infos = p.get("imageinfo", [])
        if not infos:
            continue
        ii = infos[0]
        mime = ii.get("mime", "")
        if mime not in ("image/jpeg", "image/png"):
            continue
        w = ii.get("width", 0)
        h = ii.get("height", 0)
        if w < 500 or h < 300:
            continue
        dl_url = ii.get("thumburl") or ii.get("url")
        if not dl_url:
            continue
        out.append({
            "title": p.get("title", ""),
            "url": dl_url,
            "orig_w": w,
            "orig_h": h,
            "mime": mime,
        })
    return out

def download_valid_image(url, dst_path):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=20) as r:
        data = r.read()
    if len(data) < 12_000 or len(data) > 6_000_000:
        return None
    arr = np.frombuffer(data, dtype=np.uint8)
    img = cv2.imdecode(arr, cv2.IMREAD_COLOR)
    if img is None:
        return None
    h, w = img.shape[:2]
    if w < 420 or h < 260:
        return None
    # Reject flat/empty images
    if float(np.std(img)) < 12.0:
        return None
    # Save normalized JPEG/PNG
    cv2.imwrite(dst_path, img, [cv2.IMWRITE_JPEG_QUALITY, 92])
    return (w, h)

def fetch_language_pack(lang, queries, target_count=10):
    lang_dir = os.path.join(WEB_DIR, lang)
    os.makedirs(lang_dir, exist_ok=True)
    # Check if already downloaded
    existing_meta = os.path.join(lang_dir, "_meta.json")
    if os.path.isfile(existing_meta):
        with open(existing_meta, "r", encoding="utf-8") as f:
            cached = json.load(f)
        if len(cached) >= target_count and all(os.path.isfile(x["path"]) for x in cached):
            return cached[:target_count]

    seen_titles = set()
    candidates = []
    for scene, q in queries:
        try:
            hits = commons_search(q, limit=12)
            # Take up to 4 candidates per scene first for diversity
            taken = 0
            for h in hits:
                t = h["title"]
                if t in seen_titles:
                    continue
                seen_titles.add(t)
                h["scene"] = scene
                candidates.append(h)
                taken += 1
                if taken >= 4:
                    break
        except Exception as e:
            print(f"  [WARN] search {lang}/{scene}: {e}")
        time.sleep(0.25)

    # Interleave scenes so we get diverse scenes in the first 10
    by_scene = {}
    for c in candidates:
        by_scene.setdefault(c["scene"], []).append(c)
    ordered = []
    idx = 0
    while True:
        added = False
        for sc in list(by_scene.keys()):
            if idx < len(by_scene[sc]):
                ordered.append(by_scene[sc][idx])
                added = True
        if not added:
            break
        idx += 1

    saved = []
    for cand in ordered:
        if len(saved) >= target_count:
            break
        n = len(saved) + 1
        img_id = f"web_{lang}_{n:02d}_{cand['scene']}"
        dst = os.path.join(lang_dir, f"{img_id}.jpg")
        try:
            dims = download_valid_image(cand["url"], dst)
            if dims:
                saved.append({
                    "id": img_id,
                    "lang": lang,
                    "scene": f"web_{cand['scene']}",
                    "path": dst,
                    "width": dims[0],
                    "height": dims[1],
                    "source_title": cand["title"],
                    "source_url": cand["url"],
                })
        except Exception:
            pass
        time.sleep(0.15)

    with open(existing_meta, "w", encoding="utf-8") as f:
        json.dump(saved, f, ensure_ascii=False, indent=2)
    return saved

PINNED_MANIFEST = Path(__file__).resolve().parent / "web_corpus_urls.json"


def fetch_pinned_manifest():
    if not PINNED_MANIFEST.is_file():
        return None
    data = json.loads(PINNED_MANIFEST.read_text(encoding="utf-8"))
    pinned = data.get("images", [])
    if not pinned:
        return None
    saved = []
    missing = []
    for item in pinned:
        rel_path = item["rel_path"].replace("/", os.sep)
        dst = os.path.join(CORPUS_ROOT, rel_path)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.isfile(dst):
            saved.append({
                "id": item["id"],
                "lang": item["lang"],
                "scene": item["scene"],
                "path": dst,
                "width": item.get("width", 0),
                "height": item.get("height", 0),
                "source_title": item.get("source_title", ""),
                "source_url": item.get("source_url", ""),
            })
        else:
            missing.append((item, dst))

    if missing:
        print(f"[INFO] Downloading {len(missing)} pinned web images from {PINNED_MANIFEST.name}...")
        def _dl(pair):
            item, dst = pair
            dims = download_valid_image(item["source_url"], dst)
            if not dims:
                return None
            return {
                "id": item["id"],
                "lang": item["lang"],
                "scene": item["scene"],
                "path": dst,
                "width": dims[0],
                "height": dims[1],
                "source_title": item.get("source_title", ""),
                "source_url": item.get("source_url", ""),
            }
        with ThreadPoolExecutor(max_workers=4) as ex:
            for fut in as_completed([ex.submit(_dl, m) for m in missing]):
                try:
                    rec = fut.result()
                    if rec:
                        saved.append(rec)
                except Exception as e:
                    print(f"  [WARN] pinned download failed: {e}")

    return saved if len(saved) >= len(pinned) else None


web_entries = fetch_pinned_manifest()
if web_entries is None:
    print("[INFO] Fetching 80 real-world web images from Wikimedia Commons (8 languages x 10 scenes)...")
    web_entries = []
    with ThreadPoolExecutor(max_workers=4) as ex:
        futs = {ex.submit(fetch_language_pack, lang, qs, 10): lang for lang, qs in WEB_LANG_QUERIES.items()}
        for fut in as_completed(futs):
            lang = futs[fut]
            try:
                res = fut.result()
                web_entries.extend(res)
                print(f"  [OK] web/{lang}: {len(res)} images downloaded")
            except Exception as e:
                print(f"  [ERROR] web/{lang}: {e}")
else:
    print(f"[OK] Loaded/verified {len(web_entries)} pinned real-world web images from {PINNED_MANIFEST.name}")

web_entries.sort(key=lambda x: x["id"])
print(f"[OK] Suite B (Real-World Web Images): {len(web_entries)} images total")

# Write manifests
def write_manifest(fname, items):
    p = os.path.join(CORPUS_ROOT, fname)
    with open(p, "w", encoding="utf-8") as f:
        json.dump({"count": len(items), "images": items}, f, ensure_ascii=False, indent=2)
    print(f"  -> Wrote {fname} ({len(items)} items)")

smoke_items = [
    gt_entries[0], # zh_simp
    gt_entries[3], # fr_accents
    gt_entries[4], # vi_tones
]
write_manifest("manifest_smoke.json", smoke_items)
write_manifest("manifest_project.json", project_entries)
write_manifest("manifest_web.json", web_entries)
write_manifest("manifest_gt.json", gt_entries)
write_manifest("manifest_size.json", size_entries)
write_manifest("manifest_all.json", project_entries + gt_entries + size_entries + web_entries)
