#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// DeepOcrEngineInternal.h — PP-OCRv4 深度推理引擎私有跨编译单元协作声明
//
// 职责:
//   仅供 DeepOcrEngine*.cpp 内部拆分编译单元共享：
//   - 模型文件读取、字典加载与完整性校验声明（唯一定义位于 DeepOcrEngineModel.cpp）
//   - 四点排序、局部背景估算与轴对齐判定辅助声明（唯一定义位于 DeepOcrEnginePostprocess.cpp）
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_OCR_DEEPOCRENGINEINTERNAL_H
#define TOOLS3000_OCR_DEEPOCRENGINEINTERNAL_H

#include "ocr/DeepOcrEngine.h"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace tools3000::ocr::deep_ocr_detail {

// 唯一定义位于 DeepOcrEngine.cpp
void cleanupDeepOcrWorkingSet();

// 唯一定义位于 DeepOcrEngineModel.cpp
bool readFileBytes(const std::filesystem::path& filePath,
                   std::vector<uint8_t>& outBytes,
                   size_t maxBytes = 192ULL * 1024ULL * 1024ULL);
bool isValidOnnxFileHeader(const std::filesystem::path& filePath,
                           size_t minBytes = 256,
                           size_t maxBytes = 192ULL * 1024ULL * 1024ULL);
bool loadDictionaryLines(const std::filesystem::path& dictPath, std::vector<std::string>& outKeys);
bool isDirectoryCompleteModelBundle(const std::filesystem::path& dir);
bool isServerRecModelPresent(const std::filesystem::path& dir);
std::filesystem::path resolveScriptPackModelPath(const std::filesystem::path& dir, std::string_view packId);
bool isScriptPackModelPresent(const std::filesystem::path& dir, std::string_view packId, uint64_t* outFileBytes = nullptr);
std::string normalizeScriptMode(std::string_view rawMode);
const std::array<const char*, 4>& allScriptPackIds() noexcept;

// 唯一定义位于 DeepOcrEnginePostprocess.cpp
std::array<cv::Point2f, 4> orderQuadPointsClockwise(const cv::Point2f rawPts[4]) noexcept;
cv::Vec3b estimateBorderBackgroundColor(const cv::Mat& patch, bool* outUniform = nullptr);
bool isAxisAlignedHorizontalQuad(const DbDetectedQuad& q, cv::Rect* outBox = nullptr) noexcept;

} // namespace tools3000::ocr::deep_ocr_detail

#endif // TOOLS3000_OCR_DEEPOCRENGINEINTERNAL_H
