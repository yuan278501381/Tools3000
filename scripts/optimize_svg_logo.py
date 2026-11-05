import os
import sys
import re

def simplify_curve(offsets, opacities, tol=0.005):
    """
    Ramer-Douglas-Peucker piecewise linear curve simplification for SVG gradient stops.
    Guards against degenerate inputs and division-by-zero on vertical segments.
    """
    n = len(offsets)
    if n <= 2:
        return list(range(n))

    indices = [0, n - 1]

    def rdp(i, j):
        if j <= i + 1:
            return
        x_i, y_i = offsets[i], opacities[i]
        x_j, y_j = offsets[j], opacities[j]
        dx = x_j - x_i
        dy = y_j - y_i

        max_err = 0.0
        max_k = -1
        for k in range(i + 1, j):
            if abs(dx) < 1e-9:
                interp = y_i
            else:
                interp = y_i + (offsets[k] - x_i) / dx * dy
            err = abs(opacities[k] - interp)
            if err > max_err:
                max_err = err
                max_k = k
        if max_err > tol and max_k != -1:
            indices.append(max_k)
            rdp(i, max_k)
            rdp(max_k, j)

    rdp(0, n - 1)
    indices = sorted(set(indices))
    return indices

def parse_offset(val: str) -> float:
    val = val.strip()
    if val.endswith('%'):
        return float(val[:-1]) / 100.0
    return float(val)

def format_num(v: float) -> str:
    s = f"{v:.4f}".rstrip('0').rstrip('.')
    if s == "" or s == "-0":
        return "0"
    return s

def optimize_svg(src_path: str, dst_path: str):
    if not os.path.isfile(src_path):
        raise FileNotFoundError(f"Source master SVG not found at: {src_path}")

    with open(src_path, 'r', encoding='utf-8') as f:
        content = f.read()

    # Extract defs
    defs_match = re.search(r'<defs>(.*?)</defs>', content, re.DOTALL)
    if not defs_match:
        raise ValueError("No <defs> found in SVG")

    defs_body = defs_match.group(1)

    # Extract all radial gradients
    radials = re.findall(r'<radialGradient id="([^"]+)"([^>]*)>(.*?)</radialGradient>', defs_body, re.DOTALL)

    optimized_gradients = []
    total_stops_before = 0
    total_stops_after = 0

    for grad_id, attrs, stops_text in radials:
        # Flexible stop tag matching supporting any attribute order
        stop_tags = re.findall(r'<stop\b([^>]*)/?>', stops_text)
        raw_stops = []
        for tag_attrs in stop_tags:
            off_m = re.search(r'offset="([^"]+)"', tag_attrs)
            col_m = re.search(r'stop-color="([^"]+)"', tag_attrs)
            op_m = re.search(r'stop-opacity="([^"]+)"', tag_attrs)
            if off_m and col_m:
                off_val = parse_offset(off_m.group(1))
                col_val = col_m.group(1)
                op_val = float(op_m.group(1)) if op_m else 1.0
                raw_stops.append((off_val, col_val, op_val))

        total_stops_before += len(raw_stops)
        if not raw_stops:
            optimized_gradients.append(f'<radialGradient id="{grad_id}"{attrs}></radialGradient>')
            continue

        offsets = [s[0] for s in raw_stops]
        colors = [s[1] for s in raw_stops]
        opacities = [s[2] for s in raw_stops]

        kept_indices = simplify_curve(offsets, opacities, tol=0.005)
        total_stops_after += len(kept_indices)

        stop_strs = []
        for idx in kept_indices:
            off_str = format_num(offsets[idx])
            op_str = format_num(opacities[idx])
            col_str = colors[idx]
            stop_strs.append(f'<stop offset="{off_str}" stop-color="{col_str}" stop-opacity="{op_str}"/>')

        opt_grad = f'<radialGradient id="{grad_id}"{attrs}>{"".join(stop_strs)}</radialGradient>'
        optimized_gradients.append(opt_grad)

    # Extract clipPaths from defs
    clips = re.findall(r'<clipPath[^>]*>.*?</clipPath>', defs_body, re.DOTALL)

    new_defs = f'<defs>{"".join(optimized_gradients)}{"".join(clips)}</defs>'

    # Extract body after </defs>
    body_after_defs = content[defs_match.end():]
    body_after_defs = re.sub(r'</svg>\s*$', '', body_after_defs, flags=re.DOTALL).strip()

    # Standard clean SVG root
    optimized_svg = (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1254 1254" width="100%" height="100%" fill="none">\n'
        '<title>Tools3000 Logo</title>\n'
        '<desc>High-precision vector master for Tools3000 brand ribbon.</desc>\n'
        f'{new_defs}\n'
        f'{body_after_defs}\n'
        '</svg>\n'
    )

    os.makedirs(os.path.dirname(dst_path), exist_ok=True)
    with open(dst_path, 'w', encoding='utf-8') as f:
        f.write(optimized_svg)

    orig_size = len(content.encode('utf-8'))
    opt_size = len(optimized_svg.encode('utf-8'))
    reduction_stops = (1 - total_stops_after / total_stops_before) * 100 if total_stops_before else 0
    reduction_bytes = (1 - opt_size / orig_size) * 100 if orig_size else 0
    print(f"SVG Optimization Complete:")
    print(f"  Stops: {total_stops_before} -> {total_stops_after} ({reduction_stops:.1f}% reduction)")
    print(f"  Bytes: {orig_size:,} -> {opt_size:,} ({reduction_bytes:.1f}% reduction)")
    print(f"  Saved to: {dst_path}")

if __name__ == "__main__":
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    default_src = os.path.join(repo_root, "resources", "Logosilver-ribbon.svg")
    fallback_src = os.path.join(repo_root, "dist", "Tools3000-v1.0.0-win-x64", "ui", "Logosilver-ribbon.svg")

    if os.path.isfile(default_src):
        src = default_src
    elif os.path.isfile(fallback_src):
        src = fallback_src
    else:
        src = default_src

    dst = os.path.join(repo_root, "ui", "public", "logo.svg")

    if len(sys.argv) > 1:
        src = sys.argv[1]
    if len(sys.argv) > 2:
        dst = sys.argv[2]

    optimize_svg(src, dst)
