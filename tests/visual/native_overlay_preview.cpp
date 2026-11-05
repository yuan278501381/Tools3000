#include "keycast/KeycastOverlay.h"
#include "ocr/OcrResultWindow.h"
#include "ui/ToastOverlay.h"
#include "core/utils/WinUtils.h"

#include <dwmapi.h>
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace {

void pumpMessagesFor(std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        MsgWaitForMultipleObjectsEx(0, nullptr, 8, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

bool saveWindowRegionAsBmp(HWND hwnd, const std::filesystem::path& output) {
    RECT bounds{};
    if (!hwnd || !GetWindowRect(hwnd, &bounds)) return false;
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    if (width <= 0 || height <= 0) return false;

    HDC screen = GetDC(nullptr);
    HDC memory = screen ? CreateCompatibleDC(screen) : nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* pixels = nullptr;
    HBITMAP bitmap = screen ? CreateDIBSection(
        screen, &info, DIB_RGB_COLORS, &pixels, nullptr, 0) : nullptr;
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
        std::cout << output.string() << ": dpi=" << GetDpiForWindow(hwnd)
                  << ", size=" << width << 'x' << height << '\n';
    }
    return written;
}

bool runOverlayCycle(HINSTANCE instance, const std::filesystem::path& directory,
                     const std::wstring& stem, bool isWarmup) {
    bool saved = true;

    auto& toast = tools3000::ui::ToastOverlay::instance();
    if (!toast.initialize(instance)) return false;
    toast.showToast("设置已保存  ·  Ctrl+S");
    pumpMessagesFor(std::chrono::milliseconds(240));
    HWND toastWindow = FindWindowW(L"Tools3000_ToastOverlay", nullptr);
    if (!toastWindow) saved = false;
    if (toastWindow) {
        SetWindowDisplayAffinity(toastWindow, WDA_NONE);
        DwmFlush();
        const auto out = isWarmup ? directory / L"warmup_toast.bmp" : directory / (stem + L"_toast.bmp");
        saved = saveWindowRegionAsBmp(toastWindow, out) && saved;
        if (isWarmup) { std::error_code ec; std::filesystem::remove(out, ec); }
    }
    toast.shutdown();
    pumpMessagesFor(std::chrono::milliseconds(40));

    auto& keycast = tools3000::keycast::KeycastOverlay::instance();
    if (!keycast.init()) return false;
    keycast.pushKey("Ctrl + Shift + S");
    pumpMessagesFor(std::chrono::milliseconds(240));
    HWND keycastWindow = FindWindowW(L"Tools3000_KeycastOverlay", nullptr);
    if (!keycastWindow) saved = false;
    if (keycastWindow) {
        SetWindowDisplayAffinity(keycastWindow, WDA_NONE);
        DwmFlush();
        const auto out = isWarmup ? directory / L"warmup_keycast.bmp" : directory / (stem + L"_keycast.bmp");
        saved = saveWindowRegionAsBmp(keycastWindow, out) && saved;
        if (isWarmup) { std::error_code ec; std::filesystem::remove(out, ec); }
    }
    keycast.cleanup();
    pumpMessagesFor(std::chrono::milliseconds(40));

    auto& ocrResult = tools3000::ocr::OcrResultWindow::instance();
    ocrResult.showResult(
        "Tools3000 已完成离线文字识别。\r\n\r\n"
        "结果窗口会跟随当前显示器的 Windows 缩放比例，并在底部提供克制的快捷键提示。\r\n"
        "Ctrl+C 可以复制全文，滚轮或 PgUp / PgDn 可以浏览较长内容，Esc 可以关闭。");
    pumpMessagesFor(std::chrono::milliseconds(240));
    HWND ocrWindow = FindWindowW(L"Tools3000_OcrResult", nullptr);
    if (!ocrWindow) saved = false;
    if (ocrWindow) {
        SetWindowDisplayAffinity(ocrWindow, WDA_NONE);
        DwmFlush();
        const auto out = isWarmup ? directory / L"warmup_ocr.bmp" : directory / (stem + L"_ocr.bmp");
        saved = saveWindowRegionAsBmp(ocrWindow, out) && saved;
        if (isWarmup) { std::error_code ec; std::filesystem::remove(out, ec); }
    }
    ocrResult.cleanup();
    pumpMessagesFor(std::chrono::milliseconds(100));

    return saved;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    tools3000::core::WinUtils::enableHighDpiSupport();
    const auto outputBase = argc > 1
        ? std::filesystem::path(argv[1])
        : std::filesystem::current_path() / L"native_overlay_preview";
    const auto directory = outputBase.has_parent_path()
        ? outputBase.parent_path() : std::filesystem::current_path();
    const auto stem = outputBase.stem().wstring();
    const HINSTANCE instance = GetModuleHandleW(nullptr);

    // Direct2D / DirectWrite / DWM 驱动与测试夹具在首次及二次运行时会建立进程桌面堆与窗口链表。
    // 先运行两轮预热周期使进程完全进入稳态，然后再对比后续重复周期是否存在真实的资源泄漏。
    runOverlayCycle(instance, directory, stem, true);
    runOverlayCycle(instance, directory, stem, true);

    const DWORD gdiBefore = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userBefore = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);

    const bool saved = runOverlayCycle(instance, directory, stem, false);

    const DWORD gdiAfter = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    const DWORD userAfter = GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    if (gdiAfter > gdiBefore + 3 || userAfter > userBefore + 3) {
        std::cerr << "resource leak: GDI " << gdiBefore << " -> " << gdiAfter
                  << ", USER " << userBefore << " -> " << userAfter << '\n';
        return 4;
    }
    return saved ? 0 : 5;
}
