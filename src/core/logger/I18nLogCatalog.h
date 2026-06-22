#pragma once
// ─────────────────────────────────────────────────────────────────────────────
// I18nLogCatalog.h — 世界级 0 内存分配日志多语言模板翻译映射中枢
// ─────────────────────────────────────────────────────────────────────────────

#ifndef TOOLS3000_CORE_LOGGER_I18N_LOG_CATALOG_H
#define TOOLS3000_CORE_LOGGER_I18N_LOG_CATALOG_H

#include "core/utils/Export.h"
#include <string_view>
#include <cstdint>

namespace tools3000::core {

enum class LogLanguage : uint8_t;

/// 获取带有 [{}] TraceID 统一前缀的格式化字符串 (极速无锁 O(1) 查表，0 堆内存分配)
TOOLS3000CORE_API const char* getLocalizedLogFormatWithTrace(std::string_view rawFmt, LogLanguage lang);

}  // namespace tools3000::core

#endif  // TOOLS3000_CORE_LOGGER_I18N_LOG_CATALOG_H
