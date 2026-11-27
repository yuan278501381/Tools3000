#pragma once

#include <opencv2/opencv.hpp>
#include <windows.h>
#include <string>
#include <vector>

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

/// 剪贴板可贴内容探测摘要（由 ClipboardUtils 统一提供，避免多处重复开关剪贴板）
struct ClipboardContentProbe {
    std::wstring text;
    bool preferTextOverImage = false;
    bool hasImageDropFiles = false;
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
                                     int padY = 0,
                                     const std::vector<uint8_t>* preEncodedPng = nullptr);

    /**
     * @brief 判断本地文件路径扩展名是否属于受支持的可贴图片格式（不区分大小写）
     */
    static bool isSupportedImageFilePath(const std::wstring& filePath) noexcept;

    /**
     * @brief 宽路径安全解码本地图片文件（支持 PNG/JPG/WebP/BMP/TIFF/GIF/ICO/HEIC/AVIF 等，保留透明通道与 JPEG EXIF 方向）
     */
    static cv::Mat loadImageFromFile(const std::wstring& filePath);

    /**
     * @brief 探测当前系统剪贴板的文本、图片文件与图文优先级（毫秒级锁外校验，不阻塞系统剪贴板）
     */
    static ClipboardContentProbe inspectClipboardContent();

    /**
     * @brief 从系统剪贴板读取一组图像：
     * 1. 优先读取内存位图（PNG / CF_DIBV5 / CF_BITMAP），此时返回单张图像；
     * 2. 若无内存位图但存在 CF_HDROP 文件列表，则按序解码其中受支持的本地图片文件（最多 maxFiles 张）。
     *
     * @param outMeta 若剪贴板包含 Tools3000 截图元数据，填充至此
     * @param maxFiles CF_HDROP 多图模式最大解码张数上限
     * @return std::vector<cv::Mat> 解码成功的图像列表（CV_8UC3 或 CV_8UC4）
     */
    static std::vector<cv::Mat> readImagesFromClipboard(Tools3000PinMetadata* outMeta = nullptr,
                                                        size_t maxFiles = 16);

    /**
     * @brief 从系统剪贴板读取单张图像（优先读取保留透明通道的 PNG 与 CF_DIBV5，失败时回退至 CF_BITMAP 或 CF_HDROP 首张图片文件）
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
