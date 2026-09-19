#include "gesture/RadialMenuOverlay.h"
#include "gesture/RadialMenuStyle.h"
#include "gesture/BuiltinCommands.h"
#include "core/logger/Logger.h"
#include "core/ipc/MessageBridge.h"
#include "core/utils/DpiUtils.h"
#include "core/utils/WinUtils.h"
#include "core/events/MainThreadDispatcher.h"
#include <cmath>
#include <algorithm>
#include <charconv>
#include <unordered_map>

namespace tools3000::gesture {

static const wchar_t* CLASS_NAME = L"Tools3000_RadialMenu";
const UINT_PTR TIMER_ID_ANIMATION = 1001;
constexpr float PI_F = 3.14159265358979323846f;

// 缓动函数
static float easeOutBack(float x) {
    const float c1 = 1.70158f;
    const float c3 = c1 + 1.0f;
    const float delta = x - 1.0f;
    return 1.0f + c3 * delta * delta * delta + c1 * delta * delta;
}

static float easeOutQuad(float x) {
    return 1.0f - (1.0f - x) * (1.0f - x);
}

RadialMenuOverlay& RadialMenuOverlay::instance() {
    static RadialMenuOverlay inst;
    return inst;
}

RadialMenuOverlay::RadialMenuOverlay() {
    registerWindowClass();
}

RadialMenuOverlay::~RadialMenuOverlay() {
    hide();
    discardResources();
    if (m_hBitmap) {
        DeleteObject(m_hBitmap);
    }
    if (m_hwnd) {
        DestroyWindow(m_hwnd);
    }
    if (m_helperOwnerHwnd) {
        DestroyWindow(m_helperOwnerHwnd);
    }
}

void RadialMenuOverlay::registerWindowClass() {
    WNDCLASSEXW wc = {sizeof(WNDCLASSEXW)};
    wc.lpfnWndProc = windowProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);
}

void RadialMenuOverlay::setItems(const std::vector<RadialMenuItem>& items) {
    m_items = items;
    m_hoverRadii.resize(items.size(), 0.0f);
}

void RadialMenuOverlay::show(POINT centerPt) {
    if (m_items.empty()) return;

    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this, centerPt]() {
            show(centerPt);
        });
        return;
    }

    const float newScale = tools3000::core::dpi::scaleAtPoint(centerPt);
    const auto metrics = RadialMenuStyle::metricsForDpi(
        tools3000::core::dpi::effectiveDpiForMonitor(
            tools3000::core::dpi::monitorAtPoint(centerPt)));
    const int newWindowSize = metrics.windowSize;
    if (std::abs(newScale - m_dpiScale) >= 0.01f || newWindowSize != m_windowSize) {
        discardResources();
        if (m_hBitmap) {
            DeleteObject(m_hBitmap);
            m_hBitmap = nullptr;
            m_bits = nullptr;
        }
        m_dpiScale = newScale;
        m_windowSize = newWindowSize;
    }
    m_radiusOuter = metrics.outerRadius;
    m_radiusInner = metrics.innerRadius;
    m_centerPt = centerPt;
    m_hoverIndex = -1;
    m_popScale = 0.0f;
    m_showStartTime = GetTickCount();
    m_lastTickTime = m_showStartTime;
    std::fill(m_hoverRadii.begin(), m_hoverRadii.end(), 0.0f);

    if (!m_hwnd) {
        HINSTANCE hInst = GetModuleHandle(nullptr);
        m_helperOwnerHwnd = tools3000::core::WinUtils::createOverlayHelperOwner(hInst, L"Tools3000_RadialMenuHelperOwner");

        m_hwnd = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            CLASS_NAME, L"RadialMenu",
            WS_POPUP,
            centerPt.x - m_windowSize / 2, centerPt.y - m_windowSize / 2,
            m_windowSize, m_windowSize,
            m_helperOwnerHwnd, nullptr, hInst, this
        );
        if (m_hwnd) {
            tools3000::core::WinUtils::applyTaskbarSafeOverlayStyle(m_hwnd, false);
            SetWindowDisplayAffinity(m_hwnd, WDA_NONE);
        }
    }

    if (!m_hwnd || !createResources()) {
        LOG_ERROR("手势轮盘高 DPI 渲染资源创建失败");
        return;
    }

    SetWindowPos(m_hwnd, HWND_TOPMOST, 
                 centerPt.x - m_windowSize / 2, centerPt.y - m_windowSize / 2,
                 m_windowSize, m_windowSize, SWP_SHOWWINDOW | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    
    // 捕获鼠标
    SetCapture(m_hwnd);
    m_visible = true;
    m_timerId = SetTimer(m_hwnd, TIMER_ID_ANIMATION, 16, nullptr); // ~60 FPS
    
    render();
}

void RadialMenuOverlay::hide() {
    if (tools3000::core::MainThreadDispatcher::instance().isInitialized() &&
        !tools3000::core::MainThreadDispatcher::instance().isOwnerThread()) {
        tools3000::core::MainThreadDispatcher::instance().post([this]() {
            hide();
        });
        return;
    }

    if (m_visible) {
        if (m_timerId) {
            KillTimer(m_hwnd, TIMER_ID_ANIMATION);
            m_timerId = 0;
        }
        if (GetCapture() == m_hwnd) {
            ReleaseCapture();
        }
        ShowWindow(m_hwnd, SW_HIDE);
        m_visible = false;
        m_hoverIndex = -1;
        discardResources();
        if (m_hBitmap) {
            DeleteObject(m_hBitmap);
            m_hBitmap = nullptr;
            m_bits = nullptr;
        }
    }
}

bool RadialMenuOverlay::createResources() {
    if (!m_d2dFactory) {
        D2D1_FACTORY_OPTIONS options = {};
        if (FAILED(D2D1CreateFactory(
                D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                &options, reinterpret_cast<void**>(m_d2dFactory.GetAddressOf())))) {
            return false;
        }
    }
    if (!m_dwriteFactory && FAILED(DWriteCreateFactory(
            DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
            reinterpret_cast<IUnknown**>(m_dwriteFactory.GetAddressOf())))) {
        return false;
    }

    if (!m_renderTarget) {
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            0, 0, D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE, D2D1_FEATURE_LEVEL_DEFAULT
        );
        if (FAILED(m_d2dFactory->CreateDCRenderTarget(&props, &m_renderTarget)) ||
            !m_renderTarget) {
            return false;
        }
        m_renderTarget->SetDpi(96.0f, 96.0f);

        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.08f, 0.1f, 0.13f, 0.85f), &m_bgBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(0.15f, 0.45f, 0.9f, 0.95f), &m_hoverBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &m_textBrush);
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.15f), &m_borderBrush);

        if (FAILED(m_dwriteFactory->CreateTextFormat(
            L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            RadialMenuStyle::BaseFontSize * m_dpiScale, L"en-us", &m_textFormat
        )) || !m_textFormat) return false;
        m_textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        m_textFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }

    if (!m_hBitmap) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = m_windowSize;
        bmi.bmiHeader.biHeight = -m_windowSize;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        m_hBitmap = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &m_bits, nullptr, 0);
    }
    return m_renderTarget && m_bgBrush && m_hoverBrush && m_textBrush &&
           m_borderBrush && m_textFormat && m_hBitmap && m_bits;
}

void RadialMenuOverlay::discardResources() {
    m_renderTarget.Reset();
    m_bgBrush.Reset();
    m_hoverBrush.Reset();
    m_textBrush.Reset();
    m_borderBrush.Reset();
    m_textFormat.Reset();
}

void RadialMenuOverlay::render() {
    if (!m_hwnd) return;
    if (!createResources()) return;

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) return;
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(nullptr, hdcScreen);
        return;
    }
    HGDIOBJ hOldBmp = SelectObject(hdcMem, m_hBitmap);

    RECT rc = {0, 0, m_windowSize, m_windowSize};
    m_renderTarget->BindDC(hdcMem, &rc);
    m_renderTarget->BeginDraw();
    m_renderTarget->Clear(D2D1::ColorF(0, 0, 0, 0));

    // 缩放矩阵 (出场动画)
    float cx = m_windowSize / 2.0f;
    float cy = m_windowSize / 2.0f;
    m_renderTarget->SetTransform(D2D1::Matrix3x2F::Scale(m_popScale, m_popScale, D2D1::Point2F(cx, cy)));

    // 中心取消区域反馈
    if (m_hoverIndex == -1) {
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> centerBrush;
        m_renderTarget->CreateSolidColorBrush(D2D1::ColorF(1.0f, 0.4f, 0.4f, 0.5f), &centerBrush); // 淡红取消区
        const float innerRadius = static_cast<float>(m_radiusInner);
        m_renderTarget->FillEllipse(
            D2D1::Ellipse(D2D1::Point2F(cx, cy), innerRadius, innerRadius),
            centerBrush.Get());
    }

    // 绘制轮盘
    int numItems = static_cast<int>(m_items.size());
    if (numItems > 0) {
        float angleStep = 360.0f / numItems;
        float offsetAngle = -90.0f - (angleStep / 2.0f);

        for (int i = 0; i < numItems; ++i) {
            float startAngle = offsetAngle + i * angleStep;
            float endAngle = startAngle + angleStep;
            
            float startRad = startAngle * PI_F / 180.0f;
            float endRad = endAngle * PI_F / 180.0f;

            // 应用当前扇区的额外扩展半径 (悬停动画)
            float outerR = m_radiusOuter + m_hoverRadii[i];

            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geometry;
            if (FAILED(m_d2dFactory->CreatePathGeometry(&geometry)) || !geometry) continue;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(geometry->Open(&sink)) || !sink) continue;

            const float innerRadius = static_cast<float>(m_radiusInner);
            D2D1_POINT_2F ptInnerStart = D2D1::Point2F(cx + innerRadius * std::cos(startRad), cy + innerRadius * std::sin(startRad));
            D2D1_POINT_2F ptOuterStart = D2D1::Point2F(cx + outerR * std::cos(startRad), cy + outerR * std::sin(startRad));
            D2D1_POINT_2F ptOuterEnd = D2D1::Point2F(cx + outerR * std::cos(endRad), cy + outerR * std::sin(endRad));
            D2D1_POINT_2F ptInnerEnd = D2D1::Point2F(cx + innerRadius * std::cos(endRad), cy + innerRadius * std::sin(endRad));

            sink->BeginFigure(ptInnerStart, D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(ptOuterStart);
            sink->AddArc(D2D1::ArcSegment(ptOuterEnd, D2D1::SizeF(outerR, outerR), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->AddLine(ptInnerEnd);
            sink->AddArc(D2D1::ArcSegment(ptInnerStart, D2D1::SizeF(innerRadius, innerRadius), 0.0f, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            if (FAILED(sink->Close())) continue;

            // 填充背景
            if (i == m_hoverIndex) {
                m_renderTarget->FillGeometry(geometry.Get(), m_hoverBrush.Get());
            } else {
                m_renderTarget->FillGeometry(geometry.Get(), m_bgBrush.Get());
            }
            m_renderTarget->DrawGeometry(
                geometry.Get(), m_borderBrush.Get(), 1.5f * m_dpiScale);

            // 绘制文字
            float midRad = (startRad + endRad) / 2.0f;
            float textRadius = m_radiusInner + (outerR - m_radiusInner) / 2.0f;
            D2D1_RECT_F textRect = D2D1::RectF(
                cx + textRadius * std::cos(midRad) - 50.0f * m_dpiScale,
                cy + textRadius * std::sin(midRad) - 20.0f * m_dpiScale,
                cx + textRadius * std::cos(midRad) + 50.0f * m_dpiScale,
                cy + textRadius * std::sin(midRad) + 20.0f * m_dpiScale
            );
            
            std::wstring wLabel = tools3000::core::WinUtils::utf8ToWstring(m_items[i].label);
            m_renderTarget->DrawTextW(wLabel.c_str(), static_cast<UINT32>(wLabel.length()),
                                      m_textFormat.Get(), textRect, m_textBrush.Get());
        }
    }

    if (FAILED(m_renderTarget->EndDraw())) {
        discardResources();
        SelectObject(hdcMem, hOldBmp);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return;
    }

    updateLayeredWindow(hdcScreen, hdcMem);

    SelectObject(hdcMem, hOldBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
}

void RadialMenuOverlay::updateLayeredWindow(HDC hdcScreen, HDC hdcMem) {
    if (!m_hwnd || !hdcScreen || !hdcMem || !m_hBitmap) return;
    POINT ptSrc = {0, 0};
    SIZE size = {m_windowSize, m_windowSize};
    
    // 透明度由 m_popScale 控制，产生淡入效果
    BYTE alpha = static_cast<BYTE>(std::clamp(m_popScale * 255.0f, 0.0f, 255.0f));
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA};

    UpdateLayeredWindow(m_hwnd, hdcScreen, nullptr, &size, hdcMem, &ptSrc, 0, &blend, ULW_ALPHA);
}

int RadialMenuOverlay::hitTest(POINT pt) {
    if (m_items.empty()) return -1;
    
    float dx = pt.x - (m_windowSize / 2.0f);
    float dy = pt.y - (m_windowSize / 2.0f);
    float dist = sqrt(dx * dx + dy * dy);

    // 如果鼠标位于内圆中心区，视为取消 (不选中任何菜单)
    if (dist <= m_radiusInner) {
        return -1;
    }

    // 容错度升级: 即使外圈无限长，只要超出了内圈，就锁定对应角度的扇区
    // 不再限制 dist > m_radiusOuter 会取消选中。
    float angle = std::atan2(dy, dx) * 180.0f / PI_F; // -180 ~ 180
    
    int numItems = static_cast<int>(m_items.size());
    float angleStep = 360.0f / numItems;
    float offsetAngle = -90.0f - (angleStep / 2.0f);

    float normAngle = fmod(angle - offsetAngle + 360.0f * 2.0f, 360.0f);
    int index = static_cast<int>(normAngle / angleStep) % numItems;
    return index;
}

void RadialMenuOverlay::executeAction(int index) {
    if (index >= 0 && index < static_cast<int>(m_items.size())) {
        const std::string cmd = m_items[index].command;
        LOG_INFO("RadialMenu: Execute command '{}'", cmd);

        // 兼容早期的可读字符串，并以数值索引作为当前稳定格式。
        static const std::unordered_map<std::string, BuiltinCommand> legacyCommands = {
            {"capture", BuiltinCommand::TakeScreenshot},
            {"search", BuiltinCommand::ToggleSearch},
            {"lock", BuiltinCommand::LockScreen},
            {"pin", BuiltinCommand::PasteAsPin},
            {"record", BuiltinCommand::StartRecording},
        };
        if (const auto legacy = legacyCommands.find(cmd); legacy != legacyCommands.end()) {
            BuiltinCommandDispatcher::instance().execute(legacy->second);
            return;
        }
        int commandIndex = -1;
        const auto [end, error] = std::from_chars(
            cmd.data(), cmd.data() + cmd.size(), commandIndex);
        if (error == std::errc{} && end == cmd.data() + cmd.size() && commandIndex >= 0 &&
            commandIndex <= static_cast<int>(BuiltinCommand::PasteAsPin)) {
            BuiltinCommandDispatcher::instance().execute(
                static_cast<BuiltinCommand>(commandIndex));
            return;
        }

        // 仅为旧配置保留结构化 IPC 消息兼容；普通文本不再交给 JSON 解析器。
        if (!cmd.empty() && cmd.front() == '{') {
            tools3000::core::MessageBridge::instance().handleMessage(cmd);
        } else {
            LOG_WARN("RadialMenu: 忽略未知命令 '{}'", cmd);
        }
    }
}

LRESULT CALLBACK RadialMenuOverlay::windowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    RadialMenuOverlay* pThis = nullptr;
    if (uMsg == WM_NCCREATE) {
        CREATESTRUCT* pCreate = reinterpret_cast<CREATESTRUCT*>(lParam);
        pThis = reinterpret_cast<RadialMenuOverlay*>(pCreate->lpCreateParams);
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<RadialMenuOverlay*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->handleMessage(uMsg, wParam, lParam);
    }
    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

LRESULT RadialMenuOverlay::handleMessage(UINT uMsg, WPARAM wParam, LPARAM lParam) {
    switch (uMsg) {
        case WM_TIMER: {
            if (wParam == TIMER_ID_ANIMATION) {
                bool needsRender = false;
                DWORD now = GetTickCount();
                float dt = (now - m_lastTickTime) / 1000.0f;
                m_lastTickTime = now;

                // 1. 出场动画 (0.0 -> 1.0)
                float elapsedShow = (now - m_showStartTime) / 1000.0f;
                if (elapsedShow < 0.2f) { // 200ms
                    float progress = std::clamp(elapsedShow / 0.2f, 0.0f, 1.0f);
                    m_popScale = easeOutBack(progress);
                    needsRender = true;
                } else if (m_popScale != 1.0f) {
                    m_popScale = 1.0f;
                    needsRender = true;
                }

                // 2. 悬停动画 (半径伸缩)
                for (size_t i = 0; i < m_items.size(); ++i) {
                    float targetRadii = (i == m_hoverIndex)
                        ? 15.0f * m_dpiScale : 0.0f;
                    if (std::abs(m_hoverRadii[i] - targetRadii) > 0.5f * m_dpiScale) {
                        m_hoverRadii[i] += (targetRadii - m_hoverRadii[i]) * 15.0f * dt; // 平滑差值
                        needsRender = true;
                    } else {
                        m_hoverRadii[i] = targetRadii;
                    }
                }

                if (needsRender) {
                    render();
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt;
            pt.x = static_cast<short>(LOWORD(lParam));
            pt.y = static_cast<short>(HIWORD(lParam));
            int newHover = hitTest(pt);
            if (newHover != m_hoverIndex) {
                m_hoverIndex = newHover;
                // 注意这里不需要立刻 render()，因为 WM_TIMER 的 60FPS 会自动拾取 hoverIndex 并平滑动画过去
            }
            return 0;
        }
        case WM_MBUTTONUP:
        case WM_LBUTTONUP:
        case WM_RBUTTONUP: {
            int finalIndex = m_hoverIndex;
            hide();
            if (finalIndex != -1) {
                executeAction(finalIndex);
            }
            return 0;
        }
        case WM_KILLFOCUS: {
            hide();
            return 0;
        }
        case WM_CAPTURECHANGED: {
            if (reinterpret_cast<HWND>(lParam) != m_hwnd) {
                hide();
            }
            return 0;
        }
    }
    return DefWindowProc(m_hwnd, uMsg, wParam, lParam);
}

} // namespace tools3000::gesture
