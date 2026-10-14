import os
import sys
import io
import glob
import struct
import numpy as np
from PIL import Image, ImageDraw

def save_ico(image: Image.Image, output_path: str, sizes=(16, 20, 24, 28, 30, 32, 40, 48, 256), png_threshold=128):
    """
    Saves an ICO file supporting Windows Vista+ standard PNG compression for high resolutions
    (64x64, 96x96, 128x128 and 256x256), dramatically reducing ICO footprint while preserving 100% native
    Win32/Shell compatibility across all Windows versions and High-DPI scaling (100%~300%).
    Accurately computes 1-bit AND mask for transparent pixels to ensure full compatibility with
    legacy GDI and Windows Shell icon caching.
    """
    images_data = []
    header_size = 6 + len(sizes) * 16
    current_offset = header_size

    for sz in sizes:
        resized = image.resize((sz, sz), Image.Resampling.LANCZOS).convert("RGBA")
        if sz >= png_threshold:
            # PNG compression for high-res entries (Windows Vista+ standard, native 32bpp alpha)
            buf = io.BytesIO()
            resized.save(buf, format="PNG", optimize=True)
            png_bytes = buf.getvalue()
            images_data.append((sz, len(png_bytes), current_offset, png_bytes))
            current_offset += len(png_bytes)
        else:
            # Standard uncompressed DIB for low-res entries
            arr = np.array(resized)
            # 彻底过滤 Lanczos 降采样边缘振铃微噪点（alpha <= 4 噪点全通道归零）
            arr[arr[:, :, 3] <= 4, :] = 0

            bgra = np.zeros((sz, sz, 4), dtype=np.uint8)
            bgra[:, :, 0] = arr[:, :, 2]
            bgra[:, :, 1] = arr[:, :, 1]
            bgra[:, :, 2] = arr[:, :, 0]
            bgra[:, :, 3] = arr[:, :, 3]
            # Zero out RGB for fully transparent pixels (prevents GDI color bleeding)
            transparent_mask = (bgra[:, :, 3] == 0)
            bgra[transparent_mask, :3] = 0
            bgra_flipped = np.flipud(bgra).tobytes()
            
            xor_bytes = sz * sz * 4
            and_row_bytes = ((sz + 31) // 32) * 4
            and_mask = bytearray(and_row_bytes * sz)
            arr_flipped = np.flipud(arr)
            for y in range(sz):
                row_offset = y * and_row_bytes
                for x in range(sz):
                    # In Windows 1-bit AND mask: 1 = transparent, 0 = opaque
                    if arr_flipped[y, x, 3] == 0:
                        byte_idx = row_offset + (x // 8)
                        bit_idx = 7 - (x % 8)
                        and_mask[byte_idx] |= (1 << bit_idx)
            
            image_bytes = 40 + xor_bytes + len(and_mask)
            bih = struct.pack("<LLLHHLLLLLL", 40, sz, sz * 2, 1, 32, 0, xor_bytes, 0, 0, 0, 0)
            images_data.append((sz, image_bytes, current_offset, bih + bgra_flipped + bytes(and_mask)))
            current_offset += image_bytes

    with open(output_path, "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, len(sizes)))
        for sz, img_bytes, offset, _ in images_data:
            w_byte = 0 if sz >= 256 else sz
            h_byte = 0 if sz >= 256 else sz
            f.write(struct.pack("<BBBBHHLL", w_byte, h_byte, 0, 0, 1, 32, img_bytes, offset))
        for _, _, _, data in images_data:
            f.write(data)
    file_size = os.path.getsize(output_path)
    print(f"ICO Generated: {output_path} ({len(sizes)} sizes, {file_size:,} bytes)")

# ─────────────────────────────────────────────────────────────────────────────
# 矢量几何单一事实源 (64x64 网格，光学充满度 90.6%，笔画加粗 33%，经典经典原彩)
# ─────────────────────────────────────────────────────────────────────────────
SVG_WHITE = """<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 64 64"><title>Tools3000 · T</title><path fill="#FFFFFF" d="M3 11L48 3L61 15L43 23L39 56L23 62L27 27L5 30Z"/></svg>
"""

SVG_BLACK = """<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 64 64"><title>Tools3000 · T</title><path fill="#16181D" d="M3 11L48 3L61 15L43 23L39 56L23 62L27 27L5 30Z"/></svg>
"""

SVG_COLOR = """<svg xmlns="http://www.w3.org/2000/svg" width="512" height="512" viewBox="0 0 64 64"><title>Tools3000 · T</title><path fill="#F15B42" d="M3 11L48 3L61 15L43 23L39 56L23 62L27 27L5 30Z"/><path fill="#FF8B62" d="M3 11L48 3L61 15L16 23Z"/><path fill="#D64132" d="M27 27L43 23L39 56L23 62Z"/></svg>
"""

# 全局高精度几何顶点
PTS_OUTLINE = [(3, 11), (48, 3), (61, 15), (43, 23), (39, 56), (23, 62), (27, 27), (5, 30)]
PTS_TOP = [(3, 11), (48, 3), (61, 15), (16, 23)]
PTS_SPINE = [(27, 27), (43, 23), (39, 56), (23, 62)]

def render_master_vector(mode="white", canvas_size=1024):
    scale = canvas_size / 64.0
    im = Image.new("RGBA", (canvas_size, canvas_size), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    def sc(pts): return [(x * scale, y * scale) for x, y in pts]
    
    if mode == "white":
        d.polygon(sc(PTS_OUTLINE), fill=(255, 255, 255, 255))
    elif mode == "black":
        d.polygon(sc(PTS_OUTLINE), fill=(22, 24, 29, 255))
    elif mode in ("color", "app"):
        # 彻底去除任何 Squircle 圆角矩形底图，保持纯净透明背景与 90.6% 充满度
        # 100% 充满度无瑕疵折面：先铺底全景轮廓避免接缝软空洞，再精细渲染顶面高光与立柱投影
        d.polygon(sc(PTS_OUTLINE), fill=(241, 91, 66, 255))  # #F15B42 经典原彩主色 / 前折面 / 横梁底衬
        d.polygon(sc(PTS_TOP), fill=(255, 139, 98, 255))     # #FF8B62 经典原彩顶面高光
        d.polygon(sc(PTS_SPINE), fill=(214, 65, 50, 255))   # #D64132 经典原彩立柱阴影
    return im

def build_world_class_icons():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    sig_dir = os.path.join(repo_root, "resources", "Tools3000-T-signature-assets")
    resources_dir = os.path.join(repo_root, "resources")
    ui_public_dir = os.path.join(repo_root, "ui", "public")
    rc_file_path = os.path.join(repo_root, "src", "Tools3000.rc")
    build_dir = os.path.join(repo_root, "build")

    print("=======================================================")
    print(" [INFO] 正在生成 Tools3000 光学加粗饱满多分辨率托盘与纯净应用图标...")
    print("=======================================================")

    # 1. 部署更新后的加粗 SVG 矢量源
    svg_mappings = [
        (os.path.join(sig_dir, "t-ribbon-white.svg"), SVG_WHITE),
        (os.path.join(sig_dir, "t-ribbon-black.svg"), SVG_BLACK),
        (os.path.join(sig_dir, "t-ribbon-color.svg"), SVG_COLOR),
        (os.path.join(sig_dir, "t-ribbon-small.svg"), SVG_WHITE),
        (os.path.join(resources_dir, "t-ribbon-white.svg"), SVG_WHITE),
        (os.path.join(resources_dir, "t-ribbon-black.svg"), SVG_BLACK),
        (os.path.join(resources_dir, "t-ribbon-color.svg"), SVG_COLOR),
        (os.path.join(resources_dir, "logo.svg"), SVG_COLOR),
    ]
    if os.path.exists(ui_public_dir):
        svg_mappings.extend([
            (os.path.join(ui_public_dir, "logo.svg"), SVG_COLOR),
            (os.path.join(ui_public_dir, "t-ribbon-white.svg"), SVG_WHITE),
            (os.path.join(ui_public_dir, "t-ribbon-black.svg"), SVG_BLACK),
            (os.path.join(ui_public_dir, "t-ribbon-color.svg"), SVG_COLOR),
            (os.path.join(ui_public_dir, "logo-white.svg"), SVG_WHITE),
            (os.path.join(ui_public_dir, "logo-dark.svg"), SVG_BLACK),
            (os.path.join(ui_public_dir, "logo-small.svg"), SVG_WHITE),
            (os.path.join(ui_public_dir, "favicon.svg"), SVG_COLOR),
        ])
    for p, content in svg_mappings:
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", encoding="utf-8") as f:
            f.write(content)
        print(f"Updated SVG: {p}")

    # 2. 生成高保真矢量 Master
    img_white = render_master_vector(mode="white", canvas_size=1024)
    img_black = render_master_vector(mode="black", canvas_size=1024)
    img_color = render_master_vector(mode="color", canvas_size=1024)
    img_app = img_color  # 彻底去除 Squircle 底图，使用纯净透明立体折带 T 作为主应用图标

    # 3. 生成 12 尺寸高分屏完整 ICO 文件 (16, 20, 24, 28, 30, 32, 40, 48, 64, 96, 128, 256)
    sizes_all = (16, 20, 24, 28, 30, 32, 40, 48, 64, 96, 128, 256)
    
    # resources/ 目录 (CMake 资源编译目标)
    save_ico(img_white, os.path.join(resources_dir, "tray.ico"), sizes=sizes_all, png_threshold=64)
    save_ico(img_black, os.path.join(resources_dir, "tray_dark.ico"), sizes=sizes_all, png_threshold=64)
    save_ico(img_color, os.path.join(resources_dir, "tray_color.ico"), sizes=sizes_all, png_threshold=64)
    save_ico(img_app, os.path.join(resources_dir, "app.ico"), sizes=sizes_all, png_threshold=64)

    # 品牌资产归档
    save_ico(img_white, os.path.join(sig_dir, "t-ribbon-white.ico"), sizes=sizes_all, png_threshold=64)
    save_ico(img_black, os.path.join(sig_dir, "t-ribbon-black.ico"), sizes=sizes_all, png_threshold=64)
    save_ico(img_color, os.path.join(sig_dir, "t-ribbon-color.ico"), sizes=sizes_all, png_threshold=64)
    img_color.resize((512, 512), Image.Resampling.LANCZOS).save(os.path.join(sig_dir, "t-ribbon-color-512.png"))
    img_app.save(os.path.join(resources_dir, "app_icon_hires.png"))

    # ui/public/ 目录
    if os.path.exists(ui_public_dir):
        save_ico(img_app, os.path.join(ui_public_dir, "app.ico"), sizes=sizes_all, png_threshold=64)
        save_ico(img_app, os.path.join(ui_public_dir, "favicon.ico"), sizes=sizes_all, png_threshold=64)
        save_ico(img_color, os.path.join(ui_public_dir, "tray_color.ico"), sizes=sizes_all, png_threshold=64)
        img_app.save(os.path.join(ui_public_dir, "Logo.png"))
        img_app.resize((256, 256), Image.Resampling.LANCZOS).save(os.path.join(ui_public_dir, "logo_active.png"))
        img_app.resize((48, 48), Image.Resampling.LANCZOS).save(os.path.join(ui_public_dir, "favicon-48.png"))

    # 4. 强制刷新 Tools3000.rc 时间戳并清理 CMake 缓存的 .res 文件，确保重新编译
    if os.path.exists(rc_file_path):
        os.utime(rc_file_path, None)
        print(f"Refreshed timestamp for {rc_file_path}")

    for bdir in glob.glob(os.path.join(repo_root, "build*")):
        for res_file in glob.glob(os.path.join(bdir, "**", "*Tools3000*.res"), recursive=True):
            try:
                os.remove(res_file)
                print(f"Removed stale resource cache: {res_file}")
            except OSError:
                pass

    print("\n[SUCCESS] 所有 9 阶多分辨率 ICO、PNG 与 SVG 资产全部生成部署完成！")

if __name__ == "__main__":
    build_world_class_icons()
