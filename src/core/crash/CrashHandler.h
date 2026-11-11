#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// CrashHandler — 崩溃收集 (MiniDump)
//
// 在程序异常终止时自动生成 .dmp 文件，便于事后分析崩溃原因。
// 使用 SetUnhandledExceptionFilter 安装全局异常过滤器。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_CRASH_CRASHHANDLER_H
#define TOOLS3000_CORE_CRASH_CRASHHANDLER_H

#include "core/utils/Export.h"

#include <windows.h>
#include <dbghelp.h>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <system_error>

#pragma comment(lib, "dbghelp.lib")

namespace tools3000::core {

class CrashHandler {
public:
    /// 安装崩溃处理器（启动期预先格式化并缓存转储目录至静态定长缓冲区）
    /// @param dumpDir MiniDump 文件输出目录
    static void install(const std::filesystem::path& dumpDir) {
        std::error_code ec;
        std::filesystem::create_directories(dumpDir, ec);

        const std::wstring dirStr = dumpDir.wstring();
        size_t copyLen = dirStr.size();
        while (copyLen > 0 && (dirStr[copyLen - 1] == L'\\' || dirStr[copyLen - 1] == L'/')) {
            --copyLen;
        }
        // 预留至少 64 个宽字符给 "\Tools3000_crash_YYYYMMDD_HHMMSS_pid.dmp\0"
        constexpr size_t kMaxDirChars = MAX_PATH > 68 ? (MAX_PATH - 68) : 0;
        if (copyLen > kMaxDirChars) {
            copyLen = kMaxDirChars;
        }
        for (size_t i = 0; i < copyLen; ++i) {
            s_dumpDir[i] = dirStr[i];
        }
        s_dumpDir[copyLen] = L'\0';

        s_previousFilter = SetUnhandledExceptionFilter(exceptionFilter);
        std::set_terminate(terminateHandler);
    }

    /// 重新锚定全局异常过滤器（防止 WebView2 / 第三方运行时覆盖宿主崩溃处理器）
    static void reinstallFilter() noexcept {
        if (s_dumpDir[0] != L'\0') {
            LPTOP_LEVEL_EXCEPTION_FILTER prev = SetUnhandledExceptionFilter(exceptionFilter);
            if (prev != exceptionFilter) {
                s_previousFilter = prev;
            }
        }
    }

    static void initialize(const std::filesystem::path& dumpDir) {
        install(dumpDir);
    }

    /// 卸载崩溃处理器
    static void uninstall() {
        SetUnhandledExceptionFilter(s_previousFilter);
        s_previousFilter = nullptr;
    }

private:
    static void appendStr(wchar_t* dst, size_t cap, size_t& pos, const wchar_t* src) noexcept {
        if (!dst || cap == 0 || !src) return;
        while (*src != L'\0' && pos + 1 < cap) {
            dst[pos++] = *src++;
        }
        dst[pos] = L'\0';
    }

    static void appendUintFixed(wchar_t* dst, size_t cap, size_t& pos, uint32_t val, size_t width) noexcept {
        if (!dst || cap == 0) return;
        wchar_t tmp[16]{};
        for (size_t i = 0; i < width && i < 15; ++i) {
            tmp[width - 1 - i] = static_cast<wchar_t>(L'0' + (val % 10u));
            val /= 10u;
        }
        tmp[width < 15 ? width : 15] = L'\0';
        appendStr(dst, cap, pos, tmp);
    }

    static void appendUintDec(wchar_t* dst, size_t cap, size_t& pos, uint64_t val) noexcept {
        if (!dst || cap == 0) return;
        wchar_t rev[24]{};
        size_t len = 0;
        do {
            rev[len++] = static_cast<wchar_t>(L'0' + (val % 10u));
            val /= 10u;
        } while (val != 0 && len < 22);
        while (len > 0 && pos + 1 < cap) {
            dst[pos++] = rev[--len];
        }
        dst[pos] = L'\0';
    }

    static void appendHex(wchar_t* dst, size_t cap, size_t& pos, uint64_t val, size_t hexDigits) noexcept {
        if (!dst || cap == 0) return;
        static constexpr wchar_t kHex[] = L"0123456789ABCDEF";
        for (size_t i = 0; i < hexDigits && pos + 1 < cap; ++i) {
            const size_t shift = (hexDigits - 1 - i) * 4;
            const uint64_t nibble = (shift < 64) ? ((val >> shift) & 0xFu) : 0u;
            dst[pos++] = kHex[nibble];
        }
        dst[pos] = L'\0';
    }

    /// 取得包含地址 addr 的模块路径到调用方提供的定长缓冲区（零堆分配）
    static void queryModuleOfAddress(const void* addr, wchar_t* outBuf, size_t outCap) noexcept {
        if (!outBuf || outCap == 0) return;
        outBuf[0] = L'\0';
        if (!addr) return;
        HMODULE hmod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(addr), &hmod) && hmod) {
            const DWORD n = GetModuleFileNameW(hmod, outBuf, static_cast<DWORD>(outCap));
            if (n == 0 || n >= outCap) {
                outBuf[outCap - 1] = L'\0';
            }
        }
    }

    static LONG WINAPI exceptionFilter(EXCEPTION_POINTERS* exceptionInfo) noexcept {
        // 防止并发崩溃线程或重入异常踩踏静态零堆缓冲区
        if (InterlockedCompareExchange(&s_activeCrashLock, 1, 0) != 0) {
            return EXCEPTION_EXECUTE_HANDLER;
        }

        SYSTEMTIME st{};
        GetLocalTime(&st);

        size_t basePos = 0;
        s_dumpPathBuf[0] = L'\0';
        if (s_dumpDir[0] != L'\0') {
            appendStr(s_dumpPathBuf, MAX_PATH, basePos, s_dumpDir);
            appendStr(s_dumpPathBuf, MAX_PATH, basePos, L"\\");
        }
        appendStr(s_dumpPathBuf, MAX_PATH, basePos, L"Tools3000_crash_");
        appendUintFixed(s_dumpPathBuf, MAX_PATH, basePos, st.wYear, 4);
        appendUintFixed(s_dumpPathBuf, MAX_PATH, basePos, st.wMonth, 2);
        appendUintFixed(s_dumpPathBuf, MAX_PATH, basePos, st.wDay, 2);
        appendStr(s_dumpPathBuf, MAX_PATH, basePos, L"_");
        appendUintFixed(s_dumpPathBuf, MAX_PATH, basePos, st.wHour, 2);
        appendUintFixed(s_dumpPathBuf, MAX_PATH, basePos, st.wMinute, 2);
        appendUintFixed(s_dumpPathBuf, MAX_PATH, basePos, st.wSecond, 2);

        size_t txtPos = 0;
        s_txtPathBuf[0] = L'\0';
        appendStr(s_txtPathBuf, MAX_PATH, txtPos, s_dumpPathBuf);
        appendStr(s_txtPathBuf, MAX_PATH, txtPos, L".txt");
        appendStr(s_dumpPathBuf, MAX_PATH, basePos, L".dmp");

        HANDLE hFile = CreateFileW(
            s_dumpPathBuf,
            GENERIC_WRITE,
            0,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );

        if (hFile != INVALID_HANDLE_VALUE) {
            MINIDUMP_EXCEPTION_INFORMATION mdei{};
            mdei.ThreadId = GetCurrentThreadId();
            mdei.ExceptionPointers = exceptionInfo;
            mdei.ClientPointers = FALSE;

            MiniDumpWriteDump(
                GetCurrentProcess(),
                GetCurrentProcessId(),
                hFile,
                static_cast<MINIDUMP_TYPE>(
                    MiniDumpWithHandleData |
                    MiniDumpWithThreadInfo |
                    MiniDumpWithUnloadedModules
                ),
                &mdei,
                nullptr,
                nullptr
            );

            CloseHandle(hFile);
        }

        // 零堆分配、零大栈帧写入可读崩溃摘要 (.txt)，不弹模态 MessageBoxW 防止堆损坏/UI 锁二次挂死
        writeCrashReport(exceptionInfo);

        InterlockedExchange(&s_activeCrashLock, 0);
        return EXCEPTION_EXECUTE_HANDLER;
    }

    /// 写一份可读崩溃摘要到 <dump>.txt（纯静态缓冲 + 裸 Win32 WriteFile，绝对 0 堆分配、0 大栈数组）
    static void writeCrashReport(EXCEPTION_POINTERS* ep) noexcept {
        const DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
        const void* addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        queryModuleOfAddress(addr, s_faultModBuf, MAX_PATH);

        const wchar_t* meaning = L"(其它)";
        switch (code) {
            case 0xC0000005u: meaning = L"ACCESS_VIOLATION (空指针/野指针)"; break;
            case 0xC0000374u: meaning = L"HEAP_CORRUPTION (堆损坏)"; break;
            case 0xC00000FDu: meaning = L"STACK_OVERFLOW (栈溢出/无限递归)"; break;
            case 0xE06D7363u: meaning = L"C++ EXCEPTION (未捕获的 throw)"; break;
            case 0xC0000409u: meaning = L"STACK_BUFFER_OVERRUN / __fastfail"; break;
            default: break;
        }

        s_throwModBuf[0] = L'\0';
        if (code == 0xE06D7363u && ep && ep->ExceptionRecord && ep->ExceptionRecord->NumberParameters >= 4) {
            const auto* base = reinterpret_cast<const void*>(ep->ExceptionRecord->ExceptionInformation[3]);
            queryModuleOfAddress(base, s_throwModBuf, MAX_PATH);
        }

        size_t pos = 0;
        s_reportBuf[0] = L'\0';
        appendStr(s_reportBuf, kReportBufChars, pos,
                  L"Tools3000 崩溃报告\r\n"
                  L"========================================\r\n"
                  L"线程 ID      : ");
        appendUintDec(s_reportBuf, kReportBufChars, pos, GetCurrentThreadId());
        appendStr(s_reportBuf, kReportBufChars, pos, L"\r\n异常代码     : 0x");
        appendHex(s_reportBuf, kReportBufChars, pos, code, 8);
        appendStr(s_reportBuf, kReportBufChars, pos, L"  ");
        appendStr(s_reportBuf, kReportBufChars, pos, meaning);
        appendStr(s_reportBuf, kReportBufChars, pos, L"\r\n故障地址     : 0x");
        appendHex(s_reportBuf, kReportBufChars, pos, reinterpret_cast<uintptr_t>(addr), sizeof(void*) * 2);
        appendStr(s_reportBuf, kReportBufChars, pos, L"\r\n故障模块     : ");
        appendStr(s_reportBuf, kReportBufChars, pos, s_faultModBuf[0] ? s_faultModBuf : L"(未知)");
        appendStr(s_reportBuf, kReportBufChars, pos, L"\r\n抛出模块     : ");
        appendStr(s_reportBuf, kReportBufChars, pos, s_throwModBuf[0] ? s_throwModBuf : L"(不适用/未知)");
        appendStr(s_reportBuf, kReportBufChars, pos, L"\r\n转储文件     : ");
        appendStr(s_reportBuf, kReportBufChars, pos, s_dumpPathBuf);
        appendStr(s_reportBuf, kReportBufChars, pos,
                  L"\r\n========================================\r\n"
                  L"提示: 若“抛出模块”为 msvcp140/Tools3000 等, 多为未捕获的 C++ 异常;\r\n"
                  L"请连同同目录的 tools3000.log(尤其崩溃前几秒) 一并发给开发者。\r\n");

        HANDLE h = CreateFileW(s_txtPathBuf, GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            if (pos > 0) {
                DWORD written = 0;
                WriteFile(h, s_reportBuf, static_cast<DWORD>(pos * sizeof(wchar_t)), &written, nullptr);
            }
            CloseHandle(h);
        }
    }

    static void terminateHandler() noexcept {
        if (InterlockedCompareExchange(&s_activeCrashLock, 1, 0) == 0) {
            SYSTEMTIME st{};
            GetLocalTime(&st);
            size_t txtPos = 0;
            s_txtPathBuf[0] = L'\0';
            if (s_dumpDir[0] != L'\0') {
                appendStr(s_txtPathBuf, MAX_PATH, txtPos, s_dumpDir);
                appendStr(s_txtPathBuf, MAX_PATH, txtPos, L"\\");
            }
            appendStr(s_txtPathBuf, MAX_PATH, txtPos, L"Tools3000_terminate_");
            appendUintFixed(s_txtPathBuf, MAX_PATH, txtPos, st.wYear, 4);
            appendUintFixed(s_txtPathBuf, MAX_PATH, txtPos, st.wMonth, 2);
            appendUintFixed(s_txtPathBuf, MAX_PATH, txtPos, st.wDay, 2);
            appendStr(s_txtPathBuf, MAX_PATH, txtPos, L"_");
            appendUintFixed(s_txtPathBuf, MAX_PATH, txtPos, st.wHour, 2);
            appendUintFixed(s_txtPathBuf, MAX_PATH, txtPos, st.wMinute, 2);
            appendUintFixed(s_txtPathBuf, MAX_PATH, txtPos, st.wSecond, 2);
            appendStr(s_txtPathBuf, MAX_PATH, txtPos, L".txt");

            size_t pos = 0;
            s_reportBuf[0] = L'\0';
            appendStr(s_reportBuf, kReportBufChars, pos,
                      L"Tools3000 std::terminate 报告\r\n"
                      L"========================================\r\n"
                      L"线程 ID      : ");
            appendUintDec(s_reportBuf, kReportBufChars, pos, GetCurrentThreadId());
            appendStr(s_reportBuf, kReportBufChars, pos,
                      L"\r\n原因         : std::terminate() triggered (noexcept boundary or joinable std::thread destruction)\r\n");

            HANDLE h = CreateFileW(s_txtPathBuf, GENERIC_WRITE, 0, nullptr,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                if (pos > 0) {
                    DWORD written = 0;
                    WriteFile(h, s_reportBuf, static_cast<DWORD>(pos * sizeof(wchar_t)), &written, nullptr);
                }
                CloseHandle(h);
            }
            InterlockedExchange(&s_activeCrashLock, 0);
        }
        std::abort();
    }

    static constexpr size_t kReportBufChars = 2048;
    static inline wchar_t s_dumpDir[MAX_PATH]{};
    static inline wchar_t s_dumpPathBuf[MAX_PATH]{};
    static inline wchar_t s_txtPathBuf[MAX_PATH]{};
    static inline wchar_t s_faultModBuf[MAX_PATH]{};
    static inline wchar_t s_throwModBuf[MAX_PATH]{};
    static inline wchar_t s_reportBuf[kReportBufChars]{};
    static inline volatile LONG s_activeCrashLock = 0;
    static inline LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_CRASH_CRASHHANDLER_H
