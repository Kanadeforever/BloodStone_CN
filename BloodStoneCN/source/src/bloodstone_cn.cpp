#include "win32_api.h"
#include "command_line.h"

// 本插件复用游戏的原生 LOC、DAT、BPR 加载，只重定向五个资源路径。
// 同时关闭原生 UIBondText 的随机字母扰动分支，保留文字渐显的其他三个分支。
// 不调用 GetText、SubmitText，不替换字形查询函数，也不更改用户译文。
// 原始 EXE 的代码段会在运行时解密，所以任何写入之前都需要验证五条原始指令。
// 如果文件缺失、EXE 不匹配或者指令尚未解密，则保持原始游戏字节不变。

static_assert(sizeof(void*) == 4, "此模块只支持 Win32/x86 游戏进程");

// 不链接 C 运行时；为编译器自动生成的数组清零操作提供最小实现。
extern "C" void* __cdecl memset(void* destination, int value, unsigned int count) {
    unsigned char* bytes = static_cast<unsigned char*>(destination);
    for (unsigned int index = 0; index < count; ++index) {
        bytes[index] = static_cast<unsigned char>(value);
    }
    return destination;
}

namespace {

// 下列 RVA 均从用户上传的 2026-10-04 明文运行时转储分析得出。
// 必须同时验证相应的 push imm32 指令及其原始字符串，不允许仅凭地址直接修改。
constexpr unsigned long kLocCommonPush = 0x00833948ul;       // 原生普通 LOC 目录枚举。
constexpr unsigned long kLocLevelsPush = 0x00833ACCul;       // 原生关卡 LOC 目录。
constexpr unsigned long kLocMultiplayerPush = 0x00833ABDul;  // 原生多人关卡 LOC 目录。
constexpr unsigned long kFontResourcePush = 0x00836870ul;    // EUR 字体 BPR 资源名称。
constexpr unsigned long kFontDatPush = 0x00836BA4ul;         // EUR 字体 DAT 加载名称。

constexpr unsigned long kLocCommonLiteral = 0x00D7B744ul;
constexpr unsigned long kLocLevelsLiteral = 0x00D7B758ul;
constexpr unsigned long kLocMultiplayerLiteral = 0x00D7B774ul;
constexpr unsigned long kFontResourceLiteral = 0x00D7C1B0ul;
constexpr unsigned long kFontDatLiteral = 0x00D7C0D0ul;
constexpr unsigned long kMaxEnginePath = 220;

// UIBondText 在原版中通过四项跳转表选择逐字视觉效果：透明度、颜色、随机字母、旋转。
// 第三项随机字母分支会把候选码点上限截断为 0x7A（拉丁字母 z），
// 中文字符经此分支处理后会出现连续 Z。把该分支指向原有的“跳过本效果”位置，
// 只取消随机字母扰动，不影响游戏按时间流式揭示文本或另外三个效果分支。
constexpr unsigned long kEffectDispatchRva = 0x0086993Eul;
constexpr unsigned long kEffectJumpTableRva = 0x00869C74ul;
constexpr unsigned long kEffectRandomRva = 0x00869A5Cul;
constexpr unsigned long kEffectSkipRva = 0x00869BB6ul;
constexpr unsigned long kEffectClampRva = 0x00869A9Eul;
constexpr unsigned long kEffectTargets[4] = {
    0x0086994Eul, 0x00869976ul, kEffectRandomRva, 0x00869B3Eul,
};

// 每次插件装载只会选择下列状态之一；日志不会把“修改路径”误说成“资源成功加载”。
enum class State {
    NotStarted,
    InvalidModule,
    UnsupportedDirectoryName,
    AssetsMissing,
    WaitingForDecryption,
    WrongBuild,
    ForeignPatchConflict,
    PatchFailed,
    PathsRedirected,
};

wchar_t g_exe_dir[520] = {};
wchar_t g_plugin_dir[520] = {};
char g_relative_dir[224] = {};
char g_path_mode[32] = {};
char g_legacy_modules[32] = {};
unsigned long g_patch_count = 0;
unsigned long g_failed_site = 0;
// 日志必须区分五处已验证的资源路径补丁与随机字母修复，不能混为一项成功。
const char* g_random_effect_status = "NOT_ATTEMPTED";
unsigned long g_random_effect_previous = 0;
unsigned long g_random_effect_current = 0;
unsigned long g_random_effect_error = 0;
char g_font_resource[256] = {};
char g_font_dat[256] = {};
char g_loc_common[256] = {};
char g_loc_levels[256] = {};
char g_loc_multiplayer[256] = {};
State g_state = State::NotStarted;
unsigned long g_wait_iterations = 0;
// 只有命令行中的独立 --log 参数可以开启磁盘/调试日志；普通启动不创建文件。
bool g_logging_requested = false;
Handle g_log_file = nullptr;
DWord g_log_started = 0;
unsigned long g_log_lines = 0;
unsigned long g_log_write_failures = 0;
unsigned long g_log_loc_files = 0;
unsigned long g_last_patch_error = 0;
const char* g_last_patch_step = "NOT_ATTEMPTED";


unsigned long TextLength(const char* value) {
    unsigned long length = 0;
    while (value[length] != '\0') ++length;
    return length;
}

bool Matches(const char* current, const char* expected) {
    unsigned long index = 0;
    while (current[index] == expected[index] && expected[index]) ++index;
    return current[index] == 0 && expected[index] == 0;
}

bool Append(char* output, unsigned long capacity, const char* addition) {
    unsigned long pos = TextLength(output);
    for (unsigned long src = 0; addition[src]; ++src) {
        if (pos + 1 >= capacity) return false;
        output[pos++] = addition[src];
    }
    output[pos] = 0;
    return true;
}

bool AppendWide(wchar_t* output, unsigned long capacity, const wchar_t* addition) {
    unsigned long pos = 0;
    while (pos < capacity && output[pos]) ++pos;
    for (unsigned long src = 0; addition[src]; ++src) {
        if (pos + 1 >= capacity) return false;
        output[pos++] = addition[src];
    }
    if (pos >= capacity) return false;
    output[pos] = 0;
    return true;
}

// 每个资源路径都从当前 ASI 的真实绝对目录派生，不假设 scripts、plugins 或游戏根目录。
// 相对路径只是原生引擎可识别的优先形式；如果 ASI 位于游戏目录之外，
// 则提交完整绝对路径。是否支持外部盘符路径由实际资源管理器决定并记录为待测试。
bool GetDirectory(Module module, wchar_t* output) {
    const unsigned long length = GetModuleFileNameW(module, output, 520);
    if (length < 3 || length >= 520) return false;
    unsigned long last = length;
    while (last && output[last - 1] != L'\\' && output[last - 1] != L'/') --last;
    if (!last) return false;
    output[last] = 0;
    return true;
}

wchar_t FoldAscii(wchar_t input) {
    if (input >= L'A' && input <= L'Z') return input + (L'a' - L'A');
    return input;
}

// 尝试将实际 Windows 路径无损地转为引擎的单字节路径。
// 引擎没有宽字符参数，所以优先使用系统 ANSI 代码页，不允许悄悄把汉字变成问号。
// 如果路径中的字符不属于当前代码页，再尝试 Windows 提供的短文件名别名。
// 系统关闭 8.3 短文件名且代码页不可表示的组合，目前只能安全拒绝，
// 不能生成指向错误文件的路径；后续需要原生 VFS 层的 Unicode 桥接。
bool EncodeNativePath(const wchar_t* wide, char* output, unsigned long capacity) {
    Bool replaced = 0;
    int result = WideCharToMultiByte(kAnsiCodePage, kNoBestFitChars,
        wide, -1, output, static_cast<int>(capacity), nullptr, &replaced);
    if (result > 0 && !replaced) return true;
    wchar_t short_path[520] = {};
    unsigned long length = GetShortPathNameW(wide, short_path, 520);
    if (!length || length >= 520) return false;
    replaced = 0;
    result = WideCharToMultiByte(kAnsiCodePage, kNoBestFitChars,
        short_path, -1, output, static_cast<int>(capacity), nullptr, &replaced);
    return result > 0 && !replaced;
}

// LOC 目录使用 sprintf 风格的模板：即使目录名中出现百分号，也必须按普通字符传入。
// 不进行转义时，路径中的 %d、%s 等会被引擎错误解释成格式参数并可能崩溃。
// 字体资源名并非格式模板，所以不能对字体路径进行这一转换。
bool AppendFormatSafe(char* output, unsigned long capacity, const char* input) {
    for (unsigned long index = 0; input[index]; ++index) {
        char character[2] = {input[index], 0};
        if (!Append(output, capacity, character)) return false;
        if (input[index] == '%' && !Append(output, capacity, "%")) return false;
    }
    return true;
}

// 根据游戏 EXE 的真实目录判断能否使用相对路径；判断与 ASI 名称无关。
// 即使 ASI 安装在根目录之外，也继续尝试原生绝对路径，不强制要求用户搬运文件。
State PreparePaths() {
    unsigned long base = 0;
    bool inside = true;
    while (g_exe_dir[base]) {
        if (!g_plugin_dir[base] || FoldAscii(g_exe_dir[base]) != FoldAscii(g_plugin_dir[base])) {
            inside = false;
            break;
        }
        ++base;
    }
    if (inside) {
        if (!EncodeNativePath(g_plugin_dir + base, g_relative_dir, sizeof(g_relative_dir)))
            return State::UnsupportedDirectoryName;
        g_path_mode[0] = 'R'; g_path_mode[1] = 'E'; g_path_mode[2] = 'L';
    } else {
        if (!EncodeNativePath(g_plugin_dir, g_relative_dir, sizeof(g_relative_dir)))
            return State::UnsupportedDirectoryName;
        g_path_mode[0] = 'A'; g_path_mode[1] = 'B'; g_path_mode[2] = 'S';
    }
    if (TextLength(g_relative_dir) > kMaxEnginePath) return State::UnsupportedDirectoryName;
    // 下列路径与 BSCNContents 文件夹具有固定相对关系，但不要求具体安装层级。
    if (!Append(g_font_resource, sizeof(g_font_resource), g_relative_dir) ||
        !Append(g_font_resource, sizeof(g_font_resource), "BSCNContents/fonts/ui_font_cnx") ||
        !Append(g_font_dat, sizeof(g_font_dat), g_relative_dir) ||
        !Append(g_font_dat, sizeof(g_font_dat), "BSCNContents/fonts/ui_font_cnx.dat") ||
        !AppendFormatSafe(g_loc_common, sizeof(g_loc_common), g_relative_dir) ||
        !Append(g_loc_common, sizeof(g_loc_common), "BSCNContents\\loc\\") ||
        !AppendFormatSafe(g_loc_levels, sizeof(g_loc_levels), g_relative_dir) ||
        !Append(g_loc_levels, sizeof(g_loc_levels), "BSCNContents\\loc\\Levels\\%s\\") ||
        !AppendFormatSafe(g_loc_multiplayer, sizeof(g_loc_multiplayer), g_relative_dir) ||
        !Append(g_loc_multiplayer, sizeof(g_loc_multiplayer), "BSCNContents\\loc\\MultiplayerLevels\\%s\\")) {
        return State::UnsupportedDirectoryName;
    }
    // 字体资源名称中的斜杠跟随原版格式；LOC 枚举路径保留反斜杠。
    for (unsigned long index = 0; g_font_resource[index]; ++index) {
        if (g_font_resource[index] == '\\') g_font_resource[index] = '/';
    }
    for (unsigned long index = 0; g_font_dat[index]; ++index) {
        if (g_font_dat[index] == '\\') g_font_dat[index] = '/';
    }
    return State::NotStarted;
}

bool AssetExists(const wchar_t* relative) {
    wchar_t full[520] = {};
    unsigned long pos = 0;
    while (g_plugin_dir[pos] && pos + 1 < 520) { full[pos] = g_plugin_dir[pos]; ++pos; }
    if (!AppendWide(full, 520, relative)) return false;
    const unsigned long attributes = GetFileAttributesW(full);
    return attributes != kInvalidAttributes && !(attributes & kDirectoryAttribute);
}

bool ValidateImage(unsigned char* module) {
    if (!module || module[0] != 'M' || module[1] != 'Z') return false;
    const unsigned long pe_offset = *reinterpret_cast<unsigned long*>(module + 0x3c);
    if (pe_offset < 0x40 || pe_offset > 0x100000) return false;
    unsigned char* header = module + pe_offset;
    if (header[0] != 'P' || header[1] != 'E' || header[2] || header[3]) return false;
    if (*reinterpret_cast<unsigned short*>(header + 4) != 0x014c) return false;
    const unsigned long length = *reinterpret_cast<unsigned long*>(header + 24 + 0x38);
    // 五个被修改的指令都在游戏 .text 内；原始字符串都在 .rdata 内。
    return length >= 0x00E00000 && length <= 0x02000000;
}

struct PatchSite {
    unsigned long push_rva;
    unsigned long string_rva;
    const char* original_string;
    const char* redirected_string;
};

// 顺序固定：先普通 LOC，再关卡 LOC，最后字体。应用之前一次性验证全部站点。
// 如果任何一项不匹配，禁止修改，避免只改部分路径后破坏游戏状态。
PatchSite Sites[5] = {
    {kLocCommonPush, kLocCommonLiteral, "gamedata\\xt\\Loc\\", g_loc_common},
    {kLocLevelsPush, kLocLevelsLiteral, "gamedata\\xt\\Loc\\Levels\\%s\\", g_loc_levels},
    {kLocMultiplayerPush, kLocMultiplayerLiteral, "gamedata\\xt\\Loc\\MultiplayerLevels\\%s\\", g_loc_multiplayer},
    {kFontResourcePush, kFontResourceLiteral, "Ui/Data/ui_font_eur", g_font_resource},
    {kFontDatPush, kFontDatLiteral, "Ui/data/ui_font_eur.dat", g_font_dat},
};

// 这里只把 push imm32 后面的 32 位字符串指针改成插件内的长路径。
// push 指令本身、游戏的 LOC 解析器、字体资源管理器均保持原版实现。
// 提供精确的写入失败阶段与 Win32 错误码，便于只凭诊断日志排查。
// 诊断变量仅由后台工作线程访问，不改变已实机验证的五个入口和补丁字节。
bool ChangePointer(unsigned char* instruction, unsigned long next) {
    unsigned long old_access = 0;
    g_last_patch_error = 0;
    g_last_patch_step = "VirtualProtect_RW";
    if (!VirtualProtect(instruction + 1, 4, kPageExecuteReadWrite, &old_access)) {
        g_last_patch_error = GetLastError();
        return false;
    }
    *reinterpret_cast<volatile unsigned long*>(instruction + 1) = next;
    g_last_patch_step = "FlushInstructionCache";
    const bool flushed = FlushInstructionCache(GetCurrentProcess(), instruction, 5) != 0;
    if (!flushed) g_last_patch_error = GetLastError();
    unsigned long ignored = 0;
    g_last_patch_step = "VirtualProtect_RESTORE";
    const bool restored = VirtualProtect(instruction + 1, 4, old_access, &ignored) != 0;
    if (!restored) g_last_patch_error = GetLastError();
    // 写入可能已成功而权限恢复失败；调用方必须撤销包括本站点在内的变更。
    g_last_patch_step = !flushed ? "FlushInstructionCache" :
        (restored ? "VERIFY_POINTER" : "VirtualProtect_RESTORE");
    return flushed && restored &&
        *reinterpret_cast<volatile unsigned long*>(instruction + 1) == next;
}

// 在辅助补丁中也复用按需日志；这里只声明，具体实现仍位于下文。
void LogEvent(const char* kind, const char* message);
void LogHex(const char* key, unsigned long value);

// 只改写原生 UIBondText 分发表的第三项，不注入跳板或接管逐字动画函数。
// 该表位于已解密的 EXE .text 段，需先核对指令、四个原始分支和 z 截断逻辑。
// 任何版本、指针或保护状态不符都安全跳过本效果；已成功的中文资源重定向保持运行。
bool DisableRandomLetterEffect(unsigned char* module) {
    const unsigned char dispatch_signature[] = {
        0x83, 0xF8, 0x03, 0x0F, 0x87, 0x6F, 0x02, 0x00,
        0x00, 0xFF, 0x24, 0x85, 0x74, 0x9C, 0xC6, 0x00,
    };
    const unsigned char random_signature[] = {
        0xB8, 0x01, 0x00, 0x00, 0x00, 0x39, 0x46, 0x20,
    };
    const unsigned char clamp_signature[] = {
        0x0F, 0xB7, 0xC1, 0x66, 0x83, 0xF8, 0x7A,
    };
    const unsigned char skip_signature[] = {
        0x8B, 0x55, 0x08, 0x8B, 0x44, 0x24, 0x18,
    };
    const unsigned char* locations[] = {
        module + kEffectDispatchRva,
        module + kEffectRandomRva,
        module + kEffectClampRva,
        module + kEffectSkipRva,
    };
    const unsigned char* signatures[] = {
        dispatch_signature, random_signature, clamp_signature, skip_signature,
    };
    const unsigned long lengths[] = {
        sizeof(dispatch_signature), sizeof(random_signature),
        sizeof(clamp_signature), sizeof(skip_signature),
    };
    for (unsigned long entry = 0; entry < 4; ++entry) {
        for (unsigned long byte = 0; byte < lengths[entry]; ++byte) {
            if (locations[entry][byte] != signatures[entry][byte]) {
                g_random_effect_status = "UNSUPPORTED_TEXT_EFFECT_SIGNATURE";
                LogEvent("RANDOM_EFFECT_SKIPPED", "原生文本效果指令校验失败，不修改该效果");
                return false;
            }
        }
    }

    unsigned long* const table = reinterpret_cast<unsigned long*>(
        module + kEffectJumpTableRva);
    for (unsigned long index = 0; index < 4; ++index) {
        if (table[index] != reinterpret_cast<unsigned long>(module + kEffectTargets[index])) {
            g_random_effect_status = "FOREIGN_TEXT_EFFECT_PATCH";
            LogEvent("RANDOM_EFFECT_SKIPPED", "原生文本效果分支已被其他插件修改");
            return false;
        }
    }
    volatile unsigned long* const random_entry =
        reinterpret_cast<volatile unsigned long*>(table + 2);
    const unsigned long original = reinterpret_cast<unsigned long>(module + kEffectRandomRva);
    const unsigned long bypass = reinterpret_cast<unsigned long>(module + kEffectSkipRva);
    g_random_effect_previous = *random_entry;
    LogHex("random_effect_original_branch", original);
    LogHex("random_effect_bypass_branch", bypass);

    // 对原始跳转表的单个 DWORD 开放短暂写权限，写入后立即刷新缓存并恢复权限。
    // 恢复保护失败时尽力写回原值；不允许报告为成功。
    unsigned long old_access = 0;
    if (!VirtualProtect(const_cast<unsigned long*>(table + 2), 4,
            kPageExecuteReadWrite, &old_access)) {
        g_random_effect_error = GetLastError();
        g_random_effect_status = "VIRTUALPROTECT_FAILED";
        return false;
    }
    *random_entry = bypass;
    const bool flushed = FlushInstructionCache(GetCurrentProcess(),
        const_cast<unsigned long*>(table + 2), 4) != 0;
    if (!flushed) g_random_effect_error = GetLastError();
    unsigned long ignored = 0;
    const bool restored = VirtualProtect(const_cast<unsigned long*>(table + 2),
        4, old_access, &ignored) != 0;
    if (!restored) g_random_effect_error = GetLastError();
    g_random_effect_current = *random_entry;
    if (flushed && restored && g_random_effect_current == bypass) {
        g_random_effect_status = "DISABLED_RANDOM_LETTERS";
        LogEvent("RANDOM_EFFECT_OK", "已跳过随机字母分支，其他逐字效果保持原生行为");
        return true;
    }
    // 最坏情况下维持原版随机字母效果，不得让损坏的分发表进入游戏。
    unsigned long access_for_rollback = 0;
    if (VirtualProtect(const_cast<unsigned long*>(table + 2), 4,
            kPageExecuteReadWrite, &access_for_rollback)) {
        *random_entry = original;
        FlushInstructionCache(GetCurrentProcess(),
            const_cast<unsigned long*>(table + 2), 4);
        unsigned long unused = 0;
        VirtualProtect(const_cast<unsigned long*>(table + 2),
            4, access_for_rollback, &unused);
    } else {
        g_random_effect_error = GetLastError();
    }
    g_random_effect_current = *random_entry;
    g_random_effect_status = g_random_effect_current == original ?
        "WRITE_FAILED_ROLLED_BACK" : "ROLLBACK_FAILED";
    LogEvent("RANDOM_EFFECT_ERROR", g_random_effect_status);
    return false;
}

// 以下事件函数在下文实现；调用点位于关键操作发生时，避免仅靠最终状态猜测。
void LogEvent(const char* kind, const char* message);
void LogNumber(const char* key, unsigned long value);
void LogHex(const char* key, unsigned long value);
void LogWidePath(const char* key, const wchar_t* path);
void LogSite(unsigned long index, unsigned char* module);

State ApplyAll() {
    unsigned char* module = static_cast<unsigned char*>(GetModuleHandleW(nullptr));
    if (!ValidateImage(module)) {
        LogEvent("ERROR", "游戏映像校验失败：不是受支持的 Win32 Bond.exe 映像");
        return State::InvalidModule;
    }
    // 旧 ASI 模块名只能作为诊断信息，不能直接断言五个补丁位置已冲突。
    // 只有后面的实际字符串、机器指令和指针核验失败才拒绝修改。
    const bool old_font = GetModuleHandleW(L"EurLooseRedirect.asi") != nullptr;
    const bool old_text = false;  // 当前模块名就是 BloodstoneCN，不能把自身误判为旧插件。
    g_legacy_modules[0] = old_font ? '1' : '0';
    g_legacy_modules[1] = ',';
    g_legacy_modules[2] = old_text ? '1' : '0';
    g_legacy_modules[3] = 0;
    if (!AssetExists(L"BSCNContents\\fonts\\ui_font_cnx.dat") ||
        !AssetExists(L"BSCNContents\\fonts\\ui_font_cnx.bpr") ||
        !AssetExists(L"BSCNContents\\loc\\common.loc")) {
        LogEvent("ERROR", "缺少必要的三项原生资源，禁止执行任何内存补丁");
        return State::AssetsMissing;
    }

    // 先确认静态原文字串。另一版本的 EXE、或旧插件改写过原始路径时，
    // 这些检查会拒绝错误版本，避免地址碰巧相同却修改无关指令。
    for (unsigned long index = 0; index < 5; ++index) {
        const PatchSite& site = Sites[index];
        if (!Matches(reinterpret_cast<const char*>(module + site.string_rva), site.original_string)) {
            g_failed_site = index + 1;
            LogNumber("original_literal_failed_site", g_failed_site);
            LogSite(index, module);
            return State::WrongBuild;
        }
    }

    // 原始 .text 在内存中解密之前并不是 push imm32，出现这种情况只能等待。
    for (unsigned long index = 0; index < 5; ++index) {
        const PatchSite& site = Sites[index];
        const unsigned char* instruction = module + site.push_rva;
        if (instruction[0] != 0x68) {
            g_failed_site = index + 1;
            if (g_wait_iterations == 0 || g_wait_iterations % 100 == 0)
                LogSite(index, module);
            return State::WaitingForDecryption;
        }
        // 指令已为 push imm32、却指向其他地址，意味着其他插件可能改写过本站点。
        // 此时继续等待无法使指针恢复，拒绝覆盖可避免两个 ASI 互相踩内存。
        if (*reinterpret_cast<const unsigned long*>(instruction + 1) !=
            reinterpret_cast<unsigned long>(module + site.string_rva)) {
            g_failed_site = index + 1;
            LogEvent("ERROR", "push 指向非原版位置，拒绝覆盖其他模块的补丁");
            LogSite(index, module);
            return State::ForeignPatchConflict;
        }
    }

    // 写入失败时把已经写过的站点恢复为原始路径指针，未修改的站点不碰。
    LogEvent("PATCH", "五处原始指令与原始字符串全部核验成功，准备写入");
    for (unsigned long index = 0; index < 5; ++index) {
        const PatchSite& site = Sites[index];
        unsigned char* instruction = module + site.push_rva;
        LogSite(index, module);
        if (ChangePointer(instruction, reinterpret_cast<unsigned long>(site.redirected_string))) {
            ++g_patch_count;
            LogEvent("PATCH_OK", "本处指针写入、指令缓存刷新、内存权限恢复完成");
            LogSite(index, module);
            continue;
        }
        LogEvent("ERROR", "指针写入失败，即将回滚已经修改的指令");
        LogEvent("ERROR_STAGE", g_last_patch_step);
        LogNumber("patch_win32_error", g_last_patch_error);
        // 失败可能发生在写入之后、权限恢复之前，因此失败站点也要尝试撤销。
        // 即使本站点在写入前就失败，改写回已验证的原始值仍然是安全操作。
        for (unsigned long count = index + 1; count > 0; --count) {
            const PatchSite& previous = Sites[count - 1];
            bool recovered = ChangePointer(module + previous.push_rva,
                reinterpret_cast<unsigned long>(module + previous.string_rva));
            LogEvent(recovered ? "ROLLBACK_OK" : "ROLLBACK_FAILED", "恢复原始路径指针");
            LogSite(count - 1, module);
        }
        g_failed_site = index + 1;
        return State::PatchFailed;
    }
    g_failed_site = 0;  // 等待解密期间留下的失败站点必须在全部成功后清零。
    LogEvent("PATCH", "五处指针全部成功，原生资源管理器读取结果仍需实机验证");
    // 文字特效是可选修复：即使 EXE 中该算法与当前转储不符，中文也应正常加载。
    DisableRandomLetterEffect(module);
    LogEvent("RANDOM_EFFECT_STATUS", g_random_effect_status);
    LogHex("random_effect_actual_branch", g_random_effect_current);
    if (g_random_effect_error) LogNumber("random_effect_win32_error", g_random_effect_error);
    return State::PathsRedirected;
}

void Put(char* buffer, unsigned long capacity, unsigned long& cursor, const char* text) {
    while (*text && cursor + 1 < capacity) buffer[cursor++] = *text++;
    buffer[cursor] = 0;
}

void PutNumber(char* buffer, unsigned long capacity, unsigned long& cursor, unsigned long value) {
    char reverse[12] = {};
    unsigned long count = 0;
    do { reverse[count++] = static_cast<char>('0' + value % 10); value /= 10; }
    while (value && count < 12);
    while (count && cursor + 1 < capacity) buffer[cursor++] = reverse[--count];
    buffer[cursor] = 0;
}

const char* StateText(State value) {
    switch (value) {
        case State::InvalidModule: return "INVALID_GAME_MODULE";
        case State::UnsupportedDirectoryName: return "UNSUPPORTED_ASI_DIRECTORY";
        case State::AssetsMissing: return "ASSETS_MISSING";
        case State::WaitingForDecryption: return "GAME_TEXT_NOT_DECRYPTED";
        case State::WrongBuild: return "ORIGINAL_STRING_CHANGED_OR_WRONG_BUILD";
        case State::ForeignPatchConflict: return "FOREIGN_CODE_PATCH_DETECTED";
        case State::PatchFailed: return "PATCH_FAILED_ROLLBACK_ATTEMPTED";
        case State::PathsRedirected: return "FIVE_PATHS_REDIRECTED";
        default: return "NOT_STARTED";
    }
}

// 使用单线程顺序写入；每条记录都带相对毫秒时间，便于与其他 MOD 日志对齐。
// 文件句柄只在 --log 模式打开；任何诊断失败都不能中断游戏原生加载。
void LogBytes(const char* bytes, unsigned long length) {
    if (!g_logging_requested || !g_log_file || !length) return;
    DWord written = 0;
    if (!WriteFile(g_log_file, bytes, length, &written, nullptr) || written != length)
        ++g_log_write_failures;
}

void LogEvent(const char* kind, const char* message) {
    if (!g_logging_requested || !g_log_file) return;
    char row[1600] = {};
    unsigned long cursor = 0;
    Put(row, sizeof(row), cursor, "[+");
    PutNumber(row, sizeof(row), cursor, GetTickCount() - g_log_started);
    Put(row, sizeof(row), cursor, "ms] [");
    Put(row, sizeof(row), cursor, kind);
    Put(row, sizeof(row), cursor, "] ");
    // 用户提供的文件名可能包含换行符，日志采用单行并替换为普通空格。
    for (unsigned long i = 0; message[i] && cursor + 3 < sizeof(row); ++i) {
        const char ch = message[i];
        row[cursor++] = ch == '\r' || ch == '\n' ? ' ' : ch;
    }
    Put(row, sizeof(row), cursor, "\r\n");
    LogBytes(row, cursor);
    ++g_log_lines;
}

void LogNumber(const char* key, unsigned long value) {
    if (!g_logging_requested) return;
    char row[512] = {};
    unsigned long cursor = 0;
    Put(row, sizeof(row), cursor, key);
    Put(row, sizeof(row), cursor, "=");
    PutNumber(row, sizeof(row), cursor, value);
    LogEvent("VALUE", row);
}

void PutHex(char* output, unsigned long capacity, unsigned long& cursor, unsigned long value) {
    const char* digits = "0123456789ABCDEF";
    Put(output, capacity, cursor, "0x");
    for (int shift = 28; shift >= 0; shift -= 4) {
        char one[2] = { digits[(value >> shift) & 15], 0 };
        Put(output, capacity, cursor, one);
    }
}

void LogHex(const char* key, unsigned long value) {
    if (!g_logging_requested) return;
    char row[512] = {};
    unsigned long cursor = 0;
    Put(row, sizeof(row), cursor, key);
    Put(row, sizeof(row), cursor, "=");
    PutHex(row, sizeof(row), cursor, value);
    LogEvent("VALUE", row);
}

void LogWidePath(const char* key, const wchar_t* path) {
    if (!g_logging_requested) return;
    char utf8[1050] = {};
    const int result = WideCharToMultiByte(kUtf8CodePage, 0, path, -1, utf8,
        sizeof(utf8), nullptr, nullptr);
    char row[1400] = {};
    unsigned long cursor = 0;
    Put(row, sizeof(row), cursor, key);
    Put(row, sizeof(row), cursor, "=");
    Put(row, sizeof(row), cursor, result ? utf8 : "<UTF8_CONVERSION_FAILED>");
    LogEvent("PATH", row);
}

__declspec(noinline) void LogEnginePath(const char* key, const char* ansi) {
    if (!g_logging_requested) return;
    wchar_t unicode[520] = {};
    if (!MultiByteToWideChar(kAnsiCodePage, 0, ansi, -1, unicode, 520)) {
        LogEvent("PATH_ERROR", "引擎路径不能从系统 ANSI 代码页转换为 Unicode");
        return;
    }
    LogWidePath(key, unicode);
}

void LogSite(unsigned long index, unsigned char* module) {
    if (!g_logging_requested || index >= 5 || !module) return;
    const PatchSite& site = Sites[index];
    const unsigned char* instruction = module + site.push_rva;
    char row[1100] = {};
    unsigned long cursor = 0;
    Put(row, sizeof(row), cursor, "site=");
    PutNumber(row, sizeof(row), cursor, index + 1);
    Put(row, sizeof(row), cursor, " rva=");
    PutHex(row, sizeof(row), cursor, site.push_rva);
    Put(row, sizeof(row), cursor, " opcode=");
    PutHex(row, sizeof(row), cursor, instruction[0]);
    Put(row, sizeof(row), cursor, " pointer_actual=");
    PutHex(row, sizeof(row), cursor, *reinterpret_cast<const unsigned long*>(instruction + 1));
    Put(row, sizeof(row), cursor, " pointer_original=");
    PutHex(row, sizeof(row), cursor,
        reinterpret_cast<unsigned long>(module + site.string_rva));
    Put(row, sizeof(row), cursor, " pointer_redirected=");
    PutHex(row, sizeof(row), cursor,
        reinterpret_cast<unsigned long>(site.redirected_string));
    Put(row, sizeof(row), cursor, " original_resource=");
    Put(row, sizeof(row), cursor, site.original_string);
    LogEvent("SITE", row);
}

// 指纹只用于快速比对用户日志与发行资源，不是安全用途的 SHA-256 校验。
// 仅在 --log 模式读取，单次最多检查 128 MiB，防止异常文件拖慢启动。
__declspec(noinline) void LogFileFingerprint(const wchar_t* absolute) {
    if (!g_logging_requested) return;
    Handle file = CreateFileW(absolute, kReadAccess, kReadShare | kWriteShare,
        nullptr, kOpenExisting, kNormalAttributes, nullptr);
    if (file == reinterpret_cast<Handle>(-1)) {
        LogNumber("fingerprint_open_error", GetLastError());
        return;
    }
    unsigned long digest = 2166136261ul;
    unsigned long total = 0;
    bool success = true;
    unsigned char data[2048] = {};
    for (;;) {
        DWord received = 0;
        if (!ReadFile(file, data, sizeof(data), &received, nullptr)) {
            LogNumber("fingerprint_read_error", GetLastError());
            success = false;
            break;
        }
        if (!received) break;
        total += received;
        for (DWord index = 0; index < received; ++index) {
            digest ^= data[index];
            digest *= 16777619ul;
        }
        if (total > 128ul * 1024ul * 1024ul) {
            LogEvent("FINGERPRINT_SKIP", "资源大于 128 MiB，避免诊断期间持续扫描");
            success = false;
            break;
        }
    }
    CloseHandle(file);
    if (success) {
        LogHex("file_fnv1a32", digest);
        LogNumber("file_hashed_bytes", total);
    }
}

__declspec(noinline) void LogRequiredAsset(const wchar_t* relative) {
    if (!g_logging_requested) return;
    wchar_t absolute[520] = {};
    unsigned long cursor = 0;
    while (g_plugin_dir[cursor] && cursor + 1 < 520) {
        absolute[cursor] = g_plugin_dir[cursor]; ++cursor;
    }
    if (!AppendWide(absolute, 520, relative)) {
        LogEvent("ASSET_ERROR", "文件绝对路径超过路径缓存容量");
        return;
    }
    LogWidePath("required_file", absolute);
    const DWord attributes = GetFileAttributesW(absolute);
    LogHex("file_attributes", attributes);
    if (attributes == kInvalidAttributes || (attributes & kDirectoryAttribute)) {
        if (attributes == kInvalidAttributes)
            LogNumber("file_attribute_win32_error", GetLastError());
        LogEvent("ASSET_MISSING", "必要的资源文件不存在或被当成目录");
        return;
    }
    Handle input = CreateFileW(absolute, kReadAccess,
        kReadShare | kWriteShare, nullptr, kOpenExisting, kNormalAttributes, nullptr);
    if (input == reinterpret_cast<Handle>(-1)) {
        LogNumber("file_open_win32_error", GetLastError());
        return;
    }
    DWord high = 0;
    const DWord low = GetFileSize(input, &high);
    LogNumber("file_size_low", low);
    LogNumber("file_size_high", high);
    CloseHandle(input);
    LogFileFingerprint(absolute);
}

// 只读递归枚举 LOC 文件名和大小。设置上限以避免异常目录或循环挂载拖慢游戏启动。
__declspec(noinline) void LogLocTree(wchar_t* directory, unsigned long depth) {
    if (!g_logging_requested || depth > 7 || g_log_loc_files >= 512) return;
    wchar_t search[520] = {};
    if (!AppendWide(search, 520, directory) || !AppendWide(search, 520, L"*")) {
        LogEvent("SCAN_ERROR", "LOC 搜索目录过长");
        return;
    }
    FindDataW info = {};
    Handle handle = FindFirstFileW(search, &info);
    if (handle == reinterpret_cast<Handle>(-1)) {
        LogNumber("find_first_win32_error", GetLastError());
        return;
    }
    do {
        if (info.file_name[0] == L'.' && (!info.file_name[1] ||
            (info.file_name[1] == L'.' && !info.file_name[2]))) continue;
        wchar_t item[520] = {};
        if (!AppendWide(item, 520, directory) ||
            !AppendWide(item, 520, info.file_name)) {
            LogEvent("SCAN_ERROR", "某个 LOC 文件名超过日志路径缓存");
            continue;
        }
        if (info.attributes & kDirectoryAttribute) {
            if (info.attributes & kReparseAttribute) {
                LogWidePath("skipped_reparse_directory", item);
                continue;
            }
            LogWidePath("loc_directory", item);
            if (AppendWide(item, 520, L"\\")) LogLocTree(item, depth + 1);
        } else {
            ++g_log_loc_files;
            LogWidePath("loc_file", item);
            LogNumber("loc_file_size_low", info.size_low);
            LogNumber("loc_file_size_high", info.size_high);
            LogFileFingerprint(item);
        }
    } while (g_log_loc_files < 512 && FindNextFileW(handle, &info));
    FindClose(handle);
}

__declspec(noinline) void OpenLog() {
    if (!g_logging_requested) return;
    g_log_started = GetTickCount();
    wchar_t filename[520] = {};
    if (!AppendWide(filename, 520, g_plugin_dir) ||
        !AppendWide(filename, 520, L"BloodstoneCN.log")) return;
    g_log_file = CreateFileW(filename, kWriteAccess,
        kReadShare | kWriteShare, nullptr, kCreateAlways, kNormalAttributes, nullptr);
    if (g_log_file == reinterpret_cast<Handle>(-1)) {
        g_log_file = nullptr;
        return;
    }
    LogEvent("START", "BloodstoneCN v1.2 M1 | --log 模式开启 | UTF-8 / CRLF");
    LogEvent("POLICY", "诊断原生资源路径与随机字母分支；不重封 PAK、不修改语言、不接管文本/字形函数");
    LogEvent("LIMIT", "插件路径修改成功不等于原生 VFS 已确认加载资源");
    LogNumber("process_id", GetCurrentProcessId());
    LogWidePath("asi_directory", g_plugin_dir);
    LogWidePath("exe_directory", g_exe_dir);
    // 开始写入后刷新一次，游戏在后续早期崩溃时仍有启动定位信息。
    FlushFileBuffers(g_log_file);
}

__declspec(noinline) void RecordResult() {
    if (!g_logging_requested || !g_log_file) return;
    char report[2800] = {};
    unsigned long at = 0;
    Put(report, sizeof(report), at, "BloodstoneCN v1.2 M1 diagnostics summary\r\n");
    Put(report, sizeof(report), at, "mode=EN_UNCHANGED pak=ORIGINAL text_hooks=0 glyph_hooks=0\r\n");
    Put(report, sizeof(report), at, "loc_sites=3 font_sites=2 wait_iterations=");
    PutNumber(report, sizeof(report), at, g_wait_iterations);
    Put(report, sizeof(report), at, "\r\npath_mode=");
    Put(report, sizeof(report), at, g_path_mode);
    Put(report, sizeof(report), at, "\r\nlegacy_modules_font_text=");
    Put(report, sizeof(report), at, g_legacy_modules);
    Put(report, sizeof(report), at, "\r\npatch_count=");
    PutNumber(report, sizeof(report), at, g_patch_count);
    Put(report, sizeof(report), at, "\r\nfailed_site_1_to_5=");
    PutNumber(report, sizeof(report), at, g_failed_site);
    // 路径可能含有非 ASCII 字节；前面已单独把引擎 ANSI 路径转成 UTF-8 记录。
    // 此处只记录 ASCII 状态，避免把系统代码页字节混入 UTF-8 日志。
    Put(report, sizeof(report), at, "\r\nrandom_letter_effect=");
    Put(report, sizeof(report), at, g_random_effect_status);
    Put(report, sizeof(report), at, "\r\nrandom_effect_win32_error=");
    PutNumber(report, sizeof(report), at, g_random_effect_error);
    Put(report, sizeof(report), at, "\r\nstatus=");
    Put(report, sizeof(report), at, StateText(g_state));
    Put(report, sizeof(report), at, "\r\nNOTE: pointer redirect alone does not prove that native VFS loaded external assets.\r\n");
    LogEvent("FINAL_STATUS", StateText(g_state));
    LogNumber("diagnostic_loc_files_enumerated", g_log_loc_files);
    LogNumber("diagnostic_write_errors", g_log_write_failures);
    LogNumber("diagnostic_event_lines", g_log_lines);
    LogBytes(report, at);
    FlushFileBuffers(g_log_file);
}

__declspec(noinline) void CloseLog() {
    if (!g_log_file) return;
    LogEvent("END", "日志采集结束，运行时资源路径监控仅覆盖游戏初始化阶段");
    FlushFileBuffers(g_log_file);
    CloseHandle(g_log_file);
    g_log_file = nullptr;
}

__declspec(noinline) DWord WIN_CALL Worker(void*) {
    OpenLog();
    LogEvent("STAGE", "准备解析五处原生 LOC/EUR 字体路径与随机字母扰动分支");
    g_state = PreparePaths();
    LogEvent("PATH_PREPARE", StateText(g_state));
    LogEvent("PATH_MODE", g_path_mode);
    LogEnginePath("engine_relative_prefix", g_relative_dir);
    LogEnginePath("loc_common", g_loc_common);
    LogEnginePath("loc_levels_template", g_loc_levels);
    LogEnginePath("loc_multiplayer_template", g_loc_multiplayer);
    LogEnginePath("font_resource", g_font_resource);
    LogEnginePath("font_dat", g_font_dat);
    if (g_state == State::NotStarted) {
        LogRequiredAsset(L"BSCNContents\\fonts\\ui_font_cnx.dat");
        LogRequiredAsset(L"BSCNContents\\fonts\\ui_font_cnx.bpr");
        LogRequiredAsset(L"BSCNContents\\loc\\common.loc");
        wchar_t loc_directory[520] = {};
        if (AppendWide(loc_directory, 520, g_plugin_dir) &&
            AppendWide(loc_directory, 520, L"BSCNContents\\loc\\")) {
            LogLocTree(loc_directory, 0);
        }
        LogNumber("loc_files_found", g_log_loc_files);
        LogEvent("STAGE", "开始检查 EXE 解密状态及五处原生字符串指针");
    }
    if (g_state == State::NotStarted) {
        // 游戏代码若尚未解密，短时间轮询其明确指令签名。
        // 最长 15 秒；退出时记录等待次数，不会在后台永久占用线程。
        for (g_wait_iterations = 0; g_wait_iterations < 1500; ++g_wait_iterations) {
            g_state = ApplyAll();
            if (g_state != State::WaitingForDecryption) break;
            if (g_wait_iterations == 0 || g_wait_iterations % 100 == 0)
                LogNumber("waiting_for_exe_decryption_iteration", g_wait_iterations);
            Sleep(10);
        }
    }
    RecordResult();
    CloseLog();
    return 0;
}

}  // namespace

extern "C" Bool WIN_CALL DllMain(Module self, DWord reason, void*) {
    if (reason != kDllAttach) return 1;
    // 独立参数 --log 是各插件都能自行读取的公共约定，本插件不替其他 ASI 强制开日志。
    // 不含该参数时不打开任何日志文件、不发送调试输出，正常补丁仍照常安装。
    g_logging_requested = bscn::HasLogArgument(GetCommandLineW());
    // 在加载器锁中只取得路径并创建线程；文件检查和代码写入留给后台线程。
    if (!GetDirectory(nullptr, g_exe_dir) || !GetDirectory(self, g_plugin_dir)) {
        g_state = State::InvalidModule;
        return 1;
    }
    Handle thread = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (thread) CloseHandle(thread);
    return 1;
}
