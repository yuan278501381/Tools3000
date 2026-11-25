#include "capture/ClipboardUtils.h"
#include "capture/ScreenCapture.h"
#include "common/AtomicFile.h"
#include "core/logger/Logger.h"
#include "core/utils/WinUtils.h"
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <cwctype>
#include <string_view>
#include <vector>
#include <filesystem>
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>

namespace tools3000::capture {

namespace {
std::mutex s_metaMutex;
uint32_t s_lastCopiedSeqNumber = 0;
bool s_hasPinMeta = false;
Tools3000PinMetadata s_lastPinMeta{};
std::atomic<uint64_t> s_lastTempPruneTickMs{0};

void pruneStaleClipboardTempFiles(const std::filesystem::path& tempDir,
                                  const std::wstring& keepFilePath) noexcept {
    const uint64_t nowTick = GetTickCount64();
    uint64_t prevTick = s_lastTempPruneTickMs.load(std::memory_order_relaxed);
    if (prevTick != 0 && (nowTick - prevTick) < 60'000ULL) {
        return;
    }
    if (!s_lastTempPruneTickMs.compare_exchange_strong(prevTick, nowTick, std::memory_order_relaxed)) {
        return;
    }

    std::error_code ec;
    if (!std::filesystem::exists(tempDir, ec)) return;

    struct TempEntry {
        std::filesystem::path path;
        std::filesystem::file_time_type mtime;
    };
    std::vector<TempEntry> entries;
    const auto nowFs = std::filesystem::file_time_type::clock::now();

    for (std::filesystem::directory_iterator it(tempDir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::wstring fname = it->path().filename().wstring();
        if (fname.rfind(L"Tools3000_", 0) != 0) continue;
        if (fname.size() < 4 || fname.substr(fname.size() - 4) != L".png") continue;
        if (!keepFilePath.empty() && it->path().wstring() == keepFilePath) continue;

        const auto mtime = it->last_write_time(ec);
        if (ec) {
            ec.clear();
            continue;
        }
        if (nowFs > mtime && (nowFs - mtime) > std::chrono::hours(24)) {
            std::filesystem::remove(it->path(), ec);
            ec.clear();
            continue;
        }
        entries.push_back(TempEntry{it->path(), mtime});
    }

    constexpr std::size_t kMaxRetainedTempPngs = 32;
    if (entries.size() > kMaxRetainedTempPngs) {
        std::sort(entries.begin(), entries.end(), [](const TempEntry& a, const TempEntry& b) {
            return a.mtime > b.mtime;
        });
        for (std::size_t i = kMaxRetainedTempPngs; i < entries.size(); ++i) {
            if (nowFs > entries[i].mtime && (nowFs - entries[i].mtime) > std::chrono::minutes(2)) {
                std::filesystem::remove(entries[i].path, ec);
                ec.clear();
            }
        }
    }
}
}

bool ClipboardUtils::copyImageToClipboard(const cv::Mat& image,
                                         const std::wstring& preferredFilePath,
                                         HWND ownerHwnd,
                                         const CaptureRegion* sourceRegion,
                                         int padX,
                                         int padY) {
    if (image.empty() || image.cols <= 0 || image.rows <= 0) {
        LOG_WARN("ClipboardUtils: image buffer is empty, aborting clipboard write");
        return false;
    }

    // 1. 剪贴板宿主窗口保障：若外部传入合法窗口则绑定，否则传入 nullptr 直接与当前任务(Current Task)绑定。
    // 严禁创建临时窗口又在退出时 DestroyWindow，避免破坏 Windows 剪贴板底层句柄所有权。
    HWND safeOwner = (ownerHwnd && IsWindow(ownerHwnd)) ? ownerHwnd : nullptr;

    // 2. 在进入系统剪贴板全局锁之前预先完成 PNG 编码、像素色彩转换与 CF_HDROP 临时文件落盘，
    // 将 OpenClipboard 临界区耗时从数十毫秒缩短至亚毫秒级，彻底消除剪贴板锁竞争与重复编码开销。
    std::vector<uint8_t> pngBytes;
    try {
        if (!cv::imencode(".png", image, pngBytes)) {
            pngBytes.clear();
        }
    } catch (const cv::Exception& e) {
        LOG_ERROR("ClipboardUtils: PNG encode threw OpenCV exception: {}", e.what());
        pngBytes.clear();
    }

    // 使用 64 位无符号整数计算未压缩 DIB 尺寸，防止超长图/超大图触发 32 位有符号整数溢出 (-2GB 负向堆越界写)
    constexpr uint64_t kMaxUncompressedDibBytes = 512ULL * 1024ULL * 1024ULL; // 512 MiB 物理内存安全上限
    const uint64_t w64 = static_cast<uint64_t>(image.cols);
    const uint64_t h64 = static_cast<uint64_t>(image.rows);
    const uint64_t rowBytes24 = w64 * 3ULL;
    const uint64_t stride24 = (rowBytes24 + 3ULL) & ~3ULL; // 4 字节边界对齐
    const uint64_t dibImageBytes64 = (h64 > 0 && stride24 <= (std::numeric_limits<uint64_t>::max)() / h64)
        ? (stride24 * h64)
        : (std::numeric_limits<uint64_t>::max)();
    const uint64_t stride32 = w64 * 4ULL;
    const uint64_t dib5ImageBytes64 = (h64 > 0 && stride32 <= (std::numeric_limits<uint64_t>::max)() / h64)
        ? (stride32 * h64)
        : (std::numeric_limits<uint64_t>::max)();

    const bool allowDib24 = (dibImageBytes64 > 0 &&
                             dibImageBytes64 <= kMaxUncompressedDibBytes &&
                             dibImageBytes64 <= static_cast<uint64_t>((std::numeric_limits<DWORD>::max)()));
    const bool allowDib32 = (dib5ImageBytes64 > 0 &&
                             dib5ImageBytes64 <= kMaxUncompressedDibBytes &&
                             dib5ImageBytes64 <= static_cast<uint64_t>((std::numeric_limits<DWORD>::max)()));

    std::wstring dropFilePath = preferredFilePath;
    std::error_code ec;
    if (!pngBytes.empty() && (dropFilePath.empty() || !std::filesystem::exists(dropFilePath, ec))) {
        // 若外部未显式持久化文件，自动在应用临时目录输出高可用截图副本 (宽字符时间戳原生写盘，杜绝中文路径乱码)
        auto tempDir = tools3000::core::WinUtils::getAppDataDirectory() / L"temp";
        std::filesystem::create_directories(tempDir, ec);

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
        std::tm tm{};
        localtime_s(&tm, &in_time_t);
        wchar_t timeBuf[64];
        swprintf_s(timeBuf, L"Tools3000_%04d%02d%02d_%02d%02d%02d_%03d.png",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
        auto tempFile = tempDir / timeBuf;

        if (tools3000::common::atomicWriteBinaryFileWithFlush(tempFile.wstring(), pngBytes.data(), pngBytes.size())) {
            dropFilePath = tempFile.wstring();
        } else {
            static std::atomic<uint32_t> s_dropFallbackSeq{1};
            wchar_t fallbackBuf[80];
            swprintf_s(fallbackBuf, L"Tools3000_%04d%02d%02d_%02d%02d%02d_%03d_%u.png",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()),
                      s_dropFallbackSeq.fetch_add(1, std::memory_order_relaxed));
            auto fallbackFile = tempDir / fallbackBuf;
            if (tools3000::common::atomicWriteBinaryFileWithFlush(fallbackFile.wstring(), pngBytes.data(), pngBytes.size())) {
                dropFilePath = fallbackFile.wstring();
            }
        }
        pruneStaleClipboardTempFiles(tempDir, dropFilePath);
    }

    cv::Mat bgrMat;
    if (allowDib24) {
        try {
            if (image.channels() == 4) {
                // 对于带透明通道的截图 (圆角/阴影/外壳)，合并不透明纯白底消除黑边
                cv::Mat bg(image.size(), CV_8UC3, cv::Scalar(255, 255, 255));
                for (int y = 0; y < image.rows; ++y) {
                    const auto* srcPtr = reinterpret_cast<const cv::Vec4b*>(
                        image.data + static_cast<size_t>(y) * image.step[0]);
                    auto* dstPtr = reinterpret_cast<cv::Vec3b*>(
                        bg.data + static_cast<size_t>(y) * bg.step[0]);
                    for (int x = 0; x < image.cols; ++x) {
                        const float alpha = srcPtr[x][3] / 255.0f;
                        dstPtr[x][0] = static_cast<uint8_t>(srcPtr[x][0] * alpha + 255.0f * (1.0f - alpha));
                        dstPtr[x][1] = static_cast<uint8_t>(srcPtr[x][1] * alpha + 255.0f * (1.0f - alpha));
                        dstPtr[x][2] = static_cast<uint8_t>(srcPtr[x][2] * alpha + 255.0f * (1.0f - alpha));
                    }
                }
                bgrMat = bg;
            } else if (image.channels() == 3) {
                bgrMat = image;
            } else if (image.channels() == 1) {
                cv::cvtColor(image, bgrMat, cv::COLOR_GRAY2BGR);
            }
        } catch (const cv::Exception& e) {
            LOG_WARN("ClipboardUtils: BGR conversion skipped due to OpenCV exception: {}", e.what());
            bgrMat.release();
        }
    }

    cv::Mat bgraMat;
    if (allowDib32) {
        try {
            if (image.channels() == 4) {
                bgraMat = image;
            } else if (image.channels() == 3) {
                cv::cvtColor(image, bgraMat, cv::COLOR_BGR2BGRA);
            } else if (image.channels() == 1) {
                cv::cvtColor(image, bgraMat, cv::COLOR_GRAY2BGRA);
            }
        } catch (const cv::Exception& e) {
            LOG_WARN("ClipboardUtils: BGRA conversion skipped due to OpenCV exception: {}", e.what());
            bgraMat.release();
        }
    }

    if (pngBytes.empty() && bgrMat.empty() && bgraMat.empty()) {
        LOG_WARN("ClipboardUtils: image buffer is empty, aborting clipboard write");
        return false;
    }

    // 3. 剪贴板锁定：针对系统剪贴板监听器 (如 Win+V 剪贴板历史、微信、Office) 瞬时占用的有界退避重试；
    // 若指定宿主 HWND 在后半程持续竞争失败，穿插使用 nullptr 兜底绑定当前任务，拉满 100% 写入成功率
    bool opened = false;
    for (int retry = 0; retry < 24; ++retry) {
        HWND attemptOwner = (safeOwner && (retry < 14 || (retry % 2 == 0))) ? safeOwner : nullptr;
        if (OpenClipboard(attemptOwner)) {
            opened = true;
            break;
        }
        Sleep(8 + retry * 5);
    }

    if (!opened) {
        LOG_WARN("ClipboardUtils: failed to open clipboard after 20 retries, error={}", GetLastError());
        return false;
    }

    struct ClipboardGuard {
        ~ClipboardGuard() {
            CloseClipboard();
        }
    } clipGuard;

    if (!EmptyClipboard()) {
        LOG_WARN("ClipboardUtils: EmptyClipboard failed, error={}", GetLastError());
        return false;
    }

    bool pngSet = false;
    bool bmpSet = false;
    bool dibSet = false;
    bool dib5Set = false;
    bool dropSet = false;

    // 4. 格式 1: 写入 PNG 格式 (保留透明通道与最高画质，现代社交与协同软件首选)
    UINT formatPng = RegisterClipboardFormatW(L"PNG");
    if (formatPng != 0 && !pngBytes.empty()) {
        HGLOBAL hPngGlobal = GlobalAlloc(GMEM_MOVEABLE, pngBytes.size());
        if (hPngGlobal) {
            void* pPngMem = GlobalLock(hPngGlobal);
            if (pPngMem) {
                memcpy(pPngMem, pngBytes.data(), pngBytes.size());
                GlobalUnlock(hPngGlobal);
                if (SetClipboardData(formatPng, hPngGlobal)) {
                    pngSet = true;
                } else {
                    GlobalFree(hPngGlobal);
                }
            } else {
                GlobalFree(hPngGlobal);
            }
        }
    }

    // 5. 格式 2 & 3: 写入标准底向上 CF_DIB (24位 BGR) 与 CF_BITMAP (HBITMAP)
    // 优先写入 24 位白底合并 DIB：确保微信、Office Word 等 Win32 应用粘贴时作为无损图片展示，消除透明黑底失真
    if (allowDib24 && !bgrMat.empty()) {
        BITMAPINFOHEADER bi{};
        bi.biSize = sizeof(BITMAPINFOHEADER);
        bi.biWidth = bgrMat.cols;
        bi.biHeight = bgrMat.rows; // 正数 = 规范底向上 (Bottom-Up)
        bi.biPlanes = 1;
        bi.biBitCount = 24;
        bi.biCompression = BI_RGB;
        bi.biSizeImage = static_cast<DWORD>(dibImageBytes64);

        const size_t totalSizeDib = sizeof(BITMAPINFOHEADER) + static_cast<size_t>(dibImageBytes64);
        HGLOBAL hGlobalDib = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSizeDib);
        if (hGlobalDib) {
            auto* pMemDib = static_cast<uint8_t*>(GlobalLock(hGlobalDib));
            if (pMemDib) {
                memcpy(pMemDib, &bi, sizeof(bi));
                const int h = bgrMat.rows;
                for (int y = 0; y < h; ++y) {
                    const uint8_t* srcRow = bgrMat.data + static_cast<size_t>(h - 1 - y) * bgrMat.step[0];
                    uint8_t* dstRow = pMemDib + sizeof(bi) + static_cast<size_t>(y) * static_cast<size_t>(stride24);
                    memcpy(dstRow, srcRow, static_cast<size_t>(rowBytes24));
                }

                // CF_BITMAP (HBITMAP)
                HDC screenDc = GetDC(nullptr);
                if (screenDc) {
                    HBITMAP hBmp = CreateDIBitmap(
                        screenDc,
                        &bi,
                        CBM_INIT,
                        pMemDib + sizeof(bi),
                        reinterpret_cast<const BITMAPINFO*>(&bi),
                        DIB_RGB_COLORS
                    );
                    ReleaseDC(nullptr, screenDc);
                    if (hBmp) {
                        if (SetClipboardData(CF_BITMAP, hBmp)) {
                            bmpSet = true;
                        } else {
                            DeleteObject(hBmp);
                        }
                    }
                }

                GlobalUnlock(hGlobalDib);
                if (SetClipboardData(CF_DIB, hGlobalDib)) {
                    dibSet = true;
                } else {
                    GlobalFree(hGlobalDib);
                }
            } else {
                GlobalFree(hGlobalDib);
            }
        }
    }

    // 6. 格式 4: 写入 CF_DIBV5 (BITMAPV5HEADER 32位 BGRA 带 Alpha，支持现代图形软件)
    if (allowDib32 && !bgraMat.empty()) {
        BITMAPV5HEADER bi5{};
        bi5.bV5Size = sizeof(BITMAPV5HEADER);
        bi5.bV5Width = bgraMat.cols;
        bi5.bV5Height = bgraMat.rows; // 正数 = 规范底向上 (Bottom-Up)
        bi5.bV5Planes = 1;
        bi5.bV5BitCount = 32;
        bi5.bV5Compression = BI_BITFIELDS;
        bi5.bV5RedMask   = 0x00FF0000;
        bi5.bV5GreenMask = 0x0000FF00;
        bi5.bV5BlueMask  = 0x000000FF;
        bi5.bV5AlphaMask = 0xFF000000;
        bi5.bV5CSType    = LCS_sRGB;
        bi5.bV5Intent    = LCS_GM_IMAGES;
        bi5.bV5SizeImage = static_cast<DWORD>(dib5ImageBytes64);

        const size_t totalSize5 = sizeof(BITMAPV5HEADER) + static_cast<size_t>(dib5ImageBytes64);
        HGLOBAL hGlobal5 = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalSize5);
        if (hGlobal5) {
            auto* pMem5 = static_cast<uint8_t*>(GlobalLock(hGlobal5));
            if (pMem5) {
                memcpy(pMem5, &bi5, sizeof(bi5));
                const int h = bgraMat.rows;
                for (int y = 0; y < h; ++y) {
                    const uint8_t* srcRow = bgraMat.data + static_cast<size_t>(h - 1 - y) * bgraMat.step[0];
                    uint8_t* dstRow = pMem5 + sizeof(bi5) + static_cast<size_t>(y) * static_cast<size_t>(stride32);
                    memcpy(dstRow, srcRow, static_cast<size_t>(stride32));
                }

                GlobalUnlock(hGlobal5);
                if (SetClipboardData(CF_DIBV5, hGlobal5)) {
                    dib5Set = true;
                } else {
                    GlobalFree(hGlobal5);
                }
            } else {
                GlobalFree(hGlobal5);
            }
        }
    }

    // 7. 格式 5: 写入 CF_HDROP (文件拖拽句柄，赋予 Windows 桌面与资源管理器直接 Ctrl+V 粘贴 PNG 图片文件的原生能力)
    if (!dropFilePath.empty() && std::filesystem::exists(dropFilePath, ec)) {
        DROPFILES df{};
        df.pFiles = sizeof(DROPFILES);
        df.pt = {0, 0};
        df.fNC = FALSE;
        df.fWide = TRUE; // 采用 UTF-16 宽字符编码

        const size_t pathChars = dropFilePath.length() + 1; // 包含单个终止 \0
        const size_t stringBytes = (pathChars + 1) * sizeof(wchar_t); // 规范双 \0\0 终结
        const size_t totalBytes = sizeof(DROPFILES) + stringBytes;

        HGLOBAL hGlobalDrop = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, totalBytes);
        if (hGlobalDrop) {
            auto* pMemDrop = static_cast<uint8_t*>(GlobalLock(hGlobalDrop));
            if (pMemDrop) {
                memcpy(pMemDrop, &df, sizeof(DROPFILES));
                memcpy(pMemDrop + sizeof(DROPFILES), dropFilePath.c_str(), pathChars * sizeof(wchar_t));
                // GMEM_ZEROINIT 自动保障第二个 \0 结尾
                GlobalUnlock(hGlobalDrop);
                if (SetClipboardData(CF_HDROP, hGlobalDrop)) {
                    dropSet = true;
                } else {
                    GlobalFree(hGlobalDrop);
                }
            } else {
                GlobalFree(hGlobalDrop);
            }
        }
    }
    (void)dropSet;

    // 只要 PNG 或 CF_DIBV5 / CF_DIB / CF_BITMAP 任一核心图像格式成功写入剪贴板即视为成功，
    // 杜绝超大图跳过未压缩 DIB 或 CreateDIBitmap 单项失败连坐导致整体报错
    if (!pngSet && !dib5Set && !dibSet && !bmpSet) {
        return false;
    }

    // 7. 格式 6: 写入 Tools3000_PinMetadata (原生贴图原位还原与美化外壳 padding 逆向对齐元数据)
    UINT formatPinMeta = RegisterClipboardFormatW(L"Tools3000_PinMetadata");
    if (sourceRegion && formatPinMeta != 0) {
        Tools3000PinMetadata meta{};
        meta.magic = 0x54333030;
        meta.regionX = sourceRegion->x;
        meta.regionY = sourceRegion->y;
        meta.regionW = sourceRegion->width;
        meta.regionH = sourceRegion->height;
        meta.padX = padX;
        meta.padY = padY;
        meta.cornerRadius = sourceRegion->cornerRadius;
        meta.sequenceNumber = GetClipboardSequenceNumber();

        HGLOBAL hGlobalMeta = GlobalAlloc(GMEM_MOVEABLE, sizeof(Tools3000PinMetadata));
        if (hGlobalMeta) {
            void* pMem = GlobalLock(hGlobalMeta);
            if (pMem) {
                memcpy(pMem, &meta, sizeof(Tools3000PinMetadata));
                GlobalUnlock(hGlobalMeta);
                if (!SetClipboardData(formatPinMeta, hGlobalMeta)) {
                    GlobalFree(hGlobalMeta);
                }
            } else {
                GlobalFree(hGlobalMeta);
            }
        }
    }

    {
        std::lock_guard lock(s_metaMutex);
        s_lastCopiedSeqNumber = GetClipboardSequenceNumber();
        if (sourceRegion) {
            s_hasPinMeta = true;
            s_lastPinMeta.magic = 0x54333030;
            s_lastPinMeta.regionX = sourceRegion->x;
            s_lastPinMeta.regionY = sourceRegion->y;
            s_lastPinMeta.regionW = sourceRegion->width;
            s_lastPinMeta.regionH = sourceRegion->height;
            s_lastPinMeta.padX = padX;
            s_lastPinMeta.padY = padY;
            s_lastPinMeta.cornerRadius = sourceRegion->cornerRadius;
            s_lastPinMeta.sequenceNumber = s_lastCopiedSeqNumber;
        } else {
            s_hasPinMeta = false;
        }
    }

    LOG_INFO("ClipboardUtils: image copied to clipboard successfully, size={}x{}, file={}",
             image.cols, image.rows, tools3000::core::WinUtils::wstringToUtf8(dropFilePath));
    return true;
}

namespace {

constexpr uint64_t kMaxReadDibBytes = 512ULL * 1024ULL * 1024ULL;
constexpr uint64_t kMaxDropImageFileBytes = 64ULL * 1024ULL * 1024ULL;
constexpr int kMaxDropImageDimension = 32768;
constexpr UINT kMaxInspectDropFiles = 64u;

std::wstring extractLowerExtension(const std::wstring& filePath) noexcept {
    const size_t slashPos = filePath.find_last_of(L"\\/");
    const size_t dotPos = filePath.find_last_of(L'.');
    if (dotPos == std::wstring::npos) return {};
    if (slashPos != std::wstring::npos && dotPos < slashPos) return {};
    if (dotPos + 1 >= filePath.size()) return {};
    std::wstring ext = filePath.substr(dotPos);
    for (wchar_t& ch : ext) {
        if (ch >= L'A' && ch <= L'Z') {
            ch = static_cast<wchar_t>(ch - L'A' + L'a');
        }
    }
    return ext;
}

bool isWicPreferredExtension(std::wstring_view ext) noexcept {
    static constexpr std::array<std::wstring_view, 8> kWicPreferred = {
        L".gif", L".ico", L".heic", L".heif", L".avif", L".jxr", L".wdp", L".hdp"
    };
    for (const auto candidate : kWicPreferred) {
        if (ext == candidate) return true;
    }
    return false;
}

bool isJpegExtension(std::wstring_view ext) noexcept {
    return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".jfif";
}

bool isExistingRegularFileWithinLimit(const std::wstring& filePath, uint64_t& outSize) noexcept {
    outSize = 0;
    if (filePath.empty()) return false;
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(filePath.c_str(), GetFileExInfoStandard, &fad)) {
        return false;
    }
    if ((fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }
    const uint64_t size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) |
                          static_cast<uint64_t>(fad.nFileSizeLow);
    if (size == 0 || size > kMaxDropImageFileBytes) {
        return false;
    }
    outSize = size;
    return true;
}

cv::Mat normalizeDecodedMat(cv::Mat img) {
    if (img.empty() || img.cols <= 0 || img.rows <= 0) return {};
    if (img.cols > kMaxDropImageDimension || img.rows > kMaxDropImageDimension) return {};
    const uint64_t pixels = static_cast<uint64_t>(img.cols) * static_cast<uint64_t>(img.rows);
    if (pixels > kMaxReadDibBytes / 4ULL) return {};

    try {
        if (img.depth() == CV_16U) {
            cv::Mat tmp;
            img.convertTo(tmp, CV_8U, 1.0 / 256.0);
            img = std::move(tmp);
        } else if (img.depth() != CV_8U) {
            cv::Mat tmp;
            img.convertTo(tmp, CV_8U);
            img = std::move(tmp);
        }

        if (img.channels() == 1) {
            cv::Mat bgr;
            cv::cvtColor(img, bgr, cv::COLOR_GRAY2BGR);
            return bgr;
        }
        if (img.channels() == 3) {
            return img;
        }
        if (img.channels() == 4) {
            bool hasTransparency = false;
            for (int y = 0; y < img.rows && !hasTransparency; ++y) {
                const uint8_t* row = img.ptr<uint8_t>(y);
                for (int x = 0; x < img.cols; ++x) {
                    if (row[static_cast<size_t>(x) * 4ULL + 3ULL] != 255) {
                        hasTransparency = true;
                        break;
                    }
                }
            }
            if (hasTransparency) {
                return img;
            }
            cv::Mat bgr;
            cv::cvtColor(img, bgr, cv::COLOR_BGRA2BGR);
            return bgr;
        }
    } catch (const cv::Exception&) {
        return {};
    }
    return {};
}

cv::Mat decodeImageWithOpenCv(const std::wstring& filePath,
                              uint64_t fileSize,
                              bool preferColorWithExifOrientation) {
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return {};
    }

    std::vector<uint8_t> bytes;
    try {
        bytes.resize(static_cast<size_t>(fileSize));
    } catch (...) {
        CloseHandle(hFile);
        return {};
    }

    size_t totalRead = 0;
    while (totalRead < bytes.size()) {
        const size_t remaining = bytes.size() - totalRead;
        const DWORD chunk = static_cast<DWORD>((std::min)(remaining, static_cast<size_t>(4 * 1024 * 1024)));
        DWORD got = 0;
        if (!ReadFile(hFile, bytes.data() + totalRead, chunk, &got, nullptr)) {
            CloseHandle(hFile);
            return {};
        }
        if (got == 0) break;
        totalRead += static_cast<size_t>(got);
    }
    CloseHandle(hFile);
    if (totalRead == 0) return {};
    bytes.resize(totalRead);

    try {
        const int flags = preferColorWithExifOrientation ? cv::IMREAD_COLOR : cv::IMREAD_UNCHANGED;
        cv::Mat decoded = cv::imdecode(bytes, flags);
        return normalizeDecodedMat(std::move(decoded));
    } catch (const cv::Exception&) {
        return {};
    } catch (...) {
        return {};
    }
}

cv::Mat decodeImageWithWic(const std::wstring& filePath) {
    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninit = SUCCEEDED(hrCom);
    cv::Mat result;
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&factory))) && factory) {
            Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
            if (SUCCEEDED(factory->CreateDecoderFromFilename(
                    filePath.c_str(), nullptr, GENERIC_READ,
                    WICDecodeMetadataCacheOnDemand, &decoder)) && decoder) {
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                if (SUCCEEDED(decoder->GetFrame(0, &frame)) && frame) {
                    UINT w = 0, h = 0;
                    if (SUCCEEDED(frame->GetSize(&w, &h)) &&
                        w > 0 && h > 0 &&
                        w <= static_cast<UINT>(kMaxDropImageDimension) &&
                        h <= static_cast<UINT>(kMaxDropImageDimension) &&
                        static_cast<uint64_t>(w) * static_cast<uint64_t>(h) * 4ULL <= kMaxReadDibBytes) {
                        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
                        if (SUCCEEDED(factory->CreateFormatConverter(&converter)) && converter) {
                            if (SUCCEEDED(converter->Initialize(
                                    frame.Get(),
                                    GUID_WICPixelFormat32bppBGRA,
                                    WICBitmapDitherTypeNone,
                                    nullptr,
                                    0.0,
                                    WICBitmapPaletteTypeCustom))) {
                                try {
                                    cv::Mat bgra(static_cast<int>(h), static_cast<int>(w), CV_8UC4);
                                    const UINT stride = static_cast<UINT>(bgra.step[0]);
                                    const UINT bufSize = stride * h;
                                    if (SUCCEEDED(converter->CopyPixels(nullptr, stride, bufSize, bgra.data))) {
                                        result = normalizeDecodedMat(std::move(bgra));
                                    }
                                } catch (const cv::Exception&) {
                                    result.release();
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (shouldUninit) {
        CoUninitialize();
    }
    return result;
}

bool openClipboardWithRetry(int maxRetries) noexcept {
    for (int retry = 0; retry < maxRetries; ++retry) {
        if (OpenClipboard(nullptr)) {
            SetLastError(0);
            const UINT probeFmt = EnumClipboardFormats(0);
            if ((probeFmt == 0 && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN) ||
                (probeFmt != 0 && GetClipboardData(probeFmt) == nullptr && GetLastError() == ERROR_CLIPBOARD_NOT_OPEN)) {
                CloseClipboard();
                Sleep(10);
                continue;
            }
            return true;
        }
        Sleep(5);
    }
    return false;
}

std::vector<std::wstring> extractDropPathsFromOpenClipboard() {
    std::vector<std::wstring> paths;
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
        return paths;
    }
    HDROP hDrop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!hDrop) {
        return paths;
    }
    const UINT fileCount = DragQueryFileW(hDrop, 0xFFFFFFFFu, nullptr, 0);
    const UINT cappedCount = (std::min)(fileCount, kMaxInspectDropFiles);
    paths.reserve(cappedCount);
    for (UINT i = 0; i < cappedCount; ++i) {
        const UINT len = DragQueryFileW(hDrop, i, nullptr, 0);
        if (len == 0 || len > 32767) continue;
        std::wstring buf(static_cast<size_t>(len) + 1, L'\0');
        const UINT copied = DragQueryFileW(hDrop, i, buf.data(), len + 1);
        if (copied > 0) {
            buf.resize(copied);
            paths.push_back(std::move(buf));
        }
    }
    return paths;
}

} // namespace

bool ClipboardUtils::isSupportedImageFilePath(const std::wstring& filePath) noexcept {
    const std::wstring ext = extractLowerExtension(filePath);
    if (ext.empty()) return false;
    static constexpr std::array<std::wstring_view, 22> kSupportedImageExts = {
        L".png", L".jpg", L".jpeg", L".jpe", L".jfif",
        L".bmp", L".dib", L".webp", L".tif", L".tiff",
        L".gif", L".ico", L".heic", L".heif", L".avif",
        L".pbm", L".pgm", L".ppm", L".jp2", L".jxr",
        L".wdp", L".hdp"
    };
    for (const auto candidate : kSupportedImageExts) {
        if (ext == candidate) return true;
    }
    return false;
}

cv::Mat ClipboardUtils::loadImageFromFile(const std::wstring& filePath) {
    if (!isSupportedImageFilePath(filePath)) {
        return {};
    }
    uint64_t fileSize = 0;
    if (!isExistingRegularFileWithinLimit(filePath, fileSize)) {
        return {};
    }

    const std::wstring ext = extractLowerExtension(filePath);
    const bool wicFirst = isWicPreferredExtension(ext);
    const bool jpegExif = isJpegExtension(ext);

    if (wicFirst) {
        cv::Mat img = decodeImageWithWic(filePath);
        if (!img.empty()) return img;
        return decodeImageWithOpenCv(filePath, fileSize, false);
    }

    cv::Mat img = decodeImageWithOpenCv(filePath, fileSize, jpegExif);
    if (!img.empty()) return img;
    return decodeImageWithWic(filePath);
}

ClipboardContentProbe ClipboardUtils::inspectClipboardContent() {
    ClipboardContentProbe probe;
    std::vector<std::wstring> dropPaths;

    if (!openClipboardWithRetry(40)) {
        return probe;
    }
    {
        struct ClipboardGuard {
            ~ClipboardGuard() { CloseClipboard(); }
        } guard;

        const UINT formatPinMeta = RegisterClipboardFormatW(L"Tools3000_PinMetadata");
        const UINT formatPng = RegisterClipboardFormatW(L"PNG");
        const bool hasPinMeta = (formatPinMeta != 0 && IsClipboardFormatAvailable(formatPinMeta));

        if (!hasPinMeta) {
            for (UINT fmt = EnumClipboardFormats(0); fmt != 0; fmt = EnumClipboardFormats(fmt)) {
                if (fmt == CF_UNICODETEXT || fmt == CF_TEXT || fmt == CF_OEMTEXT) {
                    probe.preferTextOverImage = true;
                    break;
                }
                if ((formatPng != 0 && fmt == formatPng) ||
                    fmt == CF_DIBV5 || fmt == CF_DIB || fmt == CF_BITMAP) {
                    probe.preferTextOverImage = false;
                    break;
                }
            }
        }

        if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                    probe.text = p;
                    GlobalUnlock(h);
                }
            }
        }

        dropPaths = extractDropPathsFromOpenClipboard();
    }

    // 锁外快速检验 CF_HDROP 中是否包含可贴图片文件，避免阻塞系统剪贴板锁；
    // 若复制的是图片文件且第三方工具同时附带了路径文本，强制优先贴出图片而非路径文字卡片。
    for (const auto& path : dropPaths) {
        uint64_t fileSize = 0;
        if (isSupportedImageFilePath(path) && isExistingRegularFileWithinLimit(path, fileSize)) {
            probe.hasImageDropFiles = true;
            probe.preferTextOverImage = false;
            break;
        }
    }

    return probe;
}

std::vector<cv::Mat> ClipboardUtils::readImagesFromClipboard(Tools3000PinMetadata* outMeta, size_t maxFiles) {
    std::vector<cv::Mat> results;
    if (maxFiles == 0) {
        return results;
    }

    if (!openClipboardWithRetry(25)) {
        return results;
    }

    cv::Mat bitmapResult;
    std::vector<std::wstring> dropPaths;
    {
        struct ClipboardGuard {
            ~ClipboardGuard() { CloseClipboard(); }
        } guard;

        if (outMeta) {
            *outMeta = Tools3000PinMetadata{};
            outMeta->magic = 0;
            UINT formatPinMeta = RegisterClipboardFormatW(L"Tools3000_PinMetadata");
            if (formatPinMeta != 0 && IsClipboardFormatAvailable(formatPinMeta)) {
                if (HANDLE h = GetClipboardData(formatPinMeta)) {
                    if (auto* p = static_cast<const Tools3000PinMetadata*>(GlobalLock(h))) {
                        *outMeta = *p;
                        GlobalUnlock(h);
                    }
                }
            }
            if (outMeta->magic != 0x54333030) {
                std::lock_guard lock(s_metaMutex);
                if (s_hasPinMeta && GetClipboardSequenceNumber() == s_lastCopiedSeqNumber) {
                    *outMeta = s_lastPinMeta;
                }
            }
        }

        // 1. 优先读取 PNG 格式（保留完整透明通道与最高画质）
        UINT formatPng = RegisterClipboardFormatW(L"PNG");
        if (formatPng != 0 && IsClipboardFormatAvailable(formatPng)) {
            if (HANDLE h = GetClipboardData(formatPng)) {
                size_t size = GlobalSize(h);
                if (size > 0) {
                    if (auto* p = static_cast<const uint8_t*>(GlobalLock(h))) {
                        std::vector<uint8_t> buffer(p, p + size);
                        GlobalUnlock(h);
                        try {
                            bitmapResult = cv::imdecode(buffer, cv::IMREAD_UNCHANGED);
                        } catch (...) {
                            bitmapResult.release();
                        }
                    }
                }
            }
        }

        // 2. 次选读取 CF_DIBV5（32位 BGRA；若为 GDI 合成的全 0 Alpha 通道则自动转为不透明 BGR）
        if (bitmapResult.empty() && IsClipboardFormatAvailable(CF_DIBV5)) {
            if (HANDLE h = GetClipboardData(CF_DIBV5)) {
                const size_t totalSize = GlobalSize(h);
                if (totalSize >= sizeof(BITMAPV5HEADER)) {
                    if (auto* p5 = static_cast<const BITMAPV5HEADER*>(GlobalLock(h))) {
                        const int64_t w64 = p5->bV5Width;
                        const int64_t hRaw = static_cast<int64_t>(p5->bV5Height);
                        const int64_t h64 = hRaw < 0 ? -hRaw : hRaw;
                        const uint64_t stride64 = (w64 > 0) ? (static_cast<uint64_t>(w64) * 4ULL) : 0ULL;
                        const uint64_t pixelBytes64 = (w64 > 0 && h64 > 0 && stride64 <= kMaxReadDibBytes / static_cast<uint64_t>(h64))
                            ? (stride64 * static_cast<uint64_t>(h64))
                            : 0ULL;
                        size_t headerOffset = p5->bV5Size;
                        if (p5->bV5Compression == BI_BITFIELDS &&
                            p5->bV5Size == sizeof(BITMAPINFOHEADER) &&
                            pixelBytes64 > 0 &&
                            totalSize >= headerOffset + 12 + static_cast<size_t>(pixelBytes64)) {
                            headerOffset += 12;
                        }
                        if (pixelBytes64 > 0 && p5->bV5BitCount == 32 &&
                            headerOffset <= totalSize &&
                            static_cast<size_t>(pixelBytes64) <= totalSize - headerOffset) {
                            try {
                                const int w = static_cast<int>(w64);
                                const int hImg = static_cast<int>(h64);
                                const size_t stride = static_cast<size_t>(stride64);
                                const uint8_t* bits = reinterpret_cast<const uint8_t*>(p5) + headerOffset;
                                cv::Mat bgra(hImg, w, CV_8UC4);
                                const bool isTopDown = (p5->bV5Height < 0);
                                bool hasNonZeroAlpha = false;
                                for (int y = 0; y < hImg; ++y) {
                                    const int srcY = isTopDown ? y : (hImg - 1 - y);
                                    const uint8_t* srcRow = bits + static_cast<size_t>(srcY) * stride;
                                    uint8_t* dstRow = bgra.data + static_cast<size_t>(y) * bgra.step[0];
                                    memcpy(dstRow, srcRow, stride);
                                    if (!hasNonZeroAlpha) {
                                        for (int x = 0; x < w; ++x) {
                                            if (dstRow[static_cast<size_t>(x) * 4ULL + 3ULL] != 0) {
                                                hasNonZeroAlpha = true;
                                                break;
                                            }
                                        }
                                    }
                                }
                                if (hasNonZeroAlpha) {
                                    bitmapResult = bgra;
                                } else {
                                    cv::cvtColor(bgra, bitmapResult, cv::COLOR_BGRA2BGR);
                                }
                            } catch (const cv::Exception&) {
                                bitmapResult.release();
                            }
                        }
                        GlobalUnlock(h);
                    }
                }
            }
        }

        // 3. 兜底读取 CF_BITMAP
        if (bitmapResult.empty() && IsClipboardFormatAvailable(CF_BITMAP)) {
            if (HBITMAP hbm = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP))) {
                BITMAP bm{};
                if (GetObject(hbm, sizeof(bm), &bm)) {
                    const int64_t w64 = bm.bmWidth;
                    const int64_t hRaw = static_cast<int64_t>(bm.bmHeight);
                    const int64_t h64 = hRaw < 0 ? -hRaw : hRaw;
                    if (w64 > 0 && h64 > 0 &&
                        static_cast<uint64_t>(w64) * static_cast<uint64_t>(h64) * 4ULL <= kMaxReadDibBytes) {
                        try {
                            const int w = static_cast<int>(w64);
                            const int hImg = static_cast<int>(h64);
                            BITMAPINFOHEADER bi{};
                            bi.biSize = sizeof(bi);
                            bi.biWidth = w;
                            bi.biHeight = -hImg; // top-down
                            bi.biPlanes = 1;
                            bi.biBitCount = 32;
                            bi.biCompression = BI_RGB;
                            cv::Mat bgra(hImg, w, CV_8UC4);
                            HDC dc = GetDC(nullptr);
                            int got = dc ? GetDIBits(dc, hbm, 0, hImg, bgra.data, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS) : 0;
                            if (dc) ReleaseDC(nullptr, dc);
                            if (got > 0) {
                                cv::cvtColor(bgra, bitmapResult, cv::COLOR_BGRA2BGR);
                            }
                        } catch (const cv::Exception&) {
                            bitmapResult.release();
                        }
                    }
                }
            }
        }

        // 4. 若剪贴板不含内存位图，仅在锁内提取 CF_HDROP 路径数组，立即释放系统剪贴板锁后再做磁盘解码
        if (bitmapResult.empty()) {
            dropPaths = extractDropPathsFromOpenClipboard();
        }
    }

    if (!bitmapResult.empty()) {
        results.push_back(std::move(bitmapResult));
        return results;
    }

    for (const auto& path : dropPaths) {
        if (results.size() >= maxFiles) break;
        cv::Mat fileImg = loadImageFromFile(path);
        if (!fileImg.empty()) {
            LOG_INFO("ClipboardUtils: decoded CF_HDROP image file ({}x{}): {}",
                     fileImg.cols, fileImg.rows, tools3000::core::WinUtils::wstringToUtf8(path));
            results.push_back(std::move(fileImg));
        }
    }

    return results;
}

cv::Mat ClipboardUtils::readImageFromClipboard(Tools3000PinMetadata* outMeta) {
    auto images = readImagesFromClipboard(outMeta, 1);
    if (images.empty()) {
        return {};
    }
    return std::move(images[0]);
}

} // namespace tools3000::capture
