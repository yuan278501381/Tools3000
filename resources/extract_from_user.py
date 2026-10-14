import os
import numpy as np
from PIL import Image, ImageFilter

def extract_logo_from_user_image():
    img_path = r"C:\Users\yuan2\.gemini\antigravity\brain\a4d8e3c0-7c1c-4d27-95cd-65baa13ee7a1\.user_uploaded\media_1787191968881.png"
    out_dir = r"C:\repo\tools3000\resources\branding_archive"
    os.makedirs(out_dir, exist_ok=True)
    
    img = Image.open(img_path).convert("RGBA")
    w, h = img.size
    
    # 1. Extract the large logo (approx bounding box in the top 3/4 of the image)
    # Looking at a typical mockup, the logo is roughly centered.
    # Let's find the bright pixels.
    arr = np.array(img)
    
    # Convert to grayscale to find brightness
    gray = np.dot(arr[..., :3], [0.2989, 0.5870, 0.1140])
    
    # The background is a dark blue/grey, the logo is white/light grey.
    # Let's threshold it.
    mask = gray > 150 # White logo parts
    
    # Find bounding box of the large logo
    # We ignore the bottom 20% where the tray mockup is.
    cutoff_y = int(h * 0.8)
    mask_large = mask.copy()
    mask_large[cutoff_y:, :] = False
    
    rows = np.any(mask_large, axis=1)
    cols = np.any(mask_large, axis=0)
    ymin, ymax = np.where(rows)[0][[0, -1]]
    xmin, xmax = np.where(cols)[0][[0, -1]]
    
    print(f"Large Logo BBox: {xmin}, {ymin}, {xmax}, {ymax}")
    
    # Crop the large logo with a little padding
    pad = 20
    large_crop = img.crop((max(0, xmin-pad), max(0, ymin-pad), min(w, xmax+pad), min(h, ymax+pad)))
    
    # We need to make the dark background transparent.
    # The background is dark. We can use a combination of color keying and alpha generation.
    arr_c = np.array(large_crop)
    gray_c = np.dot(arr_c[..., :3], [0.2989, 0.5870, 0.1140])
    
    # Soft alpha extraction: 
    # Background is roughly ~40-60 brightness. Logo is 150-255.
    # Let's map brightness [60, 200] to alpha [0, 255]
    alpha = np.clip((gray_c - 70) / (180 - 70) * 255, 0, 255).astype(np.uint8)
    
    # Enhance the white: where alpha is high, make RGB closer to white to remove background tint
    large_transparent = Image.fromarray(np.dstack((arr_c[..., 0], arr_c[..., 1], arr_c[..., 2], alpha)))
    
    # Save extracted large logo
    large_out = os.path.join(out_dir, "extracted_large_e.png")
    large_transparent.save(large_out)
    print(f"Saved large transparent logo to {large_out}")
    
    # 2. Extract the tray icon from the bottom mockup
    # The tray mockup is in the bottom 20%. Let's crop it out.
    mask_tray = mask.copy()
    mask_tray[:cutoff_y, :] = False
    
    # We might catch other icons (wifi, battery). 
    # The E icon is likely the leftmost bright object in the tray area.
    # Let's find connected components or just use vertical projection.
    cols_tray = np.any(mask_tray, axis=0)
    
    if np.any(cols_tray):
        # find the first true region in cols_tray
        col_indices = np.where(cols_tray)[0]
        # find jumps to separate icons
        jumps = np.where(np.diff(col_indices) > 10)[0]
        if len(jumps) > 0:
            tray_xmax = col_indices[jumps[0]]
        else:
            tray_xmax = col_indices[-1]
        tray_xmin = col_indices[0]
        
        rows_tray = np.any(mask_tray[:, tray_xmin:tray_xmax+1], axis=1)
        tray_ymin, tray_ymax = np.where(rows_tray)[0][[0, -1]]
        
        print(f"Tray Logo BBox: {tray_xmin}, {tray_ymin}, {tray_xmax}, {tray_ymax}")
        
        pad_t = 5
        tray_crop = img.crop((tray_xmin-pad_t, tray_ymin-pad_t, tray_xmax+pad_t, tray_ymax+pad_t))
        
        arr_t = np.array(tray_crop)
        gray_t = np.dot(arr_t[..., :3], [0.2989, 0.5870, 0.1140])
        alpha_t = np.clip((gray_t - 50) / (150 - 50) * 255, 0, 255).astype(np.uint8)
        
        # Pure white for tray icon
        white_t = np.full_like(arr_t[..., :3], 255)
        tray_transparent = Image.fromarray(np.dstack((white_t, alpha_t)))
        
        tray_out = os.path.join(out_dir, "extracted_tray_e.png")
        tray_transparent.save(tray_out)
        print(f"Saved tray transparent logo to {tray_out}")

if __name__ == "__main__":
    extract_logo_from_user_image()
