// ─────────────────────────────────────────────────────────────────────────────
// StudioPreviewWindowExport.cpp — 演播室异步文件选择、导出配置/模态窗、未保存防丢与导出工作线程
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/StudioPreviewWindowInternal.h"
#include "capture/StudioExportWorker.h"
#include "core/i18n/UiI18n.h"
#include "capture/StudioBackdropCatalog.h"
#include "../resource.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/DWriteFontUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/OutOfProcessFileDialog.h"
#include "core/utils/WinUtils.h"
#include "ui/KeyboardPipeline.h"

#include <opencv2/imgproc.hpp>
#include <d2d1_1.h>
#include <dwmapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <set>

namespace tools3000::capture {

using Microsoft::WRL::ComPtr;
using namespace studio_detail;

void StudioPreviewWindow::joinFileDialogWorkerIfDone(bool wait) {
    std::thread toJoin;
    {
        std::lock_guard lock(m_fileDialogMutex);
        if (m_fileDialogWorker.joinable() && (wait || !m_fileDialogInFlight.load(std::memory_order_acquire))) {
            if (std::this_thread::get_id() != m_fileDialogWorker.get_id()) {
                toJoin = std::move(m_fileDialogWorker);
            }
        }
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

void StudioPreviewWindow::browseCustomWallpaperImageAsync() {
    if (m_fileDialogInFlight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    joinFileDialogWorkerIfDone(true);

    const HWND targetHwnd = m_hwnd;
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    std::lock_guard lock(m_fileDialogMutex);
    m_fileDialogWorker = std::thread([this, targetHwnd, isZh]() {
        tools3000::core::FileDialogRequest req;
        req.mode = tools3000::core::FileDialogMode::OpenFile;
        req.title = tools3000::core::trWCstrFor(isZh, L"选择自定义背景图片", L"Select Custom Background Image");
        req.filters = {
            {tools3000::core::trWCstrFor(isZh, L"图片文件 (*.jpg;*.jpeg;*.png;*.webp;*.bmp)", L"Image Files (*.jpg;*.jpeg;*.png;*.webp;*.bmp)"),
             L"*.jpg;*.jpeg;*.png;*.webp;*.bmp"},
            {tools3000::core::trWCstrFor(isZh, L"所有文件 (*.*)", L"All Files (*.*)"), L"*.*"},
        };
        const auto dlgRes = tools3000::core::OutOfProcessFileDialog::show(req);
        if (dlgRes.accepted && !dlgRes.selectedPath.empty()) {
            std::lock_guard dLock(m_fileDialogMutex);
            m_pendingCustomBgWide = dlgRes.selectedPath.wstring();
        }
        m_fileDialogInFlight.store(false, std::memory_order_release);
        if (targetHwnd && IsWindow(targetHwnd)) {
            PostMessageW(targetHwnd, WM_STUDIO_CUSTOM_BG_PICKED, 0, 0);
        }
    });
}

void StudioPreviewWindow::browseExportDirectoryAsync() {
    if (m_fileDialogInFlight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    joinFileDialogWorkerIfDone(true);

    const HWND targetHwnd = m_hwnd;
    const std::wstring initDir = m_exportDirectoryWide;
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    std::lock_guard lock(m_fileDialogMutex);
    m_fileDialogWorker = std::thread([this, targetHwnd, initDir, isZh]() {
        tools3000::core::FileDialogRequest req;
        req.mode = tools3000::core::FileDialogMode::PickFolder;
        req.title = tools3000::core::trWCstrFor(isZh, L"选择导出保存目录", L"Select Export Directory");
        if (!initDir.empty()) {
            req.initialDirectory = std::filesystem::path(initDir);
        }
        const auto dlgRes = tools3000::core::OutOfProcessFileDialog::show(req);
        if (dlgRes.accepted && !dlgRes.selectedPath.empty()) {
            std::lock_guard dLock(m_fileDialogMutex);
            m_pendingExportDirWide = dlgRes.selectedPath.wstring();
        }
        m_fileDialogInFlight.store(false, std::memory_order_release);
        if (targetHwnd && IsWindow(targetHwnd)) {
            PostMessageW(targetHwnd, WM_STUDIO_EXPORT_DIR_PICKED, 0, 0);
        }
    });
}

void StudioPreviewWindow::onCustomWallpaperPicked() {
    std::wstring picked;
    {
        std::lock_guard lock(m_fileDialogMutex);
        picked = std::move(m_pendingCustomBgWide);
        m_pendingCustomBgWide.clear();
    }
    joinFileDialogWorkerIfDone(false);
    if (picked.empty()) return;

    const std::string utf8 = tools3000::core::WinUtils::wstringToUtf8(picked);
    m_config.customBackgroundPath = utf8;
    m_config.autoFrameEnabled = true;

    bool exists = false;
    for (const auto& w : m_allWallpapers) {
        if (!w.isBuiltin && w.filePathUtf8 == utf8) {
            exists = true;
            break;
        }
    }
    if (!exists) {
        WallpaperEntry customEntry{};
        customEntry.isBuiltin = false;
        customEntry.filePathUtf8 = utf8;
        customEntry.displayName = std::filesystem::path(picked).stem().wstring();
        customEntry.category = 5;
        m_allWallpapers.push_back(std::move(customEntry));
    }
    m_wallpaperCategory = 5;
    m_wallpaperPage = 0;
    rebuildFilteredWallpapers();
    updateCurrentComposedBitmap();
    if (m_hwnd) {
        SetForegroundWindow(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::onExportDirectoryPicked() {
    std::wstring picked;
    {
        std::lock_guard lock(m_fileDialogMutex);
        picked = std::move(m_pendingExportDirWide);
        m_pendingExportDirWide.clear();
    }
    joinFileDialogWorkerIfDone(false);
    if (!picked.empty()) {
        m_exportDirectoryWide = std::move(picked);
    }
    if (m_hwnd) {
        SetForegroundWindow(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::browseWatermarkLogoImageAsync() {
    if (m_fileDialogInFlight.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    joinFileDialogWorkerIfDone(true);

    const HWND targetHwnd = m_hwnd;
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();

    std::lock_guard lock(m_fileDialogMutex);
    m_fileDialogWorker = std::thread([this, targetHwnd, isZh]() {
        tools3000::core::FileDialogRequest req;
        req.mode = tools3000::core::FileDialogMode::OpenFile;
        req.title = tools3000::core::trWCstrFor(isZh, L"导入水印图片 / 企业 Logo", L"Import Watermark Logo Image");
        req.filters = {
            {tools3000::core::trWCstrFor(isZh, L"透明图片与 Logo (*.png;*.webp;*.jpg;*.jpeg;*.bmp)", L"Logo & Images (*.png;*.webp;*.jpg;*.jpeg;*.bmp)"),
             L"*.png;*.webp;*.jpg;*.jpeg;*.bmp"},
            {tools3000::core::trWCstrFor(isZh, L"所有文件 (*.*)", L"All Files (*.*)"), L"*.*"},
        };
        const auto dlgRes = tools3000::core::OutOfProcessFileDialog::show(req);
        if (dlgRes.accepted && !dlgRes.selectedPath.empty()) {
            std::lock_guard dLock(m_fileDialogMutex);
            m_pendingWatermarkImgWide = dlgRes.selectedPath.wstring();
        }
        m_fileDialogInFlight.store(false, std::memory_order_release);
        if (targetHwnd && IsWindow(targetHwnd)) {
            PostMessageW(targetHwnd, WM_STUDIO_WATERMARK_IMG_PICKED, 0, 0);
        }
    });
}

void StudioPreviewWindow::onWatermarkLogoPicked() {
    std::wstring picked;
    {
        std::lock_guard lock(m_fileDialogMutex);
        picked = std::move(m_pendingWatermarkImgWide);
        m_pendingWatermarkImgWide.clear();
    }
    joinFileDialogWorkerIfDone(false);
    if (picked.empty()) return;

    const std::string utf8 = tools3000::core::WinUtils::wstringToUtf8(picked);
    m_config.watermark.imagePathUtf8 = utf8;
    m_config.watermark.mode = WatermarkMode::ImageLogo;
    m_config.watermark.enabled = true;
    if (m_config.watermark.opacity < 0.35f) {
        m_config.watermark.opacity = 0.88f;
    }
    WatermarkRenderer::pushStudioRecentLogoPath(utf8);
    WatermarkRenderer::saveStudioWatermarkConfig(m_config.watermark);
    m_recentLogoPaths = WatermarkRenderer::loadStudioRecentLogoPaths();
    m_recentLogoThumbsDirty = true;

    updateCurrentComposedBitmap();
    if (m_hwnd) {
        SetForegroundWindow(m_hwnd);
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::initExportModalDefaults() {
    m_exportModalOpen = false;
    m_exportCompletedCard = false;
    m_closeConfirmModalOpen = false;
    m_closeAfterSaveExport = false;
    m_confirmSkipStudioNextTime = false;
    m_editingExportName = false;
    m_userEditedExportStem = false;
    m_openDropdown = ExportDropdownKind::None;
    m_sessionExportedPathsLower.clear();
    m_lastExportedFilePath.clear();
    m_lastExportedFileBytes = 0;
    m_lastExportedSpecLine.clear();
    m_copyFeedbackUntilTick = 0;

    const std::filesystem::path rawFsPath(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath));
    m_exportFileNameStem = rawFsPath.stem().wstring();
    if (m_exportFileNameStem.empty()) {
        m_exportFileNameStem = L"Tools3000_Studio";
    }
    m_initialFileNameStem = m_exportFileNameStem;
    const std::wstring rawParent = rawFsPath.parent_path().wstring();

    auto& cm = tools3000::core::ConfigManager::instance();
    m_saveExportPrefs = cm.get<bool>("/recording/studioExport/savePrefs", true);
    m_autoCloseAfterExport = cm.get<bool>("/recording/studioExport/autoClose", false);
    const std::string savedDirUtf8 = cm.get<std::string>("/recording/studioExport/directory", "");
    if (!savedDirUtf8.empty()) {
        const std::wstring savedDirW = tools3000::core::WinUtils::utf8ToWstring(savedDirUtf8);
        std::error_code ec;
        if (std::filesystem::is_directory(savedDirW, ec) && !ec) {
            m_exportDirectoryWide = savedDirW;
        }
    }
    if (m_exportDirectoryWide.empty()) {
        m_exportDirectoryWide = rawParent;
    }

    const int fmtInt = std::clamp(cm.get<int>("/recording/studioExport/format", 0), 0, 5);
    const int resInt = std::clamp(cm.get<int>("/recording/studioExport/resolution", 3), 0, 6);
    const int qualInt = std::clamp(cm.get<int>("/recording/studioExport/quality", 0), 0, 2);
    const int savedFps = cm.get<int>("/recording/studioExport/fps", 30);

    m_exportOpts.format = static_cast<StudioExportFormat>(fmtInt);
    m_exportOpts.resolution = static_cast<StudioExportResolution>(resInt);
    m_exportOpts.quality = static_cast<StudioExportQuality>(qualInt);
    m_exportOpts.fps = std::clamp(savedFps, 10, 60);
    m_exportOpts.baseBitrateMbps = m_bitrateMbps;

    if (m_exportOpts.format == StudioExportFormat::GIF_WeChat ||
        m_exportOpts.format == StudioExportFormat::GIF_HD) {
        if (m_exportOpts.fps > 15) m_exportOpts.fps = 12;
    }
}

void StudioPreviewWindow::applyFormatSmartDefaults(StudioExportFormat newFmt) {
    m_exportOpts.format = newFmt;
    if (newFmt == StudioExportFormat::GIF_WeChat) {
        m_exportOpts.resolution = StudioExportResolution::Res640P;
        m_exportOpts.fps = 12;
        m_exportOpts.quality = StudioExportQuality::Recommended;
    } else if (newFmt == StudioExportFormat::GIF_HD) {
        // 默认 1080P (长边 >= 1080px > 微信 1000px 表情阈值)，发到微信按普通高清图片展示，支持双击打开大图查看器放大
        m_exportOpts.resolution = StudioExportResolution::Res1080P;
        m_exportOpts.fps = 15;
    } else if (newFmt == StudioExportFormat::WebP_Animated) {
        if (m_exportOpts.resolution == StudioExportResolution::Res640P ||
            m_exportOpts.resolution == StudioExportResolution::Res480P) {
            m_exportOpts.resolution = StudioExportResolution::Res1080P;
        }
        m_exportOpts.fps = 24;
    } else {
        if (m_exportOpts.resolution == StudioExportResolution::Res640P ||
            m_exportOpts.resolution == StudioExportResolution::Res480P) {
            m_exportOpts.resolution = StudioExportResolution::Res1080P;
        }
        if (m_exportOpts.fps < 24) {
            m_exportOpts.fps = 30;
        }
    }
    ensureNonCollidingExportStem();
}

static std::wstring normalizeLowerFsKey(const std::filesystem::path& p) {
    std::error_code ec;
    std::wstring s = std::filesystem::weakly_canonical(p, ec).wstring();
    if (ec || s.empty()) {
        s = p.lexically_normal().wstring();
    }
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) {
        return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
}

void StudioPreviewWindow::ensureNonCollidingExportStem() {
    if (m_userEditedExportStem || m_sessionExportedPathsLower.empty()) {
        return;
    }
    const std::wstring baseStem = m_initialFileNameStem.empty() ? L"Tools3000_Studio" : m_initialFileNameStem;
    const std::wstring extW = tools3000::core::WinUtils::utf8ToWstring(
        StudioVideoCompositor::extensionForExportFormat(m_exportOpts.format));
    const std::filesystem::path dirPath = m_exportDirectoryWide.empty()
        ? std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath)).parent_path()
        : std::filesystem::path(m_exportDirectoryWide);

    auto wasAlreadyExportedInSession = [&](const std::wstring& candidateStem) -> bool {
        const std::wstring key = normalizeLowerFsKey(dirPath / (candidateStem + extW));
        return std::find(m_sessionExportedPathsLower.begin(), m_sessionExportedPathsLower.end(), key) !=
               m_sessionExportedPathsLower.end();
    };

    if (!wasAlreadyExportedInSession(baseStem)) {
        m_exportFileNameStem = baseStem;
        return;
    }
    for (int suffix = 2; suffix <= 99; ++suffix) {
        const std::wstring cand = baseStem + L"_" + std::to_wstring(suffix);
        if (!wasAlreadyExportedInSession(cand)) {
            m_exportFileNameStem = cand;
            return;
        }
    }
}

void StudioPreviewWindow::persistExportPreferencesIfChecked() {
    auto& cm = tools3000::core::ConfigManager::instance();
    cm.set<bool>("/recording/studioExport/savePrefs", m_saveExportPrefs);
    cm.set<bool>("/recording/studioExport/autoClose", m_autoCloseAfterExport);
    if (!m_saveExportPrefs) return;

    cm.set<int>("/recording/studioExport/format", static_cast<int>(m_exportOpts.format));
    cm.set<int>("/recording/studioExport/resolution", static_cast<int>(m_exportOpts.resolution));
    cm.set<int>("/recording/studioExport/fps", m_exportOpts.fps);
    cm.set<int>("/recording/studioExport/quality", static_cast<int>(m_exportOpts.quality));
    if (!m_exportDirectoryWide.empty()) {
        cm.set<std::string>("/recording/studioExport/directory",
                            tools3000::core::WinUtils::wstringToUtf8(m_exportDirectoryWide));
    }
}

std::string StudioPreviewWindow::buildFinalExportPathUtf8() const {
    std::wstring cleanStem;
    for (wchar_t ch : m_exportFileNameStem) {
        if (isValidFileNameChar(ch)) cleanStem.push_back(ch);
    }
    while (!cleanStem.empty() && (cleanStem.back() == L' ' || cleanStem.back() == L'.')) {
        cleanStem.pop_back();
    }
    if (cleanStem.empty()) {
        cleanStem = L"Tools3000_Studio";
    }

    const std::wstring extW = tools3000::core::WinUtils::utf8ToWstring(
        StudioVideoCompositor::extensionForExportFormat(m_exportOpts.format));
    std::filesystem::path dirPath = m_exportDirectoryWide.empty()
        ? std::filesystem::path(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath)).parent_path()
        : std::filesystem::path(m_exportDirectoryWide);
    const std::filesystem::path fullPath = dirPath / (cleanStem + extW);
    return tools3000::core::WinUtils::wstringToUtf8(fullPath.wstring());
}

void StudioPreviewWindow::renderExportModalOverlay(float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    m_hitZones.clear();

    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colModalBg = dark
        ? D2D1::ColorF(0.082f, 0.094f, 0.133f, 0.985f)
        : D2D1::ColorF(0.992f, 0.995f, 1.000f, 0.990f);
    const D2D1_COLOR_F colFieldBg = dark
        ? D2D1::ColorF(0.118f, 0.137f, 0.192f, 1.0f)
        : D2D1::ColorF(0.949f, 0.961f, 0.980f, 1.0f);
    const D2D1_COLOR_F colBorder = dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.14f);
    const D2D1_COLOR_F colPrimary = dark
        ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark
        ? D2D1::ColorF(0.64f, 0.70f, 0.80f, 1.0f)
        : D2D1::ColorF(0.35f, 0.41f, 0.51f, 1.0f);

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const bool exporting = m_exporting.load(std::memory_order_acquire);

    const D2D1_RECT_F fullRc = D2D1::RectF(0.0f, 0.0f, W, H);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalBackdrop, fullRc, -1});
    }
    setBrush(D2D1::ColorF(0.02f, 0.03f, 0.06f, dark ? 0.68f : 0.45f));
    m_renderTarget->FillRectangle(fullRc, brush);

    const float cardW = 556.0f;
    const float cardH = (!exporting && m_exportCompletedCard) ? 350.0f : 468.0f;
    const float cardL = (W - cardW) * 0.5f;
    const float cardT = (H - cardH) * 0.5f;
    const D2D1_RECT_F cardRc = D2D1::RectF(cardL, cardT, cardL + cardW, cardT + cardH);
    m_hitZones.push_back({Hit_ModalCardBody, cardRc, -1});

    setBrush(colModalBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush, 1.2f);

    const D2D1_RECT_F mCloseRc = D2D1::RectF(cardRc.right - 40.0f, cardT + 14.0f, cardRc.right - 14.0f, cardT + 40.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalCloseBtn, mCloseRc, -1});
    }
    setBrush(m_hoveredHitId == Hit_ModalCloseBtn ? D2D1::ColorF(0.94f, 0.27f, 0.27f, 1.0f) : colSecondary);
    const float mcx = (mCloseRc.left + mCloseRc.right) * 0.5f;
    const float mcy = (mCloseRc.top + mCloseRc.bottom) * 0.5f;
    m_renderTarget->DrawLine({mcx - 4.5f, mcy - 4.5f}, {mcx + 4.5f, mcy + 4.5f}, brush, 1.6f);
    m_renderTarget->DrawLine({mcx + 4.5f, mcy - 4.5f}, {mcx - 4.5f, mcy + 4.5f}, brush, 1.6f);

    if (!exporting && m_exportCompletedCard) {
        const float inL = cardL + 26.0f;
        const float inR = cardRc.right - 26.0f;

        // 顶部翠绿矢量对勾圆徽 + 导出成功标题
        const D2D1_POINT_2F badgePt{cardL + cardW * 0.5f - 54.0f, cardT + 31.0f};
        setBrush(D2D1::ColorF(0.13f, 0.77f, 0.44f, dark ? 0.20f : 0.14f));
        m_renderTarget->FillEllipse(D2D1::Ellipse(badgePt, 11.5f, 11.5f), brush);
        setBrush(D2D1::ColorF(0.13f, 0.77f, 0.44f, 1.0f));
        m_renderTarget->DrawLine({badgePt.x - 4.4f, badgePt.y + 0.2f}, {badgePt.x - 1.2f, badgePt.y + 3.6f}, brush, 2.0f);
        m_renderTarget->DrawLine({badgePt.x - 1.2f, badgePt.y + 3.6f}, {badgePt.x + 5.0f, badgePt.y - 3.2f}, brush, 2.0f);

        drawTxt(tools3000::core::trWCstrFor(isZh, L"导出完成", L"Export Completed"), m_titleFmt.Get(),
                D2D1::RectF(badgePt.x + 17.0f, cardT + 14.0f, cardRc.right - 46.0f, cardT + 48.0f),
                colPrimary, DWRITE_TEXT_ALIGNMENT_LEADING);

        // 中部真实落盘文件指标微晶信息舱
        const std::filesystem::path expFs(tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath));
        const std::wstring fileNameW = expFs.filename().wstring();
        const std::wstring parentDirW = expFs.parent_path().wstring();
        const std::wstring sizeW = formatEstimatedSize(m_lastExportedFileBytes);

        const D2D1_RECT_F infoBox = D2D1::RectF(inL, cardT + 58.0f, inR, cardT + 150.0f);
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(infoBox, 10.0f, 10.0f), brush);
        setBrush(colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(infoBox, 10.0f, 10.0f), brush, 1.0f);

        // 体积微晶胶囊徽章
        const D2D1_RECT_F sizePill = D2D1::RectF(infoBox.right - 92.0f, infoBox.top + 11.0f, infoBox.right - 12.0f, infoBox.top + 35.0f);
        setBrush(D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.22f : 0.12f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(sizePill, 6.0f, 6.0f), brush);
        drawTxt(sizeW, m_monoFmt.Get(), sizePill, colAccent, DWRITE_TEXT_ALIGNMENT_CENTER);

        drawTxt(fileNameW, m_sectionFmt.Get(),
                D2D1::RectF(infoBox.left + 14.0f, infoBox.top + 10.0f, sizePill.left - 8.0f, infoBox.top + 36.0f),
                colPrimary);
        drawTxt(m_lastExportedSpecLine, m_bodyFmt.Get(),
                D2D1::RectF(infoBox.left + 14.0f, infoBox.top + 39.0f, infoBox.right - 14.0f, infoBox.top + 63.0f),
                colSecondary);
        drawTxt(parentDirW, m_captionFmt.Get(),
                D2D1::RectF(infoBox.left + 14.0f, infoBox.top + 64.0f, infoBox.right - 14.0f, infoBox.top + 86.0f),
                colSecondary);

        // 快捷文件操作栏：复制文件 / 直接打开预览 / 打开所在文件夹
        const float actTop = cardT + 166.0f;
        const float actH = 38.0f;
        const float actGap = 10.0f;
        const float actW = (inR - inL - actGap * 2.0f) / 3.0f;
        const bool copiedNow = (GetTickCount64() < m_copyFeedbackUntilTick);

        auto drawActionChip = [&](int hitId, int idx, const std::wstring& label, bool highlightGreen = false) {
            const float bx = inL + static_cast<float>(idx) * (actW + actGap);
            const D2D1_RECT_F brc = D2D1::RectF(bx, actTop, bx + actW, actTop + actH);
            m_hitZones.push_back({hitId, brc, -1});
            const bool hov = (m_hoveredHitId == hitId);
            if (highlightGreen) {
                setBrush(D2D1::ColorF(0.13f, 0.77f, 0.44f, dark ? 0.24f : 0.16f));
            } else if (hov) {
                setBrush(D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.22f : 0.12f));
            } else {
                setBrush(colFieldBg);
            }
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(brc, 8.0f, 8.0f), brush);
            setBrush(highlightGreen ? D2D1::ColorF(0.13f, 0.77f, 0.44f, 0.85f) : (hov ? colAccent : colBorder));
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(brc, 8.0f, 8.0f), brush, 1.0f);
            drawTxt(label, m_bodyFmt.Get(), brc,
                    highlightGreen ? D2D1::ColorF(0.16f, 0.82f, 0.48f, 1.0f) : colPrimary,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        };

        drawActionChip(
            Hit_ModalResultCopyFileBtn, 0,
            copiedNow ? (tools3000::core::trWCstrFor(isZh, L"已复制到剪贴板", L"Copied")) : (tools3000::core::trWCstrFor(isZh, L"复制文件 (Ctrl+C)", L"Copy File")),
            copiedNow);
        drawActionChip(Hit_ModalResultOpenPlayBtn, 1, tools3000::core::trWCstrFor(isZh, L"打开预览", L"Open File"));
        drawActionChip(Hit_ModalResultRevealBtn, 2, tools3000::core::trWCstrFor(isZh, L"打开所在位置", L"Show in Folder"));

        // 提示小字：告知可随时点遮罩回画布微调，或继续导出其他格式对比（支持双行折行，零溢出）
        drawTxt(tools3000::core::trWCstrFor(isZh, L"演播室母带保持无损就绪：可继续导出 GIF / 其他参数对比，或点击外侧遮罩回画布微调", L"Master recording stays intact: export another format (e.g. GIF) or click backdrop to tweak canvas"),
                m_captionWrapFmt.Get(),
                D2D1::RectF(inL, cardT + 214.0f, inR, cardT + 250.0f),
                colSecondary, DWRITE_TEXT_ALIGNMENT_CENTER);

        // 底部主次双决策按钮：[继续导出其他格式]  |  [完成并关闭]
        const float botTop = cardT + 262.0f;
        const float botH = 40.0f;
        const float botGap = 12.0f;
        const float botHalfW = (inR - inL - botGap) * 0.5f;
        const D2D1_RECT_F moreBtnRc = D2D1::RectF(inL, botTop, inL + botHalfW, botTop + botH);
        const D2D1_RECT_F doneBtnRc = D2D1::RectF(inL + botHalfW + botGap, botTop, inR, botTop + botH);
        m_hitZones.push_back({Hit_ModalResultExportMoreBtn, moreBtnRc, -1});
        m_hitZones.push_back({Hit_ModalResultFinishCloseBtn, doneBtnRc, -1});

        setBrush(m_hoveredHitId == Hit_ModalResultExportMoreBtn
            ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.20f : 0.12f)
            : colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(moreBtnRc, 8.0f, 8.0f), brush);
        setBrush(m_hoveredHitId == Hit_ModalResultExportMoreBtn ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(moreBtnRc, 8.0f, 8.0f), brush, 1.2f);
        drawTxt(tools3000::core::trWCstrFor(isZh, L"继续导出其他格式", L"Export Another Format"),
                m_sectionFmt.Get(), moreBtnRc, colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

        setBrush(m_hoveredHitId == Hit_ModalResultFinishCloseBtn
            ? D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.07f), (std::min)(1.0f, accentRgb.g + 0.07f), (std::min)(1.0f, accentRgb.b + 0.07f), 1.0f)
            : colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(doneBtnRc, 8.0f, 8.0f), brush);
        drawTxt(tools3000::core::trWCstrFor(isZh, L"完成并关闭 (Enter)", L"Done & Close (Enter)"),
                m_sectionFmt.Get(), doneBtnRc, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
        return;
    }

    drawTxt(tools3000::core::trWCstrFor(isZh, L"导出", L"Export"), m_titleFmt.Get(),
            D2D1::RectF(cardL + 24.0f, cardT + 14.0f, cardRc.right - 24.0f, cardT + 48.0f),
            colPrimary, DWRITE_TEXT_ALIGNMENT_CENTER);

    const float inL = cardL + 26.0f;
    const float inR = cardRc.right - 26.0f;
    float y = cardT + 56.0f;

    drawTxt(tools3000::core::trWCstrFor(isZh, L"名称", L"Name"), m_captionFmt.Get(), D2D1::RectF(inL, y, inR, y + 20.0f), colSecondary);
    y += 22.0f;
    const D2D1_RECT_F nameBox = D2D1::RectF(inL, y, inR, y + 38.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalNameInput, nameBox, -1});
    }
    setBrush(colFieldBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(nameBox, 8.0f, 8.0f), brush);
    setBrush(m_editingExportName ? colAccent : colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(nameBox, 8.0f, 8.0f), brush, m_editingExportName ? 1.5f : 1.0f);

    const std::wstring extHint = tools3000::core::WinUtils::utf8ToWstring(
        StudioVideoCompositor::extensionForExportFormat(m_exportOpts.format));
    std::wstring nameShown = m_exportFileNameStem;
    if (m_editingExportName && ((GetTickCount() / 500) % 2 == 0)) {
        nameShown.push_back(L'|');
    }
    drawTxt(nameShown, m_bodyFmt.Get(), D2D1::RectF(inL + 12.0f, y, inR - 56.0f, y + 38.0f), colPrimary);
    drawTxt(extHint, m_monoFmt.Get(), D2D1::RectF(inR - 54.0f, y, inR - 12.0f, y + 38.0f), colSecondary, DWRITE_TEXT_ALIGNMENT_TRAILING);

    y += 48.0f;
    drawTxt(tools3000::core::trWCstrFor(isZh, L"保存至", L"Save To"), m_captionFmt.Get(), D2D1::RectF(inL, y, inR, y + 20.0f), colSecondary);
    y += 22.0f;
    const D2D1_RECT_F dirBox = D2D1::RectF(inL, y, inR, y + 38.0f);
    const D2D1_RECT_F dirBtn = D2D1::RectF(inR - 40.0f, y + 2.0f, inR - 2.0f, y + 36.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalBrowseDirBtn, dirBox, -1});
    }
    setBrush(colFieldBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(dirBox, 8.0f, 8.0f), brush);
    setBrush(m_hoveredHitId == Hit_ModalBrowseDirBtn ? colAccent : colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(dirBox, 8.0f, 8.0f), brush, 1.0f);
    drawTxt(m_exportDirectoryWide, m_bodyFmt.Get(), D2D1::RectF(inL + 12.0f, y, inR - 42.0f, y + 38.0f), colPrimary);
    setBrush(m_hoveredHitId == Hit_ModalBrowseDirBtn ? colAccent : colSecondary);
    drawFolderIcon(m_renderTarget.Get(), brush, (dirBtn.left + dirBtn.right) * 0.5f, (dirBtn.top + dirBtn.bottom) * 0.5f);

    y += 50.0f;
    const float colGap = 14.0f;
    const float halfW = (inR - inL - colGap) * 0.5f;
    const float rightColL = inL + halfW + colGap;

    auto drawSelectBox = [&](int hitId, float bx, float by, const std::wstring& title, const std::wstring& val, bool activeOpen) -> D2D1_RECT_F {
        drawTxt(title, m_captionFmt.Get(), D2D1::RectF(bx, by, bx + halfW, by + 20.0f), colSecondary);
        const D2D1_RECT_F box = D2D1::RectF(bx, by + 22.0f, bx + halfW, by + 60.0f);
        if (!exporting) {
            m_hitZones.push_back({hitId, box, -1});
        }
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(box, 8.0f, 8.0f), brush);
        setBrush((activeOpen || m_hoveredHitId == hitId) ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(box, 8.0f, 8.0f), brush, activeOpen ? 1.5f : 1.0f);
        drawTxt(val, m_bodyFmt.Get(), D2D1::RectF(box.left + 12.0f, box.top, box.right - 28.0f, box.bottom), colPrimary);
        setBrush(colSecondary);
        drawChevronDown(m_renderTarget.Get(), brush, box.right - 15.0f, (box.top + box.bottom) * 0.5f);
        return box;
    };

    const D2D1_RECT_F fmtBox = drawSelectBox(
        Hit_ModalFormatSelect, inL, y, tools3000::core::trWCstrFor(isZh, L"格式", L"Format"),
        formatLabel(m_exportOpts.format, isZh), m_openDropdown == ExportDropdownKind::Format);
    const D2D1_RECT_F resBox = drawSelectBox(
        Hit_ModalResSelect, rightColL, y, tools3000::core::trWCstrFor(isZh, L"分辨率", L"Resolution"),
        resolutionLabel(m_exportOpts.resolution, isZh), m_openDropdown == ExportDropdownKind::Resolution);

    y += 72.0f;
    wchar_t fpsTxt[32]{};
    std::swprintf(fpsTxt, std::size(fpsTxt), L"%d FPS", m_exportOpts.fps);
    const D2D1_RECT_F fpsBox = drawSelectBox(
        Hit_ModalFpsSelect, inL, y, tools3000::core::trWCstrFor(isZh, L"帧率", L"Frame Rate"),
        fpsTxt, m_openDropdown == ExportDropdownKind::Fps);
    const D2D1_RECT_F qualBox = drawSelectBox(
        Hit_ModalQualitySelect, rightColL, y, tools3000::core::trWCstrFor(isZh, L"质量", L"Quality"),
        qualityLabel(m_exportOpts.quality, isZh), m_openDropdown == ExportDropdownKind::Quality);

    y += 66.0f;
    {
        std::wstring fmtTip;
        if (m_exportOpts.format == StudioExportFormat::GIF_WeChat) {
            fmtTip = tools3000::core::trWCstrFor(isZh, L"提示：<=800px 微信按「动态表情」气泡内免点自动播放（不支持双击放大）；如需在微信双击打开独立查看器滚轮放大，请选「GIF 高清大图」或 MP4。", L"Note: <=800px auto-plays inline in WeChat as a Sticker (no zoom viewer). Choose 'GIF (HD · Zoomable)' or MP4 to enable double-click viewer zoom.");
        } else if (m_exportOpts.format == StudioExportFormat::GIF_HD) {
            fmtTip = tools3000::core::trWCstrFor(isZh, L"提示：长边 >=1080px 发微信按「普通高清动图」展示，支持双击打开微信独立查看器循环播放并滚轮无级放大看清细节。", L"Note: >=1080px sends as an HD animated image in WeChat — supports double-click viewer playback and scroll-wheel zoom.");
        } else if (m_exportOpts.format == StudioExportFormat::MP4_H264 ||
                   m_exportOpts.format == StudioExportFormat::MP4_HEVC) {
            fmtTip = tools3000::core::trWCstrFor(isZh, L"提示：完整保留立体声音轨与 24-bit 真彩锐利画质，微信/飞书/网页均支持独立大窗高清全屏放大播放。", L"Note: Preserves stereo audio & 24-bit crisp color with full-screen zoom playback across WeChat, Lark & web.");
        } else {
            fmtTip = tools3000::core::trWCstrFor(isZh, L"提示：现代网页 24-bit 真彩动图/视频容器，无 256 色限制，适合嵌入文档、GitHub 与技术博客。", L"Note: 24-bit true-color web media container without 256-color limits, ideal for docs, GitHub & blogs.");
        }
        const D2D1_RECT_F tipBox = D2D1::RectF(inL, y, inR, y + 42.0f);
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.045f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.038f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(tipBox, 7.0f, 7.0f), brush);
        drawTxt(fmtTip, m_captionWrapFmt.Get(),
                D2D1::RectF(tipBox.left + 10.0f, tipBox.top + 3.0f, tipBox.right - 10.0f, tipBox.bottom - 3.0f),
                colSecondary);
    }

    y += 50.0f;
    auto drawCompactCheckbox = [&](int hitId, float cxLeft, float cxRight, bool checked, const std::wstring& label) {
        const D2D1_RECT_F hitRc = D2D1::RectF(cxLeft, y, cxRight, y + 36.0f);
        if (!exporting) {
            m_hitZones.push_back({hitId, hitRc, -1});
        }
        const D2D1_RECT_F boxRc = D2D1::RectF(cxLeft, y + 9.0f, cxLeft + 17.0f, y + 26.0f);
        if (checked) {
            setBrush(colAccent);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush);
            setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
            m_renderTarget->DrawLine({boxRc.left + 3.8f, boxRc.top + 8.8f}, {boxRc.left + 7.4f, boxRc.top + 12.4f}, brush, 1.85f);
            m_renderTarget->DrawLine({boxRc.left + 7.4f, boxRc.top + 12.4f}, {boxRc.left + 13.6f, boxRc.top + 4.8f}, brush, 1.85f);
        } else {
            setBrush(colFieldBg);
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush);
            setBrush(m_hoveredHitId == hitId ? colAccent : colBorder);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush, 1.2f);
        }
        drawTxt(label, m_captionFmt.Get(),
                D2D1::RectF(boxRc.right + 6.5f, y, cxRight, y + 36.0f), colSecondary);
    };

    drawCompactCheckbox(Hit_ModalSavePrefsCheck, inL, inL + 112.0f, m_saveExportPrefs,
                        tools3000::core::trWCstrFor(isZh, L"保存偏好", L"Save Prefs"));
    drawCompactCheckbox(Hit_ModalAutoCloseCheck, inL + 118.0f, inL + 248.0f, m_autoCloseAfterExport,
                        tools3000::core::trWCstrFor(isZh, L"导完自动关闭", L"Auto-Close"));

    const cv::Size targetCanvasSz = StudioVideoCompositor::resolveOutputCanvasSize(
        m_origWidth, m_origHeight, m_config.aspectRatio);
    const uint64_t estBytes = StudioVideoCompositor::estimateExportSizeBytes(
        targetCanvasSz.width, targetCanvasSz.height, computeEffectiveDurationSec(), m_exportOpts);
    const std::wstring estStr = formatEstimatedSize(estBytes);
    const D2D1_RECT_F expBtn = D2D1::RectF(inR - 198.0f, y - 1.0f, inR, y + 37.0f);
    if (!exporting) {
        m_hitZones.push_back({Hit_ModalConfirmExportBtn, expBtn, -1});
    }

    if (exporting) {
        const float prog = static_cast<float>(std::clamp(m_exportProgress.load(std::memory_order_relaxed), 0.0, 1.0));
        setBrush(colFieldBg);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(expBtn, 8.0f, 8.0f), brush);
        const D2D1_RECT_F fillRc = D2D1::RectF(
            expBtn.left, expBtn.top,
            expBtn.left + (std::max)(10.0f, (expBtn.right - expBtn.left) * prog), expBtn.bottom);
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(fillRc, 8.0f, 8.0f), brush);
        wchar_t pTxt[64]{};
        std::swprintf(pTxt, std::size(pTxt), tools3000::core::trWCstrFor(isZh, L"正在合成 %d%%...", L"Exporting %d%%..."),
                      static_cast<int>(std::round(prog * 100.0f)));
        drawTxt(pTxt, m_sectionFmt.Get(), expBtn, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    } else {
        setBrush(m_hoveredHitId == Hit_ModalConfirmExportBtn
            ? D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.07f), (std::min)(1.0f, accentRgb.g + 0.07f), (std::min)(1.0f, accentRgb.b + 0.07f), 1.0f)
            : colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(expBtn, 8.0f, 8.0f), brush);
        const std::wstring btnTxt = (tools3000::core::trWCstrFor(isZh, L"导出  (~", L"Export (~")) + estStr + L")";
        drawTxt(btnTxt, m_sectionFmt.Get(), expBtn, D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    if (!exporting && m_openDropdown != ExportDropdownKind::None) {
        struct DropItem { int payload; std::wstring text; bool selected; };
        std::vector<DropItem> items;
        D2D1_RECT_F anchor{};

        if (m_openDropdown == ExportDropdownKind::Format) {
            anchor = fmtBox;
            const StudioExportFormat fmts[] = {
                StudioExportFormat::MP4_H264, StudioExportFormat::MP4_HEVC,
                StudioExportFormat::GIF_HD, StudioExportFormat::GIF_WeChat,
                StudioExportFormat::WebP_Animated, StudioExportFormat::WebM_VP9
            };
            for (auto f : fmts) {
                items.push_back({static_cast<int>(f), formatLabel(f, isZh), m_exportOpts.format == f});
            }
        } else if (m_openDropdown == ExportDropdownKind::Resolution) {
            anchor = resBox;
            const StudioExportResolution resList[] = {
                StudioExportResolution::Original, StudioExportResolution::Res2160P,
                StudioExportResolution::Res1440P, StudioExportResolution::Res1080P,
                StudioExportResolution::Res720P,  StudioExportResolution::Res640P,
                StudioExportResolution::Res480P
            };
            for (auto r : resList) {
                items.push_back({static_cast<int>(r), resolutionLabel(r, isZh), m_exportOpts.resolution == r});
            }
        } else if (m_openDropdown == ExportDropdownKind::Fps) {
            anchor = fpsBox;
            const bool isGif = (m_exportOpts.format == StudioExportFormat::GIF_WeChat ||
                                m_exportOpts.format == StudioExportFormat::GIF_HD);
            if (isGif) {
                for (int f : {10, 12, 15}) {
                    items.push_back({f, std::to_wstring(f) + L" FPS", m_exportOpts.fps == f});
                }
            } else {
                for (int f : {24, 30, 60}) {
                    items.push_back({f, std::to_wstring(f) + L" FPS", m_exportOpts.fps == f});
                }
            }
        } else if (m_openDropdown == ExportDropdownKind::Quality) {
            anchor = qualBox;
            for (auto q : {StudioExportQuality::High, StudioExportQuality::Recommended, StudioExportQuality::Compact}) {
                items.push_back({static_cast<int>(q), qualityLabel(q, isZh), m_exportOpts.quality == q});
            }
        }

        if (!items.empty()) {
            const float rowH = 32.0f;
            const float menuH = static_cast<float>(items.size()) * rowH + 8.0f;
            float menuTop = anchor.bottom + 4.0f;
            if (menuTop + menuH > H - 10.0f) {
                menuTop = (std::max)(10.0f, anchor.top - menuH - 4.0f);
            }
            const D2D1_RECT_F menuRc = D2D1::RectF(anchor.left, menuTop, anchor.right, menuTop + menuH);
            setBrush(dark ? D2D1::ColorF(0.10f, 0.12f, 0.17f, 0.99f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.99f));
            m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(menuRc, 8.0f, 8.0f), brush);
            setBrush(colAccent);
            m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(menuRc, 8.0f, 8.0f), brush, 1.2f);

            for (size_t i = 0; i < items.size(); ++i) {
                const float iy = menuTop + 4.0f + static_cast<float>(i) * rowH;
                const D2D1_RECT_F itemRc = D2D1::RectF(menuRc.left + 4.0f, iy, menuRc.right - 4.0f, iy + rowH);
                const int itemHitId = Hit_ModalDropdownItemBase + static_cast<int>(i);
                m_hitZones.push_back({itemHitId, itemRc, items[i].payload});
                if (items[i].selected || m_hoveredHitId == itemHitId) {
                    setBrush(items[i].selected
                        ? D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, dark ? 0.28f : 0.16f)
                        : (dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.06f, 0.09f, 0.16f, 0.06f)));
                    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(itemRc, 5.0f, 5.0f), brush);
                }
                drawTxt(items[i].text, m_bodyFmt.Get(),
                        D2D1::RectF(itemRc.left + 10.0f, itemRc.top, itemRc.right - 8.0f, itemRc.bottom),
                        items[i].selected ? colAccent : colPrimary);
            }
        }
    }
}

void StudioPreviewWindow::renderUnsavedCloseModalOverlay(
    float W, float H, bool dark, bool isZh, ID2D1SolidColorBrush* brush) {
    if (!m_renderTarget || !brush) return;

    m_hitZones.clear();

    const auto accentRgb = tools3000::core::NativeThemePalette::getEffectiveAccentColorRGB();
    const D2D1_COLOR_F colAccent = D2D1::ColorF(accentRgb.r, accentRgb.g, accentRgb.b, 1.0f);
    const D2D1_COLOR_F colModalBg = dark
        ? D2D1::ColorF(0.082f, 0.094f, 0.133f, 0.988f)
        : D2D1::ColorF(0.994f, 0.996f, 1.000f, 0.992f);
    const D2D1_COLOR_F colBorder = dark
        ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 0.14f);
    const D2D1_COLOR_F colPrimary = dark
        ? D2D1::ColorF(0.96f, 0.97f, 0.99f, 1.0f)
        : D2D1::ColorF(0.059f, 0.090f, 0.165f, 1.0f);
    const D2D1_COLOR_F colSecondary = dark
        ? D2D1::ColorF(0.66f, 0.72f, 0.82f, 1.0f)
        : D2D1::ColorF(0.35f, 0.41f, 0.51f, 1.0f);
    const D2D1_COLOR_F colWarn = D2D1::ColorF(0.95f, 0.62f, 0.14f, 1.0f);
    const D2D1_COLOR_F colDanger = D2D1::ColorF(0.92f, 0.28f, 0.30f, 1.0f);
    const ComPtr<ID2D1StrokeStyle> roundStroke = createRoundStrokeStyle(m_d2dFactory.Get());

    auto setBrush = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); };
    auto drawTxt = [&](const std::wstring& txt, IDWriteTextFormat* fmt, const D2D1_RECT_F& r,
                       const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT hAlign = DWRITE_TEXT_ALIGNMENT_LEADING,
                       DWRITE_PARAGRAPH_ALIGNMENT vAlign = DWRITE_PARAGRAPH_ALIGNMENT_CENTER) {
        if (!fmt || txt.empty() || r.right <= r.left || r.bottom <= r.top) return;
        fmt->SetTextAlignment(hAlign);
        fmt->SetParagraphAlignment(vAlign);
        setBrush(col);
        m_renderTarget->DrawText(txt.c_str(), static_cast<UINT32>(txt.size()), fmt, r, brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };

    const D2D1_RECT_F fullRc = D2D1::RectF(0.0f, 0.0f, W, H);
    m_hitZones.push_back({Hit_CloseConfirmBackdrop, fullRc, -1});
    setBrush(D2D1::ColorF(0.02f, 0.03f, 0.06f, dark ? 0.66f : 0.42f));
    m_renderTarget->FillRectangle(fullRc, brush);

    const float cardW = 472.0f;
    const float cardH = 250.0f;
    const float cardL = (W - cardW) * 0.5f;
    const float cardT = (H - cardH) * 0.5f;
    const D2D1_RECT_F cardRc = D2D1::RectF(cardL, cardT, cardL + cardW, cardT + cardH);
    m_hitZones.push_back({Hit_CloseConfirmCard, cardRc, -1});

    // 柔雾悬浮阴影层
    setBrush(D2D1::ColorF(0.01f, 0.02f, 0.05f, dark ? 0.44f : 0.18f));
    m_renderTarget->FillRoundedRectangle(
        D2D1::RoundedRect(D2D1::RectF(cardL, cardT + 8.0f, cardL + cardW, cardT + cardH + 12.0f), 18.0f, 18.0f),
        brush);

    setBrush(colModalBg);
    m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush);
    setBrush(colBorder);
    m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(cardRc, 16.0f, 16.0f), brush, 1.15f);

    // 右上角矢量 [×] 取消并返回编辑
    const D2D1_RECT_F cancelRc = D2D1::RectF(cardRc.right - 40.0f, cardT + 14.0f, cardRc.right - 14.0f, cardT + 40.0f);
    m_hitZones.push_back({Hit_CloseConfirmCancelBtn, cancelRc, -1});
    const bool hovCancel = (m_hoveredHitId == Hit_CloseConfirmCancelBtn);
    if (hovCancel) {
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.08f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(cancelRc, 7.0f, 7.0f), brush);
    }
    setBrush(hovCancel ? colPrimary : colSecondary);
    {
        const float cx = (cancelRc.left + cancelRc.right) * 0.5f;
        const float cy = (cancelRc.top + cancelRc.bottom) * 0.5f;
        m_renderTarget->DrawLine({cx - 4.4f, cy - 4.4f}, {cx + 4.4f, cy + 4.4f}, brush, 1.6f, roundStroke.Get());
        m_renderTarget->DrawLine({cx + 4.4f, cy - 4.4f}, {cx - 4.4f, cy + 4.4f}, brush, 1.6f, roundStroke.Get());
    }

    // 顶部琥珀金矢量警示圆徽 + 主标题
    const float inL = cardL + 26.0f;
    const float inR = cardRc.right - 26.0f;
    const D2D1_POINT_2F iconPt{inL + 16.0f, cardT + 40.0f};
    setBrush(D2D1::ColorF(colWarn.r, colWarn.g, colWarn.b, dark ? 0.18f : 0.14f));
    m_renderTarget->FillEllipse(D2D1::Ellipse(iconPt, 16.0f, 16.0f), brush);
    setBrush(D2D1::ColorF(colWarn.r, colWarn.g, colWarn.b, 0.42f));
    m_renderTarget->DrawEllipse(D2D1::Ellipse(iconPt, 16.0f, 16.0f), brush, 1.0f);
    setBrush(colWarn);
    m_renderTarget->DrawLine({iconPt.x, iconPt.y - 6.2f}, {iconPt.x, iconPt.y + 1.8f}, brush, 2.2f, roundStroke.Get());
    m_renderTarget->FillEllipse(D2D1::Ellipse({iconPt.x, iconPt.y + 6.0f}, 1.35f, 1.35f), brush);

    drawTxt(tools3000::core::trWCstrFor(isZh, L"尚未导出录屏内容", L"Recording Not Exported"),
            m_titleFmt.Get(),
            D2D1::RectF(inL + 42.0f, cardT + 24.0f, cancelRc.left - 8.0f, cardT + 56.0f),
            colPrimary);

    const bool hasEdits = hasStudioEditsToRender();
    const std::wstring bodyTxt = hasEdits
        ? (tools3000::core::trWCstrFor(isZh, L"当前录屏尚未导出，直接退出将丢失本次录屏与编辑内容。请选择「保存后退出」自动导出成片，或选择「不保存退出」放弃并清理临时文件。", L"This recording has not been exported yet. Choose \"Save & Exit\" to export your current edits automatically, or \"Exit Without Saving\" to discard the temporary capture."))
        : (tools3000::core::trWCstrFor(isZh, L"当前未启用运镜或背景美化，点击「保存后退出」将瞬间无损保留原始录屏；点击「不保存退出」将直接放弃并清理临时文件。", L"No studio zoom or framing is enabled. \"Save & Exit\" preserves the original lossless recording immediately; \"Exit Without Saving\" discards the temporary file."));
    drawTxt(bodyTxt,
            m_captionWrapFmt.Get(),
            D2D1::RectF(inL, cardT + 68.0f, inR, cardT + 132.0f),
            colSecondary,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    // 可选快捷记忆复选框：下次录屏直接原样保存（跳过演播室）
    const D2D1_RECT_F checkRowRc = D2D1::RectF(inL, cardT + 140.0f, inR, cardT + 170.0f);
    m_hitZones.push_back({Hit_CloseConfirmSkipStudioCheck, checkRowRc, -1});
    const bool hovCheck = (m_hoveredHitId == Hit_CloseConfirmSkipStudioCheck);
    if (hovCheck) {
        setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.04f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(checkRowRc, 7.0f, 7.0f), brush);
    }
    const D2D1_RECT_F boxRc = D2D1::RectF(inL + 4.0f, checkRowRc.top + 7.0f, inL + 20.0f, checkRowRc.top + 23.0f);
    if (m_confirmSkipStudioNextTime) {
        setBrush(colAccent);
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush);
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.98f));
        m_renderTarget->DrawLine({boxRc.left + 3.8f, boxRc.top + 8.3f}, {boxRc.left + 6.9f, boxRc.top + 11.4f},
                                 brush, 1.85f, roundStroke.Get());
        m_renderTarget->DrawLine({boxRc.left + 6.9f, boxRc.top + 11.4f}, {boxRc.right - 3.5f, boxRc.top + 4.8f},
                                 brush, 1.85f, roundStroke.Get());
    } else {
        setBrush(dark ? D2D1::ColorF(0.14f, 0.17f, 0.24f, 0.92f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush);
        setBrush(hovCheck ? colAccent : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(boxRc, 4.5f, 4.5f), brush, 1.15f);
    }
    drawTxt(tools3000::core::trWCstrFor(isZh, L"下次录屏直接原样保存（跳过演播室，记住设置）", L"Skip Studio and save raw recording next time (remember)"),
            m_captionFmt.Get(),
            D2D1::RectF(boxRc.right + 9.0f, checkRowRc.top, checkRowRc.right - 4.0f, checkRowRc.bottom),
            m_confirmSkipStudioNextTime ? colPrimary : colSecondary);

    // 底部柔和分割线与双操作按钮：[不保存退出]  [保存后退出]
    setBrush(dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f) : D2D1::ColorF(0.06f, 0.10f, 0.18f, 0.08f));
    m_renderTarget->DrawLine({inL, cardRc.bottom - 66.0f}, {inR, cardRc.bottom - 66.0f}, brush, 0.9f);

    const float btnH = 36.0f;
    const float btnY = cardRc.bottom - 52.0f;
    const float btnGap = 12.0f;
    const float btnW = (inR - inL - btnGap) * 0.5f;
    const D2D1_RECT_F discardRc = D2D1::RectF(inL, btnY, inL + btnW, btnY + btnH);
    const D2D1_RECT_F saveRc    = D2D1::RectF(discardRc.right + btnGap, btnY, inR, btnY + btnH);
    m_hitZones.push_back({Hit_CloseConfirmDiscardBtn, discardRc, -1});
    m_hitZones.push_back({Hit_CloseConfirmSaveBtn, saveRc, -1});

    // 次级警示按钮：不保存退出
    {
        const bool hovDisc = (m_hoveredHitId == Hit_CloseConfirmDiscardBtn);
        setBrush(hovDisc
            ? D2D1::ColorF(colDanger.r, colDanger.g, colDanger.b, dark ? 0.22f : 0.11f)
            : (dark ? D2D1::ColorF(0.145f, 0.170f, 0.235f, 0.92f) : D2D1::ColorF(0.956f, 0.966f, 0.984f, 1.0f)));
        m_renderTarget->FillRoundedRectangle(D2D1::RoundedRect(discardRc, 8.5f, 8.5f), brush);
        setBrush(hovDisc ? D2D1::ColorF(colDanger.r, colDanger.g, colDanger.b, 0.78f) : colBorder);
        m_renderTarget->DrawRoundedRectangle(D2D1::RoundedRect(discardRc, 8.5f, 8.5f), brush, hovDisc ? 1.2f : 1.0f);
        drawTxt(tools3000::core::trWCstrFor(isZh, L"不保存退出", L"Exit Without Saving"),
                m_sectionFmt.Get(), discardRc,
                hovDisc ? colDanger : colPrimary,
                DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // 主按钮：保存后退出（动态主题色双色渐变 + 顶部镜面亮边）
    {
        const bool hovSave = (m_hoveredHitId == Hit_CloseConfirmSaveBtn);
        const float lift = hovSave ? 0.06f : 0.0f;
        setBrush(D2D1::ColorF(accentRgb.r * 0.4f, accentRgb.g * 0.4f, accentRgb.b * 0.5f, dark ? 0.36f : 0.20f));
        m_renderTarget->FillRoundedRectangle(
            D2D1::RoundedRect(D2D1::RectF(saveRc.left, saveRc.top + 1.5f, saveRc.right, saveRc.bottom + 2.0f), 8.5f, 8.5f),
            brush);
        fillLinearVGradRounded(
            m_renderTarget.Get(), saveRc, 8.5f,
            D2D1::ColorF((std::min)(1.0f, accentRgb.r + 0.08f + lift), (std::min)(1.0f, accentRgb.g + 0.08f + lift), (std::min)(1.0f, accentRgb.b + 0.08f + lift), 1.0f),
            D2D1::ColorF((std::max)(0.0f, accentRgb.r - 0.05f + lift), (std::max)(0.0f, accentRgb.g - 0.05f + lift), (std::max)(0.0f, accentRgb.b - 0.02f + lift), 1.0f));
        setBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f));
        m_renderTarget->DrawLine({saveRc.left + 8.0f, saveRc.top + 0.8f},
                                 {saveRc.right - 8.0f, saveRc.top + 0.8f}, brush, 0.9f);
        drawTxt(tools3000::core::trWCstrFor(isZh, L"保存后退出", L"Save & Exit"),
                m_sectionFmt.Get(), saveRc,
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f),
                DWRITE_TEXT_ALIGNMENT_CENTER);
    }
}

void StudioPreviewWindow::startAsyncExport() {
    if (m_exporting.exchange(true, std::memory_order_acq_rel)) return;

    m_playing = false;
    m_preparingSession = false;
    m_hoverSkimming = false;
    m_exportCompletedCard = false;
    m_editingExportName = false;
    m_openDropdown = ExportDropdownKind::None;
    m_cancelExport.store(false, std::memory_order_release);
    m_exportProgress.store(0.01, std::memory_order_release);
    syncCutSegmentsFromVideoClips();
    persistStudioConfigToSettings(m_config);
    persistExportPreferencesIfChecked();
    InvalidateRect(m_hwnd, nullptr, FALSE);

    joinExportWorkerIfDone(true);

    std::string sourceMasterPath = m_masterVideoPath.empty() ? m_rawVideoPath : m_masterVideoPath;
    const std::string finalPath = buildFinalExportPathUtf8();
    {
        const std::filesystem::path srcFs(tools3000::core::WinUtils::utf8ToWstring(sourceMasterPath));
        const std::filesystem::path dstFs(tools3000::core::WinUtils::utf8ToWstring(finalPath));
        std::error_code eqEc;
        const bool sameTarget =
            (std::filesystem::exists(srcFs, eqEc) && std::filesystem::exists(dstFs, eqEc) &&
             std::filesystem::equivalent(srcFs, dstFs, eqEc)) ||
            (normalizeLowerFsKey(srcFs) == normalizeLowerFsKey(dstFs));
        if (sameTarget && std::filesystem::exists(srcFs, eqEc)) {
            m_hiResReader.close();
            const std::wstring isoName = std::format(
                L".studio_master_{}_{}{}",
                GetTickCount64(),
                srcFs.stem().wstring(),
                srcFs.extension().wstring());
            const std::filesystem::path isoFs = srcFs.parent_path() / isoName;
            std::error_code renEc;
            std::filesystem::rename(srcFs, isoFs, renEc);
            if (!renEc) {
                sourceMasterPath = tools3000::core::WinUtils::wstringToUtf8(isoFs.wstring());
                m_masterVideoPath = sourceMasterPath;
                m_masterIsIsolatedTemp = true;
            }
            m_hiResReader.open(m_masterVideoPath, 3200);
        }
    }

    const StudioTimelineData timelineCopy = m_timeline;
    const StudioRenderConfig configCopy = m_config;
    const StudioExportOptions exportOptsCopy = m_exportOpts;
    const double durSec = computeEffectiveDurationSec();
    const std::string stagingPath = finalPath + ".part";

    std::lock_guard lock(m_exportMutex);
    m_exportWorker = std::thread([this, sourceMasterPath, stagingPath, finalPath, timelineCopy, configCopy, exportOptsCopy, durSec]() {
        tools3000::core::TraceId::Scope traceScope;
        auto progressCb = [this](double p) {
            m_exportProgress.store(p, std::memory_order_relaxed);
        };
        const auto workerOutcome = exportStudioMediaOutOfProcess(
            sourceMasterPath,
            stagingPath,
            timelineCopy,
            configCopy,
            exportOptsCopy,
            progressCb,
            &m_cancelExport);
        bool ok = workerOutcome.handledOutOfProcess
            ? workerOutcome.succeeded
            : StudioVideoCompositor::exportStudioMedia(
                sourceMasterPath,
                stagingPath,
                timelineCopy,
                configCopy,
                exportOptsCopy,
                progressCb,
                &m_cancelExport);

        const bool canceled = m_cancelExport.load(std::memory_order_acquire);
        const std::filesystem::path stagingFs(tools3000::core::WinUtils::utf8ToWstring(stagingPath));
        const std::filesystem::path finalFs(tools3000::core::WinUtils::utf8ToWstring(finalPath));
        if (ok && !canceled) {
            std::error_code rmOldEc;
            if (std::filesystem::exists(finalFs, rmOldEc)) {
                std::filesystem::remove(finalFs, rmOldEc);
            }
            std::error_code renEc;
            std::filesystem::rename(stagingFs, finalFs, renEc);
            if (renEc) {
                ok = false;
                std::error_code cleanEc;
                std::filesystem::remove(stagingFs, cleanEc);
            }
        } else {
            std::error_code cleanEc;
            std::filesystem::remove(stagingFs, cleanEc);
        }

        tools3000::core::MainThreadDispatcher::instance().post(
            [this, ok, canceled, sourceMasterPath, finalPath, exportOptsCopy, durSec]() {
                m_exporting.store(false, std::memory_order_release);
                if (canceled) {
                    m_closeAfterSaveExport = false;
                    return;
                }
                const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
                if (ok) {
                    const std::filesystem::path doneFs(tools3000::core::WinUtils::utf8ToWstring(finalPath));
                    std::error_code szEc;
                    const auto fsz = std::filesystem::file_size(doneFs, szEc);
                    m_lastExportedFileBytes = szEc ? 0ULL : static_cast<std::uint64_t>(fsz);
                    m_lastExportedFilePath = finalPath;
                    m_lastExportedFormat = exportOptsCopy.format;
                    m_lastExportedSpecLine = std::format(
                        L"{} · {} · {} FPS",
                        formatLabel(exportOptsCopy.format, isZh),
                        resolutionLabel(exportOptsCopy.resolution, isZh),
                        exportOptsCopy.fps);

                    const std::wstring normFinal = normalizeLowerFsKey(doneFs);
                    if (std::find(m_sessionExportedPathsLower.begin(), m_sessionExportedPathsLower.end(), normFinal) ==
                        m_sessionExportedPathsLower.end()) {
                        m_sessionExportedPathsLower.push_back(normFinal);
                    }
                    m_copyFeedbackUntilTick = 0;

                    LOG_INFO("StudioPreviewWindow: exported studio video successfully: {}", finalPath);
                    const bool shouldCloseNow = m_autoCloseAfterExport || m_closeAfterSaveExport;
                    m_closeAfterSaveExport = false;
                    if (shouldCloseNow) {
                        finishExportedSessionAndClose();
                    } else {
                        m_exportModalOpen = true;
                        m_exportCompletedCard = true;
                        InvalidateRect(m_hwnd, nullptr, FALSE);
                    }
                } else {
                    m_closeAfterSaveExport = false;
                    LOG_WARN("StudioPreviewWindow: exportRenderedVideo failed, keeping raw video: {}", sourceMasterPath);
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        tools3000::core::trWCstrFor(isZh, L"演播室合成失败，已为您安全保留原始录屏", L"Studio export failed; raw recording preserved")});
                    InvalidateRect(m_hwnd, nullptr, FALSE);
                }
            });
    });
}

void StudioPreviewWindow::copyLastExportedToClipboard() {
    if (m_lastExportedFilePath.empty()) return;
    const std::wstring widePath = tools3000::core::WinUtils::utf8ToWstring(m_lastExportedFilePath);
    bool copied = false;
    if (m_lastExportedFormat == StudioExportFormat::GIF_WeChat ||
        m_lastExportedFormat == StudioExportFormat::GIF_HD) {
        copied = tools3000::core::WinUtils::copyGifToClipboard(widePath, m_hwnd);
    }
    if (!copied) {
        copied = tools3000::core::WinUtils::copyFileToClipboard(widePath, m_hwnd);
    }
    if (copied) {
        m_copyFeedbackUntilTick = GetTickCount64() + 1800;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void StudioPreviewWindow::finishExportedSessionAndClose() {
    if (m_exporting.load(std::memory_order_acquire)) return;
    persistStudioConfigToSettings(m_config);
    m_hiResReader.close();

    // 清理未作为最终成片输出的临时/原始无壁纸母带（保护所有已导出成片绝不误删）
    const std::string masterPath = m_masterVideoPath.empty() ? m_rawVideoPath : m_masterVideoPath;
    if (!masterPath.empty()) {
        const std::filesystem::path masterFs(tools3000::core::WinUtils::utf8ToWstring(masterPath));
        const std::wstring normMaster = normalizeLowerFsKey(masterFs);
        const bool isExportedArtifact =
            std::find(m_sessionExportedPathsLower.begin(), m_sessionExportedPathsLower.end(), normMaster) !=
            m_sessionExportedPathsLower.end();
        if (!isExportedArtifact) {
            std::error_code rmEc;
            std::filesystem::remove(masterFs, rmEc);
        }
    }

    const std::string deliveredPath = !m_lastExportedFilePath.empty() ? m_lastExportedFilePath : m_rawVideoPath;
    const double dur = computeEffectiveDurationSec();
    auto cb = std::move(m_onFinished);
    closeSessionAndCleanup();
    if (cb) {
        cb(deliveredPath, dur, true);
    }
}

bool StudioPreviewWindow::isRawOutputModeConfig(const StudioRenderConfig& cfg) noexcept {
    return !cfg.autoFrameEnabled &&
           !cfg.autoZoomEnabled &&
           cfg.aspectRatio == StudioAspectRatio::Original;
}

bool StudioPreviewWindow::hasStudioEditsToRender(
    const StudioRenderConfig& cfg,
    const StudioTimelineData& timeline,
    const std::vector<StudioVideoClip>& clips,
    int64_t rawTotalMs) noexcept {
    if (cfg.autoFrameEnabled) return true;
    if (cfg.aspectRatio != StudioAspectRatio::Original) return true;
    if (cfg.autoZoomEnabled && cfg.zoomMode != StudioZoomMode::None) {
        for (const auto& seg : timeline.zoomSegments) {
            if (seg.enabled) return true;
        }
    }
    if (!timeline.annotations.empty()) return true;
    if (cfg.watermark.enabled) return true;
    if (cfg.autoCaptionsEnabled) return true;
    if (cfg.smoothCursorEnabled &&
        (cfg.cursorHighlightHalo || cfg.showClickRipple || std::abs(cfg.cursorScale - 1.0f) > 0.05f)) {
        return true;
    }
    if (!clips.empty()) {
        if (clips.size() > 1) return true;
        const auto& c = clips.front();
        if (c.srcStartMs > 40) return true;
        if (rawTotalMs > 100 && c.srcEndMs < rawTotalMs - 40) return true;
        if (std::abs(c.speed - 1.0f) > 0.03f) return true;
    }
    return false;
}

bool StudioPreviewWindow::hasStudioEditsToRender() const noexcept {
    const int64_t rawTotalMs = (std::max<int64_t>)(
        100, static_cast<int64_t>(std::llround(m_durationSec * 1000.0)));
    return hasStudioEditsToRender(m_config, m_timeline, m_videoClips, rawTotalMs);
}

bool StudioPreviewWindow::isRawOutputModeActive() const noexcept {
    return isRawOutputModeConfig(m_config);
}

void StudioPreviewWindow::toggleRawOutputPreset() {
    const bool isZh = tools3000::core::NativeThemePalette::isUiLanguageChinese();
    if (!isRawOutputModeActive()) {
        m_config.autoFrameEnabled = false;
        m_config.autoZoomEnabled = false;
        m_config.zoomMode = StudioZoomMode::None;
        m_config.aspectRatio = StudioAspectRatio::Original;
        m_config.smoothCursorEnabled = false;
        m_config.showClickRipple = false;
        m_config.cursorHighlightHalo = false;
        persistStudioConfigToSettings(m_config);
        rebakeTimelinePreview();
        requestImmediatePreviewRefresh(true);
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"已切换为原样模式（无运镜/无背景边框），设置已记住", L"Switched to Raw Mode (no zoom or framing), preference saved")});
    } else {
        m_config.autoFrameEnabled = true;
        m_config.autoZoomEnabled = true;
        m_config.zoomMode = StudioZoomMode::Zoom2D;
        m_config.smoothCursorEnabled = true;
        m_config.showClickRipple = (m_config.clickEffectStyle != StudioClickEffectStyle::TactileOnly);
        persistStudioConfigToSettings(m_config);
        rebakeTimelinePreview();
        requestImmediatePreviewRefresh(true);
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"已恢复演播室运镜与背景美化，设置已记住", L"Restored Studio framing and auto zoom, preference saved")});
    }
}

void StudioPreviewWindow::closeOrKeepBySessionState() {
    if (!m_sessionExportedPathsLower.empty()) {
        finishExportedSessionAndClose();
    } else {
        keepOriginalAndClose();
    }
}

void StudioPreviewWindow::requestCloseWithConfirm() {
    if (m_exporting.load(std::memory_order_acquire)) return;
    if (!m_sessionExportedPathsLower.empty()) {
        finishExportedSessionAndClose();
        return;
    }
    m_playing = false;
    m_hoverSkimming = false;
    m_exportModalOpen = false;
    m_exportCompletedCard = false;
    m_closeConfirmModalOpen = false;
    m_closeAfterSaveExport = false;
    m_confirmSkipStudioNextTime = false;
    m_editingExportName = false;
    m_editingAnnotationText = false;
    m_editingWatermarkText = false;
    m_openDropdown = ExportDropdownKind::None;
    m_confirmSkipStudioNextTime =
        !tools3000::core::ConfigManager::instance().get<bool>("/recording/studioMode", true);
    m_closeConfirmModalOpen = true;
    InvalidateRect(m_hwnd, nullptr, FALSE);
}

void StudioPreviewWindow::discardOriginalAndClose() {
    if (m_exporting.load(std::memory_order_acquire)) return;
    persistStudioConfigToSettings(m_config);
    m_hiResReader.close();

    auto tryRemoveFile = [](const std::string& utf8Path) {
        if (utf8Path.empty()) return;
        std::error_code ec;
        const std::filesystem::path p(tools3000::core::WinUtils::utf8ToWstring(utf8Path));
        if (std::filesystem::exists(p, ec)) {
            std::filesystem::remove(p, ec);
        }
    };
    tryRemoveFile(m_masterVideoPath);
    if (m_rawVideoPath != m_masterVideoPath) {
        tryRemoveFile(m_rawVideoPath);
    }

    auto cb = std::move(m_onFinished);
    closeSessionAndCleanup();
    if (cb) {
        cb("", 0.0, false);
    }
}

void StudioPreviewWindow::keepOriginalAndClose() {
    if (m_exporting.load(std::memory_order_acquire)) return;
    persistStudioConfigToSettings(m_config);
    m_hiResReader.close();

    std::string finalRawPath = m_rawVideoPath;
    if (m_masterIsIsolatedTemp && !m_masterVideoPath.empty()) {
        const std::filesystem::path masterFs(tools3000::core::WinUtils::utf8ToWstring(m_masterVideoPath));
        const std::filesystem::path origRawFs(tools3000::core::WinUtils::utf8ToWstring(m_rawVideoPath));
        std::error_code ec;
        if (std::filesystem::exists(masterFs, ec)) {
            if (!std::filesystem::exists(origRawFs, ec)) {
                std::filesystem::rename(masterFs, origRawFs, ec);
                if (!ec) {
                    finalRawPath = m_rawVideoPath;
                } else {
                    finalRawPath = m_masterVideoPath;
                }
            } else {
                const std::filesystem::path altRawFs =
                    origRawFs.parent_path() / (origRawFs.stem().wstring() + L"_raw" + origRawFs.extension().wstring());
                std::filesystem::rename(masterFs, altRawFs, ec);
                finalRawPath = !ec ? tools3000::core::WinUtils::wstringToUtf8(altRawFs.wstring()) : m_masterVideoPath;
            }
        }
    }

    const double dur = m_durationSec;
    auto cb = std::move(m_onFinished);
    closeSessionAndCleanup();
    if (cb) {
        cb(finalRawPath, dur, false);
    }
}

void StudioPreviewWindow::joinExportWorkerIfDone(bool wait) {
    std::thread toJoin;
    {
        std::lock_guard lock(m_exportMutex);
        if (m_exportWorker.joinable() && (wait || !m_exporting.load(std::memory_order_acquire))) {
            if (std::this_thread::get_id() != m_exportWorker.get_id()) {
                toJoin = std::move(m_exportWorker);
            }
        }
    }
    if (toJoin.joinable()) {
        toJoin.join();
    }
}

} // namespace tools3000::capture
