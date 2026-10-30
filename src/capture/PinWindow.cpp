// ─────────────────────────────────────────────────────────────────────────────
// PinWindow.cpp — 贴图窗口实现
//
// 功能:
//   - 截图后按 "钉住" 将截图贴在屏幕上
//   - 左键拖拽移动, 滚轮缩放, 双击关闭
//   - 支持透明度调节 (右键菜单)
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindow.h"
#include "capture/ScreenCapture.h"
#include "capture/ClipboardUtils.h"
#include "capture/CaptureOverlay.h"
#include "capture/CaptureVectorIcons.h"
#include "capture/FloatingGlassBar.h"
#include "capture/CaptureHistory.h"
#include "capture/PinSpawnCalculator.h"
#include "capture/PinPasteCoordinator.h"
#include "capture/PinInputController.h"
#include "core/events/EventBus.h"
#include "core/logger/Logger.h"
#include "core/config/ConfigManager.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/utils/TraceId.h"
#include "ocr/OcrEngine.h"

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <shobjidl.h>
#include <windowsx.h>

namespace tools3000::capture {

using namespace Microsoft::WRL;

static constexpr const wchar_t* PIN_CLASS = L"Tools3000_PinWindow";

// 静态成员
std::vector<std::shared_ptr<PinWindow>> PinWindow::s_instances;
bool PinWindow::s_classRegistered = false;
bool PinWindow::s_allHidden = false;

// ─────────────────────────────────────────────────────────────────────────────
// 创建 / 销毁
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<PinWindow> PinWindow::create(const cv::Mat& image, int x, int y) {
    if (image.empty()) return nullptr;

    auto pin = std::shared_ptr<PinWindow>(new PinWindow());
    pin->m_origWidth = image.cols;
    pin->m_origHeight = image.rows;
    pin->m_sourceImage = image.clone();  // 保存原图，供右键"复制到剪贴板"

    if (!pin->initWindow(GetModuleHandleW(nullptr), x, y, image.cols, image.rows)) {
        LOG_ERROR("贴图窗口创建失败");
        return nullptr;
    }

    if (!pin->createRenderResources(image)) {
        LOG_ERROR("贴图窗口渲染资源创建失败");
        return nullptr;
    }

    ShowWindow(pin->m_hwnd, SW_SHOW);
    pin->focusWindow();
    pin->render();

    s_instances.push_back(pin);
    pin->ensureOcrExecutedAsync();
    LOG_INFO("贴图窗口已创建: {}x{} @ ({},{}), 总数={}", image.cols, image.rows, x, y, s_instances.size());

    return pin;
}

void PinWindow::close() {
    // 保活: 下面的 erase 会丢弃 s_instances 中指向自身的最后一个 shared_ptr，
    // 若不先持有自身引用，this 会在本函数执行途中被析构 (use-after-free)。
    // close() 通常从 pinWndProc (双击/右键) 内被调用，UAF 会偶发崩溃。
    std::shared_ptr<PinWindow> keepAlive;
    try {
        keepAlive = shared_from_this();
    } catch (const std::bad_weak_ptr&) {
        // 理论上不会发生 (实例总由 create() 用 shared_ptr 持有)
    }

    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
    m_sourceImage.release();
    // 从全局列表移除已失效的实例
    s_instances.erase(
        std::remove_if(s_instances.begin(), s_instances.end(),
                       [](const auto& p) { return !p->isAlive(); }),
        s_instances.end()
    );
    if (s_instances.empty()) {
        tools3000::core::WinUtils::trimWorkingSet();
    }
}

void PinWindow::closeAll() {
    for (auto& pin : s_instances) {
        if (pin) {
            pin->m_sourceImage.release();
            if (pin->m_hwnd) {
                DestroyWindow(pin->m_hwnd);
                pin->m_hwnd = nullptr;
            }
        }
    }
    s_instances.clear();
    s_allHidden = false;
    tools3000::core::WinUtils::trimWorkingSet();
    LOG_INFO("所有贴图窗口已关闭并释放资源");
}

void PinWindow::toggleHideAll() {
    if (s_instances.empty()) return;
    // 智能切换：只要有任意贴图被隐藏（含被 Esc 单独隐藏的），就全部显示；否则全部隐藏。
    // 这样单独 Esc 隐藏的贴图按一次此键即可找回。
    bool anyHidden = false;
    for (auto& pin : s_instances) {
        if (pin && pin->m_hwnd && !IsWindowVisible(pin->m_hwnd)) { anyHidden = true; break; }
    }
    bool show = anyHidden;
    for (auto& pin : s_instances) {
        if (pin && pin->m_hwnd) {
            ShowWindow(pin->m_hwnd, show ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
    }
    s_allHidden = !show;
    LOG_INFO("所有贴图已{}", show ? "显示" : "隐藏");
}

void PinWindow::arrangeAll() {
    if (s_instances.empty()) return;
    s_allHidden = false;

    RECT wa{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);  // 主屏工作区（排除任务栏）

    const int step = 32;        // 层叠步进
    const int perColumn = 12;   // 每列层叠数，超过换到右侧新列
    int i = 0;
    for (auto& pin : s_instances) {
        if (!pin || !pin->m_hwnd) continue;
        int col = i / perColumn;
        int row = i % perColumn;
        int px = wa.left + 40 + col * (step * 13) + row * step;
        int py = wa.top  + 40 + row * step;
        ShowWindow(pin->m_hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(pin->m_hwnd, HWND_TOPMOST, px, py, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        ++i;
    }
    LOG_INFO("已整理 {} 张贴图", i);
}

PinWindow::~PinWindow() {
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
        m_hwnd = nullptr;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 属性
// ─────────────────────────────────────────────────────────────────────────────

void PinWindow::focusWindow() {
    if (!m_hwnd || !IsWindow(m_hwnd) || m_clickThrough) return;
    HWND fg = GetForegroundWindow();
    if (fg != m_hwnd) {
        DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
        DWORD selfTid = GetCurrentThreadId();
        bool attached = false;
        if (fgTid != 0 && fgTid != selfTid) {
            attached = AttachThreadInput(fgTid, selfTid, TRUE) != FALSE;
        }
        SetForegroundWindow(m_hwnd);
        BringWindowToTop(m_hwnd);
        if (attached) {
            AttachThreadInput(fgTid, selfTid, FALSE);
        }
    }
    SetActiveWindow(m_hwnd);
    SetFocus(m_hwnd);
    if (!m_focused) {
        m_focused = true;
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }
}

void PinWindow::applyLayeredOpacity() {
    if (!m_hwnd) return;
    BYTE alpha = static_cast<BYTE>(std::clamp(m_opacity * 255.0f, 10.0f, 255.0f));
    SetLayeredWindowAttributes(m_hwnd, 0, alpha, LWA_ALPHA);
}

void PinWindow::updateHoverAnimation() {
    bool needsRender = false;
    float dt = (GetTickCount64() - m_hoverTime) / 150.0f;
    if (dt > 1.0f) dt = 1.0f;
    
    float targetAlpha = m_isHovering ? 1.0f : 0.0f;
    float startAlpha = m_isHovering ? 0.0f : 1.0f;
    
    float newAlpha = m_isHovering ? 
        startAlpha + (targetAlpha - startAlpha) * (1.0f - pow(1.0f - dt, 3.0f)) : 
        startAlpha + (targetAlpha - startAlpha) * dt; // linear fade out
        
    if (abs(m_hoverAlpha - newAlpha) > 0.01f) {
        m_hoverAlpha = newAlpha;
        needsRender = true;
    }
    
    if (dt >= 1.0f) {
        m_hoverAlpha = targetAlpha;
        KillTimer(m_hwnd, 1);
        needsRender = true;
    }
    
    if (needsRender) {
        render();
    }
}

void PinWindow::drawHoverToolbar() {
    if (!m_renderTarget) return;
    auto size = m_renderTarget->GetSize();

    const float dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);

    // 悬浮工具栏：折叠、旋转、反色、编辑、文字识别/复制、复制图片、保存、关闭 8 个高精度矢量按钮
    // 自适应多行网格与紧凑缩放，保障窄贴图（如 400x20 旋转为 20x400 后）全部按钮仍在可视区内
    const auto layout = PinInputController::computeHoverToolbarLayout(size.width, size.height, dpiScale);
    m_toolbarRect = layout.toolbarRect;
    m_btnFoldRect   = layout.buttons[0];
    m_btnRotateRect = layout.buttons[1];
    m_btnInvertRect = layout.buttons[2];
    m_btnEditRect   = layout.buttons[3];
    m_btnTextRect   = layout.buttons[4];
    m_btnCopyRect   = layout.buttons[5];
    m_btnSaveRect   = layout.buttons[6];
    m_btnCloseRect  = layout.buttons[7];
    
    // 绘制磨砂玻璃背景
    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), m_toolbarRect, 6.0f * dpiScale, true);
    
    ComPtr<ID2D1SolidColorBrush> textBrush;
    m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.9f * m_hoverAlpha), textBrush.GetAddressOf());
    
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnFoldRect, 4.0f * dpiScale, m_hoverFold, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnRotateRect, 4.0f * dpiScale, m_hoverRotate, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnInvertRect, 4.0f * dpiScale, m_hoverInvert, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnEditRect, 4.0f * dpiScale, m_hoverEdit, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnTextRect, 4.0f * dpiScale, m_hoverText, m_textSelectionMode, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCopyRect, 4.0f * dpiScale, m_hoverCopy, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnSaveRect, 4.0f * dpiScale, m_hoverSave, false, false, false, m_hoverAlpha, true);
    FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCloseRect, 4.0f * dpiScale, m_hoverClose, false, true, false, m_hoverAlpha, true);
    
    // 1. Fold 图标 (Reicon: ActionFold)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionFold, m_btnFoldRect, textBrush.Get(), dpiScale);

    // 2. Rotate 图标 (Lucide: RotateCw)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionRotate, m_btnRotateRect, textBrush.Get(), dpiScale);

    // 3. Invert 图标 (Reicon: ActionInvert)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionInvert, m_btnInvertRect, textBrush.Get(), dpiScale);

    // 4. Edit 图标 (Lucide: Pen)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ToolPen, m_btnEditRect, textBrush.Get(), dpiScale);

    // 5. Text/OCR 图标 (Lucide: Type)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ToolText, m_btnTextRect, textBrush.Get(), dpiScale);

    // 6. Copy 图标 (Lucide: Copy)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionCopy, m_btnCopyRect, textBrush.Get(), dpiScale);
    
    // 7. Save 图标 (Lucide: Download)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionSave, m_btnSaveRect, textBrush.Get(), dpiScale);
    
    // 8. Close 图标 (Lucide: X)
    CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
        CaptureIconId::ActionCancel, m_btnCloseRect, textBrush.Get(), dpiScale);

    // 悬停微晶快捷键提示胶囊 (Tooltip Pill)
    const bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
    const wchar_t* tipText = nullptr;
    float anchorX = 0.0f;
    if (m_hoverFold) {
        tipText = isZh ? L"折叠胶囊 (M)" : L"Fold Capsule (M)";
        anchorX = (m_btnFoldRect.left + m_btnFoldRect.right) * 0.5f;
    } else if (m_hoverRotate) {
        tipText = isZh ? L"旋转 90° (R / 1·2)" : L"Rotate 90° (R / 1·2)";
        anchorX = (m_btnRotateRect.left + m_btnRotateRect.right) * 0.5f;
    } else if (m_hoverInvert) {
        tipText = isZh ? L"色彩反转 (I)" : L"Invert Colors (I)";
        anchorX = (m_btnInvertRect.left + m_btnInvertRect.right) * 0.5f;
    } else if (m_hoverEdit) {
        tipText = isZh ? L"二次标注 (E)" : L"Annotate (E)";
        anchorX = (m_btnEditRect.left + m_btnEditRect.right) * 0.5f;
    } else if (m_hoverText) {
        tipText = isZh ? L"文字划选 (Alt)" : L"Select Text (Alt)";
        anchorX = (m_btnTextRect.left + m_btnTextRect.right) * 0.5f;
    } else if (m_hoverCopy) {
        tipText = isZh ? L"复制贴图 (Ctrl+C)" : L"Copy Pin (Ctrl+C)";
        anchorX = (m_btnCopyRect.left + m_btnCopyRect.right) * 0.5f;
    } else if (m_hoverSave) {
        tipText = isZh ? L"保存图片 (Ctrl+S)" : L"Save Image (Ctrl+S)";
        anchorX = (m_btnSaveRect.left + m_btnSaveRect.right) * 0.5f;
    } else if (m_hoverClose) {
        tipText = isZh ? L"关闭贴图 (Esc)" : L"Close Pin (Esc)";
        anchorX = (m_btnCloseRect.left + m_btnCloseRect.right) * 0.5f;
    }

    if (tipText) {
        const D2D1_RECT_F tipRect = PinInputController::computeTooltipRect(
            m_toolbarRect, anchorX, size.width, size.height, dpiScale);
        if (tipRect.right > tipRect.left && tipRect.bottom > tipRect.top) {
            Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
            DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
            if (dwriteFactory && textBrush) {
                Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
                dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr,
                    DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                    11.0f * dpiScale, L"zh-cn", fmt.GetAddressOf());
                if (!fmt) {
                    dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr,
                        DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                        11.0f * dpiScale, L"zh-cn", fmt.GetAddressOf());
                }
                if (fmt) {
                    fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                    FloatingGlassBar::drawGlassPanel(m_renderTarget.Get(), tipRect, 4.0f * dpiScale, true);
                    m_renderTarget->DrawText(tipText, static_cast<UINT32>(wcslen(tipText)), fmt.Get(),
                                             tipRect, textBrush.Get());
                }
            }
        }
    }
}

void PinWindow::setOpacity(float opacity) {
    m_opacity = std::clamp(opacity, 0.1f, 1.0f);
    applyLayeredOpacity();
}

void PinWindow::setClickThrough(bool enable) {
    if (!m_hwnd) return;
    LONG_PTR ex = GetWindowLongPtrW(m_hwnd, GWL_EXSTYLE);
    if (enable) ex |= WS_EX_TRANSPARENT;
    else        ex &= ~static_cast<LONG_PTR>(WS_EX_TRANSPARENT);
    SetWindowLongPtrW(m_hwnd, GWL_EXSTYLE, ex);
    m_clickThrough = enable;
    if (!enable) {
        SetWindowPos(m_hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    }
    applyLayeredOpacity();
    render();
    LOG_INFO("贴图鼠标穿透: {}", enable ? "开启" : "关闭");
    const bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{isZh ? (enable ? L"贴图已开启鼠标穿透 (按 F4 或悬浮按 Ctrl+Alt+Shift+X 恢复)"
                                                  : L"贴图已退出鼠标穿透模式")
                                        : (enable ? L"Click-through enabled (F4 or Ctrl+Alt+Shift+X to restore)"
                                                  : L"Click-through disabled")});
}

bool PinWindow::toggleClickThroughUnderCursor() {
    POINT pt;
    GetCursorPos(&pt);
    // 倒序遍历（后创建的视为更上层）：找到光标矩形命中的贴图并切换其穿透态
    for (auto it = s_instances.rbegin(); it != s_instances.rend(); ++it) {
        auto& pin = *it;
        if (!pin || !pin->m_hwnd) continue;
        RECT rc;
        if (GetWindowRect(pin->m_hwnd, &rc) && PtInRect(&rc, pt)) {
            pin->setClickThrough(!pin->m_clickThrough);
            return true;
        }
    }
    // 兜底容错：如果光标未直接命中贴图，但存在处于穿透状态的贴图，恢复最近一个穿透贴图
    for (auto it = s_instances.rbegin(); it != s_instances.rend(); ++it) {
        auto& pin = *it;
        if (pin && pin->m_hwnd && pin->m_clickThrough) {
            pin->setClickThrough(false);
            return true;
        }
    }
    return false;
}

void PinWindow::cancelAllClickThrough() {
    for (auto& pin : s_instances) {
        if (pin && pin->m_hwnd && pin->m_clickThrough) {
            pin->setClickThrough(false);
        }
    }
}

// 将 cv::Mat 以标准底向上 CF_DIB 与 PNG 写入剪贴板
static bool copyImageToClipboard(const cv::Mat& image) {
    if (image.empty()) return false;
    return copyMatToClipboard(image);
}

static bool savePinnedImage(HWND owner, const cv::Mat& image) {
    if (image.empty()) return false;

    ComPtr<IFileSaveDialog> dialog;
    HRESULT hr = CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(hr)) {
        LOG_ERROR("贴图保存对话框创建失败: hr=0x{:08X}", static_cast<unsigned>(hr));
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"无法打开保存对话框"});
        return false;
    }

    const COMDLG_FILTERSPEC filters[] = {
        {L"PNG 图片 (*.png)", L"*.png"},
        {L"JPEG 图片 (*.jpg;*.jpeg)", L"*.jpg;*.jpeg"},
        {L"WebP 图片 (*.webp)", L"*.webp"},
        {L"BMP 图片 (*.bmp)", L"*.bmp"},
    };
    dialog->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
    dialog->SetDefaultExtension(L"png");

    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t defaultName[80]{};
    swprintf_s(defaultName, L"Tools3000_%04u%02u%02u_%02u%02u%02u.png",
               now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    dialog->SetFileName(defaultName);

    hr = dialog->Show(owner);
    if (hr == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
    if (FAILED(hr)) {
        LOG_ERROR("贴图保存对话框失败: hr=0x{:08X}", static_cast<unsigned>(hr));
        return false;
    }

    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) return false;
    PWSTR rawPath = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &rawPath)) || !rawPath) return false;
    std::filesystem::path path(rawPath);
    CoTaskMemFree(rawPath);

    std::wstring ext = path.extension().wstring();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (ext.empty()) {
        path += L".png";
        ext = L".png";
    }
    if (ext == L".jpeg") ext = L".jpg";
    const std::string encodeExt = tools3000::core::WinUtils::wstringToUtf8(ext);
    const bool supported = encodeExt == ".png" || encodeExt == ".jpg" ||
                           encodeExt == ".webp" || encodeExt == ".bmp";
    if (!supported) {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"不支持该图片格式"});
        return false;
    }

    try {
        std::vector<uchar> encoded;
        std::vector<int> params;
        if (encodeExt == ".jpg") params = {cv::IMWRITE_JPEG_QUALITY, 95};
        else if (encodeExt == ".webp") params = {cv::IMWRITE_WEBP_QUALITY, 95};
        if (!cv::imencode(encodeExt, image, encoded, params)) return false;

        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(encoded.data()),
                   static_cast<std::streamsize>(encoded.size()));
        if (!file) throw std::runtime_error("write failed");
        LOG_INFO("贴图已保存: {}", tools3000::core::WinUtils::wstringToUtf8(path.wstring()));
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"贴图已保存"});
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR("贴图保存失败: {}", e.what());
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{L"贴图保存失败"});
        return false;
    }
}

void PinWindow::setScale(float scale) {
    m_scale = std::clamp(scale, 0.1f, 5.0f);
    if (m_hwnd) {
        if (m_folded) {
            return;
        }
        int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        SetWindowPos(m_hwnd, nullptr, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

        if (m_renderTarget) {
            m_renderTarget->Resize(D2D1::SizeU(w, h));
        }
        render();
    }
}

void PinWindow::rotate(int angleDeg) {
    if (m_sourceImage.empty() || !m_hwnd) return;
    if (PinInputController::normalizeRightAngleDegrees(angleDeg) == 0) return;

    RECT baseRect{};
    GetWindowRect(m_hwnd, &baseRect);
    if (m_folded) {
        const int prevW = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        const int prevH = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        baseRect.right = baseRect.left + prevW;
        baseRect.bottom = baseRect.top + prevH;
        m_folded = false;
    }

    if (!PinInputController::applyRotation(m_sourceImage, m_colorBackup, m_origWidth, m_origHeight, angleDeg)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_selectedBlockIndices.clear();
        m_hoveredBlockIndex = -1;
        m_ocrReady = false;
    }
    ensureOcrExecutedAsync();

    const int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
    const int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
    HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
    const RECT workArea = tools3000::core::dpi::workArea(mon);
    const RECT targetRect = PinInputController::calculateRotatedWindowRect(baseRect, w, h, workArea);
    m_unfoldedRect = targetRect;

    SetWindowPos(m_hwnd, nullptr,
                 targetRect.left, targetRect.top,
                 w, h,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::flip(bool horizontal) {
    if (m_sourceImage.empty() || !m_hwnd) return;
    if (!PinInputController::applyFlip(m_sourceImage, m_colorBackup, horizontal)) {
        return;
    }
    if (m_folded) {
        m_folded = false;
        RECT curRc{};
        GetWindowRect(m_hwnd, &curRc);
        const int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        const int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        const RECT workArea = tools3000::core::dpi::workArea(mon);
        int posX = curRc.left;
        int posY = curRc.top;
        if (posX + w > workArea.right) posX = (std::max)(static_cast<int>(workArea.left), static_cast<int>(workArea.right - w));
        if (posY + h > workArea.bottom) posY = (std::max)(static_cast<int>(workArea.top), static_cast<int>(workArea.bottom - h));
        m_unfoldedRect = RECT{posX, posY, posX + w, posY + h};
        SetWindowPos(m_hwnd, nullptr, posX, posY, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_selectedBlockIndices.clear();
        m_hoveredBlockIndex = -1;
        m_ocrReady = false;
    }
    ensureOcrExecutedAsync();
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::invertColors() {
    if (m_sourceImage.empty() || !m_hwnd) return;
    auto invertMat = [](cv::Mat& mat) {
        if (mat.channels() == 4) {
            std::vector<cv::Mat> channels;
            cv::split(mat, channels);
            channels[0] = 255 - channels[0];
            channels[1] = 255 - channels[1];
            channels[2] = 255 - channels[2];
            cv::merge(channels, mat);
        } else if (mat.channels() == 3) {
            mat = cv::Scalar(255, 255, 255) - mat;
        }
    };
    invertMat(m_sourceImage);
    if (!m_colorBackup.empty()) {
        invertMat(m_colorBackup);
    }
    m_inverted = !m_inverted;
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::toggleGrayscale() {
    if (m_sourceImage.empty() || !m_hwnd) return;
    if (!m_grayscale) {
        m_colorBackup = m_sourceImage.clone();
        if (m_sourceImage.channels() == 4) {
            cv::Mat bgr, gray, grayBgra;
            cv::cvtColor(m_sourceImage, bgr, cv::COLOR_BGRA2BGR);
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, grayBgra, cv::COLOR_GRAY2BGRA);
            std::vector<cv::Mat> origChannels, grayChannels;
            cv::split(m_sourceImage, origChannels);
            cv::split(grayBgra, grayChannels);
            grayChannels[3] = origChannels[3];
            cv::merge(grayChannels, m_sourceImage);
        } else if (m_sourceImage.channels() == 3) {
            cv::Mat gray;
            cv::cvtColor(m_sourceImage, gray, cv::COLOR_BGR2GRAY);
            cv::cvtColor(gray, m_sourceImage, cv::COLOR_GRAY2BGR);
        }
        m_grayscale = true;
    } else {
        if (!m_colorBackup.empty()) {
            m_sourceImage = m_colorBackup.clone();
            m_colorBackup.release();
        }
        m_grayscale = false;
    }
    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::resetScale() {
    setScale(1.0f);
}

void PinWindow::toggleFold() {
    if (!m_hwnd) return;
    const float dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);
    if (!m_folded) {
        GetWindowRect(m_hwnd, &m_unfoldedRect);
        m_folded = true;
        int foldW = static_cast<int>(148.0f * dpiScale);
        int foldH = static_cast<int>(38.0f * dpiScale);
        SetWindowPos(m_hwnd, nullptr, m_unfoldedRect.left, m_unfoldedRect.top, foldW, foldH, SWP_NOZORDER | SWP_NOACTIVATE);
        if (m_renderTarget) {
            m_renderTarget->Resize(D2D1::SizeU(foldW, foldH));
        }
        render();
    } else {
        m_folded = false;
        RECT curRc{};
        GetWindowRect(m_hwnd, &curRc);
        const int w = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
        const int h = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
        HMONITOR mon = MonitorFromWindow(m_hwnd, MONITOR_DEFAULTTONEAREST);
        RECT workArea = tools3000::core::dpi::workArea(mon);
        int posX = curRc.left;
        int posY = curRc.top;
        if (posX + w > workArea.right) posX = (std::max)(static_cast<int>(workArea.left), static_cast<int>(workArea.right - w));
        if (posY + h > workArea.bottom) posY = (std::max)(static_cast<int>(workArea.top), static_cast<int>(workArea.bottom - h));
        m_unfoldedRect = RECT{posX, posY, posX + w, posY + h};
        SetWindowPos(m_hwnd, nullptr, posX, posY, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        if (m_renderTarget) {
            m_renderTarget->Resize(D2D1::SizeU(w, h));
        }
        render();
    }
}

void PinWindow::editMarkup() {
    if (!m_hwnd || m_sourceImage.empty()) return;
    
    RECT rc{};
    GetWindowRect(m_hwnd, &rc);
    CaptureRegion reg{ rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top };

    ShowWindow(m_hwnd, SW_HIDE);
    std::weak_ptr<PinWindow> weakSelf = shared_from_this();
    CaptureOverlay::instance().startEditPinned(m_sourceImage, reg, [weakSelf](const cv::Mat& newImage) {
        if (auto self = weakSelf.lock()) {
            if (!newImage.empty()) {
                self->updateImage(newImage);
            }
            if (self->m_hwnd && IsWindow(self->m_hwnd)) {
                ShowWindow(self->m_hwnd, SW_SHOW);
                self->focusWindow();
            }
        }
    });
}

static void cleanupOcrWorkingSet() {
    tools3000::core::WinUtils::trimWorkingSet();
}

void PinWindow::ensureOcrExecutedAsync() {
    if (m_sourceImage.empty()) return;
    cv::Mat imgCopy = m_sourceImage.clone();
    uint64_t currentVer = ++m_ocrVersion;
    std::weak_ptr<PinWindow> weakSelf = shared_from_this();

    std::thread([weakSelf, imgCopy, currentVer]() {
        tools3000::core::TraceId::Scope traceScope;
        auto results = tools3000::ocr::OcrEngine::instance().extractText(imgCopy);
        tools3000::core::MainThreadDispatcher::instance().post([weakSelf, results = std::move(results), currentVer]() {
            if (auto self = weakSelf.lock()) {
                if (self->m_ocrVersion.load() != currentVer) return;
                std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                self->m_ocrBlocks.clear();
                self->m_ocrBlocks.reserve(results.size());
                for (const auto& r : results) {
                    if (!r.text.empty()) {
                        self->m_ocrBlocks.push_back({r.text, r.box});
                    }
                }
                self->m_ocrReady = true;
                if (self->m_hwnd && IsWindow(self->m_hwnd)) {
                    InvalidateRect(self->m_hwnd, nullptr, FALSE);
                }
            }
        });
        cleanupOcrWorkingSet();
    }).detach();
}

void PinWindow::toggleTextSelectionMode() {
    m_textSelectionMode = !m_textSelectionMode;
    if (!m_textSelectionMode) {
        clearTextSelection();
    }
    const bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{m_textSelectionMode
            ? (isZh ? L"已进入文字划选模式（拖拽选择文字，按 Esc 退出）" : L"Text selection mode (Drag to select, Esc to exit)")
            : (isZh ? L"已退出文字划选模式" : L"Exited text selection mode")});
    render();
}

void PinWindow::clearTextSelection() {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    m_selectedBlockIndices.clear();
    m_hoveredBlockIndex = -1;
    m_isSelectingText = false;
}

void PinWindow::copySelectedText() {
    std::string combined;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (m_selectedBlockIndices.empty()) return;

        std::vector<size_t> sorted = m_selectedBlockIndices;
        std::sort(sorted.begin(), sorted.end(), [this](size_t a, size_t b) {
            if (a >= m_ocrBlocks.size() || b >= m_ocrBlocks.size()) return a < b;
            const auto& ba = m_ocrBlocks[a].origBox;
            const auto& bb = m_ocrBlocks[b].origBox;
            float avgH = (ba.height + bb.height) * 0.5f;
            if (std::abs(ba.y - bb.y) > avgH * 0.45f) return ba.y < bb.y;
            return ba.x < bb.x;
        });

        const PinOcrBlock* prevBlock = nullptr;
        for (size_t idx : sorted) {
            if (idx < m_ocrBlocks.size()) {
                const auto& cur = m_ocrBlocks[idx];
                if (prevBlock) {
                    float avgH = (prevBlock->origBox.height + cur.origBox.height) * 0.5f;
                    bool isSameLine = std::abs(prevBlock->origBox.y - cur.origBox.y) <= avgH * 0.45f;
                    if (isSameLine) {
                        if (!combined.empty() && combined.back() != ' ' && !cur.text.empty() && cur.text.front() != ' ') {
                            combined += " ";
                        }
                    } else {
                        combined += "\r\n";
                    }
                }
                combined += cur.text;
                prevBlock = &cur;
            }
        }
        m_selectedBlockIndices.clear();
    }

    if (!combined.empty()) {
        std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
        tools3000::core::WinUtils::copyToClipboard(wtext);
        bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
        std::wstring preview = wtext.size() > 20 ? (wtext.substr(0, 20) + L"...") : wtext;
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? (L"已复制所选文字: " + preview) : (L"Copied selected text: " + preview)});
    }
    render();
}

void PinWindow::copyAllText() {
    std::string combined;
    bool hasReadyBlocks = false;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (m_ocrReady && !m_ocrBlocks.empty()) {
            hasReadyBlocks = true;
            std::vector<size_t> sorted(m_ocrBlocks.size());
            for (size_t i = 0; i < m_ocrBlocks.size(); ++i) sorted[i] = i;
            std::sort(sorted.begin(), sorted.end(), [this](size_t a, size_t b) {
                const auto& ba = m_ocrBlocks[a].origBox;
                const auto& bb = m_ocrBlocks[b].origBox;
                float avgH = (ba.height + bb.height) * 0.5f;
                if (std::abs(ba.y - bb.y) > avgH * 0.45f) return ba.y < bb.y;
                return ba.x < bb.x;
            });

            const PinOcrBlock* prevBlock = nullptr;
            for (size_t idx : sorted) {
                if (idx < m_ocrBlocks.size()) {
                    const auto& cur = m_ocrBlocks[idx];
                    if (prevBlock) {
                        float avgH = (prevBlock->origBox.height + cur.origBox.height) * 0.5f;
                        bool isSameLine = std::abs(prevBlock->origBox.y - cur.origBox.y) <= avgH * 0.45f;
                        if (isSameLine) {
                            if (!combined.empty() && combined.back() != ' ' && !cur.text.empty() && cur.text.front() != ' ') {
                                combined += " ";
                            }
                        } else {
                            combined += "\r\n";
                        }
                    }
                    combined += cur.text;
                    prevBlock = &cur;
                }
            }
        }
    }

    if (hasReadyBlocks && !combined.empty()) {
        std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
        tools3000::core::WinUtils::copyToClipboard(wtext);
        bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制贴图全部文字" : L"Copied all text from pin"});
    } else {
        extractText();
    }
}

void PinWindow::extractText() {
    if (m_sourceImage.empty()) return;
    cv::Mat imgCopy = m_sourceImage.clone();
    std::thread([imgCopy]() {
        tools3000::core::TraceId::Scope traceScope;
        std::string text = tools3000::ocr::OcrEngine::instance().recognizeToText(imgCopy);
        tools3000::core::MainThreadDispatcher::instance().post([text]() {
            bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
            if (!text.empty()) {
                std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(text);
                tools3000::core::WinUtils::copyToClipboard(wtext);
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"已提取文本并复制到剪贴板" : L"Text extracted and copied to clipboard"});
            } else {
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"未识别到文字" : L"No text recognized"});
            }
        });
        cleanupOcrWorkingSet();
    }).detach();
}

void PinWindow::updateImage(const cv::Mat& newImage) {
    if (newImage.empty() || !m_hwnd) return;
    m_colorBackup.release();
    m_grayscale = false;
    m_inverted = false;
    m_sourceImage = newImage.clone();
    m_origWidth = newImage.cols;
    m_origHeight = newImage.rows;

    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_selectedBlockIndices.clear();
        m_hoveredBlockIndex = -1;
        m_ocrReady = false;
    }
    ensureOcrExecutedAsync();

    const int newW = std::max(1, static_cast<int>(m_origWidth * m_scale));
    const int newH = std::max(1, static_cast<int>(m_origHeight * m_scale));
    if (!m_folded) {
        SetWindowPos(m_hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER);
    }

    createRenderResources(m_sourceImage);
    render();
}

POINT PinWindow::calculateMagneticSnap(HWND currentHwnd, POINT targetPos, int curW, int curH) {
    POINT res = targetPos;
    const HMONITOR monitor = MonitorFromPoint(targetPos, MONITOR_DEFAULTTONEAREST);
    const RECT work = tools3000::core::dpi::workArea(monitor);
    const float scale = tools3000::core::dpi::scaleForMonitor(monitor);
    const int snapDist = tools3000::core::dpi::scaleMetric(12, scale);

    // 1. 吸附到当前显示器工作区四边
    if (std::abs(res.x - work.left) <= snapDist) res.x = work.left;
    else if (std::abs(res.x + curW - work.right) <= snapDist) res.x = work.right - curW;

    if (std::abs(res.y - work.top) <= snapDist) res.y = work.top;
    else if (std::abs(res.y + curH - work.bottom) <= snapDist) res.y = work.bottom - curH;

    // 2. 吸附到其他贴图窗口边缘
    for (const auto& pin : s_instances) {
        if (!pin || !pin->isAlive() || pin->m_hwnd == currentHwnd) continue;
        if (!IsWindowVisible(pin->m_hwnd)) continue;

        RECT otherRc{};
        GetWindowRect(pin->m_hwnd, &otherRc);

        // X 轴吸附：左对齐、右对齐、左贴右、右贴左
        if (std::abs(res.x - otherRc.left) <= snapDist) {
            res.x = otherRc.left;
        } else if (std::abs(res.x + curW - otherRc.right) <= snapDist) {
            res.x = otherRc.right - curW;
        } else if (std::abs(res.x - otherRc.right) <= snapDist) {
            res.x = otherRc.right;
        } else if (std::abs(res.x + curW - otherRc.left) <= snapDist) {
            res.x = otherRc.left - curW;
        }

        // Y 轴吸附：顶对齐、底对齐、顶贴底、底贴顶
        if (std::abs(res.y - otherRc.top) <= snapDist) {
            res.y = otherRc.top;
        } else if (std::abs(res.y + curH - otherRc.bottom) <= snapDist) {
            res.y = otherRc.bottom - curH;
        } else if (std::abs(res.y - otherRc.bottom) <= snapDist) {
            res.y = otherRc.bottom;
        } else if (std::abs(res.y + curH - otherRc.top) <= snapDist) {
            res.y = otherRc.top - curH;
        }
    }

    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// 剪贴板贴图（图像 / 文本 / 颜色 → 浮空贴图）
// ─────────────────────────────────────────────────────────────────────────────

// 解析 #RGB / #RRGGBB 文本为 BGR 颜色（委托至 PinPasteCoordinator 单一事实源）
bool PinWindow::parseHexColor(const std::wstring& in, cv::Scalar& bgr, std::wstring& label) {
    return PinPasteCoordinator::parseHexColor(in, bgr, label);
}

// 颜色 → 色卡贴图（委托至 PinPasteCoordinator 单一事实源）
cv::Mat PinWindow::renderColorSwatch(const cv::Scalar& bgr, const std::wstring& label) {
    return PinPasteCoordinator::renderColorSwatch(bgr, label);
}

// 文本/代码 → Mac 极客风卡片贴图（委托至 PinPasteCoordinator 单一事实源）
cv::Mat PinWindow::renderTextToImage(const std::wstring& text) {
    return PinPasteCoordinator::renderTextToImage(text);
}

POINT PinWindow::calculateSmartSpawnPosition(int imageWidth, int imageHeight, const POINT* fallbackCursor, const CaptureRegion* preferredRegion, int padX, int padY) {
    POINT cursor{};
    if (fallbackCursor) {
        cursor = *fallbackCursor;
    } else {
        GetCursorPos(&cursor);
    }

    POINT refPoint = cursor;
    if (preferredRegion && preferredRegion->width > 0 && preferredRegion->height > 0) {
        refPoint = POINT{preferredRegion->x + preferredRegion->width / 2,
                         preferredRegion->y + preferredRegion->height / 2};
    }

    HMONITOR mon = MonitorFromPoint(refPoint, MONITOR_DEFAULTTONEAREST);
    RECT work = tools3000::core::dpi::workArea(mon);

    int minVx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int minVy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int maxVw = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int maxVh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    RECT vScreen{minVx, minVy, minVx + maxVw, minVy + maxVh};

    PinSpawnInput input;
    input.imageWidth = imageWidth;
    input.imageHeight = imageHeight;
    input.padX = padX;
    input.padY = padY;
    input.cursor = cursor;
    input.workArea = work;
    input.virtualScreen = vScreen;

    if (preferredRegion && preferredRegion->width > 0 && preferredRegion->height > 0) {
        input.preferredRegion = PinSpawnRegion{
            preferredRegion->x, preferredRegion->y,
            preferredRegion->width, preferredRegion->height
        };
    }

    auto lastEntry = CaptureHistory::instance().get(0);
    if (lastEntry.has_value() && !lastEntry->image.empty()) {
        PinHistorySnapshot hist;
        hist.imageWidth = lastEntry->image.cols;
        hist.imageHeight = lastEntry->image.rows;
        hist.region = PinSpawnRegion{
            lastEntry->region.x, lastEntry->region.y,
            lastEntry->region.width, lastEntry->region.height
        };
        input.lastHistory = hist;
    }

    for (const auto& pin : s_instances) {
        if (!pin || !pin->isAlive() || !pin->m_hwnd || !IsWindowVisible(pin->m_hwnd)) continue;
        RECT rc{};
        GetWindowRect(pin->m_hwnd, &rc);
        input.existingPins.push_back(rc);
    }

    POINT spawnPos = PinSpawnCalculator::calculate(input);
    LOG_INFO("PinWindow: 智能计算最佳生成坐标 @ ({}, {})", spawnPos.x, spawnPos.y);
    return spawnPos;
}

std::shared_ptr<PinWindow> PinWindow::createFromClipboard() {
    POINT pt;
    GetCursorPos(&pt);

    std::wstring text;
    bool preferTextOverImage = false;
    {
        bool opened = false;
        for (int retry = 0; retry < 10; ++retry) {
            if (OpenClipboard(nullptr)) {
                opened = true;
                break;
            }
            Sleep(5);
        }
        if (opened) {
            const UINT formatPinMeta = RegisterClipboardFormatW(L"Tools3000_PinMetadata");
            const UINT formatPng = RegisterClipboardFormatW(L"PNG");
            const bool hasPinMeta = (formatPinMeta != 0 && IsClipboardFormatAvailable(formatPinMeta));
            if (!hasPinMeta) {
                for (UINT fmt = EnumClipboardFormats(0); fmt != 0; fmt = EnumClipboardFormats(fmt)) {
                    if (fmt == CF_UNICODETEXT || fmt == CF_TEXT || fmt == CF_OEMTEXT) {
                        preferTextOverImage = true;
                        break;
                    }
                    if ((formatPng != 0 && fmt == formatPng) ||
                        fmt == CF_DIBV5 || fmt == CF_DIB || fmt == CF_BITMAP) {
                        preferTextOverImage = false;
                        break;
                    }
                }
            }
            if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    if (auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
                        text = p;
                        GlobalUnlock(h);
                    }
                }
            }
            CloseClipboard();
        }
    }

    Tools3000PinMetadata meta{};
    meta.magic = 0;
    cv::Mat img;

    auto renderTextOrColor = [&](const std::wstring& s) -> cv::Mat {
        if (s.empty()) return {};
        cv::Scalar c;
        std::wstring hexLabel;
        if (parseHexColor(s, c, hexLabel)) return renderColorSwatch(c, hexLabel);
        return renderTextToImage(s);
    };

    if (preferTextOverImage && !text.empty()) {
        img = renderTextOrColor(text);
    }

    if (img.empty()) {
        img = ClipboardUtils::readImageFromClipboard(&meta);
    }

    if (img.empty() && !text.empty()) {
        img = renderTextOrColor(text);
    }

    if (img.empty()) {
        LOG_INFO("剪贴板贴图：剪贴板无可贴内容");
        return nullptr;
    }

    CaptureRegion reg{};
    const CaptureRegion* pReg = nullptr;
    if (meta.magic == 0x54333030 && meta.regionW > 0 && meta.regionH > 0) {
        reg.x = meta.regionX;
        reg.y = meta.regionY;
        reg.width = meta.regionW;
        reg.height = meta.regionH;
        reg.cornerRadius = meta.cornerRadius;
        pReg = &reg;
    }

    POINT spawnPos = calculateSmartSpawnPosition(img.cols, img.rows, &pt, pReg, meta.padX, meta.padY);
    LOG_INFO("剪贴板贴图：{}x{} @ ({},{})", img.cols, img.rows, spawnPos.x, spawnPos.y);
    return create(img, spawnPos.x, spawnPos.y);
}

std::shared_ptr<PinWindow> PinWindow::pasteNextHistoryOrClipboard() {
    auto decision = PinPasteCoordinator::instance().stepNext();
    if (!decision.has_value()) return nullptr;
    const auto& d = *decision;
    auto pin = create(d.candidate.image, d.spawnPos.x, d.spawnPos.y);

    if (d.totalCount > 1 && d.reverseIndex > 0) {
        bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
        std::wstring toastMsg;
        if (d.isLooped) {
            toastMsg = isZh ? std::format(L"[INFO] 历史贴图: 循环回最新截图 (1/{})", d.totalCount)
                            : std::format(L"[INFO] Pin History: Looped to latest (1/{})", d.totalCount);
        } else {
            toastMsg = isZh ? std::format(L"[INFO] 历史贴图: 倒数第 {} 张 (共 {} 张)", d.targetIndex + 1, d.totalCount)
                            : std::format(L"[INFO] Pin History: #{} from last (total {})", d.targetIndex + 1, d.totalCount);
        }
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{toastMsg});
    }

    return pin;
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口初始化
// ─────────────────────────────────────────────────────────────────────────────

bool PinWindow::initWindow(HINSTANCE hInstance, int x, int y, int w, int h) {
    if (!s_classRegistered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS | 0x00020000 /*CS_DROPSHADOW*/;
        wc.lpfnWndProc = pinWndProc;
        wc.hInstance = hInstance;
        wc.hCursor = LoadCursor(nullptr, IDC_SIZEALL);
        wc.lpszClassName = PIN_CLASS;
        RegisterClassExW(&wc);
        s_classRegistered = true;
    }

    m_hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        PIN_CLASS, L"",
        WS_POPUP,
        x, y, w, h,
        nullptr, nullptr, hInstance, this
    );

    if (!m_hwnd) return false;

    SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    SetPropW(m_hwnd, L"NonRudeHWND", reinterpret_cast<HANDLE>(TRUE));
    SetLayeredWindowAttributes(m_hwnd, 0, 255, LWA_ALPHA);

    return true;
}

bool PinWindow::createRenderResources(const cv::Mat& image) {
    HRESULT hr;

    if (!m_d2dFactory) {
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, m_d2dFactory.GetAddressOf());
        if (FAILED(hr)) return false;
    }

    RECT rc{};
    GetClientRect(m_hwnd, &rc);
    const UINT targetW = static_cast<UINT>((std::max)(1L, rc.right - rc.left));
    const UINT targetH = static_cast<UINT>((std::max)(1L, rc.bottom - rc.top));

    if (!m_renderTarget) {
        auto rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
        );
        auto hwndProps = D2D1::HwndRenderTargetProperties(
            m_hwnd, D2D1::SizeU(targetW, targetH),
            D2D1_PRESENT_OPTIONS_IMMEDIATELY
        );

        hr = m_d2dFactory->CreateHwndRenderTarget(rtProps, hwndProps, m_renderTarget.GetAddressOf());
        if (FAILED(hr)) return false;

        // 禁用 D2D 的自动 DPI 缩放
        m_renderTarget->SetDpi(96.0f, 96.0f);
    } else {
        m_renderTarget->Resize(D2D1::SizeU(targetW, targetH));
    }

    // 转为 BGRA 并创建 D2D Bitmap
    cv::Mat bgra;
    if (image.channels() == 3) {
        cv::cvtColor(image, bgra, cv::COLOR_BGR2BGRA);
    } else if (image.channels() == 4) {
        bgra = image.clone();
        for (int r = 0; r < bgra.rows; ++r) {
            auto* p = bgra.ptr<cv::Vec4b>(r);
            for (int c = 0; c < bgra.cols; ++c) {
                const uchar a = p[c][3];
                if (a == 0) {
                    p[c][0] = 0; p[c][1] = 0; p[c][2] = 0;
                } else if (a < 255) {
                    p[c][0] = static_cast<uchar>((p[c][0] * a) / 255);
                    p[c][1] = static_cast<uchar>((p[c][1] * a) / 255);
                    p[c][2] = static_cast<uchar>((p[c][2] * a) / 255);
                }
            }
        }
    } else {
        return false;
    }

    D2D1_BITMAP_PROPERTIES bitmapProps = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    m_bitmap.Reset();
    hr = m_renderTarget->CreateBitmap(
        D2D1::SizeU(bgra.cols, bgra.rows),
        bgra.data, bgra.cols * 4,
        bitmapProps, m_bitmap.GetAddressOf()
    );

    return SUCCEEDED(hr);
}

void PinWindow::render() {
    if (!m_renderTarget || !m_bitmap) return;

    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    auto size = m_renderTarget->GetSize();
    if (m_folded) {
        const float dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);
        auto& cfg = tools3000::core::ConfigManager::instance();
        const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
        const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);

        ComPtr<ID2D1SolidColorBrush> bgBrush, textBrush, accentBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.13f, 0.16f, 0.94f), bgBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.95f, 0.95f, 0.98f, 0.95f), textBrush.GetAddressOf());
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.95f), accentBrush.GetAddressOf());

        D2D1_ROUNDED_RECT capsule = D2D1::RoundedRect(D2D1::RectF(0, 0, size.width, size.height), 8.0f * dpiScale, 8.0f * dpiScale);
        if (bgBrush) m_renderTarget->FillRoundedRectangle(&capsule, bgBrush.Get());
        if (accentBrush) m_renderTarget->DrawRoundedRectangle(&capsule, accentBrush.Get(), 1.5f * dpiScale);

        // Fold 图标居左
        auto iconRect = D2D1::RectF(6.0f * dpiScale, 6.0f * dpiScale, 30.0f * dpiScale, size.height - 6.0f * dpiScale);
        CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(), CaptureIconId::ActionFold, iconRect, textBrush.Get(), dpiScale);

        // 文字居右
        Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
        if (dwriteFactory && textBrush) {
            Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
            dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * dpiScale, L"zh-cn", fmt.GetAddressOf());
            if (!fmt) {
                dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 12.0f * dpiScale, L"zh-cn", fmt.GetAddressOf());
            }
            if (fmt) {
                fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
                const wchar_t* txt = isZh ? L"贴图 (双击展开)" : L"Pinned (Folded)";
                m_renderTarget->DrawText(txt, static_cast<UINT32>(wcslen(txt)), fmt.Get(), D2D1::RectF(34.0f * dpiScale, 0, size.width - 4.0f * dpiScale, size.height), textBrush.Get());
            }
        }
    } else {
        m_renderTarget->DrawBitmap(m_bitmap.Get(),
                                   D2D1::RectF(0, 0, size.width, size.height));

        // 渲染划选与悬停的文字高亮块 (Live Text Selection Overlay)
        if (m_ocrReady) {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            const float dpiScale = tools3000::core::dpi::scaleForWindow(m_hwnd);
            auto& cfg = tools3000::core::ConfigManager::instance();
            const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
            const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);

            ComPtr<ID2D1SolidColorBrush> hoverFillBrush, hoverStrokeBrush;
            ComPtr<ID2D1SolidColorBrush> selFillBrush, selStrokeBrush;
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.16f), hoverFillBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.50f), hoverStrokeBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.32f), selFillBrush.GetAddressOf());
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.90f), selStrokeBrush.GetAddressOf());

            // 0. 文字划选模式下：为全部未被选中的文字块绘制半透明引导虚光胶囊
            if (m_textSelectionMode) {
                ComPtr<ID2D1SolidColorBrush> guideFillBrush, guideStrokeBrush;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.04f), guideFillBrush.GetAddressOf());
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.22f), guideStrokeBrush.GetAddressOf());
                for (size_t i = 0; i < m_ocrBlocks.size(); ++i) {
                    bool alreadySelected = std::find(m_selectedBlockIndices.begin(), m_selectedBlockIndices.end(), i) != m_selectedBlockIndices.end();
                    if (!alreadySelected && static_cast<int>(i) != m_hoveredBlockIndex) {
                        const auto& b = m_ocrBlocks[i].origBox;
                        D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                        if (guideFillBrush) m_renderTarget->FillRoundedRectangle(&rr, guideFillBrush.Get());
                        if (guideStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, guideStrokeBrush.Get(), 0.8f * dpiScale);
                    }
                }
            }

            // 1. 悬停块高亮
            if (m_hoveredBlockIndex >= 0 && static_cast<size_t>(m_hoveredBlockIndex) < m_ocrBlocks.size()) {
                bool alreadySelected = std::find(m_selectedBlockIndices.begin(), m_selectedBlockIndices.end(),
                                                 static_cast<size_t>(m_hoveredBlockIndex)) != m_selectedBlockIndices.end();
                if (!alreadySelected) {
                    const auto& b = m_ocrBlocks[m_hoveredBlockIndex].origBox;
                    D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                    if (hoverFillBrush) m_renderTarget->FillRoundedRectangle(&rr, hoverFillBrush.Get());
                    if (hoverStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, hoverStrokeBrush.Get(), 1.0f * dpiScale);
                }
            }

            // 2. 选中块高亮
            float minSelY = size.height;
            float maxSelY = 0.0f;
            float centerSelX = size.width / 2.0f;
            for (size_t idx : m_selectedBlockIndices) {
                if (idx < m_ocrBlocks.size()) {
                    const auto& b = m_ocrBlocks[idx].origBox;
                    D2D1_RECT_F r = D2D1::RectF(b.x * m_scale, b.y * m_scale, (b.x + b.width) * m_scale, (b.y + b.height) * m_scale);
                    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(r, 3.0f * dpiScale, 3.0f * dpiScale);
                    if (selFillBrush) m_renderTarget->FillRoundedRectangle(&rr, selFillBrush.Get());
                    if (selStrokeBrush) m_renderTarget->DrawRoundedRectangle(&rr, selStrokeBrush.Get(), 1.5f * dpiScale);
                    if (r.top < minSelY) {
                        minSelY = r.top;
                        centerSelX = (r.left + r.right) / 2.0f;
                    }
                    if (r.bottom > maxSelY) {
                        maxSelY = r.bottom;
                    }
                }
            }

            // 3. 正在拉框划选时的半透明选区
            if (m_isSelectingText) {
                float sx1 = static_cast<float>((std::min)(m_selectStart.x, m_selectEnd.x));
                float sy1 = static_cast<float>((std::min)(m_selectStart.y, m_selectEnd.y));
                float sx2 = static_cast<float>((std::max)(m_selectStart.x, m_selectEnd.x));
                float sy2 = static_cast<float>((std::max)(m_selectStart.y, m_selectEnd.y));
                D2D1_RECT_F selBox = D2D1::RectF(sx1, sy1, sx2, sy2);
                if (hoverFillBrush) m_renderTarget->FillRectangle(&selBox, hoverFillBrush.Get());
                if (selStrokeBrush) m_renderTarget->DrawRectangle(&selBox, selStrokeBrush.Get(), 1.0f * dpiScale);
            }

            // 4. 浮动“复制文字”微晶胶囊（带顶部悬停工具栏重叠避让）
            if (!m_selectedBlockIndices.empty() && !m_isSelectingText) {
                float pillW = 86.0f * dpiScale;
                float pillH = 24.0f * dpiScale;
                float pillX = std::clamp(centerSelX - pillW / 2.0f, 6.0f * dpiScale, size.width - pillW - 6.0f * dpiScale);
                float pillY = minSelY - pillH - 6.0f * dpiScale;
                if (pillY < 6.0f * dpiScale) {
                    pillY = (std::max)(minSelY + 28.0f * dpiScale, maxSelY + 6.0f * dpiScale);
                }
                if (m_hoverAlpha > 0.01f) {
                    const auto tbLayout = PinInputController::computeHoverToolbarLayout(
                        size.width, size.height, dpiScale);
                    const D2D1_RECT_F tb = tbLayout.toolbarRect;
                    const bool overlapsTb =
                        (pillX < tb.right && pillX + pillW > tb.left &&
                         pillY < tb.bottom && pillY + pillH > tb.top);
                    if (overlapsTb) {
                        pillY = (std::max)(maxSelY + 6.0f * dpiScale, tb.bottom + 6.0f * dpiScale);
                    }
                }
                pillY = std::clamp(pillY, 6.0f * dpiScale, (std::max)(6.0f * dpiScale, size.height - pillH - 6.0f * dpiScale));
                m_btnCopyFloatingRect = D2D1::RectF(pillX, pillY, pillX + pillW, pillY + pillH);

                FloatingGlassBar::drawButtonPill(m_renderTarget.Get(), m_btnCopyFloatingRect, 4.0f * dpiScale,
                                                m_hoverCopyFloating, false, false, false, 0.95f, true);

                ComPtr<ID2D1SolidColorBrush> pillTextBrush;
                m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f), pillTextBrush.GetAddressOf());

                // 复制图标居左
                auto iconRect = D2D1::RectF(pillX + 4.0f * dpiScale, pillY + 3.0f * dpiScale,
                                            pillX + 22.0f * dpiScale, pillY + pillH - 3.0f * dpiScale);
                CaptureVectorIcons::renderIcon(m_renderTarget.Get(), m_d2dFactory.Get(),
                                              CaptureIconId::ActionCopy, iconRect, pillTextBrush.Get(), dpiScale);

                // “复制文字”居右
                Microsoft::WRL::ComPtr<IDWriteFactory> dwriteFactory;
                DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwriteFactory.GetAddressOf()));
                if (dwriteFactory && pillTextBrush) {
                    Microsoft::WRL::ComPtr<IDWriteTextFormat> fmt;
                    dwriteFactory->CreateTextFormat(L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dpiScale, L"zh-cn", fmt.GetAddressOf());
                    if (!fmt) {
                        dwriteFactory->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11.0f * dpiScale, L"zh-cn", fmt.GetAddressOf());
                    }
                    if (fmt) {
                        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                        fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                        bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
                        const wchar_t* btnLabel = isZh ? L"复制文字" : L"Copy";
                        m_renderTarget->DrawText(btnLabel, static_cast<UINT32>(wcslen(btnLabel)), fmt.Get(),
                                                 D2D1::RectF(pillX + 24.0f * dpiScale, pillY, pillX + pillW - 2.0f * dpiScale, pillY + pillH),
                                                 pillTextBrush.Get());
                    }
                }
            } else {
                m_btnCopyFloatingRect = D2D1::RectF(0, 0, 0, 0);
            }
        }

        if (m_hoverAlpha > 0.01f && !m_isSelectingText) {
            drawHoverToolbar();
        }

        // 边框：选中态用主题强调色 2px 内描边；幽灵穿透态用微晶青蓝 1.5px 边框；普通态 1px 半透明中性灰
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (m_focused) {
            auto& cfg = tools3000::core::ConfigManager::instance();
            const std::string accent = cfg.get<std::string>("/general/accentColor", "blue");
            const tools3000::core::AccentColorRGB themeRgb = tools3000::core::getAccentColorRGB(accent);
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(themeRgb.r, themeRgb.g, themeRgb.b, 0.95f), borderBrush.GetAddressOf());
            m_renderTarget->DrawRectangle(D2D1::RectF(1, 1, size.width - 1, size.height - 1), borderBrush.Get(), 2.0f);
        } else if (m_clickThrough) {
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.85f, 1.0f, 0.65f), borderBrush.GetAddressOf());
            m_renderTarget->DrawRectangle(D2D1::RectF(0.5f, 0.5f, size.width - 0.5f, size.height - 0.5f), borderBrush.Get(), 1.5f);
        } else {
            m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.5f, 0.5f, 0.5f, 0.5f), borderBrush.GetAddressOf());
            m_renderTarget->DrawRectangle(D2D1::RectF(0, 0, size.width, size.height), borderBrush.Get(), 1.0f);
        }
    }

    const HRESULT hrEnd = m_renderTarget->EndDraw();
    if (hrEnd == D2DERR_RECREATE_TARGET) {
        m_renderTarget.Reset();
        m_bitmap.Reset();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// 窗口过程
// ─────────────────────────────────────────────────────────────────────────────

LRESULT CALLBACK PinWindow::pinWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* self = reinterpret_cast<PinWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
        case WM_MOUSEACTIVATE: {
            if (self) {
                self->focusWindow();
            }
            return MA_ACTIVATE;
        }

        case WM_SYSCOMMAND: {
            if ((wParam & 0xFFF0) == SC_KEYMENU || (wParam & 0xFFF0) == SC_CONTEXTHELP) {
                return 0;
            }
            break;
        }

        case WM_SETCURSOR: {
            if (self && !self->m_folded) {
                // 1. 悬停在浮动复制微晶胶囊或悬浮工具栏按钮上：手型光标 IDC_HAND
                if (self->m_hoverCopyFloating || self->m_hoverFold || self->m_hoverRotate ||
                    self->m_hoverInvert || self->m_hoverEdit || self->m_hoverText ||
                    self->m_hoverCopy || self->m_hoverSave || self->m_hoverClose) {
                    SetCursor(LoadCursor(nullptr, IDC_HAND));
                    return TRUE;
                }

                // 2. Alt 键按下、处于文字划选模式、正在划选或悬停文字块：文本光标 IDC_IBEAM
                bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                if (alt || self->m_textSelectionMode || self->m_isSelectingText || self->m_hoveredBlockIndex >= 0) {
                    SetCursor(LoadCursor(nullptr, IDC_IBEAM));
                    return TRUE;
                }

                // 3. 否则默认平移拖拽光标 IDC_SIZEALL
                SetCursor(LoadCursor(nullptr, IDC_SIZEALL));
                return TRUE;
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (!self) break;
            self->focusWindow();
            int clickX = GET_X_LPARAM(lParam);
            int clickY = GET_Y_LPARAM(lParam);

            // 点击浮动“复制文字”微晶胶囊
            if (!self->m_folded && !self->m_selectedBlockIndices.empty() && self->m_hoverCopyFloating) {
                self->copySelectedText();
                return 0;
            }

            if (!self->m_folded && self->m_hoverAlpha > 0.0f) {
                if (self->m_hoverFold) {
                    self->toggleFold();
                    return 0;
                }
                if (self->m_hoverRotate) {
                    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
                    self->rotate(shift ? 270 : 90);
                    return 0;
                }
                if (self->m_hoverInvert) {
                    self->invertColors();
                    return 0;
                }
                if (self->m_hoverEdit) {
                    self->editMarkup();
                    return 0;
                }
                if (self->m_hoverText) {
                    if (!self->m_selectedBlockIndices.empty()) {
                        self->copySelectedText();
                    } else {
                        self->toggleTextSelectionMode();
                    }
                    return 0;
                }
                if (self->m_hoverCopy) {
                    copyImageToClipboard(self->m_sourceImage);
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{L"已复制贴图到剪贴板"});
                    return 0;
                }
                if (self->m_hoverSave) {
                    savePinnedImage(hwnd, self->m_sourceImage);
                    return 0;
                }
                if (self->m_hoverClose) {
                    self->close();
                    return 0;
                }
            }

            // Alt 键按下 或 处于文字划选模式：在贴图上划选/点选文字
            const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
            const bool canSelectText = alt || self->m_textSelectionMode;
            if (!self->m_folded && canSelectText) {
                self->m_isSelectingText = true;
                self->m_selectStart = {clickX, clickY};
                self->m_selectEnd = {clickX, clickY};
                self->m_selectedBlockIndices.clear();

                // 命中测试当前点击点
                {
                    std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                    float mx = static_cast<float>(clickX) / self->m_scale;
                    float my = static_cast<float>(clickY) / self->m_scale;
                    for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                        if (self->m_ocrBlocks[i].origBox.contains(cv::Point(static_cast<int>(mx), static_cast<int>(my)))) {
                            self->m_selectedBlockIndices.push_back(i);
                            break;
                        }
                    }
                }
                SetCapture(hwnd);
                self->render();
                return 0;
            }

            // 普通点击：如果有选中文字但点击在外部，先清除文字选中
            if (!self->m_selectedBlockIndices.empty()) {
                self->m_selectedBlockIndices.clear();
                self->render();
            }

            self->m_isDragging = true;
            self->m_dragOffset = {clickX, clickY};
            SetCapture(hwnd);
            return 0;
        }

        case WM_SETFOCUS: {
            if (self) { self->m_focused = true; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }

        case WM_KILLFOCUS: {
            if (self) { self->m_focused = false; InvalidateRect(hwnd, nullptr, FALSE); }
            return 0;
        }

        case WM_SYSKEYDOWN:
        case WM_KEYDOWN: {
            if (!self) break;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool alt = (msg == WM_SYSKEYDOWN) || ((GetKeyState(VK_MENU) & 0x8000) != 0);

            const auto decision = PinInputController::resolveKeyDown(wParam, ctrl, shift, alt);
            switch (decision.action) {
                case PinShortcutAction::RotateClockwise90:
                case PinShortcutAction::RotateCounterClockwise90:
                    self->rotate(decision.rotateDegrees);
                    return 0;
                case PinShortcutAction::FlipHorizontal:
                    self->flip(true);
                    return 0;
                case PinShortcutAction::FlipVertical:
                    self->flip(false);
                    return 0;
                case PinShortcutAction::SetOpacity:
                    self->setOpacity(decision.opacityValue);
                    return 0;
                case PinShortcutAction::AdjustOpacity:
                    self->setOpacity(std::clamp(self->m_opacity + decision.opacityDelta,
                                                PinInteractionConstants::kMinPinOpacity,
                                                PinInteractionConstants::kMaxPinOpacity));
                    return 0;
                case PinShortcutAction::ResetScaleAndOpacity:
                    self->resetScale();
                    self->setOpacity(PinInteractionConstants::kMaxPinOpacity);
                    return 0;
                case PinShortcutAction::ZoomIn:
                case PinShortcutAction::ZoomOut:
                    self->setScale(self->m_scale * decision.scaleFactor);
                    return 0;
                case PinShortcutAction::Nudge: {
                    RECT rc{};
                    if (GetWindowRect(hwnd, &rc)) {
                        SetWindowPos(hwnd, nullptr,
                                     rc.left + decision.nudgeDx,
                                     rc.top + decision.nudgeDy,
                                     0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                    }
                    return 0;
                }
                case PinShortcutAction::EditMarkup:
                    self->editMarkup();
                    return 0;
                case PinShortcutAction::InvertColors:
                    self->invertColors();
                    return 0;
                case PinShortcutAction::ToggleGrayscale:
                    self->toggleGrayscale();
                    return 0;
                case PinShortcutAction::ToggleFold:
                    self->toggleFold();
                    return 0;
                case PinShortcutAction::ToggleClickThrough:
                    self->setClickThrough(!self->m_clickThrough);
                    return 0;
                case PinShortcutAction::PasteHistoryOrClipboard:
                    PinWindow::pasteNextHistoryOrClipboard();
                    return 0;
                case PinShortcutAction::CopyAllText:
                    self->copyAllText();
                    return 0;
                case PinShortcutAction::CopySelectedOrImage:
                    if (!self->m_selectedBlockIndices.empty()) {
                        self->copySelectedText();
                        return 0;
                    }
                    copyImageToClipboard(self->m_sourceImage);
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{L"已复制贴图到剪贴板"});
                    return 0;
                case PinShortcutAction::ExtractText:
                    self->extractText();
                    return 0;
                case PinShortcutAction::SaveImage:
                    savePinnedImage(hwnd, self->m_sourceImage);
                    return 0;
                case PinShortcutAction::HistoryPrev: {
                    const int totalHist = CaptureHistory::instance().count();
                    if (totalHist > 1) {
                        self->m_historyBrowseIndex = std::min(self->m_historyBrowseIndex + 1, totalHist - 1);
                        auto entry = CaptureHistory::instance().get(self->m_historyBrowseIndex);
                        if (entry.has_value() && !entry->image.empty()) {
                            self->updateImage(entry->image);
                            bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
                            std::wstring msg = isZh
                                ? std::format(L"[INFO] 历史贴图: 倒数第 {} 张 (共 {} 张)", self->m_historyBrowseIndex + 1, totalHist)
                                : std::format(L"[INFO] Pin History: #{} from last (total {})", self->m_historyBrowseIndex + 1, totalHist);
                            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{msg});
                        }
                    }
                    return 0;
                }
                case PinShortcutAction::HistoryNext: {
                    const int totalHist = CaptureHistory::instance().count();
                    if (totalHist > 1 && self->m_historyBrowseIndex > 0) {
                        self->m_historyBrowseIndex = std::max(0, self->m_historyBrowseIndex - 1);
                        auto entry = CaptureHistory::instance().get(self->m_historyBrowseIndex);
                        if (entry.has_value() && !entry->image.empty()) {
                            self->updateImage(entry->image);
                            bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
                            std::wstring msg = isZh
                                ? std::format(L"[INFO] 历史贴图: 倒数第 {} 张 (共 {} 张)", self->m_historyBrowseIndex + 1, totalHist)
                                : std::format(L"[INFO] Pin History: #{} from last (total {})", self->m_historyBrowseIndex + 1, totalHist);
                            tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{msg});
                        }
                    }
                    return 0;
                }
                case PinShortcutAction::EscapeOrClose:
                    if (!self->m_selectedBlockIndices.empty()) {
                        self->m_selectedBlockIndices.clear();
                        self->render();
                        return 0;
                    }
                    if (self->m_textSelectionMode) {
                        self->m_textSelectionMode = false;
                        self->render();
                        return 0;
                    }
                    self->close();
                    return 0;
                case PinShortcutAction::ClosePin:
                    self->close();
                    return 0;
                case PinShortcutAction::None:
                default:
                    if (alt) return 0;
                    break;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            if (!self) break;
            if (!self->m_isHovering) {
                self->m_isHovering = true;
                self->m_hoverTime = GetTickCount64();
                SetTimer(hwnd, 1, 16, nullptr);
                
                TRACKMOUSEEVENT tme = { sizeof(TRACKMOUSEEVENT), TME_LEAVE, hwnd, HOVER_DEFAULT };
                TrackMouseEvent(&tme);
            }

            // 悬停即获焦（当贴图尚未获焦、不处于鼠标穿透且截图遮罩未打开时），
            // 保障无论是首次移入还是遮罩关闭后鼠标仍在贴图内移动，按键旋转/缩放/关闭均即时生效
            const bool isActuallyFocused = self->m_focused && (GetFocus() == hwnd);
            if (PinInputController::shouldAutoFocusOnHover(
                    isActuallyFocused, self->m_clickThrough, CaptureOverlay::instance().isOverlayVisible())) {
                self->focusWindow();
            }
            
            if (self->m_folded) {
                if (self->m_hoverFold || self->m_hoverRotate || self->m_hoverInvert || self->m_hoverEdit ||
                    self->m_hoverText || self->m_hoverCopy || self->m_hoverSave || self->m_hoverClose) {
                    self->m_hoverFold = false;
                    self->m_hoverRotate = false;
                    self->m_hoverInvert = false;
                    self->m_hoverEdit = false;
                    self->m_hoverText = false;
                    self->m_hoverCopy = false;
                    self->m_hoverSave = false;
                    self->m_hoverClose = false;
                    self->render();
                }
            } else {
                int x = LOWORD(lParam);
                int y = HIWORD(lParam);
                bool hoverFold = (x >= self->m_btnFoldRect.left && x <= self->m_btnFoldRect.right &&
                                  y >= self->m_btnFoldRect.top && y <= self->m_btnFoldRect.bottom);
                bool hoverRotate = (x >= self->m_btnRotateRect.left && x <= self->m_btnRotateRect.right &&
                                    y >= self->m_btnRotateRect.top && y <= self->m_btnRotateRect.bottom);
                bool hoverInvert = (x >= self->m_btnInvertRect.left && x <= self->m_btnInvertRect.right &&
                                    y >= self->m_btnInvertRect.top && y <= self->m_btnInvertRect.bottom);
                bool hoverEdit = (x >= self->m_btnEditRect.left && x <= self->m_btnEditRect.right &&
                                  y >= self->m_btnEditRect.top && y <= self->m_btnEditRect.bottom);
                bool hoverText = (x >= self->m_btnTextRect.left && x <= self->m_btnTextRect.right &&
                                  y >= self->m_btnTextRect.top && y <= self->m_btnTextRect.bottom);
                bool hoverCopy = (x >= self->m_btnCopyRect.left && x <= self->m_btnCopyRect.right &&
                                  y >= self->m_btnCopyRect.top && y <= self->m_btnCopyRect.bottom);
                bool hoverSave = (x >= self->m_btnSaveRect.left && x <= self->m_btnSaveRect.right &&
                                  y >= self->m_btnSaveRect.top && y <= self->m_btnSaveRect.bottom);
                bool hoverClose = (x >= self->m_btnCloseRect.left && x <= self->m_btnCloseRect.right &&
                                   y >= self->m_btnCloseRect.top && y <= self->m_btnCloseRect.bottom);
                bool hoverCopyFloating = (!self->m_selectedBlockIndices.empty() &&
                                          x >= self->m_btnCopyFloatingRect.left && x <= self->m_btnCopyFloatingRect.right &&
                                          y >= self->m_btnCopyFloatingRect.top && y <= self->m_btnCopyFloatingRect.bottom);
                
                if (hoverFold != self->m_hoverFold || hoverRotate != self->m_hoverRotate ||
                    hoverInvert != self->m_hoverInvert || hoverEdit != self->m_hoverEdit ||
                    hoverText != self->m_hoverText || hoverCopy != self->m_hoverCopy ||
                    hoverSave != self->m_hoverSave || hoverClose != self->m_hoverClose ||
                    hoverCopyFloating != self->m_hoverCopyFloating) {
                    self->m_hoverFold = hoverFold;
                    self->m_hoverRotate = hoverRotate;
                    self->m_hoverInvert = hoverInvert;
                    self->m_hoverEdit = hoverEdit;
                    self->m_hoverText = hoverText;
                    self->m_hoverCopy = hoverCopy;
                    self->m_hoverSave = hoverSave;
                    self->m_hoverClose = hoverClose;
                    self->m_hoverCopyFloating = hoverCopyFloating;
                    self->render();
                }

                // 正在拉框划选文字
                if (self->m_isSelectingText) {
                    self->m_selectEnd = {x, y};
                    int sx1 = (std::min)(self->m_selectStart.x, self->m_selectEnd.x);
                    int sy1 = (std::min)(self->m_selectStart.y, self->m_selectEnd.y);
                    int sx2 = (std::max)(self->m_selectStart.x, self->m_selectEnd.x);
                    int sy2 = (std::max)(self->m_selectStart.y, self->m_selectEnd.y);

                    std::vector<size_t> newIndices;
                    {
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                            const auto& b = self->m_ocrBlocks[i].origBox;
                            int bx1 = static_cast<int>(b.x * self->m_scale);
                            int by1 = static_cast<int>(b.y * self->m_scale);
                            int bx2 = static_cast<int>((b.x + b.width) * self->m_scale);
                            int by2 = static_cast<int>((b.y + b.height) * self->m_scale);

                            bool overlap = !(bx2 < sx1 || bx1 > sx2 || by2 < sy1 || by1 > sy2);
                            if (overlap) {
                                newIndices.push_back(i);
                            }
                        }
                    }
                    if (newIndices != self->m_selectedBlockIndices) {
                        self->m_selectedBlockIndices = newIndices;
                    }
                    self->render();
                    return 0;
                }

                // Alt 或处于文字划选模式下：悬停文字块检测
                const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
                const bool canInspect = alt || self->m_textSelectionMode;
                if (canInspect && self->m_ocrReady) {
                    int hovered = -1;
                    {
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        float mx = static_cast<float>(x) / self->m_scale;
                        float my = static_cast<float>(y) / self->m_scale;
                        for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                            if (self->m_ocrBlocks[i].origBox.contains(cv::Point(static_cast<int>(mx), static_cast<int>(my)))) {
                                hovered = static_cast<int>(i);
                                break;
                            }
                        }
                    }
                    if (hovered != self->m_hoveredBlockIndex) {
                        self->m_hoveredBlockIndex = hovered;
                        self->render();
                    }
                } else if (self->m_hoveredBlockIndex >= 0) {
                    self->m_hoveredBlockIndex = -1;
                    self->render();
                }
            }

            if (self->m_isDragging) {
                POINT cursor;
                GetCursorPos(&cursor);
                const float dpiScale = tools3000::core::dpi::scaleForWindow(hwnd);
                int curW = self->m_folded ? static_cast<int>(148.0f * dpiScale) : static_cast<int>(self->m_origWidth * self->m_scale);
                int curH = self->m_folded ? static_cast<int>(38.0f * dpiScale) : static_cast<int>(self->m_origHeight * self->m_scale);
                POINT targetPos = { cursor.x - self->m_dragOffset.x, cursor.y - self->m_dragOffset.y };
                targetPos = calculateMagneticSnap(hwnd, targetPos, curW, curH);
                SetWindowPos(hwnd, nullptr,
                             targetPos.x,
                             targetPos.y,
                             0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }
            return 0;
        }

        case WM_MOUSELEAVE: {
            if (!self) break;
            self->m_isHovering = false;
            self->m_hoverTime = GetTickCount64();
            self->m_hoverFold = false;
            self->m_hoverRotate = false;
            self->m_hoverInvert = false;
            self->m_hoverEdit = false;
            self->m_hoverText = false;
            self->m_hoverCopy = false;
            self->m_hoverSave = false;
            self->m_hoverClose = false;
            self->m_hoverCopyFloating = false;
            self->m_hoveredBlockIndex = -1;
            SetTimer(hwnd, 1, 16, nullptr);
            return 0;
        }

        case WM_TIMER: {
            if (self && wParam == 1) {
                self->updateHoverAnimation();
            }
            return 0;
        }

        case WM_LBUTTONUP: {
            if (self) {
                if (self->m_isSelectingText) {
                    self->m_isSelectingText = false;
                    ReleaseCapture();
                    self->render();
                    return 0;
                }
                self->m_isDragging = false;
                ReleaseCapture();
            }
            return 0;
        }

        case WM_LBUTTONDBLCLK: {
            // 双击：若折叠则展开微晶胶囊，否则进入标注编辑模式
            if (self) {
                if (self->m_folded) {
                    self->toggleFold();
                } else {
                    self->editMarkup();
                }
            }
            return 0;
        }

        case WM_MBUTTONUP: {
            // 鼠标中键单击：快速恢复 1:1 原始物理像素尺寸
            if (self) {
                self->focusWindow();
                self->resetScale();
            }
            return 0;
        }

        case WM_MOUSEWHEEL: {
            if (!self) break;
            self->focusWindow();
            int delta = GET_WHEEL_DELTA_WPARAM(wParam);
            const bool ctrl = (LOWORD(wParam) & MK_CONTROL) != 0 ||
                              ((GetKeyState(VK_CONTROL) & 0x8000) != 0);
            const bool shift = (LOWORD(wParam) & MK_SHIFT) != 0 ||
                               ((GetKeyState(VK_SHIFT) & 0x8000) != 0);

            if (ctrl || shift) {
                // Ctrl/Shift + 滚轮：平滑微调透明度 (10% ~ 100%, 步进 5%)
                float newOpacity = self->m_opacity + (delta > 0 ? 0.05f : -0.05f);
                self->setOpacity(newOpacity);
            } else {
                // 滚轮：以当前鼠标光标为锚点平滑缩放 (0.1x ~ 5.0x)
                POINT cursor;
                GetCursorPos(&cursor);
                RECT rc;
                GetWindowRect(hwnd, &rc);

                float oldScale = self->m_scale;
                float zoomFactor = (delta > 0 ? 1.1f : 0.9f);
                float newScale = std::clamp(oldScale * zoomFactor, 0.1f, 5.0f);

                if (std::abs(newScale - oldScale) > 0.001f) {
                    float relX = (cursor.x - rc.left) / static_cast<float>((std::max)(1L, rc.right - rc.left));
                    float relY = (cursor.y - rc.top) / static_cast<float>((std::max)(1L, rc.bottom - rc.top));

                    int newW = static_cast<int>(self->m_origWidth * newScale);
                    int newH = static_cast<int>(self->m_origHeight * newScale);

                    int newLeft = cursor.x - static_cast<int>(newW * relX);
                    int newTop = cursor.y - static_cast<int>(newH * relY);

                    self->m_scale = newScale;
                    SetWindowPos(hwnd, nullptr, newLeft, newTop, newW, newH, SWP_NOZORDER | SWP_NOACTIVATE);
                    if (self->m_renderTarget) {
                        self->m_renderTarget->Resize(D2D1::SizeU(newW, newH));
                    }
                    self->render();
                }
            }
            return 0;
        }

        case WM_RBUTTONUP: {
            // 右键菜单
            if (!self) break;
            self->focusWindow();
            HMENU menu = CreatePopupMenu();
            bool isZh = tools3000::core::WinUtils::isSystemLanguageChinese();
            AppendMenuW(menu, MF_STRING, 16, isZh ? L"编辑 / 二次标注  (E / 双击)" : L"Edit / Annotate  (E / Double Click)");
            AppendMenuW(menu, (self->m_folded ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 18, isZh ? L"折叠为微晶迷你胶囊  (M)" : L"Fold to Micro Capsule  (M)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, (self->m_inverted ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 19, isZh ? L"色彩反转  (I)" : L"Invert Colors  (I)");
            AppendMenuW(menu, (self->m_grayscale ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 20, isZh ? L"灰度化  (G)" : L"Grayscale  (G)");
            AppendMenuW(menu, (self->m_textSelectionMode ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 23, isZh ? L"文字划选模式  (Alt 划选)" : L"Text Selection Mode  (Alt to select)");
            if (!self->m_selectedBlockIndices.empty()) {
                AppendMenuW(menu, MF_STRING, 21, isZh ? L"复制所选文字  (Ctrl+C)" : L"Copy Selected Text  (Ctrl+C)");
            }
            AppendMenuW(menu, MF_STRING, 22, isZh ? L"复制全部文字  (Ctrl+Shift+C)" : L"Copy All Text  (Ctrl+Shift+C)");
            AppendMenuW(menu, MF_STRING, 1, isZh ? L"复制图片到剪贴板" : L"Copy Image to Clipboard");
            AppendMenuW(menu, MF_STRING, 17, isZh ? L"提取文本 (OCR)  (Ctrl+T)" : L"Extract Text (OCR)  (Ctrl+T)");
            AppendMenuW(menu, MF_STRING, 10, isZh ? L"保存图片...  (Ctrl+S)" : L"Save Image...  (Ctrl+S)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 15, isZh ? L"1:1 原始尺寸  (0 或鼠标中键)" : L"1:1 Original Scale  (0 or MButton)");
            AppendMenuW(menu, MF_STRING, 11, isZh ? L"顺时针旋转 90°  (R / 数字 2)" : L"Rotate Right 90°  (R / Key 2)");
            AppendMenuW(menu, MF_STRING, 12, isZh ? L"逆时针旋转 90°  (Shift+R / L / 数字 1)" : L"Rotate Left 90°  (Shift+R / L / Key 1)");
            AppendMenuW(menu, MF_STRING, 13, isZh ? L"水平翻转  (H / 数字 3)" : L"Flip Horizontal  (H / Key 3)");
            AppendMenuW(menu, MF_STRING, 14, isZh ? L"垂直翻转  (V / 数字 4)" : L"Flip Vertical  (V / Key 4)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 2, isZh ? L"透明度 100%  (数字 0 或 [ / ])" : L"Opacity 100%  (Key 0 or [ / ])");
            AppendMenuW(menu, MF_STRING, 3, isZh ? L"透明度 75%  (数字 7)" : L"Opacity 75%  (Key 7)");
            AppendMenuW(menu, MF_STRING, 4, isZh ? L"透明度 50%  (数字 5)" : L"Opacity 50%  (Key 5)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, (self->m_clickThrough ? MF_CHECKED : MF_UNCHECKED) | MF_STRING, 7, isZh ? L"鼠标穿透 / 幽灵模式  (T / F4)"
                                                                                                       : L"Click-through / Ghost  (T / F4)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 9, isZh ? L"整理全部贴图" : L"Arrange All Pins");
            AppendMenuW(menu, MF_STRING, 8, isZh ? L"隐藏全部贴图  (Ctrl+Alt+Shift+H 显示)"
                                               : L"Hide All Pins  (Ctrl+Alt+Shift+H shows)");
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(menu, MF_STRING, 5, isZh ? L"关闭  (Esc / Delete / Ctrl+W)" : L"Close  (Esc / Delete / Ctrl+W)");
            AppendMenuW(menu, MF_STRING, 6, isZh ? L"关闭全部贴图" : L"Close All Pins");

            POINT pt;
            GetCursorPos(&pt);
            int cmd = TrackPopupMenu(menu, TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, nullptr);
            DestroyMenu(menu);

            switch (cmd) {
                case 16: self->editMarkup(); break;
                case 18: self->toggleFold(); break;
                case 19: self->invertColors(); break;
                case 20: self->toggleGrayscale(); break;
                case 23: self->toggleTextSelectionMode(); break;
                case 21: self->copySelectedText(); break;
                case 22: self->copyAllText(); break;
                case 1: copyImageToClipboard(self->m_sourceImage); break;
                case 17: self->extractText(); break;
                case 10: savePinnedImage(hwnd, self->m_sourceImage); break;
                case 15: self->resetScale(); break;
                case 11: self->rotate(90); break;
                case 12: self->rotate(270); break;
                case 13: self->flip(true); break;
                case 14: self->flip(false); break;
                case 2: self->setOpacity(1.0f); break;
                case 3: self->setOpacity(0.75f); break;
                case 4: self->setOpacity(0.5f); break;
                case 5: self->close(); break;
                case 6: PinWindow::closeAll(); break;
                case 7: self->setClickThrough(!self->m_clickThrough); break;
                case 8: PinWindow::toggleHideAll(); break;
                case 9: PinWindow::arrangeAll(); break;
            }
            return 0;
        }

        case WM_PAINT: {
            if (self) self->render();
            ValidateRect(hwnd, nullptr);
            return 0;
        }

        default:
            return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

}  // namespace tools3000::capture
