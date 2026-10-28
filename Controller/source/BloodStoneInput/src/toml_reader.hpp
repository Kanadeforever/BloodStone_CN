#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace joyshim::toml {

// 本项目接受 TOML 1.0 中与人工配置直接相关的严格子集：普通表、布尔值、
// 整数、浮点数、单行字符串和字符串数组。日期时间、内联表、数组表、
// 多行字符串和异构数组会被明确拒绝，而不是宽松猜测。
enum class ValueType {
    Boolean,
    Integer,
    Floating,
    String,
    StringArray,
};

struct Value {
    ValueType type{ValueType::String};
    bool boolean{};
    std::int64_t integer{};
    double floating{};
    std::string string{};
    std::vector<std::string> strings{};
};

struct Entry {
    std::vector<std::string> table{};
    std::string key{};
    Value value{};
    std::size_t line{};
    std::size_t column{};
    std::string sourceLine{};
};

struct TableDecl {
    std::vector<std::string> path{};
    std::size_t line{};
    std::size_t column{};
    std::string sourceLine{};
};

struct ParseError {
    std::size_t line{};
    std::size_t column{};
    std::string message{};
    std::string suggestion{};
    std::string sourceLine{};
};

struct Document {
    std::vector<Entry> entries{};
    std::vector<TableDecl> tables{};
};

bool ParseDocument(
    const char* data,
    std::size_t size,
    Document& output,
    ParseError& error) noexcept;

const Entry* FindEntry(
    const Document& document,
    const std::vector<std::string>& table,
    const char* key) noexcept;

const TableDecl* FindTableDecl(
    const Document& document,
    const std::vector<std::string>& table) noexcept;

bool SamePath(
    const std::vector<std::string>& left,
    const std::vector<std::string>& right) noexcept;

const char* ValueTypeName(ValueType type) noexcept;
std::string JoinPath(const std::vector<std::string>& table, const std::string& key = {});

}  // namespace joyshim::toml
