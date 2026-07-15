#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tools3000::service::db::detail {

inline std::string escapeCsvField(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (const char ch : value) {
        if (ch == '"') escaped.push_back('"');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

inline bool parseCsvDocument(std::string_view content,
                             std::vector<std::vector<std::string>>& rows) {
    rows.clear();
    std::vector<std::string> row;
    std::string field;
    bool inQuotes = false;
    bool quotedFieldClosed = false;
    bool hasInput = false;

    const auto finishField = [&]() {
        row.push_back(std::move(field));
        field.clear();
        quotedFieldClosed = false;
    };
    const auto finishRow = [&]() {
        finishField();
        rows.push_back(std::move(row));
        row.clear();
        hasInput = false;
    };

    for (std::size_t index = 0; index < content.size(); ++index) {
        const char ch = content[index];
        if (inQuotes) {
            if (ch == '"') {
                if (index + 1 < content.size() && content[index + 1] == '"') {
                    field.push_back('"');
                    ++index;
                } else {
                    inQuotes = false;
                    quotedFieldClosed = true;
                }
            } else {
                field.push_back(ch);
            }
            hasInput = true;
            continue;
        }

        if (quotedFieldClosed && ch != ',' && ch != '\r' && ch != '\n') {
            return false;
        }
        if (ch == '"') {
            if (!field.empty()) return false;
            inQuotes = true;
            hasInput = true;
        } else if (ch == ',') {
            finishField();
            hasInput = true;
        } else if (ch == '\r' || ch == '\n') {
            if (ch == '\r' && index + 1 < content.size() && content[index + 1] == '\n') {
                ++index;
            }
            finishRow();
        } else {
            field.push_back(ch);
            hasInput = true;
        }
    }

    if (inQuotes) return false;
    if (hasInput || !field.empty() || !row.empty() || quotedFieldClosed) finishRow();
    return true;
}

}  // namespace tools3000::service::db::detail
