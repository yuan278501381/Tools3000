#pragma once

#include <cstddef>
#include <cstdint>

namespace tools3000::service::search_limits {

inline constexpr std::size_t MaxQueryUtf8Bytes = 1024;
inline constexpr std::size_t MaxResults = 10'000;
inline constexpr std::size_t DefaultContentMaxResults = 500;
inline constexpr std::size_t ProgressiveFirstSliceMaxHits = 25;
inline constexpr std::int64_t ProgressiveFirstSliceWithHitsMs = 160;
inline constexpr std::int64_t ProgressiveFirstSliceEmptyBudgetMs = 750;
inline constexpr std::int64_t ProgressiveNextSliceBudgetMs = 850;
inline constexpr std::size_t MaxDriveItems = 26;
inline constexpr std::size_t MaxExcludeItems = 64;
inline constexpr std::size_t MaxExcludeUtf8Bytes = 512;
inline constexpr std::size_t MaxFormatItems = 512;
inline constexpr std::size_t MaxFormatUtf8Bytes = 32;
inline constexpr std::size_t MaxHistoryResults = 500;
inline constexpr std::size_t MaxPathUtf8Bytes = 32'767;
inline constexpr std::size_t MaxRegexCharacters = 256;

}  // namespace tools3000::service::search_limits
