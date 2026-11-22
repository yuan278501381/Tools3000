// ─────────────────────────────────────────────────────────────────────────────
// PinWindowTranslate.cpp — 贴图窗口非破坏式原位翻译编排子模块
//
// 职责:
//   - 编排「离线 OCR -> 段落版面聚类 -> TranslationService 批量翻译 ->
//     InPlaceTranslateEngine 无痕擦底与 DirectWrite 原位渲染」全链路；
//   - 维护 m_translatedImage 与 m_sourceImage 的非破坏隔离，支持 0ms 原图/译图瞬切；
//   - 纳入全局 s_globalActiveOcrWorkers 停机排空门禁，严防插件卸载悬空线程；
//   - 仅在冷路径任务完成后执行物理内存修剪，严禁影响交互热路径帧率。
//
// 版权声明: Copyright (c) 2026 Yy1 (yuan278501381). All rights reserved.
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindowInternal.h"
#include "capture/InPlaceTranslateEngine.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/i18n/UiI18n.h"
#include "core/logger/Logger.h"
#include "core/translate/TranslationService.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "ocr/OcrEngine.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tools3000::capture {

using namespace pin_detail;

namespace {

std::wstring buildFriendlyTranslateErrorToast(
    const tools3000::core::translate::TranslationBatchResult& res, bool isZh) {
    if (res.errorCode == "missing_api_key") {
        return tools3000::core::trWCstrFor(
            isZh,
            L"翻译未配置 API 密钥：请前往「设置 -> 文字识别」填写或切回免 Key 翻译引擎",
            L"Translation API key missing: configure it in Settings > OCR or switch to Free Engine");
    }
    if (res.errorCode == "invalid_endpoint") {
        return tools3000::core::trWCstrFor(
            isZh,
            L"请前往「设置 -> 文字识别」配置有效的翻译接口地址",
            L"Please configure a valid translation endpoint in Settings > OCR");
    }
    if (res.errorCode == "network") {
        return tools3000::core::trWCstrFor(
            isZh,
            L"翻译网络请求失败，请检查网络连接或在设置中切换翻译引擎",
            L"Translation network request failed — check connectivity or switch provider in Settings");
    }
    if (res.errorCode == "http_error" || res.errorCode == "auth_failed" || res.errorCode == "rate_limited") {
        return tools3000::core::trWCstrFor(
            isZh,
            L"翻译服务响应异常，请检查密钥额度或切换备用引擎",
            L"Translation service returned an error — check API quota or switch provider");
    }
    return tools3000::core::trWCstrFor(
        isZh,
        L"原位翻译失败，请稍后重试或在设置中测试翻译连通性",
        L"In-place translation failed — please retry or test connection in Settings");
}

bool isChineseUiLanguage() noexcept {
    return tools3000::core::UiI18n::getEffectiveLanguage() == tools3000::core::UiLanguage::ZhCN;
}

} // namespace

void PinWindow::clearTranslationState() {
    m_pendingTranslateAfterOcr.store(false, std::memory_order_release);
    m_pendingTranslateSelectedOnly.store(false, std::memory_order_release);
    m_translationVersion.fetch_add(1, std::memory_order_acq_rel);
    if (m_translateCancelFlag) {
        m_translateCancelFlag->store(true, std::memory_order_release);
        m_translateCancelFlag.reset();
    }
    m_translationInFlight.store(false, std::memory_order_release);
    m_showingTranslation = false;
    m_translatedImage.release();
    m_translatedTargetLang.clear();
    m_translatedProviderUsed.clear();
    m_translatedParagraphs.clear();
    m_btnTranslateStatusRect = {};
    m_hoverTranslateStatus = false;
}

void PinWindow::toggleTranslationOverlay() {
    if (m_folded || !m_hwnd || !IsWindow(m_hwnd)) return;
    if (m_translatedImage.empty()) {
        translateInPlace(/*selectedOnly=*/false, /*forceRetranslate=*/false);
        return;
    }
    if (m_editingTextReplaceIdx >= 0) {
        commitInPlaceTextReplace(true);
    }

    m_showingTranslation = !m_showingTranslation;
    if (m_showingTranslation) {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_selectedBlockIndices.clear();
        m_selectedCharSpans.clear();
        m_hoveredBlockIndex = -1;
    }
    createRenderResources(activeDisplayImage());
    const bool isZh = isChineseUiLanguage();
    showTransientHudBadge(
        m_showingTranslation
            ? tools3000::core::trWCstrFor(isZh, L"译图模式 (Tab 切原图)", L"Translated (Tab: Original)")
            : tools3000::core::trWCstrFor(isZh, L"原图对照 (Tab 切译图)", L"Original (Tab: Translated)"),
        1200);
    render();
}

void PinWindow::copyTranslatedText() {
    const bool isZh = isChineseUiLanguage();
    const std::string combined = InPlaceTranslateEngine::joinTranslatedTexts(m_translatedParagraphs);
    if (combined.empty()) {
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"暂无可复制的译文", L"No translated text available")});
        return;
    }

    const std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
    tools3000::core::WinUtils::copyToClipboard(wtext);
    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
        tools3000::core::trWCstrFor(isZh, L"已复制全部译文到剪贴板", L"Translated text copied to clipboard")});
}

void PinWindow::translateInPlace(bool selectedOnly, bool forceRetranslate) {
    if (m_sourceImage.empty() || !m_hwnd || !IsWindow(m_hwnd)) return;
    if (m_folded) {
        toggleFold();
    }
    if (m_editingTextReplaceIdx >= 0) {
        commitInPlaceTextReplace(true);
    }

    const bool isZh = isChineseUiLanguage();

    // 已有全图译图且非强制重译、非局部划选翻译时：单击即 0ms 瞬切原图/译图对照
    if (!selectedOnly && !forceRetranslate && !m_translatedImage.empty()) {
        toggleTranslationOverlay();
        return;
    }

    if (m_translationInFlight.load(std::memory_order_acquire)) {
        if (!forceRetranslate) {
            showTransientHudBadge(
                tools3000::core::trWCstrFor(isZh, L"正在翻译中...", L"Translating..."), 900);
            return;
        }
        if (m_translateCancelFlag) {
            m_translateCancelFlag->store(true, std::memory_order_release);
        }
    }

    // 若尚未完成 OCR：先自动异步执行离线 OCR，完成后无缝衔接原位翻译
    if (!m_ocrReady.load(std::memory_order_acquire)) {
        m_pendingTranslateAfterOcr.store(true, std::memory_order_release);
        m_pendingTranslateSelectedOnly.store(selectedOnly, std::memory_order_release);
        ensureOcrExecutedAsync(/*silentPreload=*/false);
        return;
    }

    // 采集待翻译块与全量视觉障碍物（所有原始 OCR 块 + 二维码包围盒，防止译文踩踏编号或二维码）
    std::vector<PinOcrBlock> inputBlocks;
    std::vector<size_t> selectedSubset;
    std::vector<cv::Rect> extraObstacles;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        extraObstacles.reserve(m_ocrBlocks.size() + m_qrCodes.size());
        std::vector<cv::Rect> qrBoxes;
        qrBoxes.reserve(m_qrCodes.size());
        for (const auto& qr : m_qrCodes) {
            if (qr.origBox.width > 0 && qr.origBox.height > 0) {
                qrBoxes.push_back(qr.origBox);
                extraObstacles.push_back(qr.origBox);
            }
        }
        for (const auto& blk : m_ocrBlocks) {
            if (blk.origBox.width > 0 && blk.origBox.height > 0) {
                extraObstacles.push_back(blk.origBox);
            }
        }

        auto overlapsQrCode = [&](const cv::Rect& rc) noexcept -> bool {
            if (rc.area() <= 0) return false;
            const cv::Point center(rc.x + rc.width / 2, rc.y + rc.height / 2);
            for (const auto& q : qrBoxes) {
                const cv::Rect qPad(q.x - 4, q.y - 4, q.width + 8, q.height + 8);
                if (qPad.contains(center)) return true;
                const cv::Rect inter = rc & qPad;
                if (inter.area() * 10 >= rc.area() * 4) return true;
            }
            return false;
        };

        if (selectedOnly && !m_selectedBlockIndices.empty()) {
            if (!m_selectedCharSpans.empty()) {
                inputBlocks.reserve(m_selectedCharSpans.size());
                for (const auto& sp : m_selectedCharSpans) {
                    if (sp.blockIndex >= m_ocrBlocks.size() || sp.startChar >= sp.endExclusive) continue;
                    const auto& blk = m_ocrBlocks[sp.blockIndex];
                    std::string subText = tools3000::ocr::OcrEngine::sliceUtf8Codepoints(
                        blk.text, sp.startChar, sp.endExclusive);
                    if (subText.empty()) continue;

                    cv::Rect subBox = blk.origBox;
                    std::vector<cv::Rect> subChars;
                    if (!blk.charBoxes.empty() && sp.startChar < blk.charBoxes.size()) {
                        const size_t lastIdx = (std::min)(sp.endExclusive, blk.charBoxes.size()) - 1;
                        int x1 = blk.charBoxes[sp.startChar].x;
                        int y1 = blk.charBoxes[sp.startChar].y;
                        int x2 = blk.charBoxes[sp.startChar].x + blk.charBoxes[sp.startChar].width;
                        int y2 = blk.charBoxes[sp.startChar].y + blk.charBoxes[sp.startChar].height;
                        for (size_t k = sp.startChar; k <= lastIdx; ++k) {
                            const auto& cb = blk.charBoxes[k];
                            subChars.push_back(cb);
                            x1 = (std::min)(x1, cb.x);
                            y1 = (std::min)(y1, cb.y);
                            x2 = (std::max)(x2, cb.x + cb.width);
                            y2 = (std::max)(y2, cb.y + cb.height);
                        }
                        subBox = cv::Rect(x1, y1, (std::max)(1, x2 - x1), (std::max)(1, y2 - y1));
                    }
                    inputBlocks.push_back(PinOcrBlock{
                        std::move(subText), subBox, std::move(subChars),
                        blk.layoutRegionId, blk.alignedRowId
                    });
                }
            } else {
                inputBlocks = m_ocrBlocks;
                selectedSubset = m_selectedBlockIndices;
            }
        } else {
            // 全图翻译自动过滤落在二维码/条形码区域内的误识孤字（如二维码定位点误识为“口”-> mouth）
            inputBlocks.reserve(m_ocrBlocks.size());
            for (const auto& blk : m_ocrBlocks) {
                if (overlapsQrCode(blk.origBox)) continue;
                inputBlocks.push_back(blk);
            }
        }
    }

    auto paragraphs = InPlaceTranslateEngine::buildTranslationParagraphs(
        inputBlocks, cv::Size(m_sourceImage.cols, m_sourceImage.rows), selectedSubset, m_sourceImage);
    if (paragraphs.empty()) {
        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
            tools3000::core::trWCstrFor(isZh, L"未识别到可翻译文字", L"No translatable text recognized")});
        return;
    }

    if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    const uint64_t expectedVer = m_translationVersion.fetch_add(1, std::memory_order_acq_rel) + 1;
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    m_translateCancelFlag = cancelFlag;
    m_translationInFlight.store(true, std::memory_order_release);
    render();

    const cv::Mat baseCanvas = m_sourceImage.clone();
    const auto settings = tools3000::core::translate::TranslationService::instance().loadSettings();
    const std::string uiLangTag = tools3000::core::UiI18n::getEffectiveLanguageTag();
    const std::string traceId = tools3000::core::TraceId::current();
    std::weak_ptr<PinWindow> weakSelf = shared_from_this();
    auto activeWorkers = m_ocrActiveWorkers;
    if (activeWorkers) {
        activeWorkers->fetch_add(1, std::memory_order_acq_rel);
    }
    s_globalActiveOcrWorkers.fetch_add(1, std::memory_order_acq_rel);

    try {
        std::thread([weakSelf,
                     baseCanvas,
                     paragraphs = std::move(paragraphs),
                     extraObstacles = std::move(extraObstacles),
                     settings,
                     uiLangTag,
                     cancelFlag,
                     expectedVer,
                     traceId,
                     activeWorkers]() mutable {
            struct TranslateWorkerGuard {
                std::shared_ptr<std::atomic<uint32_t>> workers;
                ~TranslateWorkerGuard() {
                    cleanupOcrWorkingSet();
                    if (workers) {
                        workers->fetch_sub(1, std::memory_order_acq_rel);
                    }
                    s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
                }
            } workerGuard{activeWorkers};

            if (s_ocrShuttingDown.load(std::memory_order_acquire) ||
                (cancelFlag && cancelFlag->load(std::memory_order_acquire))) {
                return;
            }

            tools3000::core::TraceId::setCurrent(traceId);
            tools3000::core::translate::TranslationBatchRequest req;
            req.uiLanguageCode = uiLangTag;
            req.texts.reserve(paragraphs.size());
            for (const auto& p : paragraphs) {
                req.texts.push_back(p.originalText);
            }

            auto batchRes = tools3000::core::translate::TranslationService::instance()
                                .translateBatchWithSettings(req, settings, cancelFlag.get());

            if (s_ocrShuttingDown.load(std::memory_order_acquire) ||
                (cancelFlag && cancelFlag->load(std::memory_order_acquire))) {
                return;
            }

            InPlaceTranslateRenderResult renderRes;
            if (batchRes.success && batchRes.translations.size() == paragraphs.size()) {
                for (size_t i = 0; i < paragraphs.size(); ++i) {
                    paragraphs[i].translatedText = batchRes.translations[i];
                }
                renderRes = InPlaceTranslateEngine::renderTranslatedCanvas(
                    baseCanvas, std::move(paragraphs), batchRes.effectiveTargetLang, extraObstacles);
            }

            tools3000::core::MainThreadDispatcher::instance().post(
                [weakSelf,
                 expectedVer,
                 batchRes = std::move(batchRes),
                 renderRes = std::move(renderRes)]() mutable {
                    if (s_ocrShuttingDown.load(std::memory_order_acquire)) return;
                    auto self = weakSelf.lock();
                    if (!self || !self->isAlive()) return;
                    if (self->m_translationVersion.load(std::memory_order_acquire) != expectedVer) {
                        return;
                    }

                    self->m_translationInFlight.store(false, std::memory_order_release);
                    const bool zh = isChineseUiLanguage();

                    if (!batchRes.success) {
                        LOG_WARN("贴图原位翻译失败: code={}, detail={}",
                                 batchRes.errorCode, batchRes.errorDetail);
                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                            buildFriendlyTranslateErrorToast(batchRes, zh)});
                        self->render();
                        return;
                    }

                    if (!renderRes.success || renderRes.renderedCount == 0 || renderRes.translatedCanvas.empty()) {
                        LOG_INFO("贴图原位翻译无变化: 全部段落与原文一致（如代码标识、文件名或路径）");
                        tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                            tools3000::core::trWCstrFor(
                                zh,
                                L"未发现可翻译正文（代码标识、文件名或路径保持原样）",
                                L"No translatable prose found (code identifiers, filenames or paths kept as-is)")});
                        self->render();
                        return;
                    }

                    self->m_translatedImage = std::move(renderRes.translatedCanvas);
                    self->m_translatedParagraphs = std::move(renderRes.paragraphs);
                    self->m_translatedTargetLang = batchRes.effectiveTargetLang;
                    self->m_translatedProviderUsed = batchRes.providerUsed;
                    self->m_showingTranslation = true;
                    {
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        self->m_selectedBlockIndices.clear();
                        self->m_selectedCharSpans.clear();
                        self->m_hoveredBlockIndex = -1;
                    }
                    self->createRenderResources(self->activeDisplayImage());
                    self->render();

                    LOG_INFO("贴图原位翻译完成: 段落={}, 目标语={}, 通道={}, 耗时={}ms",
                             self->m_translatedParagraphs.size(),
                             self->m_translatedTargetLang,
                             self->m_translatedProviderUsed,
                             batchRes.elapsedMs);
                    tools3000::core::EventBus::instance().publish(tools3000::core::ShowToastEvent{
                        tools3000::core::trWCstrFor(
                            zh,
                            L"原位翻译完成（按 Tab 或点击底部胶囊切换原图对照）",
                            L"Translation complete (Press Tab or click pill for original)")});
                });
        }).detach();
    } catch (...) {
        m_translationInFlight.store(false, std::memory_order_release);
        if (activeWorkers) {
            activeWorkers->fetch_sub(1, std::memory_order_acq_rel);
        }
        s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
        render();
    }
}

} // namespace tools3000::capture
