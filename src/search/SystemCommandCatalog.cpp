// ─────────────────────────────────────────────────────────────────────────────
// SystemCommandCatalog.cpp — 进程内超轻量 Windows 极客系统命令目录与白名单执行器
// ─────────────────────────────────────────────────────────────────────────────

#include "search/SystemCommandCatalog.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cctype>

namespace tools3000::search {

namespace {

constexpr std::array<SystemCommandSpec, 37> kBuiltinCommands = {{
    {
        "env",
        "search.cmd.env",
        "Edit System Environment Variables",
        "编辑系统环境变量",
        "rundll32.exe sysdm.cpl,EditEnvironmentVariables",
        "env environment variables path sysdm huanjing huanjingbianliang hjbl 环境变量 编辑环境变量 系统环境变量 路径",
        L"rundll32.exe",
        L"sysdm.cpl,EditEnvironmentVariables",
        nullptr,
        "sliders",
        true,
        false,
    },
    {
        "services",
        "search.cmd.services",
        "Windows Services",
        "Windows 服务管理",
        "services.msc",
        "services services.msc svc fuwu fw 服务 服务管理 系统服务",
        L"services.msc",
        nullptr,
        nullptr,
        "settings",
        true,
        false,
    },
    {
        "devmgmt",
        "search.cmd.devmgmt",
        "Device Manager",
        "设备管理器",
        "devmgmt.msc",
        "devmgmt devmgmt.msc device manager driver shebei sbglq 设备管理器 设备 驱动",
        L"devmgmt.msc",
        nullptr,
        nullptr,
        "cpu",
        true,
        false,
    },
    {
        "ncpa",
        "search.cmd.ncpa",
        "Network Connections",
        "网络连接与适配器",
        "ncpa.cpl",
        "ncpa ncpa.cpl network adapter ethernet wifi wangluo wllj 网络连接 网卡 适配器",
        L"ncpa.cpl",
        nullptr,
        nullptr,
        "network",
        true,
        false,
    },
    {
        "regedit",
        "search.cmd.regedit",
        "Registry Editor",
        "注册表编辑器",
        "regedit.exe",
        "regedit registry editor reg zhucebiao zcb 注册表 注册表编辑器",
        L"regedit.exe",
        nullptr,
        nullptr,
        "wrench",
        true,
        false,
    },
    {
        "cmd",
        "search.cmd.cmd",
        "Command Prompt",
        "命令提示符",
        "cmd.exe",
        "cmd command prompt console mingling mltsf 命令提示符 命令行",
        L"cmd.exe",
        nullptr,
        nullptr,
        "terminal",
        true,
        false,
    },
    {
        "wt",
        "search.cmd.wt",
        "Windows Terminal",
        "Windows 终端",
        "wt.exe",
        "wt terminal windows terminal shell zhongduan zd 终端",
        L"wt.exe",
        nullptr,
        L"powershell.exe",
        "terminal",
        true,
        false,
    },
    {
        "powershell",
        "search.cmd.powershell",
        "Windows PowerShell",
        "Windows PowerShell",
        "powershell.exe",
        "powershell pwsh ps shell 命令行 终端",
        L"powershell.exe",
        nullptr,
        nullptr,
        "terminal",
        true,
        false,
    },
    {
        "taskmgr",
        "search.cmd.taskmgr",
        "Task Manager",
        "任务管理器",
        "taskmgr.exe",
        "taskmgr task manager process renwu rwglq 任务管理器 进程 性能",
        L"taskmgr.exe",
        nullptr,
        nullptr,
        "activity",
        true,
        false,
    },
    {
        "resmon",
        "search.cmd.resmon",
        "Resource Monitor",
        "资源监视器",
        "resmon.exe",
        "resmon resource monitor ziyuan zyjsq 资源监视器 句柄 端口",
        L"resmon.exe",
        nullptr,
        nullptr,
        "activity",
        true,
        false,
    },
    {
        "perfmon",
        "search.cmd.perfmon",
        "Performance Monitor",
        "性能监视器",
        "perfmon.exe",
        "perfmon performance monitor xingneng xnjsq 性能监视器",
        L"perfmon.exe",
        nullptr,
        nullptr,
        "activity",
        true,
        false,
    },
    {
        "diskmgmt",
        "search.cmd.diskmgmt",
        "Disk Management",
        "磁盘管理",
        "diskmgmt.msc",
        "diskmgmt disk management partition volume cipan cpgl 磁盘管理 分区 卷",
        L"diskmgmt.msc",
        nullptr,
        nullptr,
        "harddrive",
        true,
        false,
    },
    {
        "compmgmt",
        "search.cmd.compmgmt",
        "Computer Management",
        "计算机管理",
        "compmgmt.msc",
        "compmgmt computer management jisuanji jsjgl 计算机管理",
        L"compmgmt.msc",
        nullptr,
        nullptr,
        "settings",
        true,
        false,
    },
    {
        "eventvwr",
        "search.cmd.eventvwr",
        "Event Viewer",
        "事件查看器",
        "eventvwr.msc",
        "eventvwr event viewer logs shijian sjckq 事件查看器 系统日志",
        L"eventvwr.msc",
        nullptr,
        nullptr,
        "filetext",
        true,
        false,
    },
    {
        "gpedit",
        "search.cmd.gpedit",
        "Local Group Policy Editor",
        "本地组策略编辑器",
        "gpedit.msc",
        "gpedit group policy zucelue zcl 组策略 本地组策略",
        L"gpedit.msc",
        nullptr,
        nullptr,
        "shield",
        true,
        false,
    },
    {
        "secpol",
        "search.cmd.secpol",
        "Local Security Policy",
        "本地安全策略",
        "secpol.msc",
        "secpol security policy anquan aqcl 安全策略 本地安全策略",
        L"secpol.msc",
        nullptr,
        nullptr,
        "shield",
        true,
        false,
    },
    {
        "mstsc",
        "search.cmd.mstsc",
        "Remote Desktop Connection",
        "远程桌面连接",
        "mstsc.exe",
        "mstsc rdp remote desktop yuancheng yczm 远程桌面",
        L"mstsc.exe",
        nullptr,
        nullptr,
        "monitor",
        true,
        false,
    },
    {
        "control",
        "search.cmd.control",
        "Control Panel",
        "控制面板",
        "control.exe",
        "control control panel kongzhi kzmb 控制面板",
        L"control.exe",
        nullptr,
        nullptr,
        "settings",
        false,
        false,
    },
    {
        "appwiz",
        "search.cmd.appwiz",
        "Programs and Features",
        "程序和功能（卸载软件）",
        "appwiz.cpl",
        "appwiz appwiz.cpl programs features uninstall chengxu xiezai cxhgn 程序和功能 卸载软件",
        L"appwiz.cpl",
        nullptr,
        nullptr,
        "wrench",
        false,
        false,
    },
    {
        "inetcpl",
        "search.cmd.inetcpl",
        "Internet Options & Proxy",
        "Internet 属性与代理",
        "inetcpl.cpl",
        "inetcpl inetcpl.cpl internet options proxy daili wangluoshuxing 代理 互联网选项",
        L"inetcpl.cpl",
        nullptr,
        nullptr,
        "network",
        false,
        false,
    },
    {
        "firewall",
        "search.cmd.firewall",
        "Windows Defender Firewall",
        "Windows 防火墙高级安全",
        "wf.msc",
        "firewall wf.msc defender fanghuoqiang fhq 防火墙 端口规则",
        L"wf.msc",
        nullptr,
        nullptr,
        "shield",
        true,
        false,
    },
    {
        "sysdm",
        "search.cmd.sysdm",
        "System Properties",
        "系统属性",
        "sysdm.cpl",
        "sysdm sysdm.cpl system properties computer name xitongshuxing xtsx 系统属性 高级系统设置",
        L"sysdm.cpl",
        nullptr,
        nullptr,
        "settings",
        true,
        false,
    },
    {
        "msinfo32",
        "search.cmd.msinfo32",
        "System Information",
        "系统信息",
        "msinfo32.exe",
        "msinfo32 system information hardware bios xitongxinxi xtxx 系统信息 硬件信息",
        L"msinfo32.exe",
        nullptr,
        nullptr,
        "cpu",
        false,
        false,
    },
    {
        "dxdiag",
        "search.cmd.dxdiag",
        "DirectX Diagnostic Tool",
        "DirectX 诊断工具",
        "dxdiag.exe",
        "dxdiag directx diagnostic gpu xianka zhenduan 诊断工具 显卡信息",
        L"dxdiag.exe",
        nullptr,
        nullptr,
        "cpu",
        false,
        false,
    },
    {
        "winver",
        "search.cmd.winver",
        "About Windows Version",
        "关于 Windows 版本",
        "winver.exe",
        "winver windows version build banben xtbb 系统版本 关于Windows",
        L"winver.exe",
        nullptr,
        nullptr,
        "settings",
        false,
        false,
    },
    {
        "netplwiz",
        "search.cmd.netplwiz",
        "User Accounts",
        "用户账户管理",
        "netplwiz.exe",
        "netplwiz user accounts login yonghu yhzh 用户账户 自动登录",
        L"netplwiz.exe",
        nullptr,
        nullptr,
        "shield",
        true,
        false,
    },
    {
        "taskschd",
        "search.cmd.taskschd",
        "Task Scheduler",
        "任务计划程序",
        "taskschd.msc",
        "taskschd task scheduler cron jihua rwjh 任务计划程序 定时任务",
        L"taskschd.msc",
        nullptr,
        nullptr,
        "settings",
        true,
        false,
    },
    {
        "certmgr",
        "search.cmd.certmgr",
        "Certificate Manager",
        "证书管理器",
        "certmgr.msc",
        "certmgr certificates ssl tls zhengshu zsgl 证书管理 证书管理器",
        L"certmgr.msc",
        nullptr,
        nullptr,
        "shield",
        true,
        false,
    },
    {
        "cleanmgr",
        "search.cmd.cleanmgr",
        "Disk Cleanup",
        "磁盘清理",
        "cleanmgr.exe",
        "cleanmgr disk cleanup qingli cpql 磁盘清理 垃圾清理",
        L"cleanmgr.exe",
        nullptr,
        nullptr,
        "harddrive",
        true,
        false,
    },
    {
        "calc",
        "search.cmd.calc",
        "Calculator",
        "计算器",
        "calc.exe",
        "calc calculator jisuanqi jsq 计算器",
        L"calc.exe",
        nullptr,
        nullptr,
        "wrench",
        false,
        false,
    },
    {
        "notepad",
        "search.cmd.notepad",
        "Notepad",
        "记事本",
        "notepad.exe",
        "notepad editor jishiben jsb 记事本",
        L"notepad.exe",
        nullptr,
        nullptr,
        "filetext",
        true,
        false,
    },
    {
        "charmap",
        "search.cmd.charmap",
        "Character Map",
        "字符映射表",
        "charmap.exe",
        "charmap character map unicode zifu zfysb 字符映射表",
        L"charmap.exe",
        nullptr,
        nullptr,
        "filetext",
        false,
        false,
    },
    {
        "hosts",
        "search.cmd.hosts",
        "Edit Windows Hosts File",
        "编辑 Hosts 文件",
        "notepad.exe %SystemRoot%\\System32\\drivers\\etc\\hosts",
        "hosts etc dns yuming 域名映射 hosts文件",
        L"notepad.exe",
        nullptr,
        nullptr,
        "filetext",
        true,
        true,
    },
    {
        "startup",
        "search.cmd.startup",
        "Startup Apps Settings",
        "开机启动应用设置",
        "ms-settings:startupapps",
        "startup startupapps autostart kaiji qidong kjx 开机启动 启动项",
        L"ms-settings:startupapps",
        nullptr,
        nullptr,
        "settings",
        false,
        false,
    },
    {
        "windowsupdate",
        "search.cmd.windowsupdate",
        "Windows Update",
        "Windows 系统更新",
        "ms-settings:windowsupdate",
        "windowsupdate update wu gengxin xtgx 系统更新 Windows更新",
        L"ms-settings:windowsupdate",
        nullptr,
        nullptr,
        "settings",
        false,
        false,
    },
    {
        "display",
        "search.cmd.display",
        "Display & DPI Settings",
        "显示与缩放设置",
        "ms-settings:display",
        "display monitor screen resolution dpi scale xianshi xssz 显示设置 分辨率 缩放",
        L"ms-settings:display",
        nullptr,
        nullptr,
        "monitor",
        false,
        false,
    },
    {
        "sound",
        "search.cmd.sound",
        "Sound Devices Settings",
        "声音设备设置",
        "mmsys.cpl",
        "sound audio mmsys speaker mic shengyin yinpin sysz 声音 音频设置",
        L"mmsys.cpl",
        nullptr,
        nullptr,
        "sliders",
        false,
        false,
    },
}};

std::string toLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& ch : out) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch + ('a' - 'A'));
        }
    }
    return out;
}

std::string_view trimView(std::string_view sv) noexcept {
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front()))) sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back()))) sv.remove_suffix(1);
    return sv;
}

std::vector<std::string> splitLowerTokens(std::string_view text) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        if (i >= text.size()) break;
        const size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) ++i;
        tokens.push_back(toLowerAscii(text.substr(start, i - start)));
    }
    return tokens;
}

bool launchShellExecute(const wchar_t* verb, const wchar_t* file, const wchar_t* params) noexcept {
    if (!file || file[0] == L'\0') return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI;
    sei.lpVerb = verb;
    sei.lpFile = file;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    return ShellExecuteExW(&sei) != FALSE;
}

}  // namespace

std::span<const SystemCommandSpec> SystemCommandCatalog::allCommands() noexcept {
    return std::span<const SystemCommandSpec>(kBuiltinCommands.data(), kBuiltinCommands.size());
}

const SystemCommandSpec* SystemCommandCatalog::findById(std::string_view commandId) noexcept {
    if (commandId.empty() || commandId.size() > 64) return nullptr;
    for (const auto& cmd : kBuiltinCommands) {
        const std::string_view idView(cmd.id);
        if (idView.size() != commandId.size()) continue;
        bool equal = true;
        for (size_t i = 0; i < idView.size(); ++i) {
            const char c = (commandId[i] >= 'A' && commandId[i] <= 'Z')
                ? static_cast<char>(commandId[i] + ('a' - 'A'))
                : commandId[i];
            if (idView[i] != c) {
                equal = false;
                break;
            }
        }
        if (equal) return &cmd;
    }
    return nullptr;
}

std::string SystemCommandCatalog::extractCommandIdFromUri(std::string_view uri) noexcept {
    constexpr std::string_view kPrefix = "cmd://";
    if (uri.size() <= kPrefix.size() || uri.size() > 80) return {};
    for (size_t i = 0; i < kPrefix.size(); ++i) {
        const char c = (uri[i] >= 'A' && uri[i] <= 'Z') ? static_cast<char>(uri[i] + ('a' - 'A')) : uri[i];
        if (c != kPrefix[i]) return {};
    }
    const std::string_view rawId = uri.substr(kPrefix.size());
    if (rawId.empty() || rawId.size() > 64) return {};
    std::string clean;
    clean.reserve(rawId.size());
    for (char ch : rawId) {
        const char low = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch + ('a' - 'A')) : ch;
        const bool ok = (low >= 'a' && low <= 'z') || (low >= '0' && low <= '9') || low == '_' || low == '-';
        if (!ok) return {};
        clean.push_back(low);
    }
    return clean;
}

CommandQueryParseResult SystemCommandCatalog::parseCommandQuery(std::string_view rawQuery) {
    const std::string_view trimmed = trimView(rawQuery);
    if (trimmed.size() >= 4 &&
        (trimmed[0] == 'c' || trimmed[0] == 'C') &&
        (trimmed[1] == 'm' || trimmed[1] == 'M') &&
        (trimmed[2] == 'd' || trimmed[2] == 'D') &&
        trimmed[3] == ':') {
        return {true, std::string(trimView(trimmed.substr(4)))};
    }
    return {false, std::string(trimmed)};
}

bool SystemCommandCatalog::shouldMixLauncherInGeneralQuery(std::string_view rawQuery) noexcept {
    const std::string_view trimmed = trimView(rawQuery);
    if (trimmed.size() < 2) return false;
    if (trimmed.find('\\') != std::string_view::npos || trimmed.find('/') != std::string_view::npos) {
        return false;
    }
    if (trimmed.find('*') != std::string_view::npos || trimmed.find('?') != std::string_view::npos) {
        return false;
    }
    if (trimmed.size() >= 2 && std::isalpha(static_cast<unsigned char>(trimmed[0])) && trimmed[1] == ':') {
        return false;
    }

    static constexpr std::array<std::string_view, 21> kDisqualifyingOps = {
        "content:", "c:", "ext:", "path:", "parent:", "p:", "file:", "folder:", "dir:",
        "size:", "dm:", "date:", "modified:", "regex:", "r:", "case:", "cs:", "exact:",
        "bm:", "app:", "cmd:"
    };
    const std::string low = toLowerAscii(trimmed);
    if (low.find("\xE5\x86\x85\xE5\xAE\xB9:") != std::string::npos) return false;
    for (const auto& op : kDisqualifyingOps) {
        const size_t pos = low.find(op);
        if (pos != std::string::npos && (pos == 0 || std::isspace(static_cast<unsigned char>(low[pos - 1])))) {
            return false;
        }
    }
    return true;
}

std::vector<SystemCommandMatch> SystemCommandCatalog::searchCommands(
    std::string_view query,
    size_t limit,
    bool allowEmptyQuery) {
    std::vector<SystemCommandMatch> hits;
    const std::string_view trimmed = trimView(query);
    if (trimmed.empty() && !allowEmptyQuery) return hits;

    const auto terms = splitLowerTokens(trimmed);
    const std::string fullQueryLower = toLowerAscii(trimmed);
    const size_t cap = limit == 0 ? kBuiltinCommands.size() : (std::min)(limit, kBuiltinCommands.size());

    hits.reserve(kBuiltinCommands.size());
    for (size_t idx = 0; idx < kBuiltinCommands.size(); ++idx) {
        const auto& cmd = kBuiltinCommands[idx];
        if (terms.empty()) {
            hits.push_back({&cmd, 500 - static_cast<int>(idx), false});
            continue;
        }

        const std::string idLower = toLowerAscii(cmd.id);
        const std::string titleEnLower = toLowerAscii(cmd.defaultTitleEn);
        const std::string titleZhLower = toLowerAscii(cmd.defaultTitleZh);
        const std::string hintLower = toLowerAscii(cmd.commandHint);
        const auto aliasTokens = splitLowerTokens(cmd.aliases);

        bool allMatched = true;
        int totalScore = 0;
        bool isExactAlias = false;

        if (terms.size() == 1) {
            const auto& term = terms[0];
            if (term == idLower) {
                isExactAlias = true;
            } else {
                for (const auto& a : aliasTokens) {
                    if (a == term) {
                        isExactAlias = true;
                        break;
                    }
                }
            }
            if (!isExactAlias && (fullQueryLower == titleEnLower || fullQueryLower == titleZhLower)) {
                isExactAlias = true;
            }
        } else if (fullQueryLower == titleEnLower || fullQueryLower == titleZhLower) {
            isExactAlias = true;
        }

        for (const auto& term : terms) {
            int termScore = 0;
            if (idLower == term) {
                termScore = 1000;
            } else {
                for (const auto& alias : aliasTokens) {
                    if (alias == term) {
                        termScore = (std::max)(termScore, 960);
                    } else if (alias.starts_with(term)) {
                        termScore = (std::max)(termScore, 800);
                    } else if (alias.find(term) != std::string::npos) {
                        termScore = (std::max)(termScore, 640);
                    }
                }
            }
            if (idLower.starts_with(term)) {
                termScore = (std::max)(termScore, 860);
            } else if (idLower.find(term) != std::string::npos) {
                termScore = (std::max)(termScore, 700);
            }
            if (titleZhLower == term || titleEnLower == term) {
                termScore = (std::max)(termScore, 950);
            } else if (titleZhLower.starts_with(term) || titleEnLower.starts_with(term)) {
                termScore = (std::max)(termScore, 760);
            } else if (titleZhLower.find(term) != std::string::npos ||
                       titleEnLower.find(term) != std::string::npos ||
                       hintLower.find(term) != std::string::npos) {
                termScore = (std::max)(termScore, 620);
            }

            if (termScore == 0) {
                allMatched = false;
                break;
            }
            totalScore += termScore;
        }

        if (allMatched) {
            if (isExactAlias) totalScore += 120;
            hits.push_back({&cmd, totalScore, isExactAlias});
        }
    }

    std::stable_sort(hits.begin(), hits.end(), [](const SystemCommandMatch& a, const SystemCommandMatch& b) {
        if (a.isExactAlias != b.isExactAlias) return a.isExactAlias > b.isExactAlias;
        if (a.score != b.score) return a.score > b.score;
        return std::string_view(a.spec->id) < std::string_view(b.spec->id);
    });

    if (hits.size() > cap) {
        hits.resize(cap);
    }
    return hits;
}

nlohmann::json SystemCommandCatalog::searchCommandsToJson(
    std::string_view query,
    size_t limit,
    bool allowEmptyQuery,
    bool preferChinese) {
    const auto matches = searchCommands(query, limit, allowEmptyQuery);
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& m : matches) {
        const auto& cmd = *m.spec;
        const int matchRank = m.isExactAlias ? 0 : (m.score >= 760 ? 1 : 4);
        arr.push_back({
            {"name", preferChinese ? cmd.defaultTitleZh : cmd.defaultTitleEn},
            {"titleKey", cmd.titleKey},
            {"path", std::string("cmd://") + cmd.id},
            {"commandId", cmd.id},
            {"commandHint", cmd.commandHint},
            {"iconHint", cmd.iconHint},
            {"isDirectory", false},
            {"resultType", "command"},
            {"exactAliasMatch", m.isExactAlias},
            {"launcherScore", m.score},
            {"matchRank", matchRank},
            {"runCount", 0},
            {"frecencyScore", 0.0}
        });
    }
    return arr;
}

bool SystemCommandCatalog::executeWhitelistedCommand(std::string_view commandId, bool asAdmin) noexcept {
    const SystemCommandSpec* spec = findById(commandId);
    if (!spec) return false;

    const wchar_t* verb = (asAdmin && spec->supportsRunAs) ? L"runas" : nullptr;

    if (spec->isHostsEditor) {
        wchar_t sysDir[MAX_PATH]{};
        const UINT len = GetSystemDirectoryW(sysDir, MAX_PATH);
        if (len == 0 || len >= MAX_PATH) return false;
        std::wstring hostsPath(sysDir, len);
        hostsPath += L"\\drivers\\etc\\hosts";
        return launchShellExecute(verb, L"notepad.exe", hostsPath.c_str());
    }

    if (launchShellExecute(verb, spec->execFile, spec->execParams)) {
        return true;
    }
    if (verb != nullptr && launchShellExecute(nullptr, spec->execFile, spec->execParams)) {
        return true;
    }
    if (spec->fallbackExecFile != nullptr) {
        return launchShellExecute(verb, spec->fallbackExecFile, nullptr) ||
               (verb != nullptr && launchShellExecute(nullptr, spec->fallbackExecFile, nullptr));
    }
    return false;
}

std::wstring SystemCommandCatalog::resolveNativeIconPath(std::string_view commandId) {
    const SystemCommandSpec* spec = findById(commandId);
    if (!spec || !spec->execFile) return {};

    std::wstring candidate(spec->execFile);
    if (_wcsnicmp(candidate.c_str(), L"ms-settings:", 12) == 0 ||
        _wcsnicmp(candidate.c_str(), L"shell:", 6) == 0) {
        return {};
    }
    if (spec->isHostsEditor) {
        candidate = L"notepad.exe";
    } else if (_wcsicmp(candidate.c_str(), L"rundll32.exe") == 0 && spec->execParams) {
        // 提取 rundll32 目标模块（如 sysdm.cpl,EditEnvironmentVariables -> sysdm.cpl）以呈现原生图标
        std::wstring_view paramsView(spec->execParams);
        const size_t cut = paramsView.find_first_of(L", \t");
        if (cut != std::wstring_view::npos && cut > 0) {
            candidate.assign(paramsView.substr(0, cut));
        }
    }

    auto tryResolve = [](const std::wstring& fileName) -> std::wstring {
        if (fileName.empty()) return {};
        if (GetFileAttributesW(fileName.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return fileName;
        }
        wchar_t resolved[MAX_PATH * 2]{};
        const DWORD len = SearchPathW(nullptr, fileName.c_str(), nullptr,
                                      static_cast<DWORD>(std::size(resolved)), resolved, nullptr);
        if (len > 0 && len < std::size(resolved)) {
            return std::wstring(resolved, len);
        }
        return {};
    };

    if (std::wstring found = tryResolve(candidate); !found.empty()) {
        return found;
    }
    if (spec->fallbackExecFile) {
        return tryResolve(spec->fallbackExecFile);
    }
    return {};
}

}  // namespace tools3000::search
