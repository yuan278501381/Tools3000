#pragma once

#include "core/utils/Export.h"
#include "Tools3000Version.h"

#include <atomic>
#include <mutex>
#include <string_view>
#include <thread>

namespace tools3000::core {

/// Bounded, non-blocking GitHub release checker.
class TOOLS3000CORE_API UpdateChecker final {
public:
    static UpdateChecker& instance();

    /// Starts a background check. A started check emits `update.result`.
    bool checkAsync(bool force = false);
    void shutdown();

    static constexpr const char* CurrentVersion = tools3000::version::String;
    static bool isNewerVersion(std::string_view candidate, std::string_view current);

private:
    UpdateChecker() = default;
    ~UpdateChecker();
    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    void workerMain(std::stop_token stopToken);

    std::mutex m_workerMutex;
    std::mutex m_requestMutex;
    std::jthread m_worker;
    void* m_activeRequest = nullptr;
    std::atomic<bool> m_busy{false};
    std::atomic<bool> m_stopping{false};
};

}  // namespace tools3000::core
