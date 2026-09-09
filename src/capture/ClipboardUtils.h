#pragma once

#include <opencv2/opencv.hpp>
#include <windows.h>
#include <string>

namespace tools3000::capture {

/**
 * @brief 世界级多格式剪贴板写入引擎 (ClipboardUtils)
 * 
 * 解决 Windows 桌面端由于单一格式导致应用兼容性受限的行业通病，
 * 采用全矩阵剪贴板格式注入架构：
 * 1. PNG: 保留透明通道与最高画质，现代社交与协同办公软件首选 (微信、QQ、钉钉、飞书、Slack、Chrome/Edge、VSCode)；
 * 2. CF_DIBV5: BITMAPV5HEADER 32 位标准 BGRA，保留透明通道与 sRGB 色彩空间，现代 Office/GDI+ 原生高保真读取；
 * 3. CF_DIB: BITMAPINFOHEADER 24 位标准底向上 (Bottom-up) DIB，自动合并不透明纯白底消除黑边，老旧系统与经典 Win32 软件 100% 兼容；
 * 4. CF_BITMAP: 设备相关位图 (HBITMAP)，为显式探测 CF_BITMAP 的系统组件与图形编辑工具兜底；
 * 5. CF_HDROP: 文件拖放结构体 (DROPFILES)，将截图图片文件句柄注入剪贴板，彻底实现用户在 Windows 桌面或资源管理器中按 Ctrl+V 直接粘贴为真实 PNG 图片文件。
 */
struct CaptureRegion;

/// Tools3000 原生截图剪贴板元数据（用于实现世界级原位贴图与美化外壳 padding 逆向对齐）
struct Tools3000PinMetadata {
    uint32_t magic = 0x54333030; // 'T300'
    int regionX = 0;
    int regionY = 0;
    int regionW = 0;
    int regionH = 0;
    int padX = 0;
    int padY = 0;
    float cornerRadius = 0.0f;
    uint32_t sequenceNumber = 0;
};

class ClipboardUtils {
public:
    /**
     * @brief 将图像写入系统剪贴板 (全矩阵多格式并发写入)
     * 
     * @param image 待复制的图像数据 (支持 CV_8UC3 BGR 与 CV_8UC4 BGRA)
     * @param preferredFilePath 已保存到本地的真实图片路径 (若为空，自动生成高可用临时文件供 CF_HDROP 桌面粘贴)
     * @param ownerHwnd 剪贴板宿主窗口句柄
     * @param sourceRegion 来源截取区域（若非空，将注入 Tools3000 原位贴图元数据）
     * @param padX 外壳横向内边距 (px)
     * @param padY 外壳纵向内边距 (px)
     * @return true 写入成功, false 写入失败
     */
    static bool copyImageToClipboard(const cv::Mat& image,
                                     const std::wstring& preferredFilePath = L"",
                                     HWND ownerHwnd = nullptr,
                                     const CaptureRegion* sourceRegion = nullptr,
                                     int padX = 0,
                                     int padY = 0);

    /**
     * @brief 从系统剪贴板读取图像（优先读取保留透明通道的 PNG 与 CF_DIBV5，失败时回退至 CF_BITMAP）
     * 
     * @param outMeta 若剪贴板包含 Tools3000 截图元数据，填充至此
     * @return cv::Mat 读取到的图像（支持 4 通道透明 BGRA 与 3 通道 BGR）
     */
    static cv::Mat readImageFromClipboard(Tools3000PinMetadata* outMeta = nullptr);
};

/// 兼容原有函数签名的内联转发接口
inline bool copyMatToClipboard(const cv::Mat& image) {
    return ClipboardUtils::copyImageToClipboard(image);
}

} // namespace tools3000::capture
