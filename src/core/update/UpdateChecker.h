#pragma once

#include "core/utils/Export.h"
#include "Tools3000Version.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

namespace tools3000::core {

/// Bounded, non-blocking GitHub release checker with verified background auto-download & install.
class TOOLS3000CORE_API UpdateChecker final {
public:
    static UpdateChecker& instance();

    /// Starts a background check. A started check emits `update.result` and `update.state`.
    bool checkAsync(bool force = false);

    /// Starts downloading the latest verified release asset in the background.
    bool downloadUpdateAsync();

    /// Launches the verified downloaded installer or in-place portable updater and exits cleanly.
    bool installDownloadedUpdate(std::string& outError);

    /// Persists the current latestVersion as dismissed and broadcasts badgeVisible = false.
    bool dismissCurrentUpdate();

    /// Clears any persisted dismissed version so update badges can appear again.
    void clearDismissedVersion();

    /// Registers a host callback invoked after a verified updater/installer has started,
    /// allowing the current process to exit gracefully before file replacement begins.
    void setExitForUpdateCallback(std::function<void()> callback);

    /// Returns a JSON snapshot string of the latest update & download state.
    std::string getStateJsonString() const;

    void shutdown();

    static constexpr const char* CurrentVersion = tools3000::version::String;
    static bool isNewerVersion(std::string_view candidate, std::string_view current);
    static bool isTrustedReleaseDownloadUrl(std::string_view url) noexcept;
    static std::string extractExpectedSha256ForAsset(std::string_view sha256SumsBody, std::string_view assetName);
    static std::string formatReleaseNotesSummary(std::string_view rawNotes, size_t maxBytes = 600);
    static bool isPortableDirectory(const std::filesystem::path& exeDir) noexcept;
    static std::wstring buildInstallerParameters();
    static std::wstring buildInstallerParametersForDir(const std::filesystem::path& installDir);
    static std::wstring buildPortableUpdaterScript(
        const std::filesystem::path& zipPath,
        const std::filesystem::path& targetDir,
        const std::filesystem::path& logFilePath,
        uint32_t parentPid,
        bool relaunchAfterUpdate = true);
    static bool launchPortableInPlaceUpdater(
        const std::filesystem::path& zipPath,
        const std::filesystem::path& targetDir,
        uint32_t parentPid,
        std::string& outError,
        bool relaunchAfterUpdate = true,
        std::filesystem::path* outScriptPath = nullptr);

private:
    struct UpdateStateSnapshot {
        std::string status = "idle"; // idle | checking | available | downloading | readyToInstall | upToDate | unavailable | error
        bool hasUpdate = false;
        bool isPortable = false;
        std::string currentVersion = CurrentVersion;
        std::string latestVersion;
        std::string releaseUrl;
        std::string releaseNotes;
        std::string assetName;
        std::string downloadUrl;
        std::string checksumUrl;
        uint64_t assetSize = 0;
        uint64_t downloadedBytes = 0;
        int progress = 0;
        std::string downloadedFilePath;
        std::string error;
    };

    UpdateChecker() = default;
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    void workerMain(std::stop_token stopToken, bool autoDownloadIfAvailable);
    void downloadWorkerMain(std::stop_token stopToken);
    void broadcastStateLocked(bool emitLegacyResult, bool emitToast);

    std::mutex m_workerMutex;
    std::mutex m_requestMutex;
    mutable std::mutex m_stateMutex;
    std::jthread m_worker;
    std::jthread m_downloadWorker;
    void* m_activeRequest = nullptr;
    void* m_activeDownloadRequest = nullptr;
    UpdateStateSnapshot m_state;
    std::function<void()> m_exitForUpdateCallback;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_downloading{false};
    std::atomic<bool> m_launchingInstaller{false};
    std::atomic<bool> m_stopping{false};
};

}  // namespace tools3000::core
