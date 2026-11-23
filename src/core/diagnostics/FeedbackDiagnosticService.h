#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// FeedbackDiagnosticService.h — 方案 C 双轨应用内问题反馈与 10 维本地脱敏引擎
//
// 核心能力:
//   1. 10 维确定性本地隐私正则脱敏 (Windows 用户名路径 / JWT & Bearer / API Keys /
//      密码与密钥字段 / 邮箱 / 非回环 IPv4 / URL 凭据 / 手机号 / 身份证号 / Windows SID)；
//   2. 脱敏环境快照采集 (版本 / 架构 / 便携或安装模式 / Windows Build / 多屏 DPI /
//      已启插件 / 语言主题 / 进程工作集与私有内存) + 最近 200 行主/服务日志采集；
//   3. 零外部依赖生成标准 .zip 诊断包 (CRC32 + STORE，支持内嵌截图)；
//   4. 双轨提交协议:
//      - 轨 1 (已配置 relayEndpoint 且联网): 附带轻量 SHA-256 PoW 直传 Cloudflare Worker，回显追踪单号；
//      - 轨 2 (离线 / 未配置中继 / 超时降级): 一键预填 GitHub Issue URL + 本地导出脱敏 .zip + 复制 Markdown。
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_DIAGNOSTICS_FEEDBACKDIAGNOSTICSERVICE_H
#define TOOLS3000_CORE_DIAGNOSTICS_FEEDBACKDIAGNOSTICSERVICE_H

#include "core/utils/Export.h"

#include <nlohmann/json.hpp>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace tools3000::core {

struct RedactionReport {
    std::string redactedText;
    uint32_t totalReplacements = 0;
};

struct FeedbackSubmissionInput {
    std::string feedbackType;       ///< "bug" | "feature" | "performance" | "other"
    std::string title;
    std::string description;
    std::string stepsToReproduce;
    std::string contact;
    bool includeLogs = true;
    std::string screenshotPath;     ///< 可选本地图片路径
    std::string screenshotDataUri;  ///< 可选剪贴板粘贴 PNG/JPEG Data URI
};

struct FeedbackBundlePreview {
    nlohmann::json environment;
    std::string redactedMainLog;
    std::string redactedServiceLog;
    std::string redactedLatestCrash;
    std::string markdownReport;
    std::string githubIssueUrl;
    uint32_t totalRedactions = 0;
    bool relayConfigured = false;
    std::string relayEndpoint;
};

struct ClipboardImageAttachment {
    bool ok = false;
    bool tooLarge = false;
    std::string dataUri;
    std::string fileName;
    size_t byteSize = 0;
};

struct FeedbackSubmitOutcome {
    bool ok = false;
    std::string mode;               ///< "relay_submitted" | "fallback_local"
    std::string trackingId;
    std::string issueUrl;
    std::string exportedBundlePath;
    std::string markdownReport;
    std::string errorMessage;
    uint32_t totalRedactions = 0;
};

class TOOLS3000CORE_API FeedbackDiagnosticService {
public:
    static constexpr size_t kDefaultLogLines = 1000;
    static constexpr size_t kMaxScreenshotAttachmentBytes = 6u * 1024u * 1024u;

    /// 对任意文本执行 10 维本地隐私正则脱敏（纯函数，可用于离线单测验证）
    static RedactionReport redactSensitiveText(const std::string& input);

    /// 采集脱敏运行环境快照与最近 N 行主程序/搜索服务日志及近期崩溃摘要（绝不唤醒已停服的搜索服务）
    static FeedbackBundlePreview buildDiagnosticPreview(const FeedbackSubmissionInput& input,
                                                        size_t maxLogLines = kDefaultLogLines);

    /// 从 Windows 原生剪贴板读取截图位图 (CF_BITMAP/CF_DIB) 或复制的图像文件 (CF_HDROP) 并编码为 Data URI
    static ClipboardImageAttachment readClipboardImageAsDataUri(
        size_t maxImageBytes = kMaxScreenshotAttachmentBytes);

    /// 将脱敏环境快照、日志、Markdown 报告及可选截图打包为本地 .zip 诊断包
    static bool exportDiagnosticZipBundle(const FeedbackSubmissionInput& input,
                                          const std::filesystem::path& outputZipPath,
                                          std::string& outError,
                                          uint32_t* outTotalRedactions = nullptr);

    /// 计算轻量 SHA-256 PoW 凭证（用于中继 Worker 反垃圾防刷）
    static std::string computeLightweightPowNonce(const std::string& challengePrefix,
                                                  uint32_t leadingZeroHexDigits = 4);

    /// 执行方案 C 双轨提交：已配置中继优先直交，离线/未配置无缝降级为本地脱敏包 + 预填 GitHub Issue URL
    static FeedbackSubmitOutcome submitFeedback(const FeedbackSubmissionInput& input);

    /// 获取仓库官方 GitHub Issues 基础地址
    static std::string getGitHubNewIssueBaseUrl();
};

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_DIAGNOSTICS_FEEDBACKDIAGNOSTICSERVICE_H
