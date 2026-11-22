#pragma once
#ifndef TOOLS3000_CAPTURE_PINOCRTYPES_H
#define TOOLS3000_CAPTURE_PINOCRTYPES_H

#include <d2d1.h>
#include <opencv2/core.hpp>
#include <cstddef>
#include <string>
#include <vector>

namespace tools3000::capture {

struct PinOcrBlock {
    std::string text;
    cv::Rect origBox;
    std::vector<cv::Rect> charBoxes;
    int layoutRegionId = 0;
    int alignedRowId = -1;
};

struct PinQrCodeItem {
    std::string text;
    cv::Rect origBox;
    D2D1_RECT_F badgeRect{};
};

struct PinBlockCharSpan {
    size_t blockIndex = 0;
    size_t startChar = 0;
    size_t endExclusive = 0;
};

} // namespace tools3000::capture

#endif // TOOLS3000_CAPTURE_PINOCRTYPES_H
