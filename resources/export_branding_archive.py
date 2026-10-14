import os
import shutil
from PIL import Image

def export_archive():
    artifact_dir = r"C:\Users\yuan2\.gemini\antigravity\brain\a4d8e3c0-7c1c-4d27-95cd-65baa13ee7a1"
    repo_root = r"C:\repo\tools3000"
    archive_root = os.path.join(repo_root, "resources", "branding_archive")

    renders_dir = os.path.join(archive_root, "01_master_renders")
    vectors_dir = os.path.join(archive_root, "02_vector_sources_svg")
    hires_dir = os.path.join(archive_root, "03_transparent_png_hires")

    os.makedirs(renders_dir, exist_ok=True)
    os.makedirs(vectors_dir, exist_ok=True)
    os.makedirs(hires_dir, exist_ok=True)

    # 1. 拷贝 4 套母版概念渲染原图 (JPG 1024x1024)
    master_copies = [
        ("letter_e_aero_glide_1787155546091.jpg", "scheme1_aero_glide_e_master.jpg"),
        ("letter_e_bolt_fusion_1787155583798.jpg", "scheme2_bolt_fusion_e_master.jpg"),
        ("letter_e_isometric_prism_1787155627011.jpg", "scheme3_floating_prism_e_master.jpg"),
        ("letter_e_sonic_loop_1787155667576.jpg", "scheme4_sonic_loop_e_master.jpg"),
        ("master_tray_aero_glide_e_1787158355720.jpg", "tray_scheme1_aero_glide_e_dark_master.jpg"),
        ("master_tray_sonic_loop_e_1787158373479.jpg", "tray_scheme4_sonic_loop_e_dark_master.jpg"),
        ("letter_e_comparison_board.png", "all_schemes_comparison_board.png"),
    ]

    for src_name, dst_name in master_copies:
        src_path = os.path.join(artifact_dir, src_name)
        if os.path.exists(src_path):
            shutil.copy2(src_path, os.path.join(renders_dir, dst_name))
            print(f"Copied render master: {dst_name}")

    # 2. 生成单独白色 E 造型及完整图标的专业可编辑矢量源文件 (.svg)
    # ── SVG 1: 方案 1 单独白色超速滑翔光翼 E (可编辑节点)
    svg_scheme1_standalone = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" fill="none">
  <!-- Tools3000 品牌方案 1: 单独纯白超速滑翔光翼 E (The Aero-Glide 'E') -->
  <!-- 适用于 Figma / Illustrator / Sketch / Inkscape 任意编辑曲线与节点 -->
  <g fill="#FFFFFF" id="aero-glide-e">
    <!-- 顶层滑翔翼 (带气动前扬上翘角) -->
    <path id="top-wing" d="M190 85 C220 85 385 85 405 85 C430 85 440 105 415 135 C380 170 330 190 305 190 L160 190 Z" />
    <!-- 中层穿梭翼 -->
    <path id="middle-wing" d="M145 230 L365 230 C385 230 390 248 370 270 C350 292 320 312 295 312 L120 312 Z" />
    <!-- 底层滑行翼 -->
    <path id="bottom-wing" d="M110 350 L370 350 C390 350 395 368 375 395 C355 422 320 445 285 445 L65 445 Z" />
    <!-- 前倾动能主脊柱 (倾斜角 ~14°) -->
    <path id="spine" d="M190 85 L250 85 L125 445 L65 445 Z" />
  </g>
</svg>'''

    # ── SVG 2: 方案 2 单独白色闪电融合同构 E
    svg_scheme2_standalone = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" fill="none">
  <!-- Tools3000 品牌方案 2: 单独纯白闪电融合同构 E (The Bolt 'E' Fusion) -->
  <g fill="#FFFFFF" id="bolt-fusion-e">
    <path id="lightning-spine" d="M225 60 L140 250 L245 250 L160 450 L315 220 L220 220 L275 60 Z" />
    <rect id="top-bar" x="260" y="80" width="140" height="55" rx="10" />
    <rect id="mid-bar" x="235" y="225" width="165" height="55" rx="10" />
    <rect id="bot-bar" x="200" y="375" width="200" height="55" rx="10" />
  </g>
</svg>'''

    # ── SVG 3: 方案 3 单独白色悬浮三维卡片 E
    svg_scheme3_standalone = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" fill="none">
  <!-- Tools3000 品牌方案 3: 单独纯白悬浮三维卡片 E (The Layered Prism 'E') -->
  <g fill="#FFFFFF" id="layered-prism-e">
    <rect id="spine" x="130" y="90" width="70" height="332" rx="35" opacity="0.85" />
    <rect id="top-card" x="130" y="90" width="270" height="72" rx="36" />
    <rect id="mid-card" x="130" y="220" width="240" height="72" rx="36" />
    <rect id="bot-card" x="130" y="350" width="270" height="72" rx="36" />
  </g>
</svg>'''

    # ── SVG 4: 方案 4 单独白色莫比乌斯流体尾迹 e
    svg_scheme4_standalone = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" fill="none">
  <!-- Tools3000 品牌方案 4: 单独纯白莫比乌斯流体尾迹 e (The Sonic Loop 'e') -->
  <g fill="#FFFFFF" id="sonic-loop-e">
    <path fill-rule="evenodd" clip-rule="evenodd" d="M256 60 C150 60 75 145 75 270 C75 385 160 452 280 452 C350 452 405 410 455 170 C405 315 320 358 256 358 C185 358 145 305 145 240 L395 240 C405 240 405 185 375 140 C340 90 300 60 256 60 Z M170 190 C180 135 220 115 256 115 C295 115 330 135 330 190 L170 190 Z" />
  </g>
</svg>'''

    # ── 完整应用图标 SVG (带超椭圆渐变底座 + 3D 微阴影)
    svg_scheme1_full_app = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" fill="none">
  <defs>
    <linearGradient id="bgGrad1" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#38BDF8" />
      <stop offset="100%" stop-color="#6366F1" />
    </linearGradient>
    <filter id="dropShadow1" x="-10%" y="-10%" width="130%" height="130%">
      <feDropShadow dx="0" dy="12" stdDeviation="16" flood-color="#000000" flood-opacity="0.28" />
    </filter>
  </defs>
  <!-- 超椭圆底座 -->
  <rect x="20" y="20" width="472" height="472" rx="104" fill="url(#bgGrad1)" stroke="rgba(255,255,255,0.25)" stroke-width="3" />
  <!-- 纯白滑翔光翼 E -->
  <g fill="#FFFFFF" filter="url(#dropShadow1)">
    <path d="M190 85 C220 85 385 85 405 85 C430 85 440 105 415 135 C380 170 330 190 305 190 L160 190 Z" />
    <path d="M145 230 L365 230 C385 230 390 248 370 270 C350 292 320 312 295 312 L120 312 Z" />
    <path d="M110 350 L370 350 C390 350 395 368 375 395 C355 422 320 445 285 445 L65 445 Z" />
    <path d="M190 85 L250 85 L125 445 L65 445 Z" />
  </g>
</svg>'''

    svg_scheme4_full_app = '''<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 512 512" width="512" height="512" fill="none">
  <defs>
    <linearGradient id="bgGrad4" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#38BDF8" />
      <stop offset="100%" stop-color="#7C3AED" />
    </linearGradient>
    <filter id="dropShadow4" x="-10%" y="-10%" width="130%" height="130%">
      <feDropShadow dx="0" dy="12" stdDeviation="16" flood-color="#000000" flood-opacity="0.28" />
    </filter>
  </defs>
  <rect x="20" y="20" width="472" height="472" rx="104" fill="url(#bgGrad4)" stroke="rgba(255,255,255,0.25)" stroke-width="3" />
  <g fill="#FFFFFF" filter="url(#dropShadow4)">
    <path fill-rule="evenodd" clip-rule="evenodd" d="M256 60 C150 60 75 145 75 270 C75 385 160 452 280 452 C350 452 405 410 455 170 C405 315 320 358 256 358 C185 358 145 305 145 240 L395 240 C405 240 405 185 375 140 C340 90 300 60 256 60 Z M170 190 C180 135 220 115 256 115 C295 115 330 135 330 190 L170 190 Z" />
  </g>
</svg>'''

    svg_files = [
        ("standalone_white_e_scheme1_aero_glide.svg", svg_scheme1_standalone),
        ("standalone_white_e_scheme2_bolt_fusion.svg", svg_scheme2_standalone),
        ("standalone_white_e_scheme3_prism.svg", svg_scheme3_standalone),
        ("standalone_white_e_scheme4_sonic_loop.svg", svg_scheme4_standalone),
        ("app_icon_scheme1_aero_glide.svg", svg_scheme1_full_app),
        ("app_icon_scheme4_sonic_loop.svg", svg_scheme4_full_app),
    ]

    for fname, content in svg_files:
        path = os.path.join(vectors_dir, fname)
        with open(path, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"Generated Vector SVG: {fname}")

    # 3. 输出 1024x1024 独立透明通道高清免抠图 (PNG)
    # 提取纯白透明 PNG 供设计软件备用
    import sys
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from build_master_production_icons import extract_large_pure_tray_icon
    tray1_path = os.path.join(artifact_dir, "master_tray_aero_glide_e_1787158355720.jpg")
    tray4_path = os.path.join(artifact_dir, "master_tray_sonic_loop_e_1787158373479.jpg")

    if os.path.exists(tray1_path):
        img_e1 = extract_large_pure_tray_icon(tray1_path, (270, 290, 730, 710), 50)
        img_e1.resize((1024, 1024), Image.Resampling.LANCZOS).save(os.path.join(hires_dir, "standalone_white_e_scheme1_1024.png"), "PNG")
        print("Exported 1024x1024 transparent PNG: standalone_white_e_scheme1_1024.png")

    if os.path.exists(tray4_path):
        img_e4 = extract_large_pure_tray_icon(tray4_path, (320, 310, 840, 670), 50)
        img_e4.resize((1024, 1024), Image.Resampling.LANCZOS).save(os.path.join(hires_dir, "standalone_white_e_scheme4_1024.png"), "PNG")
        print("Exported 1024x1024 transparent PNG: standalone_white_e_scheme4_1024.png")

    print("[SUCCESS] Tools3000 brand archive and vector export complete.")

if __name__ == "__main__":
    export_archive()
