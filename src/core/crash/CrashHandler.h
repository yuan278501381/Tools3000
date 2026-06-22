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
#include <string>
#include <filesystem>
#include <chrono>
#include <format>
#include <cstdio>

#pragma comment(lib, "dbghelp.lib")

namespace tools3000::core {

class CrashHandler {
public:
    /// 安装崩溃处理器
    /// @param dumpDir MiniDump 文件输出目录
    static void install(const std::filesystem::path& dumpDir) {
        s_dumpDir = dumpDir;
        std::filesystem::create_directories(dumpDir);
        s_previousFilter = SetUnhandledExceptionFilter(exceptionFilter);
    }

    /// 卸载崩溃处理器
    static void uninstall() {
        SetUnhandledExceptionFilter(s_previousFilter);
        s_previousFilter = nullptr;
    }

private:
    static LONG WINAPI exceptionFilter(EXCEPTION_POINTERS* exceptionInfo) {
        // 生成带时间戳的 dump 文件名
        auto now = std::chrono::system_clock::now();
        auto timeT = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_s(&tm, &timeT);

        auto fileName = std::format(L"Tools3000_crash_{:04d}{:02d}{:02d}_{:02d}{:02d}{:02d}.dmp",
                                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                                    tm.tm_hour, tm.tm_min, tm.tm_sec);

        auto dumpPath = s_dumpDir / fileName;

        HANDLE hFile = CreateFileW(
            dumpPath.wstring().c_str(),
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

            // 写入 MiniDump（包含线程、模块、句柄信息）
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

        // 同时写一份可读的崩溃摘要 (.txt): 异常码 + 故障模块 + (C++ 异常)抛出模块。
        // 用裸 Win32 写入, 不分配堆内存, 即使堆损坏也能落盘。
        writeCrashReport(dumpPath, exceptionInfo);

        // 弹窗通知用户
        auto msg = std::format(
            L"Tools3000 遇到了意外错误，已生成崩溃转储与报告：\n\n{}\n\n请将该文件夹中的 .dmp 与 .txt 发送给开发者。",
            dumpPath.wstring()
        );
        MessageBoxW(nullptr, msg.c_str(), L"Tools3000 崩溃报告", MB_OK | MB_ICONERROR);

        return EXCEPTION_EXECUTE_HANDLER;
    }

    /// 取得包含地址 addr 的模块文件名 (失败返回空串)
    static std::wstring moduleOfAddress(const void* addr) {
        HMODULE hmod = nullptr;
        if (GetModuleHandleExW(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(addr), &hmod) && hmod) {
            wchar_t path[MAX_PATH] = {};
            if (GetModuleFileNameW(hmod, path, MAX_PATH)) return path;
        }
        return {};
    }

    /// 写一份可读崩溃摘要到 <dump>.txt (裸 Win32, 不分配堆)
    static void writeCrashReport(const std::filesystem::path& dumpPath, EXCEPTION_POINTERS* ep) {
        auto txtPath = std::filesystem::path(dumpPath).replace_extension(L".txt");

        const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
        const void* addr = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr;
        std::wstring faultMod = moduleOfAddress(addr);

        const wchar_t* meaning = L"(其它)";
        switch (code) {
            case 0xC0000005: meaning = L"ACCESS_VIOLATION (空指针/野指针)"; break;
            case 0xC0000374: meaning = L"HEAP_CORRUPTION (堆损坏)"; break;
            case 0xC00000FD: meaning = L"STACK_OVERFLOW (栈溢出/无限递归)"; break;
            case 0xE06D7363: meaning = L"C++ EXCEPTION (未捕获的 throw)"; break;
            case 0xC0000409: meaning = L"STACK_BUFFER_OVERRUN / __fastfail"; break;
            default: break;
        }

        // 对 C++ 异常: ExceptionInformation[3] = 抛出异常的模块基址
        std::wstring throwMod;
        if (code == 0xE06D7363 && ep->ExceptionRecord->NumberParameters >= 4) {
            auto base = reinterpret_cast<const void*>(ep->ExceptionRecord->ExceptionInformation[3]);
            throwMod = moduleOfAddress(base);
        }

        wchar_t buf[2048];
        int n = swprintf_s(buf, L"Tools3000 崩溃报告\r\n"
            L"========================================\r\n"
            L"线程 ID      : %lu\r\n"
            L"异常代码     : 0x%08X  %s\r\n"
            L"故障地址     : 0x%p\r\n"
            L"故障模块     : %s\r\n"
            L"抛出模块     : %s\r\n"
            L"转储文件     : %s\r\n"
            L"========================================\r\n"
            L"提示: 若“抛出模块”为 msvcp140/Tools3000 等, 多为未捕获的 C++ 异常;\r\n"
            L"请连同同目录的 tools3000.log(尤其崩溃前几秒) 一并发给开发者。\r\n",
            GetCurrentThreadId(), code, meaning, addr,
            faultMod.empty() ? L"(未知)" : faultMod.c_str(),
            throwMod.empty() ? L"(不适用/未知)" : throwMod.c_str(),
            dumpPath.wstring().c_str());

        HANDLE h = CreateFileW(txtPath.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            if (n > 0) {
                DWORD written = 0;
                WriteFile(h, buf, static_cast<DWORD>(n * sizeof(wchar_t)), &written, nullptr);
            }
            CloseHandle(h);
        }
    }

    static inline std::filesystem::path s_dumpDir;
    static inline LPTOP_LEVEL_EXCEPTION_FILTER s_previousFilter = nullptr;
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_CRASH_CRASHHANDLER_H
