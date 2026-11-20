// ─────────────────────────────────────────────────────────────────────────────
// PinWindowOcr.cpp — 贴图窗口实景 OCR、选字/表格/二维码与文字批注子模块
//
// 职责:
//   - 贴图后台异步 OCR 预热、外部识别页复用、二维码复制与表格双格式复制
//   - 字符级选区拼装、双击选词、全选与对照精修面板联动
//   - 选中文字荧光笔 / 波浪线 / 直线下划线叠加合成及独立撤销/重做栈
// ─────────────────────────────────────────────────────────────────────────────

#include "capture/PinWindowInternal.h"
#include "capture/PinPasteCoordinator.h"
#include "capture/TextAnnotationHelper.h"
#include "capture/TextReplaceEngine.h"
#include "core/config/ConfigManager.h"
#include "core/events/EventBus.h"
#include "core/events/MainThreadDispatcher.h"
#include "core/logger/Logger.h"
#include "core/utils/ThemeUtils.h"
#include "core/utils/TraceId.h"
#include "core/utils/WinUtils.h"
#include "ocr/OcrEngine.h"
#include "ocr/OcrResultWindow.h"
#include "ocr/OcrTableReconstructor.h"

#include <opencv2/imgproc.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace tools3000::capture {

using namespace pin_detail;

namespace pin_detail {
void cleanupOcrWorkingSet() {
    tools3000::core::WinUtils::trimWorkingSet();
}

void normalizePinOcrBlocksAndTableState(
    std::vector<PinOcrBlock>& blocks,
    const cv::Mat& sourceImage,
    bool& outHasTable) {
    if (blocks.empty()) {
        outHasTable = false;
        return;
    }

    bool needsLayoutInference = false;
    std::vector<tools3000::ocr::OcrResult> stageLines;
    stageLines.reserve(blocks.size());
    for (auto& blk : blocks) {
        if (blk.text.empty()) continue;
        const size_t cpCount = tools3000::ocr::OcrEngine::countUtf8Codepoints(blk.text);
        if (blk.charBoxes.size() != cpCount) {
            blk.charBoxes = tools3000::ocr::OcrEngine::buildProportionalCharBoxes(blk.text, blk.origBox);
        }
        if (blk.alignedRowId < 0) {
            needsLayoutInference = true;
        }
        tools3000::ocr::OcrResult r;
        r.text = blk.text;
        r.confidence = 1.0f;
        r.box = blk.origBox;
        r.charBoxes = blk.charBoxes;
        r.layoutRegionId = blk.layoutRegionId;
        r.alignedRowId = blk.alignedRowId;
        stageLines.push_back(std::move(r));
    }

    if (needsLayoutInference && !stageLines.empty()) {
        stageLines = tools3000::ocr::OcrEngine::assembleRecognizedBlocksWithLayout(
            std::move(stageLines), sourceImage);
    }

    // 拆解含 \t 的对齐行为零空白膨胀的紧凑原子块（同时 100% 继承 layoutRegionId 与 alignedRowId）
    auto atomicResults = tools3000::ocr::OcrEngine::splitIntoAtomicBlocks(stageLines);
    blocks.clear();
    blocks.reserve(atomicResults.size());
    for (auto& ar : atomicResults) {
        if (ar.text.empty()) continue;
        blocks.push_back(PinOcrBlock{
            ar.text,
            ar.box,
            ar.charBoxes,
            ar.layoutRegionId,
            ar.alignedRowId
        });
    }

    outHasTable = tools3000::ocr::OcrTableReconstructor::hasTableStructure(atomicResults);
}

size_t locateNearestCharInBlock(const PinOcrBlock& blk, float mx) noexcept {
    if (blk.charBoxes.empty()) return 0;
    for (size_t c = 0; c < blk.charBoxes.size(); ++c) {
        const auto& cb = blk.charBoxes[c];
        if (mx >= static_cast<float>(cb.x) && mx <= static_cast<float>(cb.x + cb.width)) {
            return c;
        }
    }
    size_t bestIdx = 0;
    float bestDist = FLT_MAX;
    for (size_t c = 0; c < blk.charBoxes.size(); ++c) {
        const auto& cb = blk.charBoxes[c];
        const float cx = static_cast<float>(cb.x) + static_cast<float>(cb.width) * 0.5f;
        const float d = std::abs(mx - cx);
        if (d < bestDist) {
            bestDist = d;
            bestIdx = c;
        }
    }
    return bestIdx;
}

std::string joinAlignedSelectedPieces(
    std::vector<tools3000::ocr::OcrResult>& pieces,
    const char* lineEnding) {
    if (pieces.empty()) return {};
    const char* eol = (lineEnding != nullptr) ? lineEnding : "\r\n";

    bool allHaveRowIds = true;
    for (const auto& p : pieces) {
        if (p.alignedRowId < 0) {
            allHaveRowIds = false;
            break;
        }
    }
    if (!allHaveRowIds) {
        return tools3000::ocr::OcrEngine::orderTextBlocksWithXYCut(pieces, eol);
    }

    std::stable_sort(pieces.begin(), pieces.end(), [](const tools3000::ocr::OcrResult& a,
                                                      const tools3000::ocr::OcrResult& b) {
        if (a.layoutRegionId != b.layoutRegionId) return a.layoutRegionId < b.layoutRegionId;
        if (a.alignedRowId != b.alignedRowId) return a.alignedRowId < b.alignedRowId;
        return a.box.x < b.box.x;
    });

    std::string out;
    const tools3000::ocr::OcrResult* prevPiece = nullptr;
    for (size_t i = 0; i < pieces.size(); ++i) {
        if (pieces[i].text.empty()) continue;
        if (!out.empty() && prevPiece != nullptr) {
            if (pieces[i].layoutRegionId == prevPiece->layoutRegionId &&
                pieces[i].alignedRowId == prevPiece->alignedRowId) {
                out.push_back('\t');
            } else if (!tools3000::ocr::OcrEngine::shouldSeamlessJoinSoftWrappedJsonLine(
                           out, pieces[i].text, &prevPiece->box, &pieces[i].box)) {
                out += eol;
            }
        }
        out += pieces[i].text;
        prevPiece = &pieces[i];
    }
    return out;
}

std::string assembleSelectedSpansText(const std::vector<PinOcrBlock>& blocks,
                                      const std::vector<size_t>& selectedBlockIndices,
                                      const std::vector<PinBlockCharSpan>& selectedCharSpans) {
    if (!selectedCharSpans.empty()) {
        std::vector<tools3000::ocr::OcrResult> slicedBlocks;
        slicedBlocks.reserve(selectedCharSpans.size());
        for (const auto& sp : selectedCharSpans) {
            if (sp.blockIndex >= blocks.size() || sp.startChar >= sp.endExclusive) continue;
            const auto& blk = blocks[sp.blockIndex];
            std::string subText = tools3000::ocr::OcrEngine::sliceUtf8Codepoints(
                blk.text, sp.startChar, sp.endExclusive);
            if (subText.empty()) continue;

            cv::Rect subBox = blk.origBox;
            if (!blk.charBoxes.empty() && sp.startChar < blk.charBoxes.size()) {
                const size_t lastIdx = (std::min)(sp.endExclusive, blk.charBoxes.size()) - 1;
                int x1 = blk.charBoxes[sp.startChar].x;
                int y1 = blk.charBoxes[sp.startChar].y;
                int x2 = blk.charBoxes[sp.startChar].x + blk.charBoxes[sp.startChar].width;
                int y2 = blk.charBoxes[sp.startChar].y + blk.charBoxes[sp.startChar].height;
                for (size_t k = sp.startChar; k <= lastIdx; ++k) {
                    const auto& cb = blk.charBoxes[k];
                    x1 = (std::min)(x1, cb.x);
                    y1 = (std::min)(y1, cb.y);
                    x2 = (std::max)(x2, cb.x + cb.width);
                    y2 = (std::max)(y2, cb.y + cb.height);
                }
                subBox = cv::Rect(x1, y1, (std::max)(1, x2 - x1), (std::max)(1, y2 - y1));
            }
            tools3000::ocr::OcrResult r;
            r.text = std::move(subText);
            r.confidence = 1.0f;
            r.box = subBox;
            r.layoutRegionId = blk.layoutRegionId;
            r.alignedRowId = blk.alignedRowId;
            slicedBlocks.push_back(std::move(r));
        }
        return joinAlignedSelectedPieces(slicedBlocks, "\r\n");
    }

    if (selectedBlockIndices.empty()) return {};
    std::vector<tools3000::ocr::OcrResult> selectedBlocks;
    selectedBlocks.reserve(selectedBlockIndices.size());
    for (size_t idx : selectedBlockIndices) {
        if (idx < blocks.size() && !blocks[idx].text.empty()) {
            tools3000::ocr::OcrResult r;
            r.text = blocks[idx].text;
            r.confidence = 1.0f;
            r.box = blocks[idx].origBox;
            r.layoutRegionId = blocks[idx].layoutRegionId;
            r.alignedRowId = blocks[idx].alignedRowId;
            selectedBlocks.push_back(std::move(r));
        }
    }
    return joinAlignedSelectedPieces(selectedBlocks, "\r\n");
}

enum class WordCharCategory {
    WhitespaceOrPunct = 0,
    AsciiWord = 1,
    CjkGlyph = 2,
};

WordCharCategory classifyUtf8CodepointForWord(std::string_view cpUtf8) noexcept {
    if (cpUtf8.empty()) return WordCharCategory::WhitespaceOrPunct;
    if (cpUtf8.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(cpUtf8[0]);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-') {
            return WordCharCategory::AsciiWord;
        }
        return WordCharCategory::WhitespaceOrPunct;
    }
    const std::wstring w = tools3000::core::WinUtils::utf8ToWstring(std::string(cpUtf8));
    if (w.empty()) return WordCharCategory::WhitespaceOrPunct;
    const wchar_t wc = w[0];
    if ((wc >= 0x4E00 && wc <= 0x9FFF) || (wc >= 0x3400 && wc <= 0x4DBF) ||
        (wc >= 0xAC00 && wc <= 0xD7AF) || (wc >= 0x3040 && wc <= 0x30FF)) {
        return WordCharCategory::CjkGlyph;
    }
    return WordCharCategory::WhitespaceOrPunct;
}

} // namespace pin_detail

bool PinWindow::isLiveTextEnabled() const noexcept {
    try {
        return tools3000::core::ConfigManager::instance().get<bool>("/ocr/liveTextOnPin", true);
    } catch (...) {
        return true;
    }
}

void PinWindow::invalidateOcrStateAndRefreshIfSelecting() {
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_qrCodes.clear();
        m_hasTableStructure = false;
        m_selectedBlockIndices.clear();
        m_selectedCharSpans.clear();
        m_hoveredBlockIndex = -1;
        m_hoveredQrBadgeIndex = -1;
        m_isCharStreamSelecting = false;
        m_anchorBlockIndex = -1;
        m_anchorCharIndex = 0;
        m_pendingSelectAllOcr = false;
        m_pendingCopyAllOcr = false;
        m_pendingOpenPanelOcr = false;
        m_silentOcrPreload = false;
        ++m_ocrVersion;
        m_ocrInFlightVersion = 0;
        if (!m_pinnedSourceText.empty()) {
            const std::string cardUtf8 = tools3000::core::WinUtils::wstringToUtf8(m_pinnedSourceText);
            const cv::Rect cardBox(4, 4, (std::max)(1, m_origWidth - 8), (std::max)(1, m_origHeight - 8));
            m_ocrBlocks.push_back(PinOcrBlock{
                cardUtf8,
                cardBox,
                tools3000::ocr::OcrEngine::buildProportionalCharBoxes(cardUtf8, cardBox)
            });
            m_ocrReady = true;
        } else {
            m_ocrReady = false;
        }
    }
    if (m_textSelectionMode && m_pinnedSourceText.empty()) {
        ensureOcrExecutedAsync(false);
    }
}

void PinWindow::ensureOcrExecutedAsync(bool silentPreload) {
    if (s_ocrShuttingDown.load(std::memory_order_acquire) ||
        m_sourceImage.empty() || !m_pinnedSourceText.empty() || m_ocrReady.load()) {
        return;
    }
    if (!silentPreload) {
        m_silentOcrPreload.store(false, std::memory_order_release);
    }
    uint64_t currentVer = 0;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (s_ocrShuttingDown.load(std::memory_order_acquire) || m_ocrReady.load() || m_sourceImage.empty()) {
            return;
        }
        currentVer = m_ocrVersion.load();
        if (currentVer == 0) currentVer = ++m_ocrVersion;
        if (m_ocrInFlightVersion.load() == currentVer) {
            if (!silentPreload && m_hwnd && IsWindow(m_hwnd)) {
                InvalidateRect(m_hwnd, nullptr, FALSE);
            }
            return;
        }
        m_silentOcrPreload.store(silentPreload, std::memory_order_release);
        m_ocrInFlightVersion.store(currentVer);
    }
    ++m_ocrRunCount;
    cv::Mat imgCopy = m_sourceImage.clone();
    std::weak_ptr<PinWindow> weakSelf = shared_from_this();
    auto activeWorkers = m_ocrActiveWorkers;
    if (activeWorkers) {
        activeWorkers->fetch_add(1, std::memory_order_acq_rel);
    }
    s_globalActiveOcrWorkers.fetch_add(1, std::memory_order_acq_rel);

    auto applyRecognizedPage = [](const std::shared_ptr<PinWindow>& self,
                                  const tools3000::ocr::OcrPageExtraction& page,
                                  uint64_t expectedVer) {
        if (self->m_ocrVersion.load() != expectedVer) return;
        self->m_ocrBlocks.clear();
        self->m_ocrBlocks.reserve(page.lines.size());
        for (const auto& r : page.lines) {
            if (!r.text.empty()) {
                self->m_ocrBlocks.push_back(PinOcrBlock{
                    r.text, r.box, r.charBoxes, r.layoutRegionId, r.alignedRowId
                });
            }
        }
        normalizePinOcrBlocksAndTableState(self->m_ocrBlocks, self->m_sourceImage, self->m_hasTableStructure);

        self->m_qrCodes.clear();
        self->m_qrCodes.reserve(page.qrCodes.size());
        for (const auto& q : page.qrCodes) {
            if (!q.text.empty()) {
                self->m_qrCodes.push_back(PinQrCodeItem{q.text, q.boundingBox, {}});
            }
        }

        self->m_silentOcrPreload.store(false, std::memory_order_release);
        self->m_ocrReady = true;

        auto buildAllTextUtf8 = [&]() -> std::string {
            std::vector<tools3000::ocr::OcrResult> allBlocks;
            allBlocks.reserve(self->m_ocrBlocks.size());
            for (const auto& blk : self->m_ocrBlocks) {
                if (!blk.text.empty()) {
                    tools3000::ocr::OcrResult r;
                    r.text = blk.text;
                    r.confidence = 1.0f;
                    r.box = blk.origBox;
                    r.layoutRegionId = blk.layoutRegionId;
                    r.alignedRowId = blk.alignedRowId;
                    allBlocks.push_back(std::move(r));
                }
            }
            std::string txt = joinAlignedSelectedPieces(allBlocks, "\r\n");
            if (txt.empty() && !self->m_qrCodes.empty()) {
                for (size_t qi = 0; qi < self->m_qrCodes.size(); ++qi) {
                    if (qi > 0) txt += "\r\n";
                    txt += self->m_qrCodes[qi].text;
                }
            }
            return txt;
        };

        if (self->m_pendingSelectAllOcr.exchange(false)) {
            if (!self->m_ocrBlocks.empty()) {
                self->m_textSelectionMode = true;
                self->m_selectedBlockIndices.resize(self->m_ocrBlocks.size());
                self->m_selectedCharSpans.clear();
                self->m_selectedCharSpans.reserve(self->m_ocrBlocks.size());
                for (size_t i = 0; i < self->m_ocrBlocks.size(); ++i) {
                    self->m_selectedBlockIndices[i] = i;
                    self->m_selectedCharSpans.push_back(PinBlockCharSpan{
                        i, 0, self->m_ocrBlocks[i].charBoxes.size()
                    });
                }
                const bool isZh = tools3000::core::isUiLanguageChinese();
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"已全选文字，按 Ctrl+C 复制"
                                                         : L"All text selected, press Ctrl+C to copy"});
            } else {
                const bool isZh = tools3000::core::isUiLanguageChinese();
                tools3000::core::EventBus::instance().publish(
                    tools3000::core::ShowToastEvent{isZh ? L"未识别到文字"
                                                         : L"No text recognized"});
            }
        }

        const bool wantCopyAll = self->m_pendingCopyAllOcr.exchange(false);
        const bool wantOpenPanel = self->m_pendingOpenPanelOcr.exchange(false);
        if (wantCopyAll || wantOpenPanel) {
            const std::string fullText = buildAllTextUtf8();
            const bool isZh = tools3000::core::isUiLanguageChinese();
            if (wantCopyAll) {
                if (!fullText.empty()) {
                    const std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(fullText);
                    tools3000::core::WinUtils::copyToClipboard(wtext);
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{isZh ? L"已提取文本并复制到剪贴板"
                                                             : L"Text extracted and copied to clipboard"});
                } else {
                    tools3000::core::EventBus::instance().publish(
                        tools3000::core::ShowToastEvent{isZh ? L"未识别到文字"
                                                             : L"No text recognized"});
                }
            }
            if (wantOpenPanel) {
                RECT pinRc{};
                const bool hasRc = (self->m_hwnd && IsWindow(self->m_hwnd) && GetWindowRect(self->m_hwnd, &pinRc) != FALSE);
                const std::string display = fullText.empty()
                    ? (isZh ? "未识别到文字" : "No text recognized")
                    : fullText;
                tools3000::ocr::OcrResultWindow::instance().showResult(display, hasRc ? &pinRc : nullptr);
            }
        }
        if (self->m_hwnd && IsWindow(self->m_hwnd)) {
            InvalidateRect(self->m_hwnd, nullptr, FALSE);
        }
    };

    if (!silentPreload && m_hwnd && IsWindow(m_hwnd)) {
        InvalidateRect(m_hwnd, nullptr, FALSE);
    }

    try {
        std::thread([weakSelf, imgCopy, currentVer, activeWorkers, applyRecognizedPage]() {
            bool inFlightHandedOff = false;
            struct WorkerScopeGuard {
                std::weak_ptr<PinWindow> weakPin;
                uint64_t ver;
                bool* handedOff;
                std::shared_ptr<std::atomic<uint32_t>> workers;
                ~WorkerScopeGuard() {
                    if (handedOff && !*handedOff) {
                        if (auto self = weakPin.lock()) {
                            uint64_t expected = ver;
                            self->m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
                        }
                    }
                    cleanupOcrWorkingSet();
                    if (workers) {
                        workers->fetch_sub(1, std::memory_order_acq_rel);
                    }
                    s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
                }
            } workerGuard{weakSelf, currentVer, &inFlightHandedOff, activeWorkers};

            if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            tools3000::core::TraceId::Scope traceScope;
            auto page = tools3000::ocr::OcrEngine::instance().extractPage(imgCopy);
            if (s_ocrShuttingDown.load(std::memory_order_acquire)) {
                return;
            }
            const bool posted = tools3000::core::MainThreadDispatcher::instance().post(
                [weakSelf, page, currentVer, applyRecognizedPage]() {
                    if (s_ocrShuttingDown.load(std::memory_order_acquire)) return;
                    if (auto self = weakSelf.lock()) {
                        uint64_t expected = currentVer;
                        self->m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
                        if (self->m_ocrVersion.load() != currentVer) return;
                        std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                        applyRecognizedPage(self, page, currentVer);
                    }
                });
            if (posted) {
                inFlightHandedOff = true;
            } else if (!s_ocrShuttingDown.load(std::memory_order_acquire)) {
                if (auto self = weakSelf.lock()) {
                    std::lock_guard<std::mutex> lock(self->m_ocrMutex);
                    uint64_t expected = currentVer;
                    self->m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
                    inFlightHandedOff = true;
                    applyRecognizedPage(self, page, currentVer);
                }
            }
        }).detach();
    } catch (...) {
        uint64_t expected = currentVer;
        m_ocrInFlightVersion.compare_exchange_strong(expected, 0);
        if (activeWorkers) {
            activeWorkers->fetch_sub(1, std::memory_order_acq_rel);
        }
        s_globalActiveOcrWorkers.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void PinWindow::injectRecognizedPage(std::vector<tools3000::ocr::OcrResult> lines,
                                     std::vector<tools3000::ocr::OcrQrResult> qrCodes) {
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks.clear();
        m_ocrBlocks.reserve(lines.size());
        for (auto& r : lines) {
            if (!r.text.empty()) {
                m_ocrBlocks.push_back(PinOcrBlock{
                    std::move(r.text), r.box, std::move(r.charBoxes), r.layoutRegionId, r.alignedRowId
                });
            }
        }
        normalizePinOcrBlocksAndTableState(m_ocrBlocks, m_sourceImage, m_hasTableStructure);

        m_qrCodes.clear();
        m_qrCodes.reserve(qrCodes.size());
        for (auto& q : qrCodes) {
            if (!q.text.empty()) {
                m_qrCodes.push_back(PinQrCodeItem{std::move(q.text), q.boundingBox, {}});
            }
        }
        m_selectedBlockIndices.clear();
        m_selectedCharSpans.clear();
        m_hoveredBlockIndex = -1;
        m_hoveredQrBadgeIndex = -1;
        m_silentOcrPreload = false;
        m_ocrReady = true;
        ++m_ocrVersion;
        m_ocrInFlightVersion = 0;
    }
    render();
}

void PinWindow::selectAllText() {
    if (m_folded) return;
    if (!m_ocrReady.load()) {
        m_pendingSelectAllOcr.store(true);
        ensureOcrExecutedAsync(false);
        return;
    }

    size_t countSelected = 0;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (!m_ocrBlocks.empty()) {
            m_textSelectionMode = true;
            m_selectedBlockIndices.resize(m_ocrBlocks.size());
            m_selectedCharSpans.clear();
            m_selectedCharSpans.reserve(m_ocrBlocks.size());
            for (size_t i = 0; i < m_ocrBlocks.size(); ++i) {
                m_selectedBlockIndices[i] = i;
                m_selectedCharSpans.push_back(PinBlockCharSpan{
                    i, 0, m_ocrBlocks[i].charBoxes.size()
                });
            }
            countSelected = m_selectedBlockIndices.size();
        }
    }

    const bool isZh = tools3000::core::isUiLanguageChinese();
    if (countSelected > 0) {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已全选文字，按 Ctrl+C 复制"
                                                 : L"All text selected, press Ctrl+C to copy"});
        render();
    } else {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"未识别到文字"
                                                 : L"No text recognized"});
    }
}

size_t PinWindow::selectedTextBlockCount() const {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    return m_selectedBlockIndices.size();
}

std::string PinWindow::selectedTextUtf8() const {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    return assembleSelectedSpansText(m_ocrBlocks, m_selectedBlockIndices, m_selectedCharSpans);
}

bool PinWindow::hasDetectedTable() const {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    return m_hasTableStructure;
}

size_t PinWindow::qrCodeCount() const {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    return m_qrCodes.size();
}

void PinWindow::setOcrBlocksForTest(std::vector<PinOcrBlock> blocks) {
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_ocrBlocks = std::move(blocks);
        normalizePinOcrBlocksAndTableState(m_ocrBlocks, m_sourceImage, m_hasTableStructure);
        m_selectedBlockIndices.clear();
        m_selectedCharSpans.clear();
        m_hoveredBlockIndex = -1;
        m_ocrReady = true;
        m_pendingSelectAllOcr = false;
        m_pendingCopyAllOcr = false;
        m_pendingOpenPanelOcr = false;
        m_silentOcrPreload = false;
        ++m_ocrVersion;
        m_ocrInFlightVersion = 0;
    }
    render();
}

void PinWindow::setQrCodesForTest(std::vector<tools3000::ocr::OcrQrResult> qrCodes) {
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        m_qrCodes.clear();
        for (auto& q : qrCodes) {
            if (!q.text.empty()) {
                m_qrCodes.push_back(PinQrCodeItem{std::move(q.text), q.boundingBox, {}});
            }
        }
        m_hoveredQrBadgeIndex = -1;
        m_ocrReady = true;
    }
    render();
}

bool PinWindow::selectWordAtClientPoint(int clientX, int clientY) {
    if (m_folded || !m_ocrReady.load()) return false;
    const bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    if (!alt && !m_textSelectionMode && !isLiveTextEnabled()) return false;

    bool selectedAny = false;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        const float safeScale = (std::max)(0.05f, m_scale);
        const float mx = static_cast<float>(clientX) / safeScale;
        const float my = static_cast<float>(clientY) / safeScale;
        const cv::Point pt(static_cast<int>(mx), static_cast<int>(my));

        for (size_t b = 0; b < m_ocrBlocks.size(); ++b) {
            const auto& blk = m_ocrBlocks[b];
            if (!blk.origBox.contains(pt)) continue;

            const auto cps = tools3000::ocr::OcrEngine::splitUtf8Codepoints(blk.text);
            if (cps.empty()) continue;

            const size_t hitChar = (std::min)(locateNearestCharInBlock(blk, mx), cps.size() - 1);
            const WordCharCategory cat = classifyUtf8CodepointForWord(cps[hitChar]);
            size_t ws = hitChar;
            size_t we = hitChar + 1;
            if (cat != WordCharCategory::WhitespaceOrPunct) {
                while (ws > 0 && classifyUtf8CodepointForWord(cps[ws - 1]) == cat) {
                    --ws;
                }
                while (we < cps.size() && classifyUtf8CodepointForWord(cps[we]) == cat) {
                    ++we;
                }
            }
            m_selectedBlockIndices = {b};
            m_selectedCharSpans = {PinBlockCharSpan{b, ws, we}};
            m_isSelectingText = false;
            m_isCharStreamSelecting = false;
            selectedAny = true;
            break;
        }
    }

    if (selectedAny) {
        render();
    }
    return selectedAny;
}

void PinWindow::toggleTextSelectionMode() {
    m_textSelectionMode = !m_textSelectionMode;
    if (m_textSelectionMode && !m_ocrReady.load()) {
        ensureOcrExecutedAsync(false);
    }
    if (!m_textSelectionMode) {
        clearTextSelection();
    }
    const bool isZh = tools3000::core::isUiLanguageChinese();
    tools3000::core::EventBus::instance().publish(
        tools3000::core::ShowToastEvent{m_textSelectionMode
            ? (isZh ? L"已进入文字划选模式（拖拽选择文字，按 Esc 退出）" : L"Text selection mode (Drag to select, Esc to exit)")
            : (isZh ? L"已退出文字划选模式" : L"Exited text selection mode")});
    render();
}

void PinWindow::clearTextSelection() {
    std::lock_guard<std::mutex> lock(m_ocrMutex);
    m_selectedBlockIndices.clear();
    m_selectedCharSpans.clear();
    m_hoveredBlockIndex = -1;
    m_isSelectingText = false;
    m_isCharStreamSelecting = false;
    m_showCrossRegionRectGuide = false;
    m_anchorBlockIndex = -1;
    m_anchorCharIndex = 0;
    m_ocrMarkupPreviewClean = false;
}

void PinWindow::copySelectedText() {
    std::string combined;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (m_selectedBlockIndices.empty()) return;
        combined = assembleSelectedSpansText(m_ocrBlocks, m_selectedBlockIndices, m_selectedCharSpans);
        m_selectedBlockIndices.clear();
        m_selectedCharSpans.clear();
    }

    if (!combined.empty()) {
        std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
        tools3000::core::WinUtils::copyToClipboard(wtext);
        bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制文字" : L"Text copied"});
    }
    render();
}

void PinWindow::copySelectedOrAllAsTable() {
    tools3000::ocr::OcrTableGrid tableGrid;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        std::vector<tools3000::ocr::OcrResult> selCandidates;
        for (size_t idx : m_selectedBlockIndices) {
            if (idx < m_ocrBlocks.size() && !m_ocrBlocks[idx].text.empty()) {
                tools3000::ocr::OcrResult r;
                r.text = m_ocrBlocks[idx].text;
                r.confidence = 1.0f;
                r.box = m_ocrBlocks[idx].origBox;
                r.charBoxes = m_ocrBlocks[idx].charBoxes;
                selCandidates.push_back(std::move(r));
            }
        }
        tableGrid = tools3000::ocr::OcrTableReconstructor::reconstruct(selCandidates);
        if (!tableGrid.isTable) {
            std::vector<tools3000::ocr::OcrResult> allCandidates;
            allCandidates.reserve(m_ocrBlocks.size());
            for (const auto& blk : m_ocrBlocks) {
                if (!blk.text.empty()) {
                    tools3000::ocr::OcrResult r;
                    r.text = blk.text;
                    r.confidence = 1.0f;
                    r.box = blk.origBox;
                    r.charBoxes = blk.charBoxes;
                    allCandidates.push_back(std::move(r));
                }
            }
            tableGrid = tools3000::ocr::OcrTableReconstructor::reconstruct(allCandidates);
        }
        if (tableGrid.isTable) {
            m_selectedBlockIndices.clear();
            m_selectedCharSpans.clear();
        }
    }

    const bool isZh = tools3000::core::isUiLanguageChinese();
    if (tableGrid.isTable && tools3000::ocr::OcrTableReconstructor::copyTableToClipboard(tableGrid, m_hwnd)) {
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{
                isZh ? std::format(L"已复制 {}×{} 表格，可直接粘贴至 Excel / 飞书", tableGrid.rowCount, tableGrid.colCount)
                     : std::format(L"Copied {}x{} table, ready to paste into Excel", tableGrid.rowCount, tableGrid.colCount)});
        render();
    } else {
        copySelectedText();
    }
}

void PinWindow::openOcrComparisonPanel() {
    RECT pinRc{};
    const bool hasRc = (m_hwnd && IsWindow(m_hwnd) && GetWindowRect(m_hwnd, &pinRc) != FALSE);
    if (!m_ocrReady.load() && m_pinnedSourceText.empty()) {
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::ocr::OcrResultWindow::instance().showResult(
            isZh ? "正在识别..." : "Recognizing...",
            hasRc ? &pinRc : nullptr);
        m_pendingOpenPanelOcr.store(true, std::memory_order_release);
        ensureOcrExecutedAsync(false);
        return;
    }

    std::string panelText;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (!m_selectedBlockIndices.empty()) {
            panelText = assembleSelectedSpansText(m_ocrBlocks, m_selectedBlockIndices, m_selectedCharSpans);
        }
        if (panelText.empty() && !m_ocrBlocks.empty()) {
            std::vector<tools3000::ocr::OcrResult> allBlocks;
            allBlocks.reserve(m_ocrBlocks.size());
            for (const auto& blk : m_ocrBlocks) {
                if (!blk.text.empty()) {
                    tools3000::ocr::OcrResult r;
                    r.text = blk.text;
                    r.confidence = 1.0f;
                    r.box = blk.origBox;
                    r.layoutRegionId = blk.layoutRegionId;
                    r.alignedRowId = blk.alignedRowId;
                    allBlocks.push_back(std::move(r));
                }
            }
            panelText = joinAlignedSelectedPieces(allBlocks, "\r\n");
        }
    }
    if (!panelText.empty()) {
        tools3000::ocr::OcrResultWindow::instance().showResult(panelText, hasRc ? &pinRc : nullptr);
    }
}

void PinWindow::copyQrCodeText(size_t qrIndex) {
    std::string qrText;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (qrIndex < m_qrCodes.size()) {
            qrText = m_qrCodes[qrIndex].text;
        }
    }
    if (!qrText.empty()) {
        const std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(qrText);
        tools3000::core::WinUtils::copyToClipboard(wtext);
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{
                isZh ? L"已复制二维码内容" : L"QR code copied"});
    }
}

void PinWindow::copyAllText() {
    if (!m_pinnedSourceText.empty()) {
        tools3000::core::WinUtils::copyToClipboard(m_pinnedSourceText);
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制贴图文字" : L"Copied note text"});
        return;
    }

    std::string combined;
    bool hasReadyBlocks = false;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        if (m_ocrReady && !m_ocrBlocks.empty()) {
            hasReadyBlocks = true;
            std::vector<tools3000::ocr::OcrResult> allBlocks;
            allBlocks.reserve(m_ocrBlocks.size());
            for (const auto& blk : m_ocrBlocks) {
                if (!blk.text.empty()) {
                    tools3000::ocr::OcrResult r;
                    r.text = blk.text;
                    r.confidence = 1.0f;
                    r.box = blk.origBox;
                    r.layoutRegionId = blk.layoutRegionId;
                    r.alignedRowId = blk.alignedRowId;
                    allBlocks.push_back(std::move(r));
                }
            }
            combined = joinAlignedSelectedPieces(allBlocks, "\r\n");
        }
    }

    if (hasReadyBlocks && !combined.empty()) {
        std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
        tools3000::core::WinUtils::copyToClipboard(wtext);
        bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制贴图全部文字" : L"Copied all text from pin"});
    } else {
        extractText();
    }
}

void PinWindow::extractText() {
    if (!m_pinnedSourceText.empty()) {
        tools3000::core::WinUtils::copyToClipboard(m_pinnedSourceText);
        const bool isZh = tools3000::core::isUiLanguageChinese();
        tools3000::core::EventBus::instance().publish(
            tools3000::core::ShowToastEvent{isZh ? L"已复制贴图文字" : L"Copied note text"});
        return;
    }
    if (s_ocrShuttingDown.load(std::memory_order_acquire) || !isAlive() || m_sourceImage.empty()) return;

    // 若贴图后台实景 OCR 已经就绪，0ms 直接复用内存版面块复制全文，彻底消灭重复深度学习推理
    if (m_ocrReady.load(std::memory_order_acquire)) {
        std::string combined;
        {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            std::vector<tools3000::ocr::OcrResult> allBlocks;
            allBlocks.reserve(m_ocrBlocks.size());
            for (const auto& blk : m_ocrBlocks) {
                if (!blk.text.empty()) {
                    tools3000::ocr::OcrResult r;
                    r.text = blk.text;
                    r.confidence = 1.0f;
                    r.box = blk.origBox;
                    r.layoutRegionId = blk.layoutRegionId;
                    r.alignedRowId = blk.alignedRowId;
                    allBlocks.push_back(std::move(r));
                }
            }
            combined = joinAlignedSelectedPieces(allBlocks, "\r\n");
            if (combined.empty() && !m_qrCodes.empty()) {
                for (size_t qi = 0; qi < m_qrCodes.size(); ++qi) {
                    if (qi > 0) combined += "\r\n";
                    combined += m_qrCodes[qi].text;
                }
            }
        }
        const bool isZh = tools3000::core::isUiLanguageChinese();
        if (!combined.empty()) {
            const std::wstring wtext = tools3000::core::WinUtils::utf8ToWstring(combined);
            tools3000::core::WinUtils::copyToClipboard(wtext);
            tools3000::core::EventBus::instance().publish(
                tools3000::core::ShowToastEvent{isZh ? L"已提取文本并复制到剪贴板" : L"Text extracted and copied to clipboard"});
        } else {
            tools3000::core::EventBus::instance().publish(
                tools3000::core::ShowToastEvent{isZh ? L"未识别到文字" : L"No text recognized"});
        }
        return;
    }

    // 若后台静默预热已在途或尚未启动，直接挂载完成复制意图并复用同一条 ensureOcrExecutedAsync 推理管线
    m_pendingCopyAllOcr.store(true, std::memory_order_release);
    ensureOcrExecutedAsync(false);
}

void PinWindow::updateImage(const cv::Mat& newImage, bool clearCardMetadata) {
    if (newImage.empty() || !m_hwnd) return;
    if (m_editingTextReplaceIdx >= 0) {
        KillTimer(m_hwnd, 3);
        m_editingTextReplaceIdx = -1;
        m_editingTextReplaceBackup.clear();
        m_editingTextReplaceWasNew = false;
        clearTextReplaceToolbarRects();
    }
    m_colorBackup.release();
    m_grayscale = false;
    m_inverted = false;
    if (clearCardMetadata) {
        m_cardCornerRadius = 0.0f;
        m_pinnedSourceText.clear();
    }
    m_sourceImage = newImage.clone();
    m_cleanBaseImage = newImage.clone();
    m_markupElements.clear();
    m_pinUndoStack.clear();
    m_pinRedoStack.clear();
    m_ocrMarkupPreviewClean = false;
    m_origWidth = newImage.cols;
    m_origHeight = newImage.rows;

    if (m_pinnedSourceText.empty()) {
        {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            m_ocrBlocks.clear();
            m_selectedBlockIndices.clear();
            m_hoveredBlockIndex = -1;
            m_ocrReady = false;
        }
        invalidateOcrStateAndRefreshIfSelecting();
    }

    const int newW = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
    const int newH = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
    if (!m_folded) {
        SetWindowPos(m_hwnd, nullptr, 0, 0, newW, newH, SWP_NOMOVE | SWP_NOZORDER);
        updateWindowCornerRegion();
    }

    createRenderResources(m_sourceImage);
    render();
}

void PinWindow::applyEditedMarkup(
    const cv::Mat& cleanBaseImage,
    const std::vector<MarkupElement>& elements,
    const cv::Mat& precomposited) {
    if (cleanBaseImage.empty() || !m_hwnd) return;
    if (m_editingTextReplaceIdx >= 0) {
        KillTimer(m_hwnd, 3);
        m_editingTextReplaceIdx = -1;
        m_editingTextReplaceBackup.clear();
        m_editingTextReplaceWasNew = false;
        clearTextReplaceToolbarRects();
    }

    m_colorBackup.release();
    m_grayscale = false;
    m_inverted = false;
    m_cleanBaseImage = cleanBaseImage.clone();
    m_markupElements = elements;

    cv::Mat composited;
    if (!precomposited.empty() &&
        precomposited.cols == m_cleanBaseImage.cols &&
        precomposited.rows == m_cleanBaseImage.rows) {
        composited = precomposited.clone();
    } else if (m_markupElements.empty()) {
        composited = m_cleanBaseImage.clone();
    } else {
        MarkupEngine engine;
        cv::Mat baseBgr = m_cleanBaseImage;
        if (baseBgr.channels() == 4) {
            cv::cvtColor(baseBgr, baseBgr, cv::COLOR_BGRA2BGR);
        } else if (baseBgr.channels() == 1) {
            cv::cvtColor(baseBgr, baseBgr, cv::COLOR_GRAY2BGR);
        }
        engine.setBaseImage(baseBgr);
        engine.importElements(m_markupElements);
        composited = engine.getCompositeImage();
    }
    if (composited.empty()) {
        composited = m_cleanBaseImage.clone();
    }

    m_sourceImage = std::move(composited);
    m_origWidth = m_sourceImage.cols;
    m_origHeight = m_sourceImage.rows;

    if (m_pinnedSourceText.empty()) {
        {
            std::lock_guard<std::mutex> lock(m_ocrMutex);
            m_ocrBlocks.clear();
            m_selectedBlockIndices.clear();
            m_hoveredBlockIndex = -1;
            m_ocrReady = false;
        }
        invalidateOcrStateAndRefreshIfSelecting();
    }

    // 保持原始 1:1 m_origWidth/Height 与用户当前 m_scale 倍率恒定不变，彻底根除缩放平方膨胀与长图糊化
    const int keepW = (std::max)(1, static_cast<int>(m_origWidth * m_scale));
    const int keepH = (std::max)(1, static_cast<int>(m_origHeight * m_scale));
    if (!m_folded) {
        SetWindowPos(m_hwnd, nullptr, 0, 0, keepW, keepH, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        updateWindowCornerRegion();
    }

    m_pinUndoStack.clear();
    m_pinRedoStack.clear();
    m_ocrMarkupPreviewClean = false;

    createRenderResources(m_sourceImage);
    render();
}

MarkupColor PinWindow::currentTextMarkupColor() const noexcept {
    const int idx = std::clamp(m_textMarkupColorIndex, 0,
                               static_cast<int>(TextAnnotationConstants::kPresetColorCount) - 1);
    return TextAnnotationConstants::kPresetColors[static_cast<size_t>(idx)];
}

std::vector<cv::Rect> PinWindow::collectCoalescedSelectedTextRectsLocked() const {
    return TextAnnotationHelper::coalesceSelectedTextSpans(
        m_ocrBlocks, m_selectedBlockIndices, m_selectedCharSpans, m_origWidth, m_origHeight);
}

void PinWindow::pushPinMarkupUndoState() {
    m_pinUndoStack.push_back(m_markupElements);
    if (m_pinUndoStack.size() > MarkupEngine::kMaxUndoSteps) {
        m_pinUndoStack.pop_front();
    }
    m_pinRedoStack.clear();
}

void PinWindow::recompositeMarkupPreservingOcr() {
    if (m_cleanBaseImage.empty() || !m_hwnd) return;
    m_colorBackup.release();
    m_grayscale = false;
    m_inverted = false;

    cv::Mat composited;
    if (m_markupElements.empty()) {
        composited = m_cleanBaseImage.clone();
    } else {
        MarkupEngine engine;
        cv::Mat baseBgr = m_cleanBaseImage;
        if (baseBgr.channels() == 4) {
            cv::cvtColor(baseBgr, baseBgr, cv::COLOR_BGRA2BGR);
        } else if (baseBgr.channels() == 1) {
            cv::cvtColor(baseBgr, baseBgr, cv::COLOR_GRAY2BGR);
        }
        engine.setBaseImage(baseBgr);
        engine.importElements(m_markupElements);
        composited = engine.getCompositeImage();
        if (m_cleanBaseImage.channels() == 4 && composited.channels() == 3) {
            std::vector<cv::Mat> bgraCh(4);
            cv::split(m_cleanBaseImage, bgraCh);
            std::vector<cv::Mat> compCh(3);
            cv::split(composited, compCh);
            cv::merge(std::vector<cv::Mat>{compCh[0], compCh[1], compCh[2], bgraCh[3]}, composited);
        }
    }
    if (composited.empty()) {
        composited = m_cleanBaseImage.clone();
    }

    m_sourceImage = std::move(composited);
    m_contentFingerprint = PinPasteCoordinator::computeContentFingerprint(m_sourceImage, m_pinnedSourceText);
    createRenderResources(m_sourceImage);
    render();
}

bool PinWindow::annotateSelectedText(TextMarkupMode mode) {
    if (m_folded || !m_pinnedSourceText.empty() || m_cleanBaseImage.empty()) return false;
    if (mode == TextMarkupMode::RectRegion) return false;

    std::vector<cv::Rect> spans;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        spans = collectCoalescedSelectedTextRectsLocked();
    }
    if (spans.empty()) return false;

    const MarkupColor targetColor = currentTextMarkupColor();
    auto matchesMode = [&](const MarkupElement& el) noexcept -> bool {
        if (el.textSpans.empty()) return false;
        if (mode == TextMarkupMode::Highlight) {
            return el.tool == MarkupTool::Highlight;
        }
        if (mode == TextMarkupMode::WavyUnderline) {
            return el.tool == MarkupTool::Line && el.lineStyle == LineStyle::Wavy;
        }
        return el.tool == MarkupTool::Line && el.lineStyle != LineStyle::Wavy;
    };

    pushPinMarkupUndoState();

    bool foundExisting = false;
    for (auto it = m_markupElements.begin(); it != m_markupElements.end(); ++it) {
        if (matchesMode(*it) && TextAnnotationHelper::spansGeometryEqual(it->textSpans, spans)) {
            foundExisting = true;
            const bool sameRgb = (it->color.r == targetColor.r &&
                                  it->color.g == targetColor.g &&
                                  it->color.b == targetColor.b);
            if (sameRgb) {
                m_markupElements.erase(it);
            } else {
                it->color.r = targetColor.r;
                it->color.g = targetColor.g;
                it->color.b = targetColor.b;
            }
            break;
        }
    }

    if (!foundExisting) {
        int minX = INT_MAX, minY = INT_MAX, maxX = INT_MIN, maxY = INT_MIN;
        int sumH = 0;
        for (const auto& r : spans) {
            minX = (std::min)(minX, r.x);
            minY = (std::min)(minY, r.y);
            maxX = (std::max)(maxX, r.x + r.width);
            maxY = (std::max)(maxY, r.y + r.height);
            sumH += r.height;
        }
        uint32_t nextId = 1;
        for (const auto& ex : m_markupElements) {
            nextId = (std::max)(nextId, ex.id + 1);
        }

        MarkupElement elem;
        elem.id = nextId;
        elem.textSpans = std::move(spans);
        elem.startPt = cv::Point(minX, minY);
        elem.endPt = cv::Point(maxX, maxY + 4);
        elem.color = targetColor;
        if (mode == TextMarkupMode::Highlight) {
            elem.tool = MarkupTool::Highlight;
            elem.color.a = 220;
        } else {
            elem.tool = MarkupTool::Line;
            elem.color.a = 255;
            elem.lineStyle = (mode == TextMarkupMode::WavyUnderline) ? LineStyle::Wavy : LineStyle::Solid;
            const float avgH = static_cast<float>(sumH) / static_cast<float>(elem.textSpans.size());
            elem.thickness = std::clamp(avgH * 0.095f, 1.8f, 2.85f);
        }
        m_markupElements.push_back(std::move(elem));
    }

    m_ocrMarkupPreviewClean = true;
    recompositeMarkupPreservingOcr();
    return true;
}

void PinWindow::setTextMarkupColorIndex(int index) {
    const int clamped = std::clamp(index, 0,
                                   static_cast<int>(TextAnnotationConstants::kPresetColorCount) - 1);
    m_textMarkupColorIndex = clamped;
    const MarkupColor targetColor = currentTextMarkupColor();

    std::vector<cv::Rect> spans;
    {
        std::lock_guard<std::mutex> lock(m_ocrMutex);
        spans = collectCoalescedSelectedTextRectsLocked();
    }

    if (!spans.empty() && !m_markupElements.empty()) {
        bool anyMatched = false;
        for (const auto& el : m_markupElements) {
            if (!el.textSpans.empty() && TextAnnotationHelper::spansGeometryEqual(el.textSpans, spans) &&
                (el.color.r != targetColor.r || el.color.g != targetColor.g || el.color.b != targetColor.b)) {
                anyMatched = true;
                break;
            }
        }
        if (anyMatched) {
            pushPinMarkupUndoState();
            for (auto& el : m_markupElements) {
                if (!el.textSpans.empty() && TextAnnotationHelper::spansGeometryEqual(el.textSpans, spans)) {
                    el.color.r = targetColor.r;
                    el.color.g = targetColor.g;
                    el.color.b = targetColor.b;
                }
            }
            m_ocrMarkupPreviewClean = true;
            recompositeMarkupPreservingOcr();
            return;
        }
    }
    render();
}

bool PinWindow::undoPinMarkup() {
    if (m_editingTextReplaceIdx >= 0) {
        commitInPlaceTextReplace(false);
        return true;
    }
    if (m_pinUndoStack.empty()) return false;
    m_pinRedoStack.push_back(m_markupElements);
    if (m_pinRedoStack.size() > MarkupEngine::kMaxUndoSteps) {
        m_pinRedoStack.pop_front();
    }
    m_markupElements = std::move(m_pinUndoStack.back());
    m_pinUndoStack.pop_back();
    recompositeMarkupPreservingOcr();
    return true;
}

bool PinWindow::redoPinMarkup() {
    if (m_editingTextReplaceIdx >= 0) return false;
    if (m_pinRedoStack.empty()) return false;
    m_pinUndoStack.push_back(m_markupElements);
    if (m_pinUndoStack.size() > MarkupEngine::kMaxUndoSteps) {
        m_pinUndoStack.pop_front();
    }
    m_markupElements = std::move(m_pinRedoStack.back());
    m_pinRedoStack.pop_back();
    recompositeMarkupPreservingOcr();
    return true;
}
} // namespace tools3000::capture
