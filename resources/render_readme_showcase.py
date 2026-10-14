import os
import asyncio
import base64
from playwright.async_api import async_playwright

async def render_banner():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    with open(os.path.join(repo_root, "VERSION"), "r", encoding="utf-8") as f:
        product_version = f.read().strip()
    logo_path = os.path.join(repo_root, "resources", "app_icon_hires.png")
    output_dir = os.path.join(repo_root, "docs", "images")
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, "about_hero_showcase.png")

    with open(logo_path, "rb") as f:
        logo_b64 = base64.b64encode(f.read()).decode()

    html_content = f"""<!DOCTYPE html>
<html>
<head>
<meta charset="utf-8">
<style>
  @import url('https://fonts.googleapis.com/css2?family=Plus+Jakarta+Sans:wght@400;500;600;700;800;900&family=JetBrains+Mono:wght@600;700&display=swap');

  * {{
    box-sizing: border-box;
    margin: 0;
    padding: 0;
  }}

  body {{
    background: transparent;
    display: flex;
    align-items: center;
    justify-content: center;
    font-family: 'Plus Jakarta Sans', -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    padding: 24px;
    width: 1400px;
    height: 680px;
    overflow: hidden;
  }}

  .grand-showcase {{
    position: relative;
    width: 100%;
    height: 100%;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    border-radius: 28px;
    background: linear-gradient(150deg, #070b14 0%, #0d1527 35%, #18233c 100%);
    overflow: hidden;
    box-shadow: 0 24px 64px rgba(0, 0, 0, 0.5), inset 0 1px 0 rgba(255, 255, 255, 0.1);
    border: 1px solid rgba(255, 255, 255, 0.08);
  }}

  .grand-showcase::before {{
    content: "";
    position: absolute;
    top: 0; left: 0; right: 0; bottom: 0;
    background-image: 
      radial-gradient(circle at 20% 45%, rgba(241, 91, 66, 0.14) 0%, transparent 50%),
      radial-gradient(circle at 80% 35%, rgba(255, 139, 98, 0.10) 0%, transparent 50%);
    pointer-events: none;
  }}

  .aura {{
    position: absolute;
    width: 380px;
    height: 380px;
    background: radial-gradient(circle, rgba(241, 91, 66, 0.28) 0%, rgba(214, 65, 50, 0.14) 50%, transparent 72%);
    filter: blur(65px);
    border-radius: 50%;
    top: 42%;
    left: 50%;
    transform: translate(-50%, -58%);
    pointer-events: none;
  }}

  .logo-container {{
    position: relative;
    z-index: 2;
    margin-bottom: 24px;
    display: flex;
    align-items: center;
    justify-content: center;
  }}

  .logo-img {{
    width: 250px;
    height: 250px;
    object-fit: contain;
    filter: drop-shadow(0 24px 44px rgba(0, 0, 0, 0.58)) drop-shadow(0 0 35px rgba(241, 91, 66, 0.25));
    display: block;
  }}

  .text-content {{
    position: relative;
    z-index: 2;
    text-align: center;
    display: flex;
    flex-direction: column;
    align-items: center;
  }}

  .brand-title {{
    font-size: 3.8rem;
    font-weight: 900;
    letter-spacing: -0.035em;
    background: linear-gradient(135deg, #ffffff 0%, #f1f5f9 45%, #94a3b8 100%);
    -webkit-background-clip: text;
    -webkit-text-fill-color: transparent;
    line-height: 1.1;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 18px;
  }}

  .version-tag {{
    font-size: 1.2rem;
    font-weight: 700;
    font-family: 'JetBrains Mono', monospace;
    background: rgba(241, 91, 66, 0.18);
    color: #ff8b62;
    border: 1px solid rgba(241, 91, 66, 0.40);
    padding: 4px 16px;
    border-radius: 999px;
    letter-spacing: 0.02em;
    box-shadow: 0 0 16px rgba(241, 91, 66, 0.25);
    -webkit-text-fill-color: #ff8b62;
  }}

  .subtitle {{
    margin-top: 12px;
    font-size: 1.2rem;
    color: #94a3b8;
    font-weight: 600;
    letter-spacing: 0.14em;
    text-transform: uppercase;
  }}
</style>
</head>
<body>
  <div class="grand-showcase">
    <div class="aura"></div>
    
    <div class="logo-container">
      <img src="data:image/png;base64,{logo_b64}" class="logo-img" alt="Tools3000 Grand Logo" />
    </div>

    <div class="text-content">
      <div class="brand-title">
        <span>Tools3000</span>
        <span class="version-tag">v{product_version}</span>
      </div>
      <p class="subtitle">Open-Source Productivity Toolkit for Windows</p>
    </div>
  </div>
</body>
</html>
"""

    temp_html = os.path.join(repo_root, "docs", "images", "temp_banner.html")
    with open(temp_html, "w", encoding="utf-8") as f:
        f.write(html_content)

    async with async_playwright() as p:
        browser = await p.chromium.launch()
        page = await browser.new_page(viewport={"width": 1400, "height": 680}, device_scale_factor=2)
        await page.goto(f"file://{temp_html}")
        await page.locator(".grand-showcase").screenshot(path=output_path, omit_background=True)
        await browser.close()

    if os.path.exists(temp_html):
        os.remove(temp_html)

    print(f"Grand Showcase Banner rendered to: {output_path}")

if __name__ == "__main__":
    asyncio.run(render_banner())
