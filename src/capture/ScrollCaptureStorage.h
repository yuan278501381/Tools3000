#pragma once

// Exact, checked raw-pixel I/O used by scroll-capture disk staging. Stream
// operators may accept fewer bytes than requested on an I/O failure; callers
// must never publish a segment record unless the complete payload was written.

#include <opencv2/core.hpp>

#include <istream>
#include <limits>
#include <ostream>

namespace tools3000::capture::scroll_storage {

inline bool writeMat(std::ostream& output, const cv::Mat& segment) {
    if (!output || segment.empty() || !segment.isContinuous()) return false;
    const std::size_t bytes = segment.total() * segment.elemSize();
    if (bytes == 0 || bytes > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return false;
    }
    output.write(reinterpret_cast<const char*>(segment.data), static_cast<std::streamsize>(bytes));
    return static_cast<bool>(output);
}

inline bool readExact(std::istream& input, void* destination, std::size_t bytes) {
    if (!input || !destination || bytes == 0 ||
        bytes > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return false;
    }
    input.read(static_cast<char*>(destination), static_cast<std::streamsize>(bytes));
    return input.good() && input.gcount() == static_cast<std::streamsize>(bytes);
}

}  // namespace tools3000::capture::scroll_storage
