#include "toml_reader.hpp"

#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace joyshim::toml {
namespace {

std::string Trim(const std::string& text) {
    std::size_t begin = 0;
    while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    std::size_t end = text.size();
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

std::size_t FirstNonSpace(const std::string& text) noexcept {
    std::size_t index = 0;
    while (index < text.size() && std::isspace(static_cast<unsigned char>(text[index]))) ++index;
    return index;
}

void SetError(
    ParseError& error,
    std::size_t line,
    std::size_t column,
    const std::string& source,
    const std::string& message,
    const std::string& suggestion = {}) {
    error.line = line;
    error.column = column == 0 ? 1 : column;
    error.sourceLine = source;
    error.message = message;
    error.suggestion = suggestion;
}

bool ValidateUtf8(const std::string& text, std::size_t& badOffset) noexcept {
    badOffset = 0;
    for (std::size_t index = 0; index < text.size();) {
        const unsigned char first = static_cast<unsigned char>(text[index]);
        if (first <= 0x7Fu) {
            if (first == 0) {
                badOffset = index;
                return false;
            }
            ++index;
            continue;
        }
        std::size_t count = 0;
        std::uint32_t codePoint = 0;
        if ((first & 0xE0u) == 0xC0u) {
            count = 2;
            codePoint = first & 0x1Fu;
            if (codePoint == 0) {
                badOffset = index;
                return false;
            }
        } else if ((first & 0xF0u) == 0xE0u) {
            count = 3;
            codePoint = first & 0x0Fu;
        } else if ((first & 0xF8u) == 0xF0u) {
            count = 4;
            codePoint = first & 0x07u;
        } else {
            badOffset = index;
            return false;
        }
        if (index + count > text.size()) {
            badOffset = index;
            return false;
        }
        for (std::size_t offset = 1; offset < count; ++offset) {
            const unsigned char continuation = static_cast<unsigned char>(text[index + offset]);
            if ((continuation & 0xC0u) != 0x80u) {
                badOffset = index + offset;
                return false;
            }
            codePoint = (codePoint << 6u) | (continuation & 0x3Fu);
        }
        if ((count == 2 && codePoint < 0x80u) ||
            (count == 3 && codePoint < 0x800u) ||
            (count == 4 && codePoint < 0x10000u) ||
            codePoint > 0x10FFFFu ||
            (codePoint >= 0xD800u && codePoint <= 0xDFFFu)) {
            badOffset = index;
            return false;
        }
        index += count;
    }
    return true;
}

bool DecodeHexDigit(char value, unsigned int& output) noexcept {
    if (value >= '0' && value <= '9') {
        output = static_cast<unsigned int>(value - '0');
        return true;
    }
    if (value >= 'a' && value <= 'f') {
        output = 10u + static_cast<unsigned int>(value - 'a');
        return true;
    }
    if (value >= 'A' && value <= 'F') {
        output = 10u + static_cast<unsigned int>(value - 'A');
        return true;
    }
    return false;
}

void AppendUtf8(std::string& output, std::uint32_t codePoint) {
    if (codePoint <= 0x7Fu) {
        output.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FFu) {
        output.push_back(static_cast<char>(0xC0u | (codePoint >> 6u)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else if (codePoint <= 0xFFFFu) {
        output.push_back(static_cast<char>(0xE0u | (codePoint >> 12u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    } else {
        output.push_back(static_cast<char>(0xF0u | (codePoint >> 18u)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 12u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | ((codePoint >> 6u) & 0x3Fu)));
        output.push_back(static_cast<char>(0x80u | (codePoint & 0x3Fu)));
    }
}

bool ParseQuotedString(
    const std::string& text,
    std::size_t& position,
    std::string& output,
    std::string& error,
    std::size_t& errorPosition) {
    if (position >= text.size() || (text[position] != '"' && text[position] != '\'')) {
        error = "需要使用引号包围字符串";
        errorPosition = position;
        return false;
    }
    const char quote = text[position++];
    output.clear();
    while (position < text.size()) {
        const char value = text[position++];
        if (value == quote) return true;
        if (value == '\n' || value == '\r') {
            error = "单行字符串不能包含换行；本配置不支持多行字符串";
            errorPosition = position - 1;
            return false;
        }
        if (quote == '\'' || value != '\\') {
            output.push_back(value);
            continue;
        }
        if (position >= text.size()) {
            error = "字符串转义不完整";
            errorPosition = position;
            return false;
        }
        const char escaped = text[position++];
        switch (escaped) {
            case 'b': output.push_back('\b'); break;
            case 't': output.push_back('\t'); break;
            case 'n': output.push_back('\n'); break;
            case 'f': output.push_back('\f'); break;
            case 'r': output.push_back('\r'); break;
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case 'u':
            case 'U': {
                const std::size_t digits = escaped == 'u' ? 4u : 8u;
                if (position + digits > text.size()) {
                    error = "Unicode 转义长度不足";
                    errorPosition = position;
                    return false;
                }
                std::uint32_t codePoint = 0;
                for (std::size_t index = 0; index < digits; ++index) {
                    unsigned int digit = 0;
                    if (!DecodeHexDigit(text[position + index], digit)) {
                        error = "Unicode 转义包含非十六进制字符";
                        errorPosition = position + index;
                        return false;
                    }
                    codePoint = (codePoint << 4u) | digit;
                }
                position += digits;
                if (codePoint > 0x10FFFFu || (codePoint >= 0xD800u && codePoint <= 0xDFFFu)) {
                    error = "Unicode 转义超出有效范围";
                    errorPosition = position - digits;
                    return false;
                }
                AppendUtf8(output, codePoint);
                break;
            }
            default:
                error = "不支持的字符串转义";
                errorPosition = position - 1;
                return false;
        }
    }
    error = "字符串缺少结束引号";
    errorPosition = text.size();
    return false;
}

std::string StripComment(const std::string& line) {
    bool inBasic = false;
    bool inLiteral = false;
    bool escaped = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char value = line[index];
        if (inBasic) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') inBasic = false;
            continue;
        }
        if (inLiteral) {
            if (value == '\'') inLiteral = false;
            continue;
        }
        if (value == '"') inBasic = true;
        else if (value == '\'') inLiteral = true;
        else if (value == '#') return line.substr(0, index);
    }
    return line;
}

bool CompositeValueBalanced(const std::string& text) noexcept {
    bool inBasic = false;
    bool inLiteral = false;
    bool escaped = false;
    int squareDepth = 0;
    for (char value : text) {
        if (inBasic) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') inBasic = false;
            continue;
        }
        if (inLiteral) {
            if (value == '\'') inLiteral = false;
            continue;
        }
        if (value == '"') inBasic = true;
        else if (value == '\'') inLiteral = true;
        else if (value == '[') ++squareDepth;
        else if (value == ']') --squareDepth;
        if (squareDepth < 0) return false;
    }
    return squareDepth == 0 && !inBasic && !inLiteral;
}

bool ParseNameSegment(
    const std::string& text,
    std::size_t& position,
    std::string& output,
    std::string& error,
    std::size_t& errorPosition) {
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size()) {
        error = "表路径包含空段";
        errorPosition = position;
        return false;
    }
    if (text[position] == '"' || text[position] == '\'') {
        return ParseQuotedString(text, position, output, error, errorPosition);
    }
    const std::size_t begin = position;
    while (position < text.size() && text[position] != '.' &&
           !std::isspace(static_cast<unsigned char>(text[position]))) {
        const unsigned char value = static_cast<unsigned char>(text[position]);
        if (!(std::isalnum(value) || value == '_' || value == '-')) {
            error = "裸表名只能包含字母、数字、下划线和连字符；中文或空格名称必须加引号";
            errorPosition = position;
            return false;
        }
        ++position;
    }
    if (position == begin) {
        error = "表路径包含空段";
        errorPosition = position;
        return false;
    }
    output = text.substr(begin, position - begin);
    return true;
}

bool ParseTablePath(
    const std::string& text,
    std::vector<std::string>& output,
    std::string& error,
    std::size_t& errorPosition) {
    output.clear();
    std::size_t position = 0;
    for (;;) {
        std::string segment;
        if (!ParseNameSegment(text, position, segment, error, errorPosition)) return false;
        output.push_back(segment);
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
        if (position == text.size()) break;
        if (text[position] != '.') {
            error = "表路径段之间必须使用点号";
            errorPosition = position;
            return false;
        }
        ++position;
    }
    return !output.empty();
}

bool ParseKey(
    const std::string& text,
    std::string& key,
    std::string& error,
    std::size_t& errorPosition) {
    std::size_t position = 0;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size()) {
        error = "键名为空";
        errorPosition = position;
        return false;
    }
    if (text[position] == '"' || text[position] == '\'') {
        if (!ParseQuotedString(text, position, key, error, errorPosition)) return false;
    } else {
        const std::size_t begin = position;
        while (position < text.size() && !std::isspace(static_cast<unsigned char>(text[position]))) {
            const unsigned char value = static_cast<unsigned char>(text[position]);
            if (!(std::isalnum(value) || value == '_' || value == '-')) {
                error = "裸键名只能包含字母、数字、下划线和连字符；本配置不接受点号键";
                errorPosition = position;
                return false;
            }
            ++position;
        }
        key = text.substr(begin, position - begin);
    }
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position != text.size()) {
        error = "键名后存在多余字符";
        errorPosition = position;
        return false;
    }
    return !key.empty();
}

bool ValidDigitForBase(char value, int base) noexcept {
    if (value >= '0' && value <= '9') return (value - '0') < base;
    if (base == 16 && value >= 'a' && value <= 'f') return true;
    if (base == 16 && value >= 'A' && value <= 'F') return true;
    return false;
}

bool RemoveStrictUnderscores(
    const std::string& text,
    int base,
    std::string& output,
    std::string& error,
    std::size_t& errorPosition) {
    output.clear();
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (value != '_') {
            output.push_back(value);
            continue;
        }
        if (index == 0 || index + 1 >= text.size() ||
            !ValidDigitForBase(text[index - 1], base) ||
            !ValidDigitForBase(text[index + 1], base)) {
            error = "数字分隔符下划线只能位于两个数字之间";
            errorPosition = index;
            return false;
        }
    }
    return true;
}

bool ParseInteger(
    const std::string& text,
    Value& output,
    std::string& error,
    std::size_t& errorPosition) {
    std::size_t position = 0;
    bool negative = false;
    if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        negative = text[position] == '-';
        ++position;
    }
    int base = 10;
    std::size_t prefixLength = 0;
    if (position + 2 <= text.size() && text[position] == '0') {
        const char marker = position + 1 < text.size() ? text[position + 1] : '\0';
        if (marker == 'x' || marker == 'X') { base = 16; prefixLength = 2; }
        else if (marker == 'o' || marker == 'O') { base = 8; prefixLength = 2; }
        else if (marker == 'b' || marker == 'B') { base = 2; prefixLength = 2; }
    }
    if (base != 10 && position != 0) {
        error = "十六进制、八进制和二进制整数不能带正负号";
        errorPosition = 0;
        return false;
    }
    const std::size_t digitsBegin = position + prefixLength;
    if (digitsBegin >= text.size()) {
        error = "整数缺少数字";
        errorPosition = digitsBegin;
        return false;
    }
    std::string digits;
    if (!RemoveStrictUnderscores(text.substr(digitsBegin), base, digits, error, errorPosition)) {
        errorPosition += digitsBegin;
        return false;
    }
    for (std::size_t index = 0; index < digits.size(); ++index) {
        if (!ValidDigitForBase(digits[index], base)) {
            error = "整数包含不属于当前进制的字符";
            errorPosition = digitsBegin + index;
            return false;
        }
    }
    if (base == 10 && digits.size() > 1 && digits[0] == '0') {
        error = "十进制整数不能包含前导零";
        errorPosition = digitsBegin;
        return false;
    }
    std::string normalized;
    if (negative) normalized.push_back('-');
    normalized += digits;
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(normalized.c_str(), &end, base);
    if (end == normalized.c_str() || *end != '\0' || errno == ERANGE) {
        error = "整数超出 64 位有符号范围";
        errorPosition = 0;
        return false;
    }
    output = {};
    output.type = ValueType::Integer;
    output.integer = static_cast<std::int64_t>(value);
    return true;
}

bool ParseFloat(
    const std::string& text,
    Value& output,
    std::string& error,
    std::size_t& errorPosition) {
    if (text == "inf" || text == "+inf" || text == "-inf" ||
        text == "nan" || text == "+nan" || text == "-nan") {
        output = {};
        output.type = ValueType::Floating;
        if (text.find("nan") != std::string::npos) {
            output.floating = std::numeric_limits<double>::quiet_NaN();
        } else {
            output.floating = text[0] == '-' ? -std::numeric_limits<double>::infinity()
                                             : std::numeric_limits<double>::infinity();
        }
        return true;
    }
    std::string normalized;
    if (!RemoveStrictUnderscores(text, 10, normalized, error, errorPosition)) return false;
    const std::size_t dot = normalized.find('.');
    const std::size_t exponent = normalized.find_first_of("eE");
    if (dot == std::string::npos && exponent == std::string::npos) {
        error = "不是浮点数";
        errorPosition = 0;
        return false;
    }
    if (dot != std::string::npos) {
        if (dot == 0 || dot + 1 >= normalized.size() ||
            !std::isdigit(static_cast<unsigned char>(normalized[dot - 1])) ||
            !std::isdigit(static_cast<unsigned char>(normalized[dot + 1]))) {
            error = "小数点两侧都必须有数字";
            errorPosition = dot;
            return false;
        }
    }
    errno = 0;
    char* end = nullptr;
    const double value = std::strtod(normalized.c_str(), &end);
    if (end == normalized.c_str() || *end != '\0' || errno == ERANGE) {
        error = "无效浮点数";
        errorPosition = static_cast<std::size_t>(end != nullptr ? end - normalized.c_str() : 0);
        return false;
    }
    output = {};
    output.type = ValueType::Floating;
    output.floating = value;
    return true;
}

bool ParseStringArray(
    const std::string& text,
    Value& output,
    std::string& error,
    std::size_t& errorPosition) {
    std::size_t position = 0;
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position >= text.size() || text[position++] != '[') {
        error = "字符串数组缺少左方括号";
        errorPosition = position;
        return false;
    }
    output = {};
    output.type = ValueType::StringArray;
    bool needValue = true;
    for (;;) {
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
        if (position >= text.size()) {
            error = "字符串数组缺少右方括号";
            errorPosition = position;
            return false;
        }
        if (text[position] == ']') {
            ++position;
            break;
        }
        if (!needValue) {
            error = "字符串数组元素之间必须使用逗号";
            errorPosition = position;
            return false;
        }
        if (text[position] != '"' && text[position] != '\'') {
            error = "该数组只允许字符串元素";
            errorPosition = position;
            return false;
        }
        std::string item;
        if (!ParseQuotedString(text, position, item, error, errorPosition)) return false;
        output.strings.push_back(item);
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
        if (position < text.size() && text[position] == ',') {
            ++position;
            needValue = true;
            continue;
        }
        needValue = false;
    }
    while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
    if (position != text.size()) {
        error = "字符串数组后存在多余字符";
        errorPosition = position;
        return false;
    }
    return true;
}

bool ParseValue(
    const std::string& raw,
    Value& output,
    std::string& error,
    std::size_t& errorPosition) {
    const std::string text = Trim(raw);
    if (text.empty()) {
        error = "等号后缺少值";
        errorPosition = 0;
        return false;
    }
    if (text.rfind("[[", 0) == 0) {
        error = "不支持数组表；请使用普通表 [Table]";
        errorPosition = 0;
        return false;
    }
    if (text[0] == '[') return ParseStringArray(text, output, error, errorPosition);
    if (text[0] == '{') {
        error = "本配置不支持内联表";
        errorPosition = 0;
        return false;
    }
    if (text.rfind("\"\"\"", 0) == 0 || text.rfind("'''", 0) == 0) {
        error = "本配置不支持多行字符串";
        errorPosition = 0;
        return false;
    }
    if (text[0] == '"' || text[0] == '\'') {
        std::size_t position = 0;
        std::string value;
        if (!ParseQuotedString(text, position, value, error, errorPosition)) return false;
        while (position < text.size() && std::isspace(static_cast<unsigned char>(text[position]))) ++position;
        if (position != text.size()) {
            error = "字符串后存在多余字符";
            errorPosition = position;
            return false;
        }
        output = {};
        output.type = ValueType::String;
        output.string = value;
        return true;
    }
    if (text == "true" || text == "false") {
        output = {};
        output.type = ValueType::Boolean;
        output.boolean = text == "true";
        return true;
    }
    if (text == "True" || text == "False" || text == "TRUE" || text == "FALSE") {
        error = "TOML 布尔值必须写成小写 true 或 false";
        errorPosition = 0;
        return false;
    }
    const bool looksFloat = text.find('.') != std::string::npos ||
        text.find('e') != std::string::npos || text.find('E') != std::string::npos ||
        text.find("inf") != std::string::npos || text.find("nan") != std::string::npos;
    if (looksFloat) return ParseFloat(text, output, error, errorPosition);
    if (std::isdigit(static_cast<unsigned char>(text[0])) || text[0] == '+' || text[0] == '-') {
        return ParseInteger(text, output, error, errorPosition);
    }
    if (text.find('-') != std::string::npos || text.find(':') != std::string::npos) {
        error = "本配置不支持 TOML 日期时间值；需要字符串时请加引号";
        errorPosition = 0;
        return false;
    }
    error = "无法识别的 TOML 值；字符串必须加引号";
    errorPosition = 0;
    return false;
}

std::size_t FindEquals(const std::string& text) noexcept {
    bool inBasic = false;
    bool inLiteral = false;
    bool escaped = false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char value = text[index];
        if (inBasic) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') inBasic = false;
            continue;
        }
        if (inLiteral) {
            if (value == '\'') inLiteral = false;
            continue;
        }
        if (value == '"') inBasic = true;
        else if (value == '\'') inLiteral = true;
        else if (value == '=') return index;
    }
    return std::string::npos;
}

bool ContainsEntry(
    const Document& document,
    const std::vector<std::string>& table,
    const std::string& key) noexcept {
    for (const Entry& entry : document.entries) {
        if (entry.key == key && SamePath(entry.table, table)) return true;
    }
    return false;
}

const TableDecl* FindTableInternal(
    const Document& document,
    const std::vector<std::string>& table) noexcept {
    for (const TableDecl& existing : document.tables) {
        if (SamePath(existing.path, table)) return &existing;
    }
    return nullptr;
}

void OffsetToLineColumn(
    const std::string& content,
    std::size_t offset,
    std::size_t& line,
    std::size_t& column,
    std::string& sourceLine) {
    line = 1;
    std::size_t lineStart = 0;
    for (std::size_t index = 0; index < offset && index < content.size(); ++index) {
        if (content[index] == '\n') {
            ++line;
            lineStart = index + 1;
        }
    }
    const std::size_t lineEnd = content.find('\n', lineStart);
    sourceLine = content.substr(lineStart, lineEnd == std::string::npos ? std::string::npos : lineEnd - lineStart);
    if (!sourceLine.empty() && sourceLine.back() == '\r') sourceLine.pop_back();
    column = offset >= lineStart ? offset - lineStart + 1 : 1;
}

}  // namespace

bool SamePath(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right) noexcept {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index] != right[index]) return false;
    }
    return true;
}

const char* ValueTypeName(ValueType type) noexcept {
    switch (type) {
        case ValueType::Boolean: return "布尔值";
        case ValueType::Integer: return "整数";
        case ValueType::Floating: return "浮点数";
        case ValueType::String: return "字符串";
        case ValueType::StringArray: return "字符串数组";
        default: return "未知类型";
    }
}

std::string JoinPath(const std::vector<std::string>& table, const std::string& key) {
    std::string result;
    for (const std::string& segment : table) {
        if (!result.empty()) result.push_back('.');
        result += segment;
    }
    if (!key.empty()) {
        if (!result.empty()) result.push_back('.');
        result += key;
    }
    return result;
}

bool ParseDocument(
    const char* data,
    std::size_t size,
    Document& output,
    ParseError& error) noexcept {
    output = {};
    error = {};
    if (data == nullptr && size != 0) {
        SetError(error, 1, 1, {}, "输入缓冲区为空");
        return false;
    }

    std::string content(data != nullptr ? data : "", size);
    if (content.size() >= 3 &&
        static_cast<unsigned char>(content[0]) == 0xEFu &&
        static_cast<unsigned char>(content[1]) == 0xBBu &&
        static_cast<unsigned char>(content[2]) == 0xBFu) {
        content.erase(0, 3);
    }
    std::size_t badUtf8 = 0;
    if (!ValidateUtf8(content, badUtf8)) {
        std::size_t line = 1;
        std::size_t column = 1;
        std::string source;
        OffsetToLineColumn(content, badUtf8, line, column, source);
        SetError(error, line, column, source, "文件不是有效 UTF-8 或包含 NUL 字符",
                 "请将 joyshim.toml 保存为 UTF-8 编码。");
        return false;
    }

    std::vector<std::string> currentTable;
    std::size_t offset = 0;
    std::size_t lineNumber = 1;
    while (offset <= content.size()) {
        const std::size_t end = content.find('\n', offset);
        const std::size_t lineEnd = end == std::string::npos ? content.size() : end;
        std::string sourceLine = content.substr(offset, lineEnd - offset);
        if (!sourceLine.empty() && sourceLine.back() == '\r') sourceLine.pop_back();
        const std::size_t firstLine = lineNumber;
        offset = end == std::string::npos ? content.size() + 1 : end + 1;
        ++lineNumber;

        const std::string uncommented = StripComment(sourceLine);
        const std::size_t first = FirstNonSpace(uncommented);
        std::string line = Trim(uncommented);
        if (line.empty()) continue;

        if (line.front() == '[') {
            if (line.rfind("[[", 0) == 0) {
                SetError(error, firstLine, first + 1, sourceLine,
                         "不支持数组表 [[Table]]",
                         "请改用普通表 [Table]，并使用自定义别名区分规则。");
                return false;
            }
            if (line.size() < 2 || line.back() != ']') {
                SetError(error, firstLine, first + line.size(), sourceLine,
                         "表声明缺少结束方括号 ]");
                return false;
            }
            std::string pathError;
            std::size_t pathErrorPosition = 0;
            if (!ParseTablePath(Trim(line.substr(1, line.size() - 2)), currentTable,
                                pathError, pathErrorPosition)) {
                SetError(error, firstLine, first + 2 + pathErrorPosition, sourceLine,
                         pathError, "中文或含空格的别名请使用引号，例如 [Controllers.\"我的手柄\"]。");
                return false;
            }
            if (const TableDecl* existing = FindTableInternal(output, currentTable)) {
                SetError(error, firstLine, first + 1, sourceLine,
                         "同一个 TOML 表被重复声明：" + JoinPath(currentTable),
                         "请合并两个表；首次声明位于第 " + std::to_string(existing->line) + " 行。");
                return false;
            }
            TableDecl table;
            table.path = currentTable;
            table.line = firstLine;
            table.column = first + 1;
            table.sourceLine = sourceLine;
            output.tables.push_back(std::move(table));
            continue;
        }

        const std::size_t equals = FindEquals(line);
        if (equals == std::string::npos) {
            SetError(error, firstLine, first + 1, sourceLine,
                     "配置项缺少等号 =",
                     "正确格式示例：Enabled = true");
            return false;
        }
        std::string key;
        std::string keyError;
        std::size_t keyErrorPosition = 0;
        if (!ParseKey(Trim(line.substr(0, equals)), key, keyError, keyErrorPosition)) {
            SetError(error, firstLine, first + 1 + keyErrorPosition, sourceLine,
                     keyError, "请使用单一键名；层级应写在 [表路径] 中。");
            return false;
        }

        std::string valueText = Trim(line.substr(equals + 1));
        if (!valueText.empty() && valueText.front() == '[' && !CompositeValueBalanced(valueText)) {
            while (offset <= content.size() && !CompositeValueBalanced(valueText)) {
                const std::size_t nextEnd = content.find('\n', offset);
                const std::size_t nextLineEnd = nextEnd == std::string::npos ? content.size() : nextEnd;
                std::string continuation = content.substr(offset, nextLineEnd - offset);
                if (!continuation.empty() && continuation.back() == '\r') continuation.pop_back();
                offset = nextEnd == std::string::npos ? content.size() + 1 : nextEnd + 1;
                ++lineNumber;
                valueText += " " + Trim(StripComment(continuation));
            }
        }
        if (!CompositeValueBalanced(valueText)) {
            SetError(error, firstLine, first + equals + 2, sourceLine,
                     "数组或字符串没有正确结束",
                     "检查右方括号、字符串引号和反斜杠转义。");
            return false;
        }
        if (ContainsEntry(output, currentTable, key)) {
            SetError(error, firstLine, first + 1, sourceLine,
                     "同一表中出现重复键：" + key,
                     "每个表中的同名字段只能出现一次。");
            return false;
        }

        Entry entry;
        entry.table = currentTable;
        entry.key = key;
        entry.line = firstLine;
        entry.column = first + 1;
        entry.sourceLine = sourceLine;
        std::string valueError;
        std::size_t valueErrorPosition = 0;
        if (!ParseValue(valueText, entry.value, valueError, valueErrorPosition)) {
            SetError(error, firstLine, first + equals + 2 + valueErrorPosition, sourceLine,
                     valueError,
                     "请按字段要求使用严格 TOML 类型；字符串加引号，布尔值使用小写 true/false。");
            return false;
        }
        output.entries.push_back(std::move(entry));
    }
    return true;
}

const Entry* FindEntry(
    const Document& document,
    const std::vector<std::string>& table,
    const char* key) noexcept {
    if (key == nullptr) return nullptr;
    for (const Entry& entry : document.entries) {
        if (entry.key == key && SamePath(entry.table, table)) return &entry;
    }
    return nullptr;
}

const TableDecl* FindTableDecl(
    const Document& document,
    const std::vector<std::string>& table) noexcept {
    return FindTableInternal(document, table);
}

}  // namespace joyshim::toml
