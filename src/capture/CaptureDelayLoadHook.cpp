// ─────────────────────────────────────────────────────────────────────────────
// CaptureDelayLoadHook.cpp — Plugin_Capture.dll 延迟加载安全寻址钩子
//
// 职责:
//   Plugin_Capture.dll 位于 plugins/ 子目录，而 FFmpeg / OpenCV / WebP 依赖位于
//   主程序可执行文件根目录。通过 MSVC /DELAYLOAD 通知钩子显式限定从应用目录与
//   系统默认目录加载，在冷启动免映射 ~47 MB 重型 DLL 的同时杜绝 CWD DLL 劫持。
// ─────────────────────────────────────────────────────────────────────────────

#ifdef _MSC_VER

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <delayimp.h>

namespace {

FARPROC WINAPI captureDelayLoadNotifyHook(unsigned dliNotify, PDelayLoadInfo pdli) {
    if (dliNotify != dliNotePreLoadLibrary || !pdli || !pdli->szDll || pdli->szDll[0] == '\0') {
        return nullptr;
    }

    // 优先在 exe 根目录（LOAD_LIBRARY_SEARCH_APPLICATION_DIR）及系统安全目录解析，
    // 兼顾主程序与 --worker=* 子进程在任意工作目录下的稳定按需加载（纯 Unicode LoadLibraryExW）。
    wchar_t wideDll[MAX_PATH]{};
    const int converted = MultiByteToWideChar(
        CP_UTF8, 0, pdli->szDll, -1, wideDll, static_cast<int>(sizeof(wideDll) / sizeof(wideDll[0])));
    if (converted <= 0) {
        return nullptr;
    }
    HMODULE mod = LoadLibraryExW(
        wideDll,
        nullptr,
        LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (mod) {
        return reinterpret_cast<FARPROC>(mod);
    }

    // 回退到默认 delayimp 逻辑（例如开发态 bin/Debug 或自定义 PATH）
    return nullptr;
}

}  // namespace

ExternC const PfnDliHook __pfnDliNotifyHook2 = captureDelayLoadNotifyHook;

#endif  // _MSC_VER
