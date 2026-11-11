#include "core/plugin/PluginManager.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/hotkey/HotkeyManager.h"
#include "core/hotkey/KeyboardHook.h"
#include "core/hotkey/MouseHook.h"
#include "core/ipc/MessageBridge.h"
#include "core/logger/Logger.h"
#include "core/stats/PerformanceMonitor.h"
#include "core/utils/WinUtils.h"
#include "Tools3000Version.h"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <filesystem>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace tools3000::core {

namespace {

std::string pluginIdFromPath(const std::filesystem::path& path) {
    std::string value = WinUtils::wstringToUtf8(path.stem().wstring());
    constexpr std::string_view prefix = "Plugin_";
    if (value.starts_with(prefix)) value.erase(0, prefix.size());

    std::string result;
    result.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) result.push_back(static_cast<char>(std::tolower(ch)));
        else if (ch == '-' || ch == '_') result.push_back(static_cast<char>(ch));
        else if (result.empty() || result.back() != '-') result.push_back('-');
    }
    return result;
}

std::string pluginConfigKey(const std::string& id) {
    return "/plugins/" + id + "/enabled";
}

bool hasReparsePointAttribute(const std::filesystem::path& targetPath) noexcept {
    if (targetPath.empty()) return false;
    const DWORD attrs = GetFileAttributesW(targetPath.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool isHostSignedForElevatedPluginGate() noexcept {
    static const bool s_hostSigned = []() {
        const auto exePath = WinUtils::getExePath();
        return !exePath.empty() && WinUtils::hasValidAuthenticodeSignature(exePath);
    }();
    return s_hostSigned;
}

bool configuredEnabled(const std::string& id) {
    auto& config = ConfigManager::instance();
    if (id == "dialogenhancer" || id == "dialog_enhancer") {
        if (config.has("/plugins/dialogenhancer/enabled")) return config.get<bool>("/plugins/dialogenhancer/enabled", true);
        if (config.has("/plugins/dialog_enhancer/enabled")) return config.get<bool>("/plugins/dialog_enhancer/enabled", true);
        if (config.has("/dialog/enabled")) return config.get<bool>("/dialog/enabled", true);
        return true;
    }
    if (id == "keycast") {
        if (config.has("/plugins/keycast/enabled")) return config.get<bool>("/plugins/keycast/enabled", true);
        if (config.has("/keycast/enabled")) return config.get<bool>("/keycast/enabled", true);
        if (config.has("/general/keycastEnabled")) return config.get<bool>("/general/keycastEnabled", true);
        return true;
    }
    if (id == "spotlight") {
        if (config.has("/plugins/spotlight/enabled")) return config.get<bool>("/plugins/spotlight/enabled", true);
        if (config.has("/spotlight/enabled")) return config.get<bool>("/spotlight/enabled", true);
        return true;
    }
    if (id == "remote_boost" || id == "remote") {
        if (config.has("/plugins/remote_boost/enabled")) return config.get<bool>("/plugins/remote_boost/enabled", true);
        if (config.has("/plugins/remote/enabled")) return config.get<bool>("/plugins/remote/enabled", true);
        if (config.has("/remote_boost/enabled")) return config.get<bool>("/remote_boost/enabled", true);
        return true;
    }
    if (id == "gesture") {
        // 手势插件加载状态仅受 /plugins/gesture/enabled 控制，绝不受运行期临时暂停 (/gesture/paused) 影响
        if (config.has("/plugins/gesture/enabled")) return config.get<bool>("/plugins/gesture/enabled", true);
        return true;
    }
    if (id == "search") {
        if (config.has("/plugins/search/enabled")) return config.get<bool>("/plugins/search/enabled", true);
        if (config.has("/search/enabled")) return config.get<bool>("/search/enabled", true);
        return true;
    }
    const auto key = pluginConfigKey(id);
    if (config.has(key)) return config.get<bool>(key, true);
    return true;
}

} // namespace

PluginManager& PluginManager::instance() {
    static PluginManager inst;
    return inst;
}

void PluginManager::reconcilePluginConfigs() {
    auto& cfg = ConfigManager::instance();
    nlohmann::json patch = nlohmann::json::object();

    // 1. dialogenhancer: /dialog/enabled 与 /plugins/dialogenhancer/enabled 强同构
    bool dialogEnabled = true;
    if (cfg.has("/plugins/dialogenhancer/enabled")) {
        dialogEnabled = cfg.get<bool>("/plugins/dialogenhancer/enabled", true);
    } else if (cfg.has("/dialog/enabled")) {
        dialogEnabled = cfg.get<bool>("/dialog/enabled", true);
    }
    patch["dialog"]["enabled"] = dialogEnabled;
    patch["plugins"]["dialogenhancer"]["enabled"] = dialogEnabled;
    patch["plugins"]["dialog_enhancer"]["enabled"] = dialogEnabled;

    // 2. keycast: /keycast/enabled, /general/keycastEnabled, /plugins/keycast/enabled
    bool keycastEnabled = true;
    if (cfg.has("/plugins/keycast/enabled")) {
        keycastEnabled = cfg.get<bool>("/plugins/keycast/enabled", true);
    } else if (cfg.has("/keycast/enabled")) {
        keycastEnabled = cfg.get<bool>("/keycast/enabled", true);
    } else if (cfg.has("/general/keycastEnabled")) {
        keycastEnabled = cfg.get<bool>("/general/keycastEnabled", true);
    }
    patch["keycast"]["enabled"] = keycastEnabled;
    patch["general"]["keycastEnabled"] = keycastEnabled;
    patch["plugins"]["keycast"]["enabled"] = keycastEnabled;

    // 3. spotlight: /spotlight/enabled 与 /plugins/spotlight/enabled
    bool spotlightEnabled = true;
    if (cfg.has("/plugins/spotlight/enabled")) {
        spotlightEnabled = cfg.get<bool>("/plugins/spotlight/enabled", true);
    } else if (cfg.has("/spotlight/enabled")) {
        spotlightEnabled = cfg.get<bool>("/spotlight/enabled", true);
    }
    patch["spotlight"]["enabled"] = spotlightEnabled;
    patch["plugins"]["spotlight"]["enabled"] = spotlightEnabled;

    // 4. remote_boost: /remote_boost/enabled 与 /plugins/remote_boost/enabled
    bool remoteEnabled = true;
    if (cfg.has("/plugins/remote_boost/enabled")) {
        remoteEnabled = cfg.get<bool>("/plugins/remote_boost/enabled", true);
    } else if (cfg.has("/plugins/remote/enabled")) {
        remoteEnabled = cfg.get<bool>("/plugins/remote/enabled", true);
    } else if (cfg.has("/remote_boost/enabled")) {
        remoteEnabled = cfg.get<bool>("/remote_boost/enabled", true);
    }
    patch["remote_boost"]["enabled"] = remoteEnabled;
    patch["plugins"]["remote_boost"]["enabled"] = remoteEnabled;
    patch["plugins"]["remote"]["enabled"] = remoteEnabled;

    // 5. gesture: 插件加载单一事实源为 /plugins/gesture/enabled；冷启动时若插件启用则自动解除上次会话遗留的临时暂停
    bool gesturePluginEnabled = true;
    if (cfg.has("/plugins/gesture/enabled")) {
        gesturePluginEnabled = cfg.get<bool>("/plugins/gesture/enabled", true);
    }
    patch["plugins"]["gesture"]["enabled"] = gesturePluginEnabled;
    if (gesturePluginEnabled) {
        patch["gesture"]["enabled"] = true;
        patch["gesture"]["paused"] = false;
    } else {
        patch["gesture"]["enabled"] = false;
        patch["gesture"]["paused"] = true;
    }

    // 6. search: /search/enabled 与 /plugins/search/enabled
    bool searchEnabled = true;
    if (cfg.has("/plugins/search/enabled")) {
        searchEnabled = cfg.get<bool>("/plugins/search/enabled", true);
    } else if (cfg.has("/search/enabled")) {
        searchEnabled = cfg.get<bool>("/search/enabled", true);
    }
    patch["search"]["enabled"] = searchEnabled;
    patch["plugins"]["search"]["enabled"] = searchEnabled;

    cfg.mergePatch(patch, "/plugins");
}

bool PluginManager::loadPluginBinaryLocked(PluginInstance& inst) {
    if (inst.plugin && inst.handle) {
        return true;
    }
    inst.error.clear();
    const std::wstring filename = inst.path.filename().wstring();

    // 防御重解析点（符号链接 / 目录联接）劫持与运行期热加载 TOCTOU 替换
    if (hasReparsePointAttribute(inst.path.parent_path()) || hasReparsePointAttribute(inst.path)) {
        inst.error = "plugin path contains an untrusted reparse point";
        LOG_ERROR("Rejected plugin DLL due to reparse point attribute: {}", inst.fileName);
        return false;
    }

    // 提权宿主签名门禁：正式发版签名宿主以管理员运行时，外部插件 DLL 必须同样具备有效 Authenticode 签名；
    // 本地未签名开发/测试构建不受影响。
    if (tools3000::core::WinUtils::isProcessElevated() && isHostSignedForElevatedPluginGate()) {
        if (!tools3000::core::WinUtils::hasValidAuthenticodeSignature(inst.path)) {
            inst.error = "unsigned or untrusted plugin DLL rejected in elevated signed host";
            LOG_ERROR("Rejected unsigned plugin DLL in elevated signed host: {}", inst.fileName);
            return false;
        }
    }

    HMODULE hMod = LoadLibraryExW(
        inst.path.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!hMod) {
        const DWORD loadErr = GetLastError();
        inst.error = "load failed (Windows error " + std::to_string(loadErr) + ")";
        LOG_ERROR("无法加载插件 DLL: {}, error={}", inst.fileName, loadErr);
        return false;
    }

    using GetPluginAbiVersionFunc = std::uint32_t (*)();
    const auto getAbiVersion = reinterpret_cast<GetPluginAbiVersionFunc>(
        GetProcAddress(hMod, "GetPluginAbiVersion"));
    if (!getAbiVersion) {
        inst.error = "missing GetPluginAbiVersion export";
        LOG_ERROR("插件 DLL 未导出 ABI 握手函数: {}", inst.fileName);
        FreeLibrary(hMod);
        return false;
    }

    std::uint32_t binaryAbiVersion = 0;
    try {
        binaryAbiVersion = getAbiVersion();
    } catch (...) {
        inst.error = "plugin ABI handshake failed";
    }
    if (!inst.error.empty() || binaryAbiVersion != inst.manifest.abiVersion) {
        if (inst.error.empty()) inst.error = "plugin binary ABI does not match manifest";
        LOG_ERROR("插件 ABI 校验失败: {}, manifest={}, binary={}",
                  inst.fileName, inst.manifest.abiVersion, binaryAbiVersion);
        FreeLibrary(hMod);
        return false;
    }

    using CreatePluginFunc = IPlugin* (*)();
    const auto createFunc = reinterpret_cast<CreatePluginFunc>(
        GetProcAddress(hMod, inst.manifest.entryPoint.c_str()));
    if (!createFunc) {
        LOG_ERROR("插件 DLL 未导出 CreatePlugin: {}",
                  tools3000::core::WinUtils::wstringToUtf8(filename));
        inst.error = "missing CreatePlugin export";
        FreeLibrary(hMod);
        return false;
    }

    IPlugin* plugin = nullptr;
    try {
        plugin = createFunc();
    } catch (const std::exception& e) {
        LOG_ERROR("CreatePlugin 异常: {}, error={}",
                  tools3000::core::WinUtils::wstringToUtf8(filename), e.what());
    } catch (...) {
        LOG_ERROR("CreatePlugin 未知异常: {}", tools3000::core::WinUtils::wstringToUtf8(filename));
    }
    if (!plugin) {
        LOG_ERROR("CreatePlugin 返回 null: {}", tools3000::core::WinUtils::wstringToUtf8(filename));
        inst.error = "CreatePlugin returned null";
        FreeLibrary(hMod);
        return false;
    }

    try {
        const char* name = plugin->getName();
        const char* version = plugin->getVersion();
        if (!name || !*name || !version || !*version) {
            throw std::runtime_error("empty plugin metadata");
        }
        if (inst.name != name || inst.version != version) {
            throw std::runtime_error("binary metadata does not match manifest");
        }
    } catch (const std::exception& e) {
        LOG_ERROR("插件元数据异常: {}, error={}",
                  tools3000::core::WinUtils::wstringToUtf8(filename), e.what());
        inst.error = std::string("invalid metadata: ") + e.what();
        FreeLibrary(hMod);
        return false;
    } catch (...) {
        LOG_ERROR("插件元数据未知异常: {}", tools3000::core::WinUtils::wstringToUtf8(filename));
        inst.error = "invalid metadata";
        FreeLibrary(hMod);
        return false;
    }

    inst.plugin = plugin;
    inst.handle = hMod;
    LOG_INFO("插件已成功加载: {} (v{})", inst.name, inst.version);
    return true;
}

bool PluginManager::initializeSinglePluginLocked(PluginInstance& inst) {
    if (!inst.plugin || !inst.handle) return false;
    if (inst.initialized) return true;

    LOG_INFO("初始化插件: {}", inst.name);
    const auto started = std::chrono::steady_clock::now();
    try {
        inst.initialized = inst.plugin->initialize();
    } catch (const std::exception& e) {
        LOG_ERROR("插件初始化异常: {}, error={}", inst.name, e.what());
        inst.initialized = false;
        inst.error = e.what();
    } catch (...) {
        LOG_ERROR("插件初始化未知异常: {}", inst.name);
        inst.initialized = false;
        inst.error = "unknown initialize exception";
    }
    if (!inst.initialized) {
        LOG_ERROR("插件初始化失败: {}", inst.name);
        if (inst.error.empty()) inst.error = "initialize returned false";
        // initialize() may fail after partially registering callbacks.
        try {
            inst.plugin->shutdown();
        } catch (const std::exception& e) {
            LOG_WARN("插件初始化失败后的清理异常: {}, error={}", inst.name, e.what());
        } catch (...) {
            LOG_WARN("插件初始化失败后的清理发生未知异常: {}", inst.name);
        }
    }
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    PerformanceMonitor::instance().recordPluginInit(inst.name, elapsedMs);
    return inst.initialized;
}

bool PluginManager::loadPlugins(const std::string& directory) {
    std::lock_guard lock(m_mutex);
    if (!m_plugins.empty()) {
        LOG_WARN("插件已加载，拒绝重复扫描");
        return false;
    }
    reconcilePluginConfigs();
    std::filesystem::path dirPath = tools3000::core::WinUtils::utf8ToWstring(directory);

    // 如果是相对路径，则相对于 exe 所在目录
    if (dirPath.is_relative()) {
        dirPath = tools3000::core::WinUtils::getExeDirectory() / dirPath;
    }
    dirPath = dirPath.lexically_normal();

    std::error_code filesystemError;
    if (!std::filesystem::is_directory(dirPath, filesystemError)) {
        LOG_WARN("插件目录不存在: {}", directory);
        return false;
    }
    if (hasReparsePointAttribute(dirPath)) {
        LOG_ERROR("Rejected plugin directory with reparse point attribute: {}", directory);
        return false;
    }

    LOG_INFO("开始扫描插件目录: {}", directory);

    std::vector<std::filesystem::path> pluginPaths;
    std::filesystem::directory_iterator pluginIt(dirPath, filesystemError);
    const std::filesystem::directory_iterator end;
    for (; pluginIt != end && !filesystemError; pluginIt.increment(filesystemError)) {
        const auto& entry = *pluginIt;
        if (entry.is_symlink(filesystemError) || hasReparsePointAttribute(entry.path())) {
            filesystemError.clear();
            LOG_WARN("Skipping plugin entry with reparse point attribute: {}",
                     tools3000::core::WinUtils::wstringToUtf8(entry.path().filename().wstring()));
            continue;
        }
        if (entry.is_regular_file(filesystemError) && !filesystemError &&
            _wcsicmp(entry.path().extension().c_str(), L".dll") == 0) {
            std::wstring filename = entry.path().filename().wstring();
            if (filename.find(L"Plugin_") == 0) {
                pluginPaths.push_back(entry.path());
            }
        }
    }
    if (filesystemError) {
        LOG_ERROR("扫描插件目录失败: {}, error={}", directory, filesystemError.message());
        return false;
    }
    std::sort(pluginPaths.begin(), pluginPaths.end());
    std::unordered_set<std::string> pluginNames;
    std::unordered_set<std::string> pluginIds;

    for (const auto& pluginPath : pluginPaths) {
        const std::wstring filename = pluginPath.filename().wstring();
        PluginInstance inst;
        inst.id = pluginIdFromPath(pluginPath);
        inst.name = inst.id;
        inst.fileName = tools3000::core::WinUtils::wstringToUtf8(filename);
        inst.path = pluginPath;
        inst.configuredEnabled = configuredEnabled(inst.id);
        inst.enabledAtLaunch = inst.configuredEnabled;
        LOG_INFO("发现插件 DLL: {}", tools3000::core::WinUtils::wstringToUtf8(filename));

        if (!pluginIds.insert(inst.id).second) {
            inst.error = "duplicate plugin id";
            LOG_ERROR("插件 ID 重复，已拒绝加载: {}", inst.id);
            m_plugins.push_back(std::move(inst));
            continue;
        }

        auto manifestPath = pluginPath;
        manifestPath.replace_extension(L".plugin.json");
        if (hasReparsePointAttribute(manifestPath)) {
            inst.error = "plugin manifest is an untrusted reparse point";
            LOG_ERROR("Rejected plugin manifest with reparse point attribute: {}", inst.fileName);
            m_plugins.push_back(std::move(inst));
            continue;
        }
        auto manifestResult = loadPluginManifest(manifestPath, inst.id, tools3000::version::String);
        if (!manifestResult) {
            inst.error = manifestResult.error;
            LOG_ERROR("插件清单校验失败: {}, error={}", inst.fileName, inst.error);
            m_plugins.push_back(std::move(inst));
            continue;
        }
        inst.manifest = std::move(manifestResult.manifest);
        inst.name = inst.manifest.name;
        inst.version = inst.manifest.version;
        if (!pluginNames.insert(inst.name).second) {
            inst.error = "duplicate plugin name";
            LOG_ERROR("插件名称重复，已拒绝加载: {}", inst.name);
            m_plugins.push_back(std::move(inst));
            continue;
        }

        // 清单提供稳定 ID 和展示元数据。禁用模块无需加载 DLL；Capture 等大型
        // 模块因此不会连带映射 OpenCV/FFmpeg，显著降低冷启动 I/O。
        if (!inst.enabledAtLaunch) {
            LOG_INFO("插件已禁用，完全跳过 DLL 加载: {}", inst.id);
            m_plugins.push_back(std::move(inst));
            continue;
        }

        loadPluginBinaryLocked(inst);
        m_plugins.push_back(std::move(inst));
    }

    return !m_plugins.empty();
}

void PluginManager::initializePlugins() {
    std::lock_guard lock(m_mutex);
    for (auto& inst : m_plugins) {
        if (!inst.enabledAtLaunch || !inst.plugin || !inst.handle) continue;
        initializeSinglePluginLocked(inst);
    }
}

void PluginManager::shutdownPlugins() {
    {
        std::lock_guard lock(m_mutex);
        if (m_shuttingDown) return;
        m_shuttingDown = true;
    }
    // Phase 1: let every plugin stop its workers and release its own windows
    // while every DLL is still loaded.
    for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it) {
        if (!it->initialized) continue;
        LOG_INFO("停止插件: {}", it->name);
        try {
            it->plugin->shutdown();
        } catch (const std::exception& e) {
            LOG_ERROR("插件关闭异常: {}, error={}", it->name, e.what());
        } catch (...) {
            LOG_ERROR("插件关闭未知异常: {}", it->name);
        }
        it->initialized = false;
    }

    // Phase 2: destroy all callbacks owned by external core singletons before
    // FreeLibrary. Otherwise std::function destructors or in-flight low-level
    // hooks can jump into unloaded plugin code during process shutdown.
    KeyboardHook::instance().uninstall();
    KeyboardHook::instance().clearCallbacks();
    MouseHook::instance().uninstall();
    MainThreadDispatcher::instance().drain();
    HotkeyManager::instance().shutdown();
    const bool eventBusDrained = EventBus::instance().clearAll();
    ConfigManager::instance().clearCallbacks();
    MessageBridge::instance().clearHandlers();

    const bool retainModules = !eventBusDrained ||
                               EventBus::instance().hasQuarantinedSubscriptions() ||
                               MessageBridge::instance().hasQuarantinedHandlers();
    if (retainModules) {
        LOG_CRITICAL("Quarantined callbacks or IPC handlers detected during shutdown; skipping FreeLibrary to prevent unmapped code execution.");
    }
    for (auto it = m_plugins.rbegin(); it != m_plugins.rend(); ++it) {
        if (!it->handle) continue;
        if (!retainModules) {
            LOG_INFO("卸载插件 DLL: {}", it->name);
            FreeLibrary(it->handle);
        }
    }
    m_plugins.clear();
}

std::vector<PluginStatus> PluginManager::getPluginStatuses() const {
    std::lock_guard lock(m_mutex);
    if (m_shuttingDown) return {};
    std::vector<PluginStatus> result;
    result.reserve(m_plugins.size());
    for (const auto& inst : m_plugins) {
        const bool desiredEnabled = configuredEnabled(inst.id);
        const bool isRunning = inst.initialized && desiredEnabled;
        const bool restartRequired = desiredEnabled
            ? (!inst.initialized && desiredEnabled != inst.enabledAtLaunch)
            : inst.initialized;
        std::string state;
        if (!inst.error.empty()) state = "failed";
        else if (restartRequired) state = "pendingRestart";
        else if (isRunning) state = "running";
        else if (!desiredEnabled) state = "disabled";
        else state = "failed";
        result.push_back({
            inst.id, inst.name, inst.version, inst.fileName,
            inst.manifest.abiVersion, inst.manifest.capabilities, inst.manifest.permissions,
            inst.manifest.executionModel,
            desiredEnabled, isRunning, restartRequired,
            std::move(state), inst.error
        });
    }
    return result;
}

bool PluginManager::setPluginEnabled(const std::string& id, bool enabled,
                                     bool& restartRequired, std::string& error) {
    std::lock_guard lock(m_mutex);
    if (m_shuttingDown) {
        error = "application is shutting down";
        return false;
    }
    const std::string normalizedId = (id == "dialog_enhancer") ? "dialogenhancer" : id;
    auto it = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginInstance& item) {
        return item.id == normalizedId || item.id == id;
    });
    if (it == m_plugins.end()) {
        error = "plugin not found";
        return false;
    }
    if (!it->error.empty() && enabled && (it->handle != nullptr || !std::filesystem::exists(it->path))) {
        error = it->error;
        return false;
    }

    nlohmann::json patch = {{"plugins", {{it->id, {{"enabled", enabled}}}}}};
    if (it->id == "keycast") {
        patch["general"] = {{"keycastEnabled", enabled}};
        patch["keycast"] = {{"enabled", enabled}};
        patch["plugins"]["keycast"] = {{"enabled", enabled}};
    } else if (it->id == "dialogenhancer" || it->id == "dialog_enhancer") {
        patch["dialog"] = {{"enabled", enabled}};
        patch["plugins"]["dialogenhancer"] = {{"enabled", enabled}};
        patch["plugins"]["dialog_enhancer"] = {{"enabled", enabled}};
    } else if (it->id == "spotlight") {
        patch["spotlight"] = {{"enabled", enabled}};
        patch["plugins"]["spotlight"] = {{"enabled", enabled}};
    } else if (it->id == "remote_boost" || it->id == "remote") {
        patch["remote_boost"] = {{"enabled", enabled}};
        patch["plugins"]["remote_boost"] = {{"enabled", enabled}};
        patch["plugins"]["remote"] = {{"enabled", enabled}};
    } else if (it->id == "gesture") {
        patch["gesture"] = {{"enabled", enabled}, {"paused", !enabled}};
        patch["plugins"]["gesture"] = {{"enabled", enabled}};
    } else if (it->id == "search") {
        patch["search"] = {{"enabled", enabled}};
        patch["plugins"]["search"] = {{"enabled", enabled}};
    }
    if (!ConfigManager::instance().mergePatch(patch, "/plugins/" + it->id)) {
        error = "failed to persist plugin setting";
        return false;
    }
    it->configuredEnabled = enabled;

    // A3: 当重新启用本次启动因禁用而跳过加载的插件时，当场执行动态热加载与初始化
    if (enabled && !it->initialized) {
        if (!it->handle || !it->plugin) {
            if (!loadPluginBinaryLocked(*it)) {
                error = it->error;
                restartRequired = false;
                return false;
            }
        }
        if (!initializeSinglePluginLocked(*it)) {
            error = it->error;
            restartRequired = false;
            return false;
        }
        it->enabledAtLaunch = true;
        it->error.clear();
        restartRequired = false;
        LOG_INFO("Plugin hot-loaded and initialized at runtime: {}", it->id);
        return true;
    }

    restartRequired = enabled ? !it->initialized : it->initialized;
    return true;
}

bool PluginManager::isEnabledAtLaunch(const std::string& id) const {
    std::lock_guard lock(m_mutex);
    if (m_shuttingDown) return false;
    const auto it = std::find_if(m_plugins.begin(), m_plugins.end(), [&](const PluginInstance& item) {
        return item.id == id;
    });
    return it != m_plugins.end() && it->enabledAtLaunch && it->error.empty();
}

} // namespace tools3000::core
