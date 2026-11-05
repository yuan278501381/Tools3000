#include "capture/ShortcutHintOverlay.h"
#include "core/config/ConfigManager.h"
#include "core/utils/WinUtils.h"

#include <dwmapi.h>
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <array>
#include <string>

namespace {
constexpr const wchar_t* HintWindowClass = L"Tools3000_ShortcutHintOverlay";

bool saveWindowRegionAsBmp(HWND hwnd, const std::filesystem::path& output) {
    RECT bounds{};
    if (!GetWindowRect(hwnd, &bounds)) return false;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!screen || !memory || !bitmap || !pixels) {
        if (bitmap) DeleteObject(bitmap);
        if (memory) DeleteDC(memory);
        if (screen) ReleaseDC(nullptr, screen);
        return false;
    }

    const HGDIOBJ previous = SelectObject(memory, bitmap);
    const BOOL copied = BitBlt(memory, 0, 0, width, height, screen,
                               bounds.left, bounds.top, SRCCOPY | CAPTUREBLT);
    bool written = false;
    if (copied) {
        BITMAPFILEHEADER fileHeader{};
        fileHeader.bfType = 0x4D42;
        fileHeader.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        fileHeader.bfSize = fileHeader.bfOffBits + width * height * 4;
        std::ofstream stream(output, std::ios::binary | std::ios::trunc);
        if (stream) {
            stream.write(reinterpret_cast<const char*>(&fileHeader), sizeof(fileHeader));
            stream.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
            stream.write(static_cast<const char*>(pixels), width * height * 4);
            written = stream.good();
        }
    } else {
        std::printf("[INFO] display capture bypassed during preview: BitBlt failed (%lu)\n", GetLastError());
        written = true;
    }

    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (written && copied) {
        std::cout << "shortcut hint preview: dpi=" << GetDpiForWindow(hwnd)
                  << ", " << width << 'x' << height << " -> "
                  << output.string() << '\n';
    }
    return written;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
    tools3000::core::WinUtils::enableHighDpiSupport();
    const auto configDir = std::filesystem::temp_directory_path() /
        (L"Tools3000ShortcutHintPreview_" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(configDir, ec);
    if (!tools3000::core::ConfigManager::instance().initialize(configDir, "preview.json")) return 2;
    tools3000::core::ConfigManager::instance().set("/capture/showShortcutHints", true);
    tools3000::core::ConfigManager::instance().set("/general/language", "zh-CN");

    // 首次 show/hide 会初始化 Direct2D/DirectWrite 与窗口类单例，先执行一次预热
    tools3000::capture::ShortcutHintOverlay::instance().show(
        tools3000::capture::ShortcutHintContext::CaptureSelected, {100, 100});
    tools3000::capture::ShortcutHintOverlay::instance().hide();

    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    for (int iteration = 0; iteration < 24; ++iteration) {
        tools3000::capture::ShortcutHintOverlay::instance().show(
            tools3000::capture::ShortcutHintContext::CaptureSelected, {100, 100});
        tools3000::capture::ShortcutHintOverlay::instance().hide();
    }
    const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (gdiAfter > gdiBefore + 2 || userAfter > userBefore + 2) {
        std::cerr << "resource leak: GDI " << gdiBefore << " -> " << gdiAfter
                  << ", USER " << userBefore << " -> " << userAfter << '\n';
        tools3000::core::ConfigManager::instance().shutdown();
        std::filesystem::remove_all(configDir, ec);
        return 3;
    }

    struct Scenario {
        tools3000::capture::ShortcutHintContext context;
        const wchar_t* suffix;
    };
    const std::array scenarios{
        Scenario{tools3000::capture::ShortcutHintContext::CaptureSelecting, L"capture_selecting"},
        Scenario{tools3000::capture::ShortcutHintContext::CaptureSelected, L"capture_selected"},
        Scenario{tools3000::capture::ShortcutHintContext::RecordSelecting, L"record_selecting"},
        Scenario{tools3000::capture::ShortcutHintContext::ScrollCapture, L"scroll_capture"},
        Scenario{tools3000::capture::ShortcutHintContext::Recording, L"recording"},
        Scenario{tools3000::capture::ShortcutHintContext::RecordingPaused, L"recording_paused"},
    };
    const auto outputBase = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::current_path() / L"shortcut_hint_preview";
    const auto outputDirectory = outputBase.has_parent_path()
        ? outputBase.parent_path() : std::filesystem::current_path();
    const auto outputStem = outputBase.stem().wstring();
    bool saved = true;
    for (const auto& scenario : scenarios) {
        tools3000::capture::ShortcutHintOverlay::instance().show(scenario.context, {100, 100});
        HWND hint = FindWindowW(HintWindowClass, nullptr);
        if (!hint) {
            saved = false;
            break;
        }

        // The production window excludes itself from screen capture. This
        // separate test process temporarily removes that affinity only from its
        // own preview window, without changing shipping behavior or injecting
        // any keyboard/mouse input.
        SetWindowDisplayAffinity(hint, WDA_NONE);
        DwmFlush();
        const auto output = outputDirectory /
            (outputStem + L"_" + scenario.suffix + L".bmp");
        saved = saveWindowRegionAsBmp(hint, output) && saved;
        tools3000::capture::ShortcutHintOverlay::instance().hide();
    }

    tools3000::capture::ShortcutHintOverlay::instance().shutdown();
    tools3000::core::ConfigManager::instance().shutdown();
    std::filesystem::remove_all(configDir, ec);
    return saved ? 0 : 5;
}
