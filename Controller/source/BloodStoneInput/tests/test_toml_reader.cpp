#include <cstring>
#include <iostream>

#include "default_config.hpp"
#include "toml_reader.hpp"

namespace {

bool Check(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << "FAILED: " << (message != nullptr ? message : "unknown") << '\n';
    return false;
}

}  // namespace

int main() {
    using namespace joyshim::toml;
    bool ok = true;

    const char text[] =
        "# comment\n"
        "[Global]\n"
        "Enabled = true\n"
        "Count = 0x18D1\n"
        "Scale = 1.25\n"
        "[Controllers.\"我的手柄\"]\n"
        "Guids = [\n"
        "  \"030068bad11800000094000000016800\", # inline\n"
        "]\n"
        "Name = \"Google Stadia Controller\"\n";
    Document document;
    ParseError error;
    ok &= Check(ParseDocument(text, std::strlen(text), document, error), "parse valid document");
    ok &= Check(document.entries.size() == 5, "entry count");
    const Entry* enabled = FindEntry(document, {"Global"}, "Enabled");
    ok &= Check(enabled != nullptr, "find boolean");
    ok &= Check(enabled != nullptr && enabled->value.type == ValueType::Boolean, "boolean type");
    ok &= Check(enabled != nullptr && enabled->value.boolean, "boolean value");
    const Entry* count = FindEntry(document, {"Global"}, "Count");
    ok &= Check(count != nullptr, "find integer");
    ok &= Check(count != nullptr && count->value.type == ValueType::Integer, "integer type");
    ok &= Check(count != nullptr && count->value.integer == 0x18D1, "integer value");
    const TableDecl* controllerTable = FindTableDecl(document, {"Controllers", "我的手柄"});
    ok &= Check(controllerTable != nullptr, "find quoted unicode table");
    ok &= Check(controllerTable != nullptr && controllerTable->line == 6, "unicode table line");
    const Entry* guids = FindEntry(document, {"Controllers", "我的手柄"}, "Guids");
    ok &= Check(guids != nullptr, "find guid array");
    ok &= Check(guids != nullptr && guids->value.type == ValueType::StringArray, "guid array type");
    ok &= Check(guids != nullptr && guids->value.strings.size() == 1, "guid array size");

    Document defaults;
    ok &= Check(ParseDocument(
        joyshim::DefaultTomlTemplate,
        joyshim::DefaultTomlTemplateSize,
        defaults,
        error), "parse default template");
    // 默认配置只暴露最常用的三个开关，复杂设置仍以内部默认值运行。
    const Entry* rumble = FindEntry(defaults, {"Input"}, "Rumble");
    ok &= Check(rumble != nullptr && rumble->value.type == ValueType::Boolean &&
        rumble->value.boolean, "default rumble enabled");
    const Entry* hide = FindEntry(defaults, {"Compatibility"}, "HideDirectInput");
    ok &= Check(hide != nullptr && hide->value.type == ValueType::Boolean &&
        !hide->value.boolean, "default native DirectInput remains visible");
    const Entry* log = FindEntry(defaults, {"Debug"}, "Log");
    ok &= Check(log != nullptr && log->value.type == ValueType::Boolean &&
        !log->value.boolean, "default log disabled");

    const char specialized[] =
        "[Project]\n"
        "ProcessNames = [\"Bond.exe\", \"BondMP.exe\"]\n"
        "[NativeInput]\n"
        "BlockDirectInput = true\n"
        "[Controllers.Stadia]\n"
        "Enabled = true\n"
        "DeviceIds = [\"18D1:9400\"]\n"
        "[Controllers.Stadia.NativeInput]\n"
        "TraceDirectInput = true\n";
    Document profile;
    ok &= Check(ParseDocument(
        specialized, std::strlen(specialized), profile, error),
        "parse project and controller subset profile");
    const Entry* processNames = FindEntry(
        profile, {"Project"}, "ProcessNames");
    ok &= Check(processNames != nullptr, "find Project.ProcessNames");
    ok &= Check(processNames != nullptr &&
        processNames->value.type == ValueType::StringArray,
        "ProcessNames type");
    ok &= Check(processNames != nullptr && processNames->value.strings.size() == 2,
        "ProcessNames size");
    const Entry* blockDirectInput = FindEntry(
        profile, {"NativeInput"}, "BlockDirectInput");
    ok &= Check(blockDirectInput != nullptr, "find project DirectInput policy");
    ok &= Check(blockDirectInput != nullptr &&
        blockDirectInput->value.type == ValueType::Boolean,
        "project DirectInput type");
    ok &= Check(blockDirectInput != nullptr && blockDirectInput->value.boolean,
        "project DirectInput value");
    const Entry* traceDirectInput = FindEntry(
        profile, {"Controllers", "Stadia", "NativeInput"}, "TraceDirectInput");
    ok &= Check(traceDirectInput != nullptr, "find controller subset override");
    ok &= Check(traceDirectInput != nullptr && traceDirectInput->value.boolean,
        "controller subset override value");

    const char duplicateKey[] = "[A]\nX = 1\nX = 2\n";
    ok &= Check(!ParseDocument(duplicateKey, std::strlen(duplicateKey), document, error), "reject duplicate key");
    ok &= Check(error.line == 3, "duplicate key line");
    ok &= Check(error.column > 0, "duplicate key column");
    ok &= Check(!error.suggestion.empty(), "duplicate key suggestion");

    const char duplicateTable[] = "[A]\nX = 1\n[A]\nY = 2\n";
    ok &= Check(!ParseDocument(duplicateTable, std::strlen(duplicateTable), document, error), "reject duplicate table");
    ok &= Check(error.line == 3, "duplicate table line");

    const char wrongArray[] = "[A]\nX = [1, 2]\n";
    ok &= Check(!ParseDocument(wrongArray, std::strlen(wrongArray), document, error), "reject non-string array");
    ok &= Check(error.line == 2, "array error line");

    const char looseBoolean[] = "[A]\nEnabled = True\n";
    ok &= Check(!ParseDocument(looseBoolean, std::strlen(looseBoolean), document, error), "reject loose boolean");
    ok &= Check(error.line == 2, "boolean error line");

    const char leadingZero[] = "[A]\nValue = 0123\n";
    ok &= Check(!ParseDocument(leadingZero, std::strlen(leadingZero), document, error), "reject leading zero");
    ok &= Check(error.line == 2, "integer error line");

    if (!ok) return 1;
    std::cout << "toml reader tests passed\n";
    return 0;
}
