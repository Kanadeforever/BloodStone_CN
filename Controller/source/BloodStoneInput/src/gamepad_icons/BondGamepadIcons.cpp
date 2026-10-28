#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#ifdef BSCN_SINGLE_DLL
#include "../bond_one_log.hpp"
#include "../bond_one_bridge.hpp"
#endif
#include <shellapi.h>
#include <d3d9.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <string.h>
#include <math.h>
#include <cwctype>
#include <cctype>

#include <algorithm>
#include <array>
#include <iterator>
#include <vector>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

#include "generated/EmbeddedIcons.generated.h"
#include "generated/AtlasReference.generated.h"

#ifndef IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
#define IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT 13
#endif

// 此模块只在 Bond.exe 中转换原生按键提示；不会接管其他游戏的 UI。
// 图标使用经过实机确认的图集和 Action 规则，特别保留 D3DPOOL_MANAGED
// 图集在 Reset 后的候选状态以及 (3,7) 无限弹药单元。
// 合并构建从输入代理获取 SDL 已发布槽位，不对 SDL 做第二轮设备枚举。
// 输入代理负责 XInput/DirectInput，图标模块只处理原生提示和 D3D9 纹理。

namespace
{
constexpr wchar_t kPluginName[] = L"BondGamepadIcons";
constexpr wchar_t kPluginVersion[] = L"1.4.20-reset-managed-atlas-fix-test";

HMODULE g_selfModule = nullptr;
HMODULE g_mainModule = nullptr;
uintptr_t g_base = 0;

wchar_t g_moduleDirectory[MAX_PATH] = L"";
wchar_t g_iniPath[MAX_PATH] = L"";
wchar_t g_logPath[MAX_PATH] = L"";
HANDLE g_logFile = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_logLock;
CRITICAL_SECTION g_textureLock;

// 目标 EXE 版本特征。只对已经分析过的 Bond.exe 安装地址补丁。
constexpr DWORD kExpectedTimeDateStamp = 0x4CA36144u;
constexpr DWORD kExpectedSizeOfImage = 0x010CC000u;
constexpr DWORD kExpectedEntryRva = 0x010CBC00u;

// 运行时地址 = 主模块 Base + RVA。
constexpr uintptr_t kRvaGetPromptIcon = 0x00830F20u;     // 00C30F20
constexpr uintptr_t kRvaGetPromptName = 0x00830F80u;     // 00C30F80
constexpr uintptr_t kRvaGetKeyboardValue = 0x00830DD0u;  // 00C30DD0
constexpr uintptr_t kRvaGetMouseValue = 0x00830E40u;     // 00C30E40
constexpr uintptr_t kRvaKeyboardIconTable = 0x00EEAD18u; // 012EAD18
constexpr uintptr_t kRvaMouseIconTable = 0x00EEAAB0u;    // 012EAAB0

// 玩家对象中的 UseJoypad 是游戏选项实际使用的输入模式，不依赖热切换猜测。
constexpr uintptr_t kRvaPlayerManagerGlobal = 0x00F74A30u;       // 01374A30
constexpr uintptr_t kRvaGetPlayerObject = 0x000256E0u;           // 004256E0
constexpr size_t kPlayerControlsOffset = 0x64u;
constexpr size_t kControlsUseJoypadOffset = 0x00u;
constexpr BYTE kExpectedGetPlayerObjectHead[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8, 0xFF};
constexpr uintptr_t kRvaFontSequenceVector = 0x01077700u; // 01477700
constexpr uintptr_t kRvaTokenRenderer = 0x00A85740u;      // 00E85740
// QTE 相关 UI API。静态注册表确认名称与函数地址，代码区运行时解密；这里只记录
// 函数头，不安装未知调用约定的 Hook。
constexpr uintptr_t kRvaGetCurrentControlConfigEmptyButtons = 0x008531D0u; // 00C531D0
constexpr uintptr_t kRvaInputGadgetSetGameButton = 0x0085D430u;            // 00C5D430

// 动作 38/39 的 GetPromptIcon 调用点。实机日志确认：
// - 00C32B7E 构造动作 38（PC A），在掩体切换教程中语义是左摇杆向左；
// - 00C32B5C 构造动作 39（PC D），在掩体切换教程中语义是左摇杆向右；
// - 00C75873 只在黑客 QTE 运行阶段逐帧查询当前动作，语义才是 Xbox A/X。
// 因此两个构造调用点按掩体教程解释，不能误认为黑客 QTE。
constexpr uintptr_t kRvaQteBuildPromptD = 0x00832B5Cu; // 00C32B5C
constexpr uintptr_t kRvaQteBuildPromptA = 0x00832B7Eu; // 00C32B7E
constexpr uintptr_t kRvaQteRuntimePrompt = 0x00875873u; // 00C75873
constexpr int kQteActionA = 38;
constexpr int kQteActionD = 39;

// 掩体教程确实返回 SCROLLLOCK / NUMLOCK，但该教程
// 完全没有进入 TokenRenderer/Draw 事件纹理链，因此临时事件变体从未生效，最终
// 只能显示基础图集中的 LS_UP / LS_DOWN。为解决此约束，四个左摇杆方向载体采用全局分配：
// SCROLLLOCK / NUMLOCK 固定承载 LS_LEFT / LS_RIGHT；k1 / k4 迁移承载 LS_UP / LS_DOWN。
// 当前完整日志没有任何已解析动作使用 LS_UP / LS_DOWN；若以后出现，普通动态事件仍
// 会经过 GetPromptName 抑制键名，因此 k1/k4 不会像本教程一样必然叠加数字。

// 游戏真实输入层。PadUpdate 提供当前手柄对象；ActionHeld 提供当前逻辑动作管理器。
// 两个 Hook 都只保存 this 指针并原样调用游戏函数，不改变任何输入状态。
constexpr uintptr_t kRvaPadUpdate = 0x0042F8F0u;       // 0082F8F0
constexpr uintptr_t kRvaActionHeld = 0x0042D300u;      // 0082D300
constexpr BYTE kExpectedPadUpdateHead[] = {0x83, 0xEC, 0x10, 0x56, 0x8B, 0xF1};
constexpr BYTE kExpectedActionHeldHead[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x51, 0x08};
constexpr BYTE kExpectedTokenRendererHead[] = {0x53, 0x56, 0x8B, 0xF1, 0x33, 0xC9, 0x57};

// 已从运行时解密代码逐项核对的 binding 节点 vtable RVA。
// 运行时 vtable 地址 = Bond.exe Base + 下列 RVA。
constexpr uintptr_t kRvaBindingDirectButton = 0x00D1766Cu;      // 0111766C
constexpr uintptr_t kRvaBindingLatch = 0x00D1767Cu;             // 0111767C
constexpr uintptr_t kRvaBindingDirectionalAxis = 0x00D1768Cu;   // 0111768C
constexpr uintptr_t kRvaBindingDirectAxis = 0x00D1769Cu;        // 0111769C
constexpr uintptr_t kRvaBindingDifference = 0x00D176A4u;        // 011176A4
constexpr uintptr_t kRvaBindingThresholdDifference = 0x00D176ACu; // 011176AC
constexpr uintptr_t kRvaBindingGate = 0x00D176B4u;              // 011176B4
constexpr uintptr_t kRvaBindingScale = 0x00D176BCu;             // 011176BC
constexpr uintptr_t kRvaBindingAggregate = 0x00D176C4u;         // 011176C4
constexpr uintptr_t kRvaBindingSum = 0x00D176D4u;               // 011176D4

// objdump 显示的 IAT RVA；正常情况下优先通过 PE 导入表按名称查找，
// 只有名称遍历失败时才使用该地址作为已知版本的后备。
constexpr uintptr_t kRvaD3DXCreateTextureFromFileInMemoryIat = 0x00C91708u;
constexpr uintptr_t kRvaDirect3DCreate9Iat = 0x00C916C8u;

// COM vtable 索引来自 Direct3D 9 SDK 接口顺序。
constexpr size_t kIDirect3D9CreateDeviceIndex = 16;
constexpr size_t kIDirect3DDevice9ResetIndex = 16;
constexpr size_t kIDirect3DDevice9CreateTextureIndex = 23;
constexpr size_t kIDirect3DDevice9SetTextureIndex = 65;
constexpr size_t kIDirect3DDevice9DrawPrimitiveIndex = 81;
constexpr size_t kIDirect3DDevice9DrawIndexedPrimitiveIndex = 82;
constexpr size_t kIDirect3DDevice9DrawPrimitiveUPIndex = 83;
constexpr size_t kIDirect3DDevice9DrawIndexedPrimitiveUPIndex = 84;

constexpr BYTE kExpectedGetPromptIconHead[8] = {
    0x53, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57, 0x8B
};
// GetPromptName 的入口位于运行时解密代码区。当前 7 字节签名按相邻 GetPromptIcon 的
// 同类编译模板推定，并严格停在完整指令边界。只有运行时字节完全一致时才写入 JMP；
// 不一致时绝不尝试 Hook，而是把 16 字节函数头写入日志，供下一轮准确校准。
constexpr BYTE kExpectedGetPromptNameHead[7] = {
    0x53, 0x56, 0x8B, 0x74, 0x24, 0x10, 0x57
};

using BindingValueFn = int(__cdecl*)(int playerSlot, int actionId);
using PromptNameFn = const wchar_t*(__cdecl*)(int playerSlot, int actionId);
using GetPlayerObjectFn = void*(__thiscall*)(void* manager, int playerSlot);
using PadUpdateFn = void(__thiscall*)(void* self);
using ActionHeldFn = int(__thiscall*)(void* self, int actionId);
using TokenRendererFn = float(__thiscall*)(void* self,
                                           uintptr_t arg1,
                                           uintptr_t arg2,
                                           uintptr_t arg3,
                                           uintptr_t arg4,
                                           uintptr_t arg5,
                                           uintptr_t arg6,
                                           uintptr_t arg7,
                                           uintptr_t arg8,
                                           uintptr_t arg9);
using D3DXCreateTextureFromFileInMemoryFn = HRESULT(WINAPI*)(
    LPDIRECT3DDEVICE9 device,
    LPCVOID sourceData,
    UINT sourceDataSize,
    LPDIRECT3DTEXTURE9* texture);

using SdlJoystickId = uint32_t;
using SdlGetGamepadsFn = SdlJoystickId*(__cdecl*)(int* count);
using SdlFreeFn = void(__cdecl*)(void* memory);
using SdlGetGamepadTypeForIdFn = int(__cdecl*)(SdlJoystickId instanceId);
using SdlGetRealGamepadTypeForIdFn = int(__cdecl*)(SdlJoystickId instanceId);
using SdlGetGamepadStringForTypeFn = const char*(__cdecl*)(int type);
using SdlGetGamepadButtonLabelForTypeFn = int(__cdecl*)(int type, int button);
using SdlGetGamepadNameForIdFn = const char*(__cdecl*)(SdlJoystickId instanceId);
using SdlGetGamepadVendorForIdFn = uint16_t(__cdecl*)(SdlJoystickId instanceId);

struct SdlReadOnlyApi
{
    HMODULE module = nullptr;
    SdlGetGamepadsFn getGamepads = nullptr;
    SdlFreeFn freeMemory = nullptr;
    SdlGetGamepadTypeForIdFn getMappedTypeForId = nullptr;
    SdlGetRealGamepadTypeForIdFn getRealTypeForId = nullptr;
    SdlGetGamepadStringForTypeFn getTypeString = nullptr;
    SdlGetGamepadButtonLabelForTypeFn getButtonLabelForType = nullptr;
    SdlGetGamepadNameForIdFn getNameForId = nullptr;
    SdlGetGamepadVendorForIdFn getVendorForId = nullptr;
};

SdlReadOnlyApi g_sdlApi = {};

using Direct3DCreate9Fn = IDirect3D9*(WINAPI*)(UINT sdkVersion);
using IDirect3D9CreateDeviceFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3D9* self,
    UINT adapter,
    D3DDEVTYPE deviceType,
    HWND focusWindow,
    DWORD behaviorFlags,
    D3DPRESENT_PARAMETERS* presentationParameters,
    IDirect3DDevice9** returnedDevice);
using IDirect3DDevice9ResetFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self,
    D3DPRESENT_PARAMETERS* presentationParameters);
using IDirect3DDevice9CreateTextureFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self,
    UINT width,
    UINT height,
    UINT levels,
    DWORD usage,
    D3DFORMAT format,
    D3DPOOL pool,
    IDirect3DTexture9** returnedTexture,
    HANDLE* sharedHandle);
using IDirect3DDevice9SetTextureFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self,
    DWORD stage,
    IDirect3DBaseTexture9* texture);
using IDirect3DDevice9DrawPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT startVertex,
    UINT primitiveCount);
using IDirect3DDevice9DrawIndexedPrimitiveFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    INT baseVertexIndex,
    UINT minVertexIndex,
    UINT numVertices,
    UINT startIndex,
    UINT primitiveCount);
using IDirect3DDevice9DrawPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT primitiveCount,
    const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride);
using IDirect3DDevice9DrawIndexedPrimitiveUPFn = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9* self,
    D3DPRIMITIVETYPE primitiveType,
    UINT minVertexIndex,
    UINT numVertices,
    UINT primitiveCount,
    const void* indexData,
    D3DFORMAT indexDataFormat,
    const void* vertexStreamZeroData,
    UINT vertexStreamZeroStride);

BindingValueFn g_getKeyboardValue = nullptr;
BindingValueFn g_getMouseValue = nullptr;
const wchar_t** g_keyboardIconTable = nullptr;
const wchar_t** g_mouseIconTable = nullptr;

PadUpdateFn g_originalPadUpdate = nullptr;
ActionHeldFn g_originalActionHeld = nullptr;
TokenRendererFn g_originalTokenRenderer = nullptr;
PromptNameFn g_originalGetPromptName = nullptr;
PVOID volatile g_currentPadObject = nullptr;
PVOID volatile g_currentActionManager = nullptr;
std::vector<void*> g_inputTrampolines;

D3DXCreateTextureFromFileInMemoryFn g_originalCreateTextureFromMemory = nullptr;
void** g_createTextureIatSlot = nullptr;

Direct3DCreate9Fn g_originalDirect3DCreate9 = nullptr;
IDirect3D9CreateDeviceFn g_originalD3D9CreateDevice = nullptr;
IDirect3DDevice9ResetFn g_originalDeviceReset = nullptr;
IDirect3DDevice9CreateTextureFn g_originalDeviceCreateTexture = nullptr;
IDirect3DDevice9SetTextureFn g_originalDeviceSetTexture = nullptr;
IDirect3DDevice9DrawPrimitiveFn g_originalDeviceDrawPrimitive = nullptr;
IDirect3DDevice9DrawIndexedPrimitiveFn g_originalDeviceDrawIndexedPrimitive = nullptr;
IDirect3DDevice9DrawPrimitiveUPFn g_originalDeviceDrawPrimitiveUP = nullptr;
IDirect3DDevice9DrawIndexedPrimitiveUPFn g_originalDeviceDrawIndexedPrimitiveUP = nullptr;
IDirect3DDevice9* g_activeD3D9Device = nullptr; // 游戏持有；插件不 AddRef。
void** g_direct3DCreate9IatSlot = nullptr;

volatile LONG g_factoryHookCallCount = 0;
volatile LONG g_createDeviceCallCount = 0;
volatile LONG g_deviceCreateTextureCalls = 0;
volatile LONG g_deviceSetTextureCalls = 0;
volatile LONG g_deviceDrawPrimitiveCalls = 0;
volatile LONG g_deviceDrawIndexedPrimitiveCalls = 0;
volatile LONG g_deviceDrawPrimitiveUPCalls = 0;
volatile LONG g_deviceDrawIndexedPrimitiveUPCalls = 0;
volatile LONG g_device512Created = 0;
volatile LONG g_device512Inspected = 0;
volatile LONG g_configurationReady = 0;

enum class IconStyleMode : LONG
{
    Auto = 0,
    Xbox = 1,
    PlayStation = 2,
    Nintendo = 3
};

enum class ControllerPromptFamily : int
{
    Unknown = 0,
    Xbox,
    PlayStation,
    Nintendo
};

enum class FamilyResolutionSource : int
{
    NoSdl = 0,
    NoDevice,
    EffectiveType,
    EffectiveTypeString,
    FaceButtonLabels,
    RealType,
    RealTypeString,
    DeviceName,
    VendorId,
    DefaultXbox
};

int g_enable = 1;
int g_enableLog = 1;
bool g_iniPresent = false;
volatile LONG g_iconStyleMode = static_cast<LONG>(IconStyleMode::Xbox);
volatile LONG g_iconStyleIndex = 0; // 0=Xbox，1=PS5，2=Switch。只影响图集像素。
bool g_iconStyleValueValid = true;
std::wstring g_requestedIconStyle = L"Xbox";
bool g_commandLineStyleOverride = false;
std::wstring g_commandLineStyleArgument;

constexpr DWORD kSdlFamilyPollIntervalMs = 1000;
constexpr DWORD kSdlModuleStabilizationMs = 1500;
constexpr int kSdlFamilyStablePollCount = 3;
volatile LONG g_autoAtlasRepairCount = 0;
DWORD g_lastAutoStyleApplyTick = 0;
bool g_hasAutoStyleApplyTick = false;
std::array<uint32_t, 4> g_sdlMirrorSlotIds = {};
HMODULE g_sdlModule = nullptr; // 只借用进程中已经加载的 SDL3.dll；不增加引用计数。
DWORD g_sdlModuleFirstSeenTick = 0;
bool g_hasSdlModuleFirstSeenTick = false;
volatile LONG g_workerStopRequested = 0;
volatile LONG g_sdlObserverQueryFaults = 0;
bool g_threadLibraryCallsDisabled = false;
DWORD g_threadRuntimeTlsIndex = TLS_OUT_OF_INDEXES;

// JoyShim 新版会周期性扫描游戏目录模块的 PE Import Directory。UPX 存根本身
// 必然导入 GetProcAddress，因此仅仅从 C++ 源码中移除 GetProcAddress 还不够。
// 下面只记录本模块“内存映像”中被隐藏的目录元数据，便于日志核对；磁盘文件和
// 已经解析完成的 IAT 均保持不变。
struct RuntimeImportShieldState
{
    DWORD originalImportRva = 0;
    DWORD originalImportSize = 0;
    DWORD originalDelayImportRva = 0;
    DWORD originalDelayImportSize = 0;
    bool applied = false;
    bool alreadyHidden = false;
};

RuntimeImportShieldState g_runtimeImportShield = {};
SRWLOCK g_autoFamilySnapshotLock = SRWLOCK_INIT;
int g_lastLoggedFamily = -1;
int g_lastLoggedFamilySource = -1;
uint32_t g_lastLoggedSdlDeviceId = 0;
int g_enablePromptHook = 1;
int g_enablePromptNameHook = 1;
int g_enableTextureHook = 1;
int g_enableD3DXFallbackHook = 0;
int g_fallbackToKeyboardMouse = 1;
int g_useGameControllerBindings = 1;
int g_logFirstPromptCalls = 128;
int g_logEveryPromptCall = 0;
int g_logFirstBindingResolutions = 256;
int g_logEveryBindingResolution = 0;
int g_logUnknownBindingNodes = 1;
int g_enableEventRendererHook = 0;
// 掩体教程未进入 TokenRenderer/Draw，不能依赖临时事件纹理。
// 相关诊断分支保留为不可配置的关闭状态，避免影响既有通用诊断代码结构。
int g_enableCoverStaticFix = 0;
int g_enableQteDiagnosticHook = 0;
int g_forceControllerMode = -1; // -1=读取游戏 UseJoypad；0=强制键鼠；1=强制手柄。
int g_enableSequenceAliasMatching = 0;
// 每种图标使用不同载体；此寿命只用于防止极少数未被渲染的载体长期滞留。
int g_logFirstEventRenders = 256;
int g_logEveryEventRender = 0;
int g_logFirstEventArgumentSnapshots = 0;
int g_logFirstQteCandidates = 64;
int g_logQteRecordHex = 1;
int g_promptHintLifetimeMs = 3000;
// 某些 UI 路径可能只在 TokenRenderer 返回后批量提交 D3D draw。短期延迟窗口只会
// 在确认图集实际绑定时生效，不会对无关纹理执行覆盖。
int g_deferredDrawLifetimeMs = 100;
int g_deferredAtlasDrawBudget = 4;
int g_initialDelayMs = 0;
int g_retryDurationMs = 30000;
int g_retryIntervalMs = 10;

int g_autoDetectTexture = 1;
double g_autoDetectThreshold = 18.0;
int g_targetCandidateIndex = 0;
uint32_t g_targetSourceCrc32 = 0;
uint32_t g_targetPixelCrc32 = 0;
// 设备图集可能在第一次绑定后的若干帧内继续填充。检查节奏必须由单调时钟决定，
// 不能由日志 WriteFile/FlushFileBuffers 的快慢间接决定。以下参数作为内部固定策略，
// 不暴露到默认 INI，避免用户配置重新引入同帧快速淘汰。
constexpr DWORD kDeviceInspectionInitialGraceMs = 250;
constexpr DWORD kDeviceInspectionIntervalMs = 100;
constexpr DWORD kDeviceInspectionTimeoutMs = 5000;
constexpr int kDeviceInspectionMaximumAttempts = 48;
int g_stopScanningAfterFirstPatch = 1;
int g_logAll512Candidates = 1;
int g_patchEveryMatchingAtlas = 1;
int g_premultiplyEmbeddedIcons = 0;
int g_flipAtlasVertically = -1; // -1=根据识别签名自动决定，0=不翻转，1=垂直翻转。

volatile LONG g_totalPromptCalls = 0;
volatile LONG g_totalPromptNameCalls = 0;
volatile LONG g_suppressedPromptNameCalls = 0;
volatile LONG g_controllerModeActive = 0;
volatile LONG g_controllerModeReadFailures = 0;
volatile LONG g_resolvedPromptCalls = 0;
volatile LONG g_unresolvedPromptCalls = 0;
volatile LONG g_ambiguousPromptCalls = 0;
volatile LONG g_fallbackPromptCalls = 0;
volatile LONG g_nullPromptCalls = 0;
volatile LONG g_bindingResolutionCalls = 0;
volatile LONG g_padCaptureChanges = 0;
volatile LONG g_managerCaptureChanges = 0;
volatile LONG g_textureLoadCalls = 0;
volatile LONG g_512CandidateCalls = 0;
volatile LONG g_atlasPatchCount = 0;
volatile LONG g_eventRenderCalls = 0;
volatile LONG g_eventMatchedCalls = 0;
volatile LONG g_eventTextureBindings = 0;
volatile LONG g_eventTextureOverrideAttempts = 0;
volatile LONG g_eventTextureOverrideApplied = 0;
volatile LONG g_eventTextureOverrideRestored = 0;
volatile LONG g_eventTextureOverrideFailures = 0;
volatile LONG g_eventTextureUnexpectedStage = 0;
volatile LONG g_eventDrawCalls = 0;
volatile LONG g_eventDrawOverrideApplied = 0;
volatile LONG g_eventDrawOverrideRestored = 0;
volatile LONG g_eventDrawOverrideFailures = 0;
volatile LONG g_deferredDrawHintsCreated = 0;
volatile LONG g_deferredDrawOverrideApplied = 0;
volatile LONG g_deferredDrawHintsExpired = 0;
volatile LONG g_eventUnmatchedLogged = 0;
volatile LONG g_qteDiagnosticCalls = 0;
volatile LONG g_qteCandidateCalls = 0;
volatile LONG g_qteUniqueCandidates = 0;
volatile LONG g_qtePromptOverrides = 0;
volatile LONG g_coverDirectionOverrides = 0;
volatile LONG g_coverStaticArmed = 0;
volatile LONG g_coverStaticRendered = 0;
volatile LONG g_promptCarrierIssued = 0;
volatile LONG g_promptCarrierConsumed = 0;
volatile LONG g_promptCarrierExpired = 0;
volatile LONG g_promptCarrierUnarmed = 0;
volatile LONG g_promptCarrierPendingDropped = 0;

struct Pixel
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
};

static_assert(sizeof(Pixel) == 4, "Pixel 必须为紧凑 RGBA 四字节结构");

enum class ControllerIcon : int
{
    A = 0,
    B,
    X,
    Y,
    LB,
    RB,
    LT,
    RT,
    Dpu,
    Dpd,
    Dpl,
    Dpr,
    LsUp,
    LsDown,
    LsLeft,
    LsRight,
    Lsc,
    RsUp,
    RsDown,
    RsAll,
    RsRight,
    Rsc,
    Start,
    Back,
    Count,
    Invalid = -1
};

constexpr int kControllerIconCount = static_cast<int>(ControllerIcon::Count);
static_assert(kControllerIconCount == kEmbeddedIconCount,
              "C++ ControllerIcon 枚举必须与生成图标顺序完全一致");
static_assert(kEmbeddedIconStyleCount == 3,
              "当前运行时与生成器必须同时提供 Xbox、PS5、Switch 三套图标");

// 每种控制器图标使用一个不同的 FontSequence 作为“载体”。这里不能根据
// token 表索引推算图集坐标：Blood Stone 的 token 表顺序、实际 8x8 图集格位并不
// 一致。四个左摇杆方向载体采用全局分配：掩体教程直接使用无附加文字的
// SCROLLLOCK / NUMLOCK 显示 LS_LEFT / LS_RIGHT；较少见且当前日志从未命中的
// LS_UP / LS_DOWN 迁移到 k1 / k4。这样不再依赖未被该教程调用的 TokenRenderer。
enum class CarrierTable
{
    Keyboard,
    Mouse
};

struct DistinctCarrierSpec
{
    CarrierTable table = CarrierTable::Keyboard;
    int tableIndex = -1;
    int cellX = -1;
    int cellY = -1;
    int secondaryCellX = -1;
    int secondaryCellY = -1;
};

constexpr std::array<DistinctCarrierSpec, kControllerIconCount> kDistinctCarrierSpecs = {{
    {CarrierTable::Keyboard,  27, 4, 7, -1, -1}, // A / RETURN；动态 A 与 QTE A 改走此载体
    {CarrierTable::Keyboard,  57, 1, 2, -1, -1}, // B / CAPSLOCK；动态 B 改走此载体
    {CarrierTable::Keyboard,  14, 3, 2, -1, -1}, // X / TAB
    {CarrierTable::Keyboard,  56, 0, 7, -1, -1}, // Y / SPACE；静态过场跳过
    {CarrierTable::Keyboard, 121, 0, 1, -1, -1}, // LB
    {CarrierTable::Keyboard, 126, 1, 1, -1, -1}, // RB
    {CarrierTable::Keyboard, 122, 2, 3, -1, -1}, // LT / ARROWLEFT；静态 Tips 左切换
    {CarrierTable::Keyboard, 123, 3, 3, -1, -1}, // RT / ARROWRIGHT；静态 Tips 右切换
    {CarrierTable::Keyboard, 120, 0, 3, -1, -1}, // D-pad Up
    {CarrierTable::Keyboard, 125, 1, 3, -1, -1}, // D-pad Down
    {CarrierTable::Keyboard, 127, 2, 1, -1, -1}, // D-pad Left / INSERT
    {CarrierTable::Keyboard, 128, 3, 1, -1, -1}, // D-pad Right / DELETE
    {CarrierTable::Keyboard,   1, 0, 5, -1, -1}, // Left stick Up / k1；当前日志未见该动态方向
    {CarrierTable::Keyboard,   4, 2, 6, -1, -1}, // Left stick Down / k4；当前日志未见该动态方向
    {CarrierTable::Keyboard,  69, 0, 2, -1, -1}, // Left stick Left / SCROLLLOCK；掩体教程固定方向
    {CarrierTable::Keyboard,  68, 2, 2, -1, -1}, // Left stick Right / NUMLOCK；掩体教程固定方向
    {CarrierTable::Keyboard,  41, 6, 7, -1, -1}, // Left stick Click / LSHIFT
    {CarrierTable::Keyboard,  28, 7, 7, -1, -1}, // Right stick Up / CTRL
    {CarrierTable::Mouse,      0, 4, 6, -1, -1}, // Right stick Down / MOUSELEFT
    {CarrierTable::Mouse,      1, 3, 6, -1, -1}, // Right stick All / MOUSERIGHT；初始瞄准教程
    {CarrierTable::Mouse,      2, 1, 5, -1, -1}, // Right stick Right / MOUSEMIDDLE
    {CarrierTable::Mouse,      5, 5, 6, -1, -1}, // Right stick Click / WHEELUP
    {CarrierTable::Keyboard,   0, 5, 7, -1, -1}, // Start / ESCAPE；静态过场暂停/继续
    {CarrierTable::Mouse,      6, 6, 6, -1, -1}, // Back / WHEELDOWN；保留 (3,7) 无限弹药
}};

// token core 直接从目标 EXE 的真实 token 字符串提取。这样既避免硬编码高位键名，
// 也能在安装 Hook 前验证表索引确实可读、格式确实为 ~~{...}~~。
std::array<std::array<wchar_t, 24>, kControllerIconCount> g_distinctCarrierCores = {};

struct PendingCarrierState
{
    int actionId = -1;
    DWORD lastIssuedTick = 0;
    int pendingCount = 0;
};

struct RenderEventContext
{
    ControllerIcon icon = ControllerIcon::Invalid;
    int actionId = -1;
    int sequenceIndex = -1;
    uintptr_t parentCaller = 0;
    int depth = 0;
    int drawCalls = 0;
    int drawOverrides = 0;
    int drawFailures = 0;
};

// 若 TokenRenderer 只生成/排队几何而不立即调用 D3D draw，则函数内上下文会在真正
// 提交前结束。这个提示把已确认事件短暂交给后续 draw；只有 draw 当时确实绑定了
// 已确认键盘图集，才允许替换，并设置很小的图集 draw 预算。
struct DeferredEventDrawHint
{
    ControllerIcon icon = ControllerIcon::Invalid;
    int actionId = -1;
    DWORD tick = 0;
    int remainingAtlasDraws = 0;
};

struct ThreadRuntimeState
{
    std::array<PendingCarrierState, kControllerIconCount> pendingCarrierStates = {};
    RenderEventContext renderEventContext = {};
    DeferredEventDrawHint deferredEventDrawHint = {};
};

// 使用 Win32 动态 TLS，而不是 C++ static thread_local。动态 TLS 不会生成 PE TLS
// Directory，因此 UPX 压缩后的 DLL 可以成功关闭 DLL_THREAD_ATTACH/DETACH 通知。
// 每个实际进入提示/渲染 Hook 的线程只分配一个很小的 POD 状态块；进程退出时由
// 系统整体回收。ASI 不支持运行中卸载，因此不在 loader lock 中遍历并释放其他线程状态。
ThreadRuntimeState g_emergencyThreadRuntimeState = {};

ThreadRuntimeState& CurrentThreadRuntimeState()
{
    if (g_threadRuntimeTlsIndex == TLS_OUT_OF_INDEXES)
        return g_emergencyThreadRuntimeState;

    auto* state = static_cast<ThreadRuntimeState*>(
        TlsGetValue(g_threadRuntimeTlsIndex));
    if (state)
        return *state;

    state = static_cast<ThreadRuntimeState*>(
        HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(ThreadRuntimeState)));
    if (!state || !TlsSetValue(g_threadRuntimeTlsIndex, state))
    {
        if (state)
            HeapFree(GetProcessHeap(), 0, state);
        return g_emergencyThreadRuntimeState;
    }
    return *state;
}

std::array<PendingCarrierState, kControllerIconCount>& CurrentPendingCarrierStates()
{
    return CurrentThreadRuntimeState().pendingCarrierStates;
}

RenderEventContext& CurrentRenderEventContext()
{
    return CurrentThreadRuntimeState().renderEventContext;
}

DeferredEventDrawHint& CurrentDeferredEventDrawHint()
{
    return CurrentThreadRuntimeState().deferredEventDrawHint;
}

struct SimilarityResult
{
    double score = 1.0e30;
    int alphaMode = -1;
    bool verticallyFlipped = false;
};


struct DeviceTextureCandidate
{
    IDirect3DTexture9* original = nullptr; // 游戏持有；插件不 AddRef，避免改变游戏资源所有权。
    // 每个图标家族保留独立缓存。Auto family 切换时不释放正在被 D3D9 或状态块引用的
    // 旧纹理，避免原键盘图集被恢复后短暂或持续显示键盘图标。
    std::array<IDirect3DTexture9*, kEmbeddedIconStyleCount> globalReplacements = {};
    std::array<std::array<IDirect3DTexture9*, kControllerIconCount>,
               kEmbeddedIconStyleCount> eventReplacements = {};
    std::vector<Pixel> sourcePixels;
    D3DSURFACE_DESC creationDesc = {};
    UINT requestedLevels = 0;
    DWORD requestedUsage = 0;
    int candidateIndex = 0;
    int inspectionAttempts = 0;
    uint32_t lastPixelCrc32 = 0;
    DWORD firstObservedTick = 0;
    DWORD lastInspectionTick = 0;
    bool hasFirstObservedTick = false;
    bool detectedVerticalFlip = false;
    bool atlasConfirmed = false;
    bool permanentlyRejected = false;
    DWORD lastObservedStage = 0;
    bool hasObservedStage = false;
};

// TokenRenderer 进入前主动切换图集，并在原函数返回后恢复。若引擎内部通过
// StateBlock 或绕过 Hook 的状态管理器恢复了原图集，DrawPrimitive 系列 Hook 会在
// 真正提交绘制前再次临时绑定替换纹理，绘制后立即恢复当时的状态。
struct EventTextureOverrideState
{
    IDirect3DDevice9* device = nullptr;
    IDirect3DBaseTexture9* previousTexture = nullptr;
    IDirect3DTexture9* replacementTexture = nullptr;
    DWORD stage = 0;
    bool active = false;
};


enum class AxisPolarity : int
{
    Negative = -1,
    Unknown = 0,
    Positive = 1
};

struct BindingVisit
{
    const void* node = nullptr;
    AxisPolarity polarity = AxisPolarity::Unknown;
};

struct BindingResolution
{
    void* manager = nullptr;
    void* pad = nullptr;
    void* root = nullptr;
    uintptr_t rootVtable = 0;
    uint16_t buttonMask = 0;
    uint8_t positiveAxisMask = 0;
    uint8_t negativeAxisMask = 0;
    uint8_t unknownAxisMask = 0;
    int visitedNodes = 0;
    int padLeaves = 0;
    int unknownNodes = 0;
    bool truncated = false;
};

constexpr int kMaxBindingDepth = 20;
constexpr int kMaxBindingVisits = 96;
std::array<uintptr_t, 256> g_lastLoggedBindingRoot = {};
std::array<int, 256> g_lastLoggedBindingResult = {};
std::array<uint8_t, 256> g_lastLoggedBindingSeen = {};

std::vector<DeviceTextureCandidate> g_deviceTextureCandidates;

void Log(const wchar_t* format, ...);
bool InstallEarlyDirect3DCreate9Hook();
bool InstallD3D9FactoryHook();
bool HookD3D9Object(IDirect3D9* object);
bool HookD3D9Device(IDirect3DDevice9* device);
bool FunctionHeadMatches(uintptr_t rva, const BYTE* expected, size_t length);
bool BeginEventTextureOverride(ControllerIcon icon, EventTextureOverrideState* state, bool allowNullTexture);
bool EndEventTextureOverride(EventTextureOverrideState* state);
bool IsConfirmedEventAtlasCurrentlyBound();
bool TickIntervalElapsed(DWORD now, DWORD earlier, DWORD interval);
IDirect3DTexture9* EnsureGlobalReplacementLocked(IDirect3DDevice9* device,
                                                  DeviceTextureCandidate& candidate);
bool IsCandidateGlobalReplacementLocked(const DeviceTextureCandidate& candidate,
                                        IDirect3DBaseTexture9* texture);
bool IsCandidateEventReplacementLocked(const DeviceTextureCandidate& candidate,
                                       IDirect3DBaseTexture9* texture);

void InitializePaths()
{
    wchar_t modulePath[MAX_PATH] = L"";
    GetModuleFileNameW(g_selfModule, modulePath, MAX_PATH);
    wcscpy_s(g_moduleDirectory, std::size(g_moduleDirectory), modulePath);

    wchar_t* lastSlash = wcsrchr(g_moduleDirectory, L'\\');
    if (lastSlash)
        *(lastSlash + 1) = L'\0';

    swprintf_s(g_iniPath, std::size(g_iniPath), L"%s%s.ini", g_moduleDirectory, kPluginName);
    swprintf_s(g_logPath, std::size(g_logPath), L"%s%s.log", g_moduleDirectory, kPluginName);

    const DWORD attributes = GetFileAttributesW(g_iniPath);
#ifdef BSCN_SINGLE_DLL
    // 专版不打开或生成任何配置文件，保持原始默认行为。
    g_iniPresent = false;
#else
    g_iniPresent = attributes != INVALID_FILE_ATTRIBUTES &&
                   (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
#endif
}

void OpenLog()
{
    // 允许日志查看器、编辑器和上传工具在游戏运行期间读取、复制、重命名日志。
    g_logFile = CreateFileW(g_logPath,
                            FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr,
                            OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            nullptr);
}

void CloseLog()
{
    EnterCriticalSection(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        FlushFileBuffers(g_logFile);
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&g_logLock);
}

void LogV(const wchar_t* format, va_list arguments)
{
    wchar_t message[4096] = L"";
    _vsnwprintf_s(message, std::size(message), _TRUNCATE, format, arguments);

    SYSTEMTIME time = {};
    GetLocalTime(&time);
    wchar_t line[4352] = L"";
    _snwprintf_s(line,
                 std::size(line),
                 _TRUNCATE,
                 L"[%04u-%02u-%02u %02u:%02u:%02u] %ls\r\n",
                 time.wYear,
                 time.wMonth,
                 time.wDay,
                 time.wHour,
                 time.wMinute,
                 time.wSecond,
                 message);

#ifdef BSCN_SINGLE_DLL
    bondone::WriteLog(message);
    return;
#endif
    EnterCriticalSection(&g_logLock);
    if (g_logFile != INVALID_HANDLE_VALUE)
    {
        const int required = WideCharToMultiByte(CP_UTF8, 0, line, -1, nullptr, 0, nullptr, nullptr);
        if (required > 1)
        {
            std::vector<char> utf8(static_cast<size_t>(required));
            const int converted = WideCharToMultiByte(CP_UTF8,
                                                      0,
                                                      line,
                                                      -1,
                                                      utf8.data(),
                                                      required,
                                                      nullptr,
                                                      nullptr);
            if (converted > 1)
            {
                DWORD written = 0;
                WriteFile(g_logFile,
                          utf8.data(),
                          static_cast<DWORD>(converted - 1),
                          &written,
                          nullptr);
                FlushFileBuffers(g_logFile);
            }
        }
    }
    LeaveCriticalSection(&g_logLock);
}

void Log(const wchar_t* format, ...)
{
    va_list arguments;
    va_start(arguments, format);
    LogV(format, arguments);
    va_end(arguments);
}

int ReadIniInt(const wchar_t* section, const wchar_t* key, int defaultValue)
{
#ifdef BSCN_SINGLE_DLL
    (void)section; (void)key;
    return defaultValue;
#else
    return GetPrivateProfileIntW(section, key, defaultValue, g_iniPath);
#endif
}

std::wstring ReadIniString(const wchar_t* section,
                           const wchar_t* key,
                           const wchar_t* defaultValue)
{
#ifdef BSCN_SINGLE_DLL
    (void)section; (void)key;
    return defaultValue ? defaultValue : L"";
#else

    wchar_t value[128] = L"";
    GetPrivateProfileStringW(section,
                             key,
                             defaultValue,
                             value,
                             static_cast<DWORD>(std::size(value)),
                             g_iniPath);
    return value;
#endif
}

void TrimWhitespace(std::wstring* value)
{
    if (!value)
        return;
    while (!value->empty() && std::iswspace(value->front()))
        value->erase(value->begin());
    while (!value->empty() && std::iswspace(value->back()))
        value->pop_back();
}

IconStyleMode ParseIconStyleMode(const std::wstring& requested, bool* valid)
{
    if (valid)
        *valid = true;

    if (_wcsicmp(requested.c_str(), L"Auto") == 0 ||
        _wcsicmp(requested.c_str(), L"Automatic") == 0)
    {
        return IconStyleMode::Auto;
    }
    if (_wcsicmp(requested.c_str(), L"Xbox") == 0 ||
        _wcsicmp(requested.c_str(), L"XInput") == 0 ||
        requested == L"0")
    {
        return IconStyleMode::Xbox;
    }
    if (_wcsicmp(requested.c_str(), L"PS5") == 0 ||
        _wcsicmp(requested.c_str(), L"PS") == 0 ||
        _wcsicmp(requested.c_str(), L"PlayStation") == 0 ||
        _wcsicmp(requested.c_str(), L"DualSense") == 0 ||
        requested == L"1")
    {
        return IconStyleMode::PlayStation;
    }
    if (_wcsicmp(requested.c_str(), L"Switch") == 0 ||
        _wcsicmp(requested.c_str(), L"Nintendo") == 0 ||
        _wcsicmp(requested.c_str(), L"NS") == 0 ||
        requested == L"2")
    {
        return IconStyleMode::Nintendo;
    }

    if (valid)
        *valid = false;
    return IconStyleMode::Xbox;
}

int IconStyleIndexForMode(IconStyleMode mode)
{
    switch (mode)
    {
    case IconStyleMode::PlayStation:
        return 1;
    case IconStyleMode::Nintendo:
        return 2;
    case IconStyleMode::Xbox:
    case IconStyleMode::Auto:
    default:
        return 0;
    }
}

int CurrentIconStyleIndex()
{
    return std::clamp(
        static_cast<int>(InterlockedCompareExchange(&g_iconStyleIndex, 0, 0)),
        0,
        kEmbeddedIconStyleCount - 1);
}

IconStyleMode CurrentIconStyleMode()
{
    const LONG value = InterlockedCompareExchange(&g_iconStyleMode, 0, 0);
    if (value < static_cast<LONG>(IconStyleMode::Auto) ||
        value > static_cast<LONG>(IconStyleMode::Nintendo))
    {
        return IconStyleMode::Xbox;
    }
    return static_cast<IconStyleMode>(value);
}

const wchar_t* IconStyleModeName(IconStyleMode mode)
{
    switch (mode)
    {
    case IconStyleMode::Auto:
        return L"Auto";
    case IconStyleMode::PlayStation:
        return L"PlayStation";
    case IconStyleMode::Nintendo:
        return L"Nintendo";
    case IconStyleMode::Xbox:
    default:
        return L"Xbox";
    }
}

const wchar_t* IconStyleNameFromIndex(int index)
{
    switch (index)
    {
    case 1:
        return L"PS5";
    case 2:
        return L"Switch";
    default:
        return L"Xbox";
    }
}

const wchar_t* SelectedIconStyleName()
{
    return IconStyleNameFromIndex(CurrentIconStyleIndex());
}

struct CommandLineStyleSelection
{
    bool present = false;
    IconStyleMode mode = IconStyleMode::Auto;
    std::wstring argument;
};

CommandLineStyleSelection ParseCommandLineStyleSelection()
{
    CommandLineStyleSelection selection = {};
    int argumentCount = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
        return selection;

    for (int index = 1; index < argumentCount; ++index)
    {
        const wchar_t* argument = arguments[index];
        IconStyleMode mode = IconStyleMode::Auto;
        bool matched = true;
        if (_wcsicmp(argument, L"-ps") == 0)
            mode = IconStyleMode::PlayStation;
        else if (_wcsicmp(argument, L"-ns") == 0)
            mode = IconStyleMode::Nintendo;
        else if (_wcsicmp(argument, L"-xb") == 0)
            mode = IconStyleMode::Xbox;
        else if (_wcsicmp(argument, L"-auto") == 0)
            mode = IconStyleMode::Auto;
        else
            matched = false;

        if (matched)
        {
            // 与常见命令行工具一致：多个有效参数同时存在时，最后一个覆盖前面的值。
            selection.present = true;
            selection.mode = mode;
            selection.argument = argument;
        }
    }

    LocalFree(arguments);
    return selection;
}

constexpr int kSdlGamepadTypeUnknown = 0;
constexpr int kSdlGamepadTypeStandard = 1;
constexpr int kSdlGamepadTypeXbox360 = 2;
constexpr int kSdlGamepadTypeXboxOne = 3;
constexpr int kSdlGamepadTypePs3 = 4;
constexpr int kSdlGamepadTypePs4 = 5;
constexpr int kSdlGamepadTypePs5 = 6;
constexpr int kSdlGamepadTypeSwitchPro = 7;
constexpr int kSdlGamepadTypeJoyConLeft = 8;
constexpr int kSdlGamepadTypeJoyConRight = 9;
constexpr int kSdlGamepadTypeJoyConPair = 10;
constexpr int kSdlGamepadTypeGameCube = 11;
constexpr int kSdlGamepadTypeSteam = 12;

constexpr int kSdlButtonSouth = 0;
constexpr int kSdlButtonEast = 1;
constexpr int kSdlButtonWest = 2;
constexpr int kSdlButtonNorth = 3;

constexpr int kSdlButtonLabelUnknown = 0;
constexpr int kSdlButtonLabelA = 1;
constexpr int kSdlButtonLabelB = 2;
constexpr int kSdlButtonLabelX = 3;
constexpr int kSdlButtonLabelY = 4;
constexpr int kSdlButtonLabelCross = 5;
constexpr int kSdlButtonLabelCircle = 6;
constexpr int kSdlButtonLabelSquare = 7;
constexpr int kSdlButtonLabelTriangle = 8;

struct ResolvedControllerFamily
{
    ControllerPromptFamily family = ControllerPromptFamily::Xbox;
    FamilyResolutionSource source = FamilyResolutionSource::DefaultXbox;
    uint32_t deviceId = 0;
    int effectiveType = kSdlGamepadTypeUnknown;
    int realType = kSdlGamepadTypeUnknown;
    uint16_t vendor = 0;
    char effectiveTypeString[64] = {};
    char realTypeString[64] = {};
    char deviceName[256] = {};
};

// WorkerThread 的低频 family 观察循环只写该快照；D3D9 Hook 只读。SRWLOCK 为静态初始化对象，
// 不需要在 DllMain 中创建/销毁，也不会与新版 JoyShim 的 IAT 扫描或 SDL 输入线程
// 共享任何插件内部锁。
ResolvedControllerFamily g_publishedAutoFamily = {};
ResolvedControllerFamily g_pendingAutoFamily = {};
int g_pendingAutoFamilyPolls = 0;
bool g_hasStableAutoFamily = false;

FARPROC ResolveModuleExportReadOnly(HMODULE module, const char* name)
{
    if (!module || !name || !name[0])
        return nullptr;

    // JoyShim 会周期性扫描游戏目录模块并改写 GetProcAddress IAT。为避免本 ASI
    // 被改写后再通过 JoyShim 回调进入 SDL 查询链，这里完全不导入 GetProcAddress，
    // 只读解析目标模块的标准 PE 导出目录。SDL3 的这些 API 是直接命名导出，
    // 不需要处理序号或转发导出。所有边界都以 SizeOfImage 验证。
    __try
    {
        const auto* base = reinterpret_cast<const uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return nullptr;

        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return nullptr;
        const size_t imageSize = static_cast<size_t>(nt->OptionalHeader.SizeOfImage);
        const auto& directory =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_EXPORT_DIRECTORY) ||
            static_cast<size_t>(directory.VirtualAddress) + directory.Size > imageSize)
        {
            return nullptr;
        }

        const auto* exports = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(
            base + directory.VirtualAddress);
        if (exports->NumberOfNames > imageSize / sizeof(DWORD) ||
            exports->NumberOfNames > imageSize / sizeof(WORD) ||
            exports->NumberOfFunctions > imageSize / sizeof(DWORD))
        {
            return nullptr;
        }
        const size_t namesBytes = static_cast<size_t>(exports->NumberOfNames) * sizeof(DWORD);
        const size_t ordinalsBytes = static_cast<size_t>(exports->NumberOfNames) * sizeof(WORD);
        const size_t functionsBytes = static_cast<size_t>(exports->NumberOfFunctions) * sizeof(DWORD);
        if (static_cast<size_t>(exports->AddressOfNames) + namesBytes > imageSize ||
            static_cast<size_t>(exports->AddressOfNameOrdinals) + ordinalsBytes > imageSize ||
            static_cast<size_t>(exports->AddressOfFunctions) + functionsBytes > imageSize)
        {
            return nullptr;
        }

        const auto* names = reinterpret_cast<const DWORD*>(base + exports->AddressOfNames);
        const auto* ordinals = reinterpret_cast<const WORD*>(base + exports->AddressOfNameOrdinals);
        const auto* functions = reinterpret_cast<const DWORD*>(base + exports->AddressOfFunctions);
        for (DWORD index = 0; index < exports->NumberOfNames; ++index)
        {
            const DWORD nameRva = names[index];
            if (nameRva >= imageSize)
                continue;
            const char* exportedName = reinterpret_cast<const char*>(base + nameRva);
            const size_t maximum = imageSize - nameRva;
            if (!memchr(exportedName, '\0', maximum) || strcmp(exportedName, name) != 0)
                continue;

            const WORD ordinal = ordinals[index];
            if (ordinal >= exports->NumberOfFunctions)
                return nullptr;
            const DWORD functionRva = functions[ordinal];
            if (functionRva == 0 || functionRva >= imageSize)
                return nullptr;

            // 位于导出目录内的是 forwarder 字符串。SDL3 当前所需接口均为直接导出；
            // 遇到 forwarder 时宁可禁用 Auto 并回退 Xbox，也不调用可被接管的解析器。
            const DWORD exportBegin = directory.VirtualAddress;
            const DWORD exportEnd = directory.VirtualAddress + directory.Size;
            if (functionRva >= exportBegin && functionRva < exportEnd)
                return nullptr;
            return reinterpret_cast<FARPROC>(const_cast<uint8_t*>(base) + functionRva);
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
    return nullptr;
}

template <typename T>
T ResolveSdlProcedure(HMODULE module, const char* name)
{
    return reinterpret_cast<T>(ResolveModuleExportReadOnly(module, name));
}

void ResetSdlReadOnlyApi()
{
    g_sdlApi = {};
    g_sdlModule = nullptr;
    g_sdlMirrorSlotIds.fill(0);
    g_sdlModuleFirstSeenTick = 0;
    g_hasSdlModuleFirstSeenTick = false;
}

bool ResolveSdlReadOnlyApi()
{
    HMODULE module = GetModuleHandleW(L"SDL3.dll");
    if (!module)
    {
        if (g_sdlModule || g_hasSdlModuleFirstSeenTick)
            ResetSdlReadOnlyApi();
        return false;
    }

    const DWORD now = GetTickCount();
    if (g_sdlModule != module)
    {
        // 新版 JoyShim 先 LoadLibrary(SDL3.dll)，随后才在自己的输入线程设置 Hint、
        // 初始化 Gamepad 子系统并打开设备。仅凭模块已加载不能证明运行时已经可查询。
        // 记录模块首次出现时间，并在稳定窗口结束前完全不调用任何 SDL API。
        g_sdlApi = {};
        g_sdlModule = module;
        g_sdlMirrorSlotIds.fill(0);
        g_sdlModuleFirstSeenTick = now;
        g_hasSdlModuleFirstSeenTick = true;
        return false;
    }

    if (!g_hasSdlModuleFirstSeenTick)
    {
        g_sdlModuleFirstSeenTick = now;
        g_hasSdlModuleFirstSeenTick = true;
        return false;
    }
    if (!TickIntervalElapsed(now,
                             g_sdlModuleFirstSeenTick,
                             kSdlModuleStabilizationMs))
    {
        return false;
    }

    if (g_sdlApi.module == module &&
        g_sdlApi.getGamepads &&
        g_sdlApi.freeMemory)
    {
        return true;
    }

    SdlReadOnlyApi api = {};
    api.module = module;
    api.getGamepads = ResolveSdlProcedure<SdlGetGamepadsFn>(module, "SDL_GetGamepads");
    api.freeMemory = ResolveSdlProcedure<SdlFreeFn>(module, "SDL_free");
    api.getMappedTypeForId = ResolveSdlProcedure<SdlGetGamepadTypeForIdFn>(
        module, "SDL_GetGamepadTypeForID");
    api.getRealTypeForId = ResolveSdlProcedure<SdlGetRealGamepadTypeForIdFn>(
        module, "SDL_GetRealGamepadTypeForID");
    api.getTypeString = ResolveSdlProcedure<SdlGetGamepadStringForTypeFn>(
        module, "SDL_GetGamepadStringForType");
    api.getButtonLabelForType = ResolveSdlProcedure<SdlGetGamepadButtonLabelForTypeFn>(
        module, "SDL_GetGamepadButtonLabelForType");
    api.getNameForId = ResolveSdlProcedure<SdlGetGamepadNameForIdFn>(
        module, "SDL_GetGamepadNameForID");
    api.getVendorForId = ResolveSdlProcedure<SdlGetGamepadVendorForIdFn>(
        module, "SDL_GetGamepadVendorForID");

    if (!api.getGamepads || !api.freeMemory)
    {
        // 不是可用的 SDL3 Gamepad 运行时。绝不主动 LoadLibrary 或初始化 SDL。
        // 保留模块首次出现时间，后续仍可重试，避免 JoyShim 初始化期间导出尚未可用
        // 时把同一模块永久排除。
        g_sdlApi = {};
        return false;
    }

    g_sdlApi = api;
    return true;
}

void CopyAsciiText(char* destination, size_t destinationCount, const char* source)
{
    if (!destination || destinationCount == 0)
        return;
    destination[0] = '\0';
    if (!source)
        return;
    strncpy_s(destination, destinationCount, source, _TRUNCATE);
}

std::string NormalizeFamilyToken(const char* text)
{
    std::string normalized;
    if (!text)
        return normalized;
    while (*text)
    {
        const unsigned char character = static_cast<unsigned char>(*text++);
        if (std::isalnum(character))
            normalized.push_back(static_cast<char>(std::tolower(character)));
    }
    return normalized;
}

bool StartsWithToken(const std::string& value, const char* prefix)
{
    if (!prefix)
        return false;
    const size_t prefixLength = strlen(prefix);
    return value.size() >= prefixLength &&
           value.compare(0, prefixLength, prefix) == 0;
}

bool ContainsToken(const std::string& value, const char* token)
{
    return token && value.find(token) != std::string::npos;
}

ControllerPromptFamily FamilyFromKnownSdlType(int type)
{
    switch (type)
    {
    case kSdlGamepadTypeXbox360:
    case kSdlGamepadTypeXboxOne:
        return ControllerPromptFamily::Xbox;

    case kSdlGamepadTypePs3:
    case kSdlGamepadTypePs4:
    case kSdlGamepadTypePs5:
        return ControllerPromptFamily::PlayStation;

    case kSdlGamepadTypeSwitchPro:
    case kSdlGamepadTypeJoyConLeft:
    case kSdlGamepadTypeJoyConRight:
    case kSdlGamepadTypeJoyConPair:
    case kSdlGamepadTypeGameCube:
        return ControllerPromptFamily::Nintendo;

    case kSdlGamepadTypeUnknown:
    case kSdlGamepadTypeStandard:
    case kSdlGamepadTypeSteam:
    default:
        return ControllerPromptFamily::Unknown;
    }
}

ControllerPromptFamily FamilyFromTypeString(const char* typeString)
{
    const std::string token = NormalizeFamilyToken(typeString);
    if (token.empty())
        return ControllerPromptFamily::Unknown;

    if (StartsWithToken(token, "playstation") ||
        StartsWithToken(token, "dualshock") ||
        StartsWithToken(token, "dualsense") ||
        (StartsWithToken(token, "ps") && token.size() >= 3 &&
         std::isdigit(static_cast<unsigned char>(token[2]))))
    {
        return ControllerPromptFamily::PlayStation;
    }

    if (StartsWithToken(token, "xbox") ||
        StartsWithToken(token, "xinput"))
    {
        return ControllerPromptFamily::Xbox;
    }

    if (StartsWithToken(token, "nintendo") ||
        StartsWithToken(token, "switch") ||
        StartsWithToken(token, "joycon") ||
        StartsWithToken(token, "gamecube") ||
        StartsWithToken(token, "ns2") ||
        StartsWithToken(token, "nspro"))
    {
        return ControllerPromptFamily::Nintendo;
    }

    return ControllerPromptFamily::Unknown;
}

ControllerPromptFamily FamilyFromDeviceName(const char* name)
{
    const std::string token = NormalizeFamilyToken(name);
    if (token.empty())
        return ControllerPromptFamily::Unknown;

    if (ContainsToken(token, "playstation") ||
        ContainsToken(token, "dualshock") ||
        ContainsToken(token, "dualsense") ||
        ContainsToken(token, "sonyinteractive") ||
        ContainsToken(token, "ps5controller") ||
        ContainsToken(token, "ps6controller"))
    {
        return ControllerPromptFamily::PlayStation;
    }

    if (ContainsToken(token, "xbox") ||
        ContainsToken(token, "xinput"))
    {
        return ControllerPromptFamily::Xbox;
    }

    if (ContainsToken(token, "nintendo") ||
        ContainsToken(token, "switch") ||
        ContainsToken(token, "joycon") ||
        ContainsToken(token, "gamecube") ||
        ContainsToken(token, "ns2pro"))
    {
        return ControllerPromptFamily::Nintendo;
    }

    return ControllerPromptFamily::Unknown;
}

ControllerPromptFamily FamilyFromFaceButtonLabels(int type)
{
    if (!g_sdlApi.getButtonLabelForType)
        return ControllerPromptFamily::Unknown;

    const int south = g_sdlApi.getButtonLabelForType(type, kSdlButtonSouth);
    const int east = g_sdlApi.getButtonLabelForType(type, kSdlButtonEast);
    const int west = g_sdlApi.getButtonLabelForType(type, kSdlButtonWest);
    const int north = g_sdlApi.getButtonLabelForType(type, kSdlButtonNorth);

    if (south == kSdlButtonLabelCross &&
        east == kSdlButtonLabelCircle &&
        west == kSdlButtonLabelSquare &&
        north == kSdlButtonLabelTriangle)
    {
        return ControllerPromptFamily::PlayStation;
    }

    if (south == kSdlButtonLabelB &&
        east == kSdlButtonLabelA &&
        west == kSdlButtonLabelY &&
        north == kSdlButtonLabelX)
    {
        return ControllerPromptFamily::Nintendo;
    }

    if (south == kSdlButtonLabelA &&
        east == kSdlButtonLabelB &&
        west == kSdlButtonLabelX &&
        north == kSdlButtonLabelY)
    {
        return ControllerPromptFamily::Xbox;
    }

    if (south == kSdlButtonLabelUnknown &&
        east == kSdlButtonLabelUnknown &&
        west == kSdlButtonLabelUnknown &&
        north == kSdlButtonLabelUnknown)
    {
        return ControllerPromptFamily::Unknown;
    }

    return ControllerPromptFamily::Unknown;
}

ControllerPromptFamily FamilyFromVendor(uint16_t vendor)
{
    switch (vendor)
    {
    case 0x054C: // Sony
        return ControllerPromptFamily::PlayStation;
    case 0x057E: // Nintendo
        return ControllerPromptFamily::Nintendo;
    case 0x045E: // Microsoft
        return ControllerPromptFamily::Xbox;
    default:
        return ControllerPromptFamily::Unknown;
    }
}

const wchar_t* ControllerPromptFamilyName(ControllerPromptFamily family)
{
    switch (family)
    {
    case ControllerPromptFamily::PlayStation:
        return L"PlayStation";
    case ControllerPromptFamily::Nintendo:
        return L"Nintendo";
    case ControllerPromptFamily::Xbox:
        return L"Xbox";
    case ControllerPromptFamily::Unknown:
    default:
        return L"Unknown";
    }
}

const wchar_t* FamilyResolutionSourceName(FamilyResolutionSource source)
{
    switch (source)
    {
    case FamilyResolutionSource::NoSdl:
        return L"NoSDL";
    case FamilyResolutionSource::NoDevice:
        return L"NoDevice";
    case FamilyResolutionSource::EffectiveType:
        return L"EffectiveType";
    case FamilyResolutionSource::EffectiveTypeString:
        return L"EffectiveTypeString";
    case FamilyResolutionSource::FaceButtonLabels:
        return L"FaceButtonLabels";
    case FamilyResolutionSource::RealType:
        return L"RealType";
    case FamilyResolutionSource::RealTypeString:
        return L"RealTypeString";
    case FamilyResolutionSource::DeviceName:
        return L"DeviceName";
    case FamilyResolutionSource::VendorId:
        return L"VendorId";
    case FamilyResolutionSource::DefaultXbox:
    default:
        return L"DefaultXbox";
    }
}

int IconStyleIndexForFamily(ControllerPromptFamily family)
{
    switch (family)
    {
    case ControllerPromptFamily::PlayStation:
        return 1;
    case ControllerPromptFamily::Nintendo:
        return 2;
    case ControllerPromptFamily::Xbox:
    case ControllerPromptFamily::Unknown:
    default:
        return 0;
    }
}

ResolvedControllerFamily ResolveAutomaticControllerFamily()
{
#ifdef BSCN_SINGLE_DLL
    ResolvedControllerFamily result = {};
    result.family = ControllerPromptFamily::Xbox;
    result.source = FamilyResolutionSource::NoDevice;
    char name[256]{};
    char typeName[80]{};
    int sdlType = 0;
    unsigned short vendor = 0;
    unsigned int deviceId = 0;
    if (!BondOneReadControllerInfo(0, &sdlType, typeName, sizeof(typeName),
                                   name, sizeof(name), &vendor, &deviceId))
        return result;
    result.deviceId = deviceId;
    result.effectiveType = sdlType;
    result.vendor = vendor;
    strncpy_s(result.effectiveTypeString, typeName, _TRUNCATE);
    strncpy_s(result.deviceName, name, _TRUNCATE);
    // SDL 有效映射类型优先；未来类型字符串、设备名称和 VID 只作回退。
    result.family = FamilyFromKnownSdlType(sdlType);
    result.source = FamilyResolutionSource::EffectiveType;
    if (result.family != ControllerPromptFamily::Unknown) return result;
    result.family = FamilyFromTypeString(typeName);
    result.source = FamilyResolutionSource::EffectiveTypeString;
    if (result.family != ControllerPromptFamily::Unknown) return result;
    result.family = FamilyFromDeviceName(name);
    result.source = FamilyResolutionSource::DeviceName;
    if (result.family != ControllerPromptFamily::Unknown) return result;
    result.family = FamilyFromVendor(vendor);
    result.source = FamilyResolutionSource::VendorId;
    if (result.family != ControllerPromptFamily::Unknown) return result;
    result.family = ControllerPromptFamily::Xbox;
    result.source = FamilyResolutionSource::DefaultXbox;
    return result;
#else
    ResolvedControllerFamily result = {};
    if (!ResolveSdlReadOnlyApi())
    {
        result.family = ControllerPromptFamily::Xbox;
        result.source = FamilyResolutionSource::NoSdl;
        return result;
    }

    // 这些只读查询固定在 WorkerThread 的低频观察循环执行。不要取得 SDL 全局 joystick 锁：
    // JoyShim 的输入线程正在 SDL_UpdateGamepads/热插拔重整，外部长期持锁会扩大
    // 两个插件之间的锁顺序和停顿风险。设备若在两次查询间消失，SDL 会返回
    // Unknown/0；下方稳定发布层会忽略这种瞬态。

    int count = 0;
    SdlJoystickId* ids = g_sdlApi.getGamepads(&count);
    if (!ids || count <= 0)
    {
        if (ids)
            g_sdlApi.freeMemory(ids);
        g_sdlMirrorSlotIds.fill(0);
        result.family = ControllerPromptFamily::Xbox;
        result.source = FamilyResolutionSource::NoDevice;
        return result;
    }

    const int boundedCount = std::clamp(count, 0, 64);

    // 精确镜像 SimpleS2input 的 4 槽分配规则：仍在线设备保留原槽位，
    // 新设备按 SDL 枚举顺序进入最低空槽。Bond.exe 使用的主提示图标跟随 XInput 槽 0，
    // 而不是简单跟随“当前枚举列表第一个设备”，从而正确处理多手柄和槽 0 热拔插。
    const auto isConnected = [&](uint32_t deviceId) {
        if (deviceId == 0)
            return false;
        for (int index = 0; index < boundedCount; ++index)
        {
            if (ids[index] == deviceId)
                return true;
        }
        return false;
    };
    const auto isAlreadyAssigned = [&](uint32_t deviceId) {
        return std::find(g_sdlMirrorSlotIds.begin(),
                         g_sdlMirrorSlotIds.end(),
                         deviceId) != g_sdlMirrorSlotIds.end();
    };

    for (uint32_t& slotId : g_sdlMirrorSlotIds)
    {
        if (slotId != 0 && !isConnected(slotId))
            slotId = 0;
    }
    for (int index = 0; index < boundedCount; ++index)
    {
        const uint32_t deviceId = ids[index];
        if (deviceId == 0 || isAlreadyAssigned(deviceId))
            continue;
        auto emptySlot = std::find(g_sdlMirrorSlotIds.begin(),
                                   g_sdlMirrorSlotIds.end(),
                                   0u);
        if (emptySlot == g_sdlMirrorSlotIds.end())
            break;
        *emptySlot = deviceId;
    }

    result.deviceId = g_sdlMirrorSlotIds[0];
    g_sdlApi.freeMemory(ids);

    if (result.deviceId == 0)
    {
        result.family = ControllerPromptFamily::Xbox;
        result.source = FamilyResolutionSource::NoDevice;
        return result;
    }

    if (g_sdlApi.getMappedTypeForId)
        result.effectiveType = g_sdlApi.getMappedTypeForId(result.deviceId);
    if (g_sdlApi.getRealTypeForId)
        result.realType = g_sdlApi.getRealTypeForId(result.deviceId);
    if (g_sdlApi.getVendorForId)
        result.vendor = g_sdlApi.getVendorForId(result.deviceId);
    if (g_sdlApi.getNameForId)
        CopyAsciiText(result.deviceName,
                      std::size(result.deviceName),
                      g_sdlApi.getNameForId(result.deviceId));
    if (g_sdlApi.getTypeString)
    {
        CopyAsciiText(result.effectiveTypeString,
                      std::size(result.effectiveTypeString),
                      g_sdlApi.getTypeString(result.effectiveType));
        CopyAsciiText(result.realTypeString,
                      std::size(result.realTypeString),
                      g_sdlApi.getTypeString(result.realType));
    }

    ControllerPromptFamily family = FamilyFromKnownSdlType(result.effectiveType);
    if (family != ControllerPromptFamily::Unknown)
    {
        result.family = family;
        result.source = FamilyResolutionSource::EffectiveType;
        return result;
    }

    family = FamilyFromTypeString(result.effectiveTypeString);
    if (family != ControllerPromptFamily::Unknown)
    {
        result.family = family;
        result.source = FamilyResolutionSource::EffectiveTypeString;
        return result;
    }

    family = FamilyFromFaceButtonLabels(result.effectiveType);
    if (family != ControllerPromptFamily::Unknown)
    {
        result.family = family;
        result.source = FamilyResolutionSource::FaceButtonLabels;
        return result;
    }

    family = FamilyFromKnownSdlType(result.realType);
    if (family != ControllerPromptFamily::Unknown)
    {
        result.family = family;
        result.source = FamilyResolutionSource::RealType;
        return result;
    }

    family = FamilyFromTypeString(result.realTypeString);
    if (family != ControllerPromptFamily::Unknown)
    {
        result.family = family;
        result.source = FamilyResolutionSource::RealTypeString;
        return result;
    }

    family = FamilyFromDeviceName(result.deviceName);
    if (family != ControllerPromptFamily::Unknown)
    {
        result.family = family;
        result.source = FamilyResolutionSource::DeviceName;
        return result;
    }

    family = FamilyFromVendor(result.vendor);
    if (family != ControllerPromptFamily::Unknown)
    {
        result.family = family;
        result.source = FamilyResolutionSource::VendorId;
        return result;
    }

    result.family = ControllerPromptFamily::Xbox;
    result.source = FamilyResolutionSource::DefaultXbox;
    return result;
#endif
}

void PublishAutomaticControllerFamily(const ResolvedControllerFamily& resolved)
{
    AcquireSRWLockExclusive(&g_autoFamilySnapshotLock);
    g_publishedAutoFamily = resolved;
    ReleaseSRWLockExclusive(&g_autoFamilySnapshotLock);
}

ResolvedControllerFamily ReadPublishedAutomaticControllerFamily()
{
    ResolvedControllerFamily resolved = {};
    AcquireSRWLockShared(&g_autoFamilySnapshotLock);
    resolved = g_publishedAutoFamily;
    ReleaseSRWLockShared(&g_autoFamilySnapshotLock);
    return resolved;
}

bool SameResolvedFamilyCandidate(const ResolvedControllerFamily& left,
                                 const ResolvedControllerFamily& right)
{
    return left.family == right.family && left.deviceId == right.deviceId;
}

bool IsTransientAutomaticFamilyResult(const ResolvedControllerFamily& resolved)
{
    return resolved.source == FamilyResolutionSource::NoSdl ||
           resolved.source == FamilyResolutionSource::NoDevice;
}

void ConsiderAutomaticControllerFamily(const ResolvedControllerFamily& resolved)
{
    // 在 SDL 初始加载或 JoyShim 热插拔重扫期间，GetGamepads 可能短暂返回空。
    // 已有稳定结果后不把这种瞬态发布为 Xbox，避免图集在 Xbox/PS/NS 之间抖动。
    if (IsTransientAutomaticFamilyResult(resolved) && g_hasStableAutoFamily)
        return;

    if (!SameResolvedFamilyCandidate(g_pendingAutoFamily, resolved))
    {
        g_pendingAutoFamily = resolved;
        g_pendingAutoFamilyPolls = 1;
        return;
    }

    if (g_pendingAutoFamilyPolls < kSdlFamilyStablePollCount)
        ++g_pendingAutoFamilyPolls;
    if (g_pendingAutoFamilyPolls < kSdlFamilyStablePollCount)
        return;

    const ResolvedControllerFamily current = ReadPublishedAutomaticControllerFamily();
    if (!SameResolvedFamilyCandidate(current, resolved) || current.source != resolved.source)
        PublishAutomaticControllerFamily(resolved);
    if (!IsTransientAutomaticFamilyResult(resolved))
        g_hasStableAutoFamily = true;
}

void RunAutomaticFamilyObserverLoop()
{
    // 图标工作线程负责周期性读取 JoyShim 发布的设备家族快照。
    // 读取时不打开、关闭或重新枚举物理手柄，也不持有 SDL 内部锁。
    ResolvedControllerFamily fallback = {};
    fallback.family = ControllerPromptFamily::Xbox;
    fallback.source = FamilyResolutionSource::NoSdl;
    PublishAutomaticControllerFamily(fallback);

    while (InterlockedCompareExchange(&g_workerStopRequested, 0, 0) == 0)
    {
        if (CurrentIconStyleMode() == IconStyleMode::Auto)
        {
            ResolvedControllerFamily resolved = {};
            // 输入线程在独立发布锁下更新状态；渲染线程只读取内存快照。
            // 强制图标样式由命令行覆盖，不需要触碰 SDL。
            __try
            {
                resolved = ResolveAutomaticControllerFamily();
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                const LONG faultNumber = InterlockedIncrement(&g_sdlObserverQueryFaults);
                ResetSdlReadOnlyApi();
                resolved.family = ControllerPromptFamily::Xbox;
                resolved.source = FamilyResolutionSource::NoSdl;
                if (faultNumber <= 4 || (faultNumber % 64) == 0)
                {
                    Log(L"[警告] SDL 只读观察发生外部异常，已清空函数缓存并回退 Xbox；Fault=%ld。",
                        static_cast<long>(faultNumber));
                }
            }
            ConsiderAutomaticControllerFamily(resolved);
        }

        Sleep(kSdlFamilyPollIntervalMs);
    }
}

double ReadIniDouble(const wchar_t* section, const wchar_t* key, double defaultValue)
{
#ifdef BSCN_SINGLE_DLL
    (void)section; (void)key;
    return defaultValue;
#else

    wchar_t defaultText[64] = L"";
    wchar_t value[64] = L"";
    swprintf_s(defaultText, std::size(defaultText), L"%.6f", defaultValue);
    GetPrivateProfileStringW(section,
                             key,
                             defaultText,
                             value,
                             static_cast<DWORD>(std::size(value)),
                             g_iniPath);

    wchar_t* end = nullptr;
    const double parsed = wcstod(value, &end);
    if (!end || *end != L'\0')
        return defaultValue;
    return parsed;
#endif
}

uint32_t ReadIniUInt32(const wchar_t* section, const wchar_t* key, uint32_t defaultValue)
{
#ifdef BSCN_SINGLE_DLL
    (void)section; (void)key;
    return defaultValue;
#else

    wchar_t defaultText[64] = L"";
    wchar_t value[64] = L"";
    swprintf_s(defaultText, std::size(defaultText), L"0x%08X", defaultValue);
    GetPrivateProfileStringW(section,
                             key,
                             defaultText,
                             value,
                             static_cast<DWORD>(std::size(value)),
                             g_iniPath);

    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value, &end, 0);
    if (!end || *end != L'\0')
        return defaultValue;
    return static_cast<uint32_t>(parsed);
#endif
}

bool QueryMainModuleInfo(DWORD* timeDateStamp, DWORD* sizeOfImage, DWORD* entryRva)
{
    if (!g_mainModule)
        return false;

    BYTE* base = reinterpret_cast<BYTE*>(g_mainModule);
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return false;

    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return false;

    if (timeDateStamp)
        *timeDateStamp = ntHeaders->FileHeader.TimeDateStamp;
    if (sizeOfImage)
        *sizeOfImage = ntHeaders->OptionalHeader.SizeOfImage;
    if (entryRva)
        *entryRva = ntHeaders->OptionalHeader.AddressOfEntryPoint;
    return true;
}

bool VerifyMainExecutable()
{
    DWORD timeDateStamp = 0;
    DWORD sizeOfImage = 0;
    DWORD entryRva = 0;
    if (!QueryMainModuleInfo(&timeDateStamp, &sizeOfImage, &entryRva))
    {
        Log(L"[错误] 无法读取主 EXE 的 PE 头。停止安装。 ");
        return false;
    }

    Log(L"[信息] 主模块 Base=%08X，TimeDateStamp=0x%08X，SizeOfImage=0x%08X，EntryRVA=0x%08X。",
        static_cast<unsigned>(g_base),
        timeDateStamp,
        sizeOfImage,
        entryRva);

    if (timeDateStamp != kExpectedTimeDateStamp ||
        sizeOfImage != kExpectedSizeOfImage ||
        entryRva != kExpectedEntryRva)
    {
        Log(L"[错误] EXE 版本特征不匹配。期望 TimeDateStamp=0x%08X，SizeOfImage=0x%08X，EntryRVA=0x%08X。",
            kExpectedTimeDateStamp,
            kExpectedSizeOfImage,
            kExpectedEntryRva);
        return false;
    }

    return true;
}

bool IsExecutableAddress(uintptr_t address)
{
    if (!address)
        return false;

    MEMORY_BASIC_INFORMATION information = {};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &information, sizeof(information)))
        return false;
    if (information.State != MEM_COMMIT)
        return false;

    const DWORD protection = information.Protect & 0xFFu;
    return protection == PAGE_EXECUTE ||
           protection == PAGE_EXECUTE_READ ||
           protection == PAGE_EXECUTE_READWRITE ||
           protection == PAGE_EXECUTE_WRITECOPY;
}

bool IsReadablePointer(const void* pointer)
{
    if (!pointer)
        return false;

    MEMORY_BASIC_INFORMATION information = {};
    if (!VirtualQuery(pointer, &information, sizeof(information)))
        return false;
    if (information.State != MEM_COMMIT)
        return false;

    const DWORD protection = information.Protect & 0xFFu;
    return protection != PAGE_NOACCESS && protection != PAGE_EXECUTE;
}

const wchar_t* ReadTokenPointer(const wchar_t** table, int index, int count)
{
    if (!table || index < 0 || index >= count)
        return nullptr;

    __try
    {
        const wchar_t* token = table[index];
        if (!IsReadablePointer(token) || token[0] == L'\0')
            return nullptr;
        return token;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

int SafeCallBindingValue(BindingValueFn function, int playerSlot, int actionId, int failValue)
{
    if (!function)
        return failValue;

    __try
    {
        return function(playerSlot, actionId);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(L"[警告] 绑定查询发生异常：fn=%p，playerSlot=%d，actionId=%d。",
            function,
            playerSlot,
            actionId);
        return failValue;
    }
}

const wchar_t* ControllerIconName(ControllerIcon icon, wchar_t* buffer, size_t bufferCount)
{
    if (!buffer || bufferCount == 0)
        return L"NONE";

    const int index = static_cast<int>(icon);
    if (index < 0 || index >= kControllerIconCount)
    {
        wcsncpy_s(buffer, bufferCount, L"NONE", _TRUNCATE);
        return buffer;
    }

    buffer[0] = L'\0';
    MultiByteToWideChar(CP_UTF8,
                        0,
                        kEmbeddedIconNames[index],
                        -1,
                        buffer,
                        static_cast<int>(bufferCount));
    return buffer[0] ? buffer : L"NONE";
}

bool SafeReadPointer(const void* address, void** output)
{
    if (!address || !output)
        return false;
    __try
    {
        *output = *reinterpret_cast<void* const*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = nullptr;
        return false;
    }
}

bool SafeReadUInt32(const void* address, uint32_t* output)
{
    if (!address || !output)
        return false;
    __try
    {
        *output = *reinterpret_cast<const uint32_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = 0;
        return false;
    }
}

bool SafeReadInt32(const void* address, int32_t* output)
{
    uint32_t value = 0;
    if (!SafeReadUInt32(address, &value))
        return false;
    *output = static_cast<int32_t>(value);
    return true;
}

bool SafeReadFloat(const void* address, float* output)
{
    if (!address || !output)
        return false;
    __try
    {
        *output = *reinterpret_cast<const float*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = 0.0f;
        return false;
    }
}

bool SafeReadByte(const void* address, uint8_t* output)
{
    if (!address || !output)
        return false;
    __try
    {
        *output = *reinterpret_cast<const uint8_t*>(address);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = 0;
        return false;
    }
}

AxisPolarity CombinePolarity(AxisPolarity first, AxisPolarity second)
{
    if (first == AxisPolarity::Unknown)
        return second;
    if (second == AxisPolarity::Unknown)
        return first;
    return static_cast<AxisPolarity>(static_cast<int>(first) * static_cast<int>(second));
}

AxisPolarity InvertPolarity(AxisPolarity value)
{
    if (value == AxisPolarity::Unknown)
        return value;
    return value == AxisPolarity::Positive ? AxisPolarity::Negative : AxisPolarity::Positive;
}

bool HasVisitedBinding(const std::array<BindingVisit, kMaxBindingVisits>& visits,
                       int visitCount,
                       const void* node,
                       AxisPolarity polarity)
{
    for (int index = 0; index < visitCount; ++index)
    {
        if (visits[index].node == node && visits[index].polarity == polarity)
            return true;
    }
    return false;
}

void AddPadAxis(BindingResolution* result, int axisIndex, AxisPolarity polarity)
{
    if (!result || axisIndex < 0 || axisIndex >= 4)
        return;

    const uint8_t bit = static_cast<uint8_t>(1u << axisIndex);
    if (polarity == AxisPolarity::Positive)
        result->positiveAxisMask = static_cast<uint8_t>(result->positiveAxisMask | bit);
    else if (polarity == AxisPolarity::Negative)
        result->negativeAxisMask = static_cast<uint8_t>(result->negativeAxisMask | bit);
    else
        result->unknownAxisMask = static_cast<uint8_t>(result->unknownAxisMask | bit);
    ++result->padLeaves;
}

void TraverseBindingNode(const void* node,
                         void* pad,
                         AxisPolarity polarity,
                         int depth,
                         std::array<BindingVisit, kMaxBindingVisits>* visits,
                         int* visitCount,
                         BindingResolution* result)
{
    if (!node || !pad || !visits || !visitCount || !result)
        return;
    if (depth > kMaxBindingDepth || *visitCount >= kMaxBindingVisits)
    {
        result->truncated = true;
        return;
    }
    if (HasVisitedBinding(*visits, *visitCount, node, polarity))
        return;

    (*visits)[*visitCount] = BindingVisit{node, polarity};
    ++(*visitCount);
    ++result->visitedNodes;

    uint32_t vtableValue = 0;
    if (!SafeReadUInt32(node, &vtableValue))
    {
        ++result->unknownNodes;
        return;
    }

    const uintptr_t vtable = static_cast<uintptr_t>(vtableValue);
    const uintptr_t directButtonVtable = g_base + kRvaBindingDirectButton;
    const uintptr_t latchVtable = g_base + kRvaBindingLatch;
    const uintptr_t directionalAxisVtable = g_base + kRvaBindingDirectionalAxis;
    const uintptr_t directAxisVtable = g_base + kRvaBindingDirectAxis;
    const uintptr_t differenceVtable = g_base + kRvaBindingDifference;
    const uintptr_t thresholdDifferenceVtable = g_base + kRvaBindingThresholdDifference;
    const uintptr_t gateVtable = g_base + kRvaBindingGate;
    const uintptr_t scaleVtable = g_base + kRvaBindingScale;
    const uintptr_t aggregateVtable = g_base + kRvaBindingAggregate;
    const uintptr_t sumVtable = g_base + kRvaBindingSum;
    const BYTE* bytes = static_cast<const BYTE*>(node);

    if (vtable == directButtonVtable)
    {
        void* device = nullptr;
        int32_t index = -1;
        if (SafeReadPointer(bytes + 0x08, &device) &&
            SafeReadInt32(bytes + 0x0C, &index) &&
            device == pad && index >= 0 && index < 16)
        {
            result->buttonMask = static_cast<uint16_t>(result->buttonMask | static_cast<uint16_t>(1u << index));
            ++result->padLeaves;
        }
        return;
    }

    if (vtable == directAxisVtable)
    {
        void* device = nullptr;
        int32_t index = -1;
        if (SafeReadPointer(bytes + 0x08, &device) &&
            SafeReadInt32(bytes + 0x0C, &index) &&
            device == pad)
        {
            AddPadAxis(result, index, polarity);
        }
        return;
    }

    if (vtable == latchVtable)
    {
        void* child = nullptr;
        if (SafeReadPointer(bytes + 0x14, &child))
            TraverseBindingNode(child, pad, polarity, depth + 1, visits, visitCount, result);
        return;
    }

    if (vtable == directionalAxisVtable)
    {
        void* child = nullptr;
        uint8_t invert = 0;
        SafeReadPointer(bytes + 0x08, &child);
        SafeReadByte(bytes + 0x0C, &invert);
        const AxisPolarity direction = invert ? AxisPolarity::Negative : AxisPolarity::Positive;
        TraverseBindingNode(child,
                            pad,
                            CombinePolarity(polarity, direction),
                            depth + 1,
                            visits,
                            visitCount,
                            result);
        return;
    }

    if (vtable == differenceVtable || vtable == thresholdDifferenceVtable)
    {
        // 两类节点的值均建立在第二子项减第一子项之上；对轴方向必须保留符号。
        void* first = nullptr;
        void* second = nullptr;
        SafeReadPointer(bytes + 0x08, &first);
        SafeReadPointer(bytes + 0x0C, &second);
        TraverseBindingNode(first,
                            pad,
                            InvertPolarity(polarity),
                            depth + 1,
                            visits,
                            visitCount,
                            result);
        TraverseBindingNode(second,
                            pad,
                            polarity,
                            depth + 1,
                            visits,
                            visitCount,
                            result);
        return;
    }

    if (vtable == gateVtable)
    {
        // Gate 的 +08 是实际输出，+0C 是阈值条件。两者都可能是手柄输入；
        // 同时遍历可正确保留“按钮 + 修饰键”一类组合，而不会复制键鼠分支。
        void* outputChild = nullptr;
        void* conditionChild = nullptr;
        SafeReadPointer(bytes + 0x08, &outputChild);
        SafeReadPointer(bytes + 0x0C, &conditionChild);
        TraverseBindingNode(outputChild, pad, polarity, depth + 1, visits, visitCount, result);
        TraverseBindingNode(conditionChild, pad, AxisPolarity::Unknown, depth + 1, visits, visitCount, result);
        return;
    }

    if (vtable == scaleVtable)
    {
        void* child = nullptr;
        float scale = 1.0f;
        SafeReadPointer(bytes + 0x08, &child);
        SafeReadFloat(bytes + 0x0C, &scale);
        AxisPolarity scaledPolarity = polarity;
        if (scale < 0.0f)
            scaledPolarity = InvertPolarity(scaledPolarity);
        TraverseBindingNode(child, pad, scaledPolarity, depth + 1, visits, visitCount, result);
        return;
    }

    if (vtable == aggregateVtable || vtable == sumVtable)
    {
        int32_t count = 0;
        if (!SafeReadInt32(bytes + 0x18, &count) || count <= 0)
            return;

        // 该类对象在 +08 起内联保存 4 个子指针，+18 才是数量字段；构造和析构
        // 均按此布局迭代。异常数量一律截断，避免把 +18/+1C 当作指针读取。
        const int safeCount = std::min(count, 4);
        if (count > 4)
            result->truncated = true;
        for (int index = 0; index < safeCount; ++index)
        {
            void* child = nullptr;
            if (SafeReadPointer(bytes + 0x08 + index * sizeof(void*), &child))
                TraverseBindingNode(child, pad, polarity, depth + 1, visits, visitCount, result);
        }
        return;
    }

    ++result->unknownNodes;
}

bool ReadBindingRootForAction(int actionId, BindingResolution* output)
{
    if (!output || actionId < 0 || actionId >= 256)
        return false;

    BindingResolution result = {};
    result.manager = InterlockedCompareExchangePointer(&g_currentActionManager, nullptr, nullptr);
    result.pad = InterlockedCompareExchangePointer(&g_currentPadObject, nullptr, nullptr);
    if (!result.manager || !result.pad)
    {
        *output = result;
        return false;
    }

    __try
    {
        BYTE* stateArray = *reinterpret_cast<BYTE**>(static_cast<BYTE*>(result.manager) + 0x08);
        if (!stateArray)
        {
            *output = result;
            return false;
        }
        result.root = *reinterpret_cast<void**>(stateArray + static_cast<size_t>(actionId) * 0x10 + 0x0C);
        if (!result.root)
        {
            *output = result;
            return false;
        }
        result.rootVtable = *reinterpret_cast<uint32_t*>(result.root);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = result;
        return false;
    }

    *output = result;
    return true;
}

ControllerIcon ButtonIndexToIcon(int index)
{
    switch (index)
    {
    case 0: return ControllerIcon::A;
    case 1: return ControllerIcon::B;
    case 2: return ControllerIcon::X;
    case 3: return ControllerIcon::Y;
    case 4: return ControllerIcon::Dpu;
    case 5: return ControllerIcon::Dpd;
    case 6: return ControllerIcon::Dpl;
    case 7: return ControllerIcon::Dpr;
    case 8: return ControllerIcon::Start;
    case 9: return ControllerIcon::Back;
    case 10: return ControllerIcon::LB;
    case 11: return ControllerIcon::RB;
    case 12: return ControllerIcon::LT;
    case 13: return ControllerIcon::RT;
    case 14: return ControllerIcon::Lsc;
    case 15: return ControllerIcon::Rsc;
    default: return ControllerIcon::Invalid;
    }
}

ControllerIcon AxisDirectionToIcon(int axisIndex, AxisPolarity polarity)
{
    if (polarity == AxisPolarity::Unknown)
        return ControllerIcon::Invalid;

    const bool positive = polarity == AxisPolarity::Positive;
    switch (axisIndex)
    {
    case 0: return positive ? ControllerIcon::LsRight : ControllerIcon::LsLeft;
    case 1: return positive ? ControllerIcon::LsUp : ControllerIcon::LsDown;
    // MOUSERIGHT 的静态提示语义已实机确认是“移动右摇杆瞄准”，因此该槽
    // 改为 RS_ALL。真正的右摇杆向左动态轴没有独立可靠载体时安全回退。
    case 2: return positive ? ControllerIcon::RsRight : ControllerIcon::Invalid;
    case 3: return positive ? ControllerIcon::RsUp : ControllerIcon::RsDown;
    default: return ControllerIcon::Invalid;
    }
}

int CountBits(uint32_t value)
{
    int count = 0;
    while (value)
    {
        value &= value - 1;
        ++count;
    }
    return count;
}

ControllerIcon FinalizeBindingResolution(BindingResolution* result, bool* ambiguous)
{
    if (ambiguous)
        *ambiguous = false;
    if (!result || result->padLeaves <= 0)
        return ControllerIcon::Invalid;

    const int buttonCount = CountBits(result->buttonMask);
    const uint32_t directionalMask =
        static_cast<uint32_t>(result->positiveAxisMask) |
        (static_cast<uint32_t>(result->negativeAxisMask) << 4) |
        (static_cast<uint32_t>(result->unknownAxisMask) << 8);
    const int axisDirectionCount = CountBits(directionalMask);

    // 单一数字按钮/扳机优先。若同一根节点里出现多个手柄按钮，可能是 AND 组合、
    // 多方案 OR 或仍未识别的上下文节点；当前图标接口只返回一个 token，因此宁可回退。
    if (buttonCount == 1 && axisDirectionCount == 0)
    {
        for (int index = 0; index < 16; ++index)
        {
            if (result->buttonMask & (1u << index))
                return ButtonIndexToIcon(index);
        }
    }

    if (buttonCount == 0 && axisDirectionCount == 1 && result->unknownAxisMask == 0)
    {
        for (int axis = 0; axis < 4; ++axis)
        {
            if (result->positiveAxisMask & (1u << axis))
                return AxisDirectionToIcon(axis, AxisPolarity::Positive);
            if (result->negativeAxisMask & (1u << axis))
                return AxisDirectionToIcon(axis, AxisPolarity::Negative);
        }
    }

    if (ambiguous)
        *ambiguous = true;
    return ControllerIcon::Invalid;
}

ControllerIcon ResolveControllerIconForAction(int actionId,
                                               BindingResolution* outputResolution,
                                               bool* outputAmbiguous)
{
    InterlockedIncrement(&g_bindingResolutionCalls);
    BindingResolution result = {};
    bool ambiguous = false;
    ControllerIcon icon = ControllerIcon::Invalid;

    if (ReadBindingRootForAction(actionId, &result))
    {
        std::array<BindingVisit, kMaxBindingVisits> visits = {};
        int visitCount = 0;
        TraverseBindingNode(result.root,
                            result.pad,
                            AxisPolarity::Unknown,
                            0,
                            &visits,
                            &visitCount,
                            &result);
        icon = FinalizeBindingResolution(&result, &ambiguous);
    }

    if (outputResolution)
        *outputResolution = result;
    if (outputAmbiguous)
        *outputAmbiguous = ambiguous;
    return icon;
}

struct SequenceInspection
{
    std::array<std::array<wchar_t, 24>, 12> cores = {};
    int coreCount = 0;
    std::array<std::array<wchar_t, 64>, 16> textRuns = {};
    int textRunCount = 0;
    int standaloneAorDCount = 0;
};

struct FontSequenceVectorState
{
    BYTE* storage = nullptr;
    int capacity = 0;
    int count = 0;
};

struct EventRule
{
    const wchar_t* alias;
    int actionId;
};

// 这些关系来自游戏自己的 35+9 个控制器描述符及已确认的实机序列。
// 规则左侧是“事件语义”，右侧只是该事件当前应查询哪个逻辑动作；按钮本身
// 仍由运行时 binding 图读取，因此不是 ActionID->按钮固定映射。
static constexpr EventRule kEventRules[] = {
    {L"{WDRAW}", 33}, {L"{FLOW}", 34}, {L"{PHNUS}", 40},
    {L"{WFIRE}", 41}, {L"{WRLD}", 42}, {L"{WAIM}", 43},
    {L"{WFKL}", 44}, {L"{WFLK}", 45}, {L"{WSWP}", 46},
    {L"{WPRI}", 47}, {L"{WSEC}", 48}, {L"{PHN}", 49},
    {L"{QSWP}", 50}, {L"{PICK}", 51}, {L"{CCHNG}", 52},
    {L"{CCNTR}", 53}, {L"{SPRNT}", 54}, {L"{CTXAC}", 55},
    {L"{CTXMO}", 56}, {L"{WSZC}", 57}, {L"{BLIP}", 58},
    {L"{PAUSE}", 59}, {L"{VETHR}", 60}, {L"{VEBRK}", 61},
    {L"{VEHBRK}", 62}, {L"{VEVIEW}", 63}, {L"{VEHORN}", 64},
    {L"{VEFIRE}", 65}, {L"{VESTL}", 66}, {L"{VESTR}", 67}
};

std::array<uint64_t, 256> g_loggedRenderEvents = {};
std::array<uint64_t, 256> g_loggedQteCandidates = {};

bool ReadFontSequenceVectorState(FontSequenceVectorState* output)
{
    if (!output)
        return false;
    *output = {};

    __try
    {
        BYTE* container = reinterpret_cast<BYTE*>(g_base + kRvaFontSequenceVector);
        BYTE* storage = *reinterpret_cast<BYTE**>(container + 0x00);
        const int capacity = *reinterpret_cast<int*>(container + 0x04);
        const int count = *reinterpret_cast<int*>(container + 0x08);
        if (capacity < 0 || capacity > 1024 || count < 0 || count > capacity || count > 512)
            return false;
        if (count > 0 && !IsReadablePointer(storage))
            return false;
        output->storage = storage;
        output->capacity = capacity;
        output->count = count;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = {};
        return false;
    }
}

bool InspectionHasCore(const SequenceInspection& inspection, const wchar_t* core)
{
    for (int index = 0; index < inspection.coreCount; ++index)
    {
        if (wcscmp(inspection.cores[static_cast<size_t>(index)].data(), core) == 0)
            return true;
    }
    return false;
}

bool IsPrintableSequenceCharacter(uint16_t value)
{
    return value >= 0x20u && value <= 0x7Eu;
}

bool IsStandaloneQteLetter(const wchar_t* text)
{
    if (!text)
        return false;
    while (*text == L' ' || *text == L'\t')
        ++text;
    if (*text != L'A' && *text != L'D')
        return false;
    ++text;
    while (*text == L' ' || *text == L'\t')
        ++text;
    return *text == L'\0';
}

void InspectSequenceRecord(const BYTE* record, SequenceInspection* output)
{
    if (!output)
        return;
    *output = {};
    if (!record || !IsReadablePointer(record))
        return;

    __try
    {
        // 第一遍提取可识别的 {...} token core，保持原始文本结构。
        for (size_t offset = 0; offset + 4 <= 0xACu; offset += 2)
        {
            if (*reinterpret_cast<const uint16_t*>(record + offset) != static_cast<uint16_t>('{'))
                continue;

            wchar_t core[24] = L"";
            size_t length = 0;
            bool complete = false;
            for (size_t cursor = offset;
                 cursor + 2 <= 0xACu && length + 1 < std::size(core) && length < 20;
                 cursor += 2)
            {
                const uint16_t value = *reinterpret_cast<const uint16_t*>(record + cursor);
                if (!IsPrintableSequenceCharacter(value))
                    break;
                core[length++] = static_cast<wchar_t>(value);
                if (value == static_cast<uint16_t>('}'))
                {
                    complete = length >= 3;
                    break;
                }
            }
            core[length] = L'\0';
            if (!complete || InspectionHasCore(*output, core))
                continue;
            if (output->coreCount >= static_cast<int>(output->cores.size()))
                break;
            wcsncpy_s(output->cores[static_cast<size_t>(output->coreCount)].data(),
                      output->cores[static_cast<size_t>(output->coreCount)].size(),
                      core,
                      _TRUNCATE);
            ++output->coreCount;
        }

        // 第二遍提取记录中的全部可打印 UTF-16LE 片段。黑客 QTE 的 A/D 不是
        // GetPromptName 动态键名，因此必须先确认它们落在哪条 FontSequence 记录中。
        size_t offset = 0;
        while (offset + 2 <= 0xACu &&
               output->textRunCount < static_cast<int>(output->textRuns.size()))
        {
            const uint16_t first = *reinterpret_cast<const uint16_t*>(record + offset);
            if (!IsPrintableSequenceCharacter(first))
            {
                offset += 2;
                continue;
            }

            wchar_t run[64] = L"";
            size_t length = 0;
            size_t cursor = offset;
            while (cursor + 2 <= 0xACu && length + 1 < std::size(run))
            {
                const uint16_t value = *reinterpret_cast<const uint16_t*>(record + cursor);
                if (!IsPrintableSequenceCharacter(value))
                    break;
                run[length++] = static_cast<wchar_t>(value);
                cursor += 2;
            }
            run[length] = L'\0';

            if (length > 0)
            {
                auto& destination =
                    output->textRuns[static_cast<size_t>(output->textRunCount)];
                wcsncpy_s(destination.data(), destination.size(), run, _TRUNCATE);
                if (IsStandaloneQteLetter(destination.data()))
                    ++output->standaloneAorDCount;
                ++output->textRunCount;
            }
            offset = cursor > offset ? cursor : offset + 2;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        *output = {};
    }
}

const BYTE* GetSequenceRecordPointer(int sequenceIndex)
{
    FontSequenceVectorState state = {};
    if (!ReadFontSequenceVectorState(&state) ||
        sequenceIndex < 0 || sequenceIndex >= state.count)
    {
        return nullptr;
    }
    return state.storage + static_cast<size_t>(sequenceIndex) * 0xACu;
}

bool ReadSequenceInspection(int sequenceIndex, SequenceInspection* output)
{
    if (!output)
        return false;
    *output = {};

    const BYTE* record = GetSequenceRecordPointer(sequenceIndex);
    if (!record)
        return false;
    InspectSequenceRecord(record, output);
    return output->coreCount > 0 || output->textRunCount > 0;
}

const EventRule* FindEventRule(const SequenceInspection& inspection)
{
    for (const EventRule& rule : kEventRules)
    {
        if (InspectionHasCore(inspection, rule.alias))
            return &rule;
    }
    return nullptr;
}

enum class Action38_39Semantic : int
{
    None = 0,
    CoverDirection,
    HackingQte
};

// action 38/39 没有可解析的通用手柄 binding，必须按实际调用阶段解释：
// - 两个构造调用点在掩体教程中代表 PC A/D，也就是左摇杆左/右；
// - 00C75873 是黑客 QTE 的独立逐帧查询点，才代表 Xbox A/X。
// 这仍然是“调用点 + ActionID”的定向规则，不会形成全局 A/D 字符映射。
bool ResolveContextualAction38_39Icon(uintptr_t directCaller,
                                      int actionId,
                                      ControllerIcon* output,
                                      Action38_39Semantic* semantic)
{
    if (!output || !semantic)
        return false;

    *semantic = Action38_39Semantic::None;
    const bool isActionA = actionId == kQteActionA;
    const bool isActionD = actionId == kQteActionD;
    if (!isActionA && !isActionD)
        return false;

    const uintptr_t buildA = g_base + kRvaQteBuildPromptA;
    const uintptr_t buildD = g_base + kRvaQteBuildPromptD;
    const uintptr_t runtime = g_base + kRvaQteRuntimePrompt;

    if (directCaller == runtime)
    {
        *output = isActionA ? ControllerIcon::A : ControllerIcon::X;
        *semantic = Action38_39Semantic::HackingQte;
        return true;
    }

    if (isActionA && directCaller == buildA)
    {
        *output = ControllerIcon::LsLeft;
        *semantic = Action38_39Semantic::CoverDirection;
        return true;
    }

    if (isActionD && directCaller == buildD)
    {
        *output = ControllerIcon::LsRight;
        *semantic = Action38_39Semantic::CoverDirection;
        return true;
    }

    return false;
}

// 动态提示 Hook 只允许“已经确认属于事件系统的逻辑动作”改用专属载体。
// 未列入事件规则的 ActionID 继续走原生键鼠回退，避免一个尚未识别的调用路径
// 因为恰好存在手柄 binding，就被错误改成内部载体并污染其他界面。
// 这里的判断不是 ActionID->按钮映射：ActionID 仅用于确认该动作属于已知事件，
// 实际显示按钮仍在运行时从该动作当前 binding 图中读取。
bool IsKnownEventActionId(int actionId)
{
    for (const EventRule& rule : kEventRules)
    {
        if (rule.actionId == actionId)
            return true;
    }
    return false;
}


void* GetPlayerObjectForSlot(int playerSlot)
{
    if (playerSlot < 0 || playerSlot > 4 ||
        !FunctionHeadMatches(kRvaGetPlayerObject,
                             kExpectedGetPlayerObjectHead,
                             std::size(kExpectedGetPlayerObjectHead)))
    {
        return nullptr;
    }

    __try
    {
        void* manager = *reinterpret_cast<void**>(g_base + kRvaPlayerManagerGlobal);
        if (!manager)
            return nullptr;
        auto getPlayerObject = reinterpret_cast<GetPlayerObjectFn>(g_base + kRvaGetPlayerObject);
        return getPlayerObject(manager, playerSlot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

bool ReadControllerModeForPlayer(int playerSlot, bool* controllerMode)
{
    if (!controllerMode)
        return false;
    *controllerMode = false;

    if (g_forceControllerMode == 0 || g_forceControllerMode == 1)
    {
        *controllerMode = g_forceControllerMode != 0;
        return true;
    }

    void* playerObject = GetPlayerObjectForSlot(playerSlot);
    if (!playerObject)
        return false;

    uint8_t useJoypad = 0;
    const BYTE* controls = static_cast<const BYTE*>(playerObject) + kPlayerControlsOffset;
    if (!SafeReadByte(controls + kControlsUseJoypadOffset, &useJoypad))
        return false;
    *controllerMode = useJoypad != 0;
    return true;
}

bool UpdateControllerModeForPlayer(int playerSlot)
{
    bool controllerMode = false;
    if (!ReadControllerModeForPlayer(playerSlot, &controllerMode))
    {
        const LONG failure = InterlockedIncrement(&g_controllerModeReadFailures);
        if (failure <= 16)
            Log(L"[输入模式] 无法读取 player=%d 的 UseJoypad；本次保持键鼠提示。", playerSlot);
        InterlockedExchange(&g_controllerModeActive, 0);
        return false;
    }

    const LONG newValue = controllerMode ? 1 : 0;
    const LONG oldValue = InterlockedExchange(&g_controllerModeActive, newValue);
    if (oldValue != newValue)
        Log(L"[输入模式] player=%d UseJoypad=%d；全局图集模式切换为%ls。",
            playerSlot,
            controllerMode ? 1 : 0,
            controllerMode ? L"手柄" : L"键鼠");
    return controllerMode;
}

void AppendCoreList(const SequenceInspection& inspection, wchar_t* output, size_t outputCount)
{
    if (!output || outputCount == 0)
        return;
    output[0] = L'\0';
    for (int index = 0; index < inspection.coreCount; ++index)
    {
        if (index > 0)
            wcsncat_s(output, outputCount, L"|", _TRUNCATE);
        wcsncat_s(output,
                  outputCount,
                  inspection.cores[static_cast<size_t>(index)].data(),
                  _TRUNCATE);
    }
    if (inspection.coreCount == 0)
        wcsncpy_s(output, outputCount, L"<none>", _TRUNCATE);
}

uintptr_t FindPromptParentCaller(const uintptr_t* originalStack,
                                     uintptr_t directCaller,
                                     int* stackWordIndex)
{
    if (stackWordIndex)
        *stackWordIndex = -1;
    if (!originalStack)
        return 0;

    // 原始入口布局：[0]=直接返回地址，[1]=playerSlot，[2]=actionId。薄包装器的
    // 上层返回地址通常从 [3] 开始出现；不硬编码固定偏移，而是寻找第一个可执行地址。
    __try
    {
        for (int index = 3; index < 24; ++index)
        {
            const uintptr_t candidate = originalStack[index];
            if (candidate == directCaller)
                continue;
            if (IsExecutableAddress(candidate))
            {
                if (stackWordIndex)
                    *stackWordIndex = index;
                return candidate;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return 0;
    }
    return 0;
}

const DistinctCarrierSpec* CarrierSpecForIcon(ControllerIcon icon)
{
    const int iconIndex = static_cast<int>(icon);
    if (iconIndex < 0 || iconIndex >= kControllerIconCount)
        return nullptr;
    const DistinctCarrierSpec& spec =
        kDistinctCarrierSpecs[static_cast<size_t>(iconIndex)];
    if (spec.tableIndex < 0 || spec.cellX < 0 || spec.cellY < 0)
        return nullptr;
    return &spec;
}

bool IsPreservedAtlasCell(int cellX, int cellY)
{
    // TexMod 参考图集底行第 4 格是有语义的无限弹药符号，不是可牺牲键帽。
    return cellX == 3 && cellY == 7;
}

const wchar_t* CarrierTableName(CarrierTable table)
{
    return table == CarrierTable::Mouse ? L"M" : L"K";
}

const wchar_t* ReadDistinctCarrierToken(const DistinctCarrierSpec& spec)
{
    if (spec.table == CarrierTable::Mouse)
        return ReadTokenPointer(g_mouseIconTable, spec.tableIndex, 7);
    return ReadTokenPointer(g_keyboardIconTable, spec.tableIndex, 0x90);
}

bool ExtractTokenCore(const wchar_t* token, wchar_t* output, size_t outputCount)
{
    if (!token || !output || outputCount < 4)
        return false;
    output[0] = L'\0';

    const size_t length = wcslen(token);
    if (length < 7 || length >= 64 ||
        token[0] != L'~' || token[1] != L'~' ||
        token[2] != L'{' ||
        token[length - 3] != L'}' ||
        token[length - 2] != L'~' ||
        token[length - 1] != L'~')
    {
        return false;
    }

    const size_t coreLength = length - 4; // 去掉首尾各两个 '~'，保留 {...}。
    if (coreLength + 1 > outputCount)
        return false;

    wmemcpy(output, token + 2, coreLength);
    output[coreLength] = L'\0';
    return true;
}

const wchar_t* GetDistinctCarrierToken(ControllerIcon icon)
{
    const DistinctCarrierSpec* spec = CarrierSpecForIcon(icon);
    if (!spec)
        return nullptr;
    return ReadDistinctCarrierToken(*spec);
}

ControllerIcon FindDistinctCarrierIcon(const SequenceInspection& inspection)
{
    for (int iconIndex = 0; iconIndex < kControllerIconCount; ++iconIndex)
    {
        const wchar_t* core =
            g_distinctCarrierCores[static_cast<size_t>(iconIndex)].data();
        if (core[0] != L'\0' && InspectionHasCore(inspection, core))
            return static_cast<ControllerIcon>(iconIndex);
    }
    return ControllerIcon::Invalid;
}

void IssuePendingCarrier(ControllerIcon icon, int actionId)
{
    const int iconIndex = static_cast<int>(icon);
    if (iconIndex < 0 || iconIndex >= kControllerIconCount)
        return;

    PendingCarrierState& state = CurrentPendingCarrierStates()[static_cast<size_t>(iconIndex)];
    state.actionId = actionId;
    state.lastIssuedTick = GetTickCount();

    // 同一 UI 元素可能每帧重复查询同一 ActionID。若把每次查询都累计为许可，
    // 一次 A 提示可留下数百个 SPACE 许可，随后普通 SPACE 键帽也会被误替换。
    // 因此每种图标只保留一个待消费许可；重复签发只刷新寿命并计入合并统计。
    if (state.pendingCount > 0)
        InterlockedIncrement(&g_promptCarrierPendingDropped);
    state.pendingCount = 1;
}

bool IsCoverDisplayIcon(ControllerIcon icon)
{
    return icon == ControllerIcon::LsLeft || icon == ControllerIcon::LsRight;
}

ControllerIcon CoverCarrierIconForDisplay(ControllerIcon displayIcon)
{
    if (displayIcon == ControllerIcon::LsLeft)
        return ControllerIcon::LsUp;     // SCROLLLOCK：预制键帽，不附加动态文字
    if (displayIcon == ControllerIcon::LsRight)
        return ControllerIcon::LsDown;   // NUMLOCK：预制键帽，不附加动态文字
    return ControllerIcon::Invalid;
}

ControllerIcon CoverDisplayIconForCarrier(int actionId, ControllerIcon carrierIcon)
{
    if (actionId == kQteActionA && carrierIcon == ControllerIcon::LsUp)
        return ControllerIcon::LsLeft;
    if (actionId == kQteActionD && carrierIcon == ControllerIcon::LsDown)
        return ControllerIcon::LsRight;
    return ControllerIcon::Invalid;
}

bool IsCoverCarrierIcon(ControllerIcon icon)
{
    return icon == ControllerIcon::LsUp || icon == ControllerIcon::LsDown;
}

void ArmCoverStatic(ControllerIcon displayIcon, int actionId)
{
    if (!g_enableCoverStaticFix || !IsCoverDisplayIcon(displayIcon))
        return;

    const ControllerIcon carrierIcon = CoverCarrierIconForDisplay(displayIcon);
    if (CoverDisplayIconForCarrier(actionId, carrierIcon) != displayIcon)
        return;

    IssuePendingCarrier(carrierIcon, actionId);
    InterlockedIncrement(&g_coverStaticArmed);
}

bool ConsumePendingCarrier(ControllerIcon icon,
                           int* actionId,
                           DWORD* ageMs,
                           int* remaining)
{
    if (actionId)
        *actionId = -1;
    if (ageMs)
        *ageMs = 0;
    if (remaining)
        *remaining = 0;

    const int iconIndex = static_cast<int>(icon);
    if (iconIndex < 0 || iconIndex >= kControllerIconCount)
        return false;

    PendingCarrierState& state = CurrentPendingCarrierStates()[static_cast<size_t>(iconIndex)];
    if (state.pendingCount <= 0)
        return false;

    const DWORD age = GetTickCount() - state.lastIssuedTick;
    if (age > static_cast<DWORD>(g_promptHintLifetimeMs))
    {
        InterlockedExchangeAdd(&g_promptCarrierExpired, state.pendingCount);
        state = {};
        return false;
    }

    --state.pendingCount;
    if (actionId)
        *actionId = state.actionId;
    if (ageMs)
        *ageMs = age;
    if (remaining)
        *remaining = state.pendingCount;
    InterlockedIncrement(&g_promptCarrierConsumed);
    return true;
}

extern "C" const wchar_t* __cdecl HookGetPromptIconImpl(int playerSlot,
                                                            int actionId,
                                                            const uintptr_t* originalStack)
{
    const LONG callNumber = InterlockedIncrement(&g_totalPromptCalls);
    const uintptr_t directCaller = originalStack ? originalStack[0] : 0;
    int parentStackWord = -1;
    const uintptr_t parentCaller = FindPromptParentCaller(originalStack,
                                                          directCaller,
                                                          &parentStackWord);
    const bool controllerMode = UpdateControllerModeForPlayer(playerSlot);
    const wchar_t* result = nullptr;
    const wchar_t* source = L"NULL";
    ControllerIcon resolvedIcon = ControllerIcon::Invalid;
    BindingResolution binding = {};
    bool ambiguous = false;

    const int keyboardValue = SafeCallBindingValue(g_getKeyboardValue, playerSlot, actionId, 0x90);
    const int mouseValue = SafeCallBindingValue(g_getMouseValue, playerSlot, actionId, 7);

    Action38_39Semantic action38_39Semantic = Action38_39Semantic::None;
    if (controllerMode &&
        ResolveContextualAction38_39Icon(directCaller,
                                         actionId,
                                         &resolvedIcon,
                                         &action38_39Semantic))
    {
        if (action38_39Semantic == Action38_39Semantic::HackingQte)
        {
            result = GetDistinctCarrierToken(resolvedIcon);
            if (result)
            {
                InterlockedIncrement(&g_resolvedPromptCalls);
                InterlockedIncrement(&g_qtePromptOverrides);
                source = L"QteRuntimeSemanticCarrier";
            }
            else
            {
                source = L"QteRuntimeCarrierTokenUnavailable";
            }
        }
        else
        {
            // LS_LEFT / LS_RIGHT 已全局绑定到指定载体，不需要临时事件纹理许可：
            // SCROLLLOCK / NUMLOCK 两个无动态文字的预制载体，直接返回即可。
            result = GetDistinctCarrierToken(resolvedIcon);
            if (result)
            {
                InterlockedIncrement(&g_resolvedPromptCalls);
                InterlockedIncrement(&g_coverDirectionOverrides);
                source = L"CoverDirectionGlobalCarrier";
            }
            else
            {
                source = L"CoverDirectionCarrierTokenUnavailable";
            }
        }
    }
    else if (controllerMode && g_useGameControllerBindings && IsKnownEventActionId(actionId))
    {
        resolvedIcon = ResolveControllerIconForAction(actionId, &binding, &ambiguous);
        if (resolvedIcon != ControllerIcon::Invalid)
        {
            result = GetDistinctCarrierToken(resolvedIcon);
            if (result)
            {
                InterlockedIncrement(&g_resolvedPromptCalls);
                source = L"EventBindingCarrier";
            }
            else
            {
                source = L"CarrierTokenUnavailable";
            }
        }
        else
        {
            InterlockedIncrement(&g_unresolvedPromptCalls);
            if (ambiguous)
                InterlockedIncrement(&g_ambiguousPromptCalls);
        }
    }
    else if (controllerMode && !IsKnownEventActionId(actionId))
    {
        source = L"UnknownEventPreserved";
    }

    if (!result && g_fallbackToKeyboardMouse)
    {
        result = ReadTokenPointer(g_keyboardIconTable, keyboardValue, 0x90);
        if (result)
        {
            InterlockedIncrement(&g_fallbackPromptCalls);
            if (wcscmp(source, L"NULL") == 0)
                source = L"Keyboard";
        }
        else
        {
            result = ReadTokenPointer(g_mouseIconTable, mouseValue, 7);
            if (result)
            {
                InterlockedIncrement(&g_fallbackPromptCalls);
                if (wcscmp(source, L"NULL") == 0)
                    source = L"Mouse";
            }
        }
    }

    if (!result)
        InterlockedIncrement(&g_nullPromptCalls);

    const LONG bindingCall = InterlockedCompareExchange(&g_bindingResolutionCalls, 0, 0);
    const bool shouldLogBinding = g_logEveryBindingResolution ||
                                  bindingCall <= g_logFirstBindingResolutions;
    bool bindingChanged = true;
    if (actionId >= 0 && actionId < static_cast<int>(g_lastLoggedBindingRoot.size()))
    {
        const uintptr_t root = reinterpret_cast<uintptr_t>(binding.root);
        const int resolved = static_cast<int>(resolvedIcon);
        bindingChanged = g_lastLoggedBindingSeen[actionId] == 0 ||
                         g_lastLoggedBindingRoot[actionId] != root ||
                         g_lastLoggedBindingResult[actionId] != resolved;
        g_lastLoggedBindingSeen[actionId] = 1;
        g_lastLoggedBindingRoot[actionId] = root;
        g_lastLoggedBindingResult[actionId] = resolved;
    }

    wchar_t iconName[32] = L"";
    ControllerIconName(resolvedIcon, iconName, std::size(iconName));
    if (shouldLogBinding && bindingChanged && controllerMode && IsKnownEventActionId(actionId))
    {
        Log(L"[绑定解析] action=%d manager=%p pad=%p root=%p vtable=%08X nodes=%d leaves=%d buttons=0x%04X axis(+%02X -%02X ?%02X) unknown=%d truncated=%d ambiguous=%d result=%ls。",
            actionId,
            binding.manager,
            binding.pad,
            binding.root,
            static_cast<unsigned>(binding.rootVtable),
            binding.visitedNodes,
            binding.padLeaves,
            binding.buttonMask,
            binding.positiveAxisMask,
            binding.negativeAxisMask,
            binding.unknownAxisMask,
            binding.unknownNodes,
            binding.truncated ? 1 : 0,
            ambiguous ? 1 : 0,
            iconName);
    }

    if (g_logEveryPromptCall || callNumber <= g_logFirstPromptCalls)
    {
        Log(L"[图标提示] #%ld direct=%08X parent=%08X stackWord=%d player=%d action=%d mode=%ls binding=%ls key=%d mouse=%d -> %ls %ls",
            static_cast<long>(callNumber),
            static_cast<unsigned>(directCaller),
            static_cast<unsigned>(parentCaller),
            parentStackWord,
            playerSlot,
            actionId,
            controllerMode ? L"Controller" : L"KeyboardMouse",
            iconName,
            keyboardValue,
            mouseValue,
            source,
            result ? result : L"<NULL>");
    }
    return result;
}

#if defined(_MSC_VER)
extern "C" __declspec(naked) void HookGetPromptIconNaked()
{
    __asm
    {
        // 原函数是 cdecl：[esp+4] = playerSlot，[esp+8] = actionId。
        // 将原始 ESP 一并传入 C++，由其扫描薄包装器之上的真实返回地址。
        mov ecx, esp
        mov eax, [ecx + 4]
        mov edx, [ecx + 8]
        push ecx
        push edx
        push eax
        call HookGetPromptIconImpl
        add esp, 12
        ret
    }
}
#else
extern "C" void HookGetPromptIconNaked();
__asm__(
    ".text\n"
    ".globl _HookGetPromptIconNaked\n"
    "_HookGetPromptIconNaked:\n"
    "movl %esp, %ecx\n"
    "movl 4(%ecx), %eax\n"
    "movl 8(%ecx), %edx\n"
    "pushl %ecx\n"
    "pushl %edx\n"
    "pushl %eax\n"
    "call _HookGetPromptIconImpl\n"
    "addl $12, %esp\n"
    "ret\n");
#endif

#if defined(_MSC_VER)
#define CAPTURE_TOKEN_RENDERER_CALLER() reinterpret_cast<uintptr_t>(_ReturnAddress())
#elif defined(__GNUC__) || defined(__clang__)
#define CAPTURE_TOKEN_RENDERER_CALLER() reinterpret_cast<uintptr_t>(__builtin_return_address(0))
#else
#define CAPTURE_TOKEN_RENDERER_CALLER() static_cast<uintptr_t>(0)
#endif

int ReadSegmentSequenceIndex(uintptr_t segmentAddress)
{
    if (!segmentAddress)
        return -1;
    uint32_t value = 0;
    if (!SafeReadUInt32(reinterpret_cast<const void*>(segmentAddress + 0x08u), &value))
        return -1;
    if (value > 511u)
        return -1;
    return static_cast<int>(value);
}


const wchar_t* __cdecl HookGetPromptName(int playerSlot, int actionId)
{
    const LONG callNumber = InterlockedIncrement(&g_totalPromptNameCalls);
    const wchar_t* original = g_originalGetPromptName
                                  ? g_originalGetPromptName(playerSlot, actionId)
                                  : nullptr;
    const bool controllerMode = UpdateControllerModeForPlayer(playerSlot);
    ControllerIcon resolvedIcon = ControllerIcon::Invalid;
    BindingResolution binding = {};
    bool ambiguous = false;
    bool bindingResolved = false;
    bool hasDynamicLabel = false;

    if (controllerMode)
    {
        const uintptr_t promptNameCaller = CAPTURE_TOKEN_RENDERER_CALLER();
        Action38_39Semantic semantic = Action38_39Semantic::None;
        if (ResolveContextualAction38_39Icon(promptNameCaller,
                                             actionId,
                                             &resolvedIcon,
                                             &semantic))
        {
            bindingResolved = resolvedIcon != ControllerIcon::Invalid;
        }
        else if (g_useGameControllerBindings && IsKnownEventActionId(actionId))
        {
            resolvedIcon = ResolveControllerIconForAction(actionId, &binding, &ambiguous);
            bindingResolved = resolvedIcon != ControllerIcon::Invalid;
        }
    }

    // 仅抑制此函数实际返回的非空动态键名。掩体教程的 1/4 并不经过该函数，
    // 掩体修复不依赖 GetPromptName 来触发。
    wchar_t originalPreview[64] = L"<NULL>";
    if (original && IsReadablePointer(original))
    {
        __try
        {
            wcsncpy_s(originalPreview, std::size(originalPreview), original, _TRUNCATE);
            hasDynamicLabel = originalPreview[0] != L'\0';
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            wcsncpy_s(originalPreview, std::size(originalPreview), L"<INVALID>", _TRUNCATE);
            hasDynamicLabel = false;
        }
    }

    const bool suppress = bindingResolved && hasDynamicLabel;
    if (suppress)
        InterlockedIncrement(&g_suppressedPromptNameCalls);

    if (g_logEveryPromptCall || callNumber <= g_logFirstPromptCalls)
    {
        wchar_t iconName[32] = L"";
        ControllerIconName(resolvedIcon, iconName, std::size(iconName));
        Log(L"[键名提示] #%ld player=%d action=%d mode=%ls original=%ls dynamic=%d binding=%ls suppress=%d。",
            static_cast<long>(callNumber),
            playerSlot,
            actionId,
            controllerMode ? L"Controller" : L"KeyboardMouse",
            originalPreview,
            hasDynamicLabel ? 1 : 0,
            iconName,
            suppress ? 1 : 0);
    }

    static const wchar_t kEmptyPromptName[] = L"";
    return suppress ? kEmptyPromptName : original;
}

void LogTokenRendererArgumentSnapshot(LONG callNumber,
                                      void* self,
                                      uintptr_t caller,
                                      const std::array<uintptr_t, 9>& arguments)
{
    if (g_logFirstEventArgumentSnapshots <= 0 ||
        callNumber > g_logFirstEventArgumentSnapshots)
    {
        return;
    }

    Log(L"[事件参数] #%ld caller=%08X self=%p a1=%08X a2=%08X a3=%08X a4=%08X a5=%08X a6=%08X a7=%08X a8=%08X a9=%08X。",
        static_cast<long>(callNumber),
        static_cast<unsigned>(caller),
        self,
        static_cast<unsigned>(arguments[0]),
        static_cast<unsigned>(arguments[1]),
        static_cast<unsigned>(arguments[2]),
        static_cast<unsigned>(arguments[3]),
        static_cast<unsigned>(arguments[4]),
        static_cast<unsigned>(arguments[5]),
        static_cast<unsigned>(arguments[6]),
        static_cast<unsigned>(arguments[7]),
        static_cast<unsigned>(arguments[8]));

    wchar_t memorySummary[1200] = L"";
    for (size_t index = 0; index < arguments.size(); ++index)
    {
        const uintptr_t address = arguments[index];
        uint32_t values[4] = {};
        bool readable = address >= 0x10000u;
        for (size_t word = 0; readable && word < std::size(values); ++word)
        {
            readable = SafeReadUInt32(reinterpret_cast<const void*>(address + word * 4u),
                                      &values[word]);
        }
        if (!readable)
            continue;

        wchar_t item[128] = L"";
        _snwprintf_s(item,
                     std::size(item),
                     _TRUNCATE,
                     L"a%u@%08X=[%08X,%08X,%08X,%08X] ",
                     static_cast<unsigned>(index + 1),
                     static_cast<unsigned>(address),
                     values[0], values[1], values[2], values[3]);
        wcsncat_s(memorySummary, std::size(memorySummary), item, _TRUNCATE);
    }
    if (memorySummary[0] != L'\0')
        Log(L"[事件参数内存] #%ld %ls", static_cast<long>(callNumber), memorySummary);
}


void AppendTextRunList(const SequenceInspection& inspection,
                       wchar_t* output,
                       size_t outputCount)
{
    if (!output || outputCount == 0)
        return;
    output[0] = L'\0';
    for (int index = 0; index < inspection.textRunCount; ++index)
    {
        if (index > 0)
            wcsncat_s(output, outputCount, L"|", _TRUNCATE);
        wcsncat_s(output,
                  outputCount,
                  inspection.textRuns[static_cast<size_t>(index)].data(),
                  _TRUNCATE);
    }
    if (inspection.textRunCount == 0)
        wcsncpy_s(output, outputCount, L"<none>", _TRUNCATE);
}

bool ReadQteLetterAtAddress(uintptr_t address,
                            wchar_t* output,
                            size_t outputCount)
{
    if (!output || outputCount < 2 || address < 0x10000u)
        return false;
    output[0] = L'\0';

    uint8_t bytes[2] = {};
    for (size_t index = 0; index < std::size(bytes); ++index)
    {
        if (!SafeReadByte(reinterpret_cast<const void*>(address + index), &bytes[index]))
            return false;
    }

    // ANSI 与 UTF-16LE 的单字符 "A"/"D" 都以 [0x41/0x44, 0x00] 开头。
    // 诊断只需要识别候选，不要求在此处区分两种字符串编码。
    if ((bytes[0] == 'A' || bytes[0] == 'D') && bytes[1] == 0)
    {
        output[0] = static_cast<wchar_t>(bytes[0]);
        output[1] = L'\0';
        return true;
    }
    return false;
}

bool InspectQteArgumentPointers(const std::array<uintptr_t, 9>& arguments,
                                wchar_t* output,
                                size_t outputCount)
{
    if (!output || outputCount == 0)
        return false;
    output[0] = L'\0';
    bool found = false;

    const auto appendHit = [&](size_t argumentIndex,
                               const wchar_t* source,
                               size_t offset,
                               const wchar_t* letter)
    {
        wchar_t item[96] = L"";
        if (offset == static_cast<size_t>(-1))
        {
            _snwprintf_s(item,
                         std::size(item),
                         _TRUNCATE,
                         L"a%u:%ls=%ls ",
                         static_cast<unsigned>(argumentIndex + 1),
                         source,
                         letter);
        }
        else
        {
            _snwprintf_s(item,
                         std::size(item),
                         _TRUNCATE,
                         L"a%u:%ls+0x%02X=%ls ",
                         static_cast<unsigned>(argumentIndex + 1),
                         source,
                         static_cast<unsigned>(offset),
                         letter);
        }
        wcsncat_s(output, outputCount, item, _TRUNCATE);
        found = true;
    };

    for (size_t argumentIndex = 0; argumentIndex < arguments.size(); ++argumentIndex)
    {
        const uintptr_t value = arguments[argumentIndex];

        // 有些字符绘制路径直接把码点作为整数参数传入，而不是传字符串指针。
        if (value == static_cast<uintptr_t>('A') || value == static_cast<uintptr_t>('D'))
        {
            wchar_t letter[2] = {static_cast<wchar_t>(value), L'\0'};
            appendHit(argumentIndex, L"scalar", static_cast<size_t>(-1), letter);
        }

        if (value < 0x10000u)
            continue;

        // 参数本身或其前 64 字节对象内可能保存内嵌 ANSI/UTF-16 单字符。
        for (size_t offset = 0; offset < 64; ++offset)
        {
            wchar_t letter[2] = L"";
            if (!ReadQteLetterAtAddress(value + offset, letter, std::size(letter)))
                continue;
            appendHit(argumentIndex, L"inline", offset, letter);
            break;
        }

        // 对象也可能保存短标签字符串的一级指针。只检查前 64 字节内对齐的
        // Win32 指针字段，不递归追踪，避免诊断扩大到无关对象图。
        for (size_t offset = 0; offset + sizeof(void*) <= 64; offset += sizeof(void*))
        {
            void* child = nullptr;
            if (!SafeReadPointer(reinterpret_cast<const void*>(value + offset), &child))
                continue;
            const uintptr_t childAddress = reinterpret_cast<uintptr_t>(child);
            wchar_t letter[2] = L"";
            if (!ReadQteLetterAtAddress(childAddress, letter, std::size(letter)))
                continue;
            appendHit(argumentIndex, L"ptr", offset, letter);
            break;
        }

    }
    return found;
}

void FormatSequenceRecordHex(const BYTE* record,
                             wchar_t* output,
                             size_t outputCount)
{
    if (!output || outputCount == 0)
        return;
    output[0] = L'\0';
    if (!record)
        return;

    size_t position = 0;
    for (size_t offset = 0; offset < 0xACu && position + 3 < outputCount; ++offset)
    {
        uint8_t value = 0;
        if (!SafeReadByte(record + offset, &value))
            break;
        const int written = _snwprintf_s(output + position,
                                          outputCount - position,
                                          _TRUNCATE,
                                          L"%02X",
                                          static_cast<unsigned>(value));
        if (written <= 0)
            break;
        position += static_cast<size_t>(written);
    }
}

uint64_t MakeQteCandidateHash(uintptr_t caller,
                              int sequenceIndex,
                              const SequenceInspection& inspection,
                              const wchar_t* argumentHits)
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(caller);
    mix(static_cast<uint32_t>(sequenceIndex));
    for (int index = 0; index < inspection.textRunCount; ++index)
    {
        const wchar_t* text = inspection.textRuns[static_cast<size_t>(index)].data();
        while (*text)
            mix(static_cast<uint16_t>(*text++));
    }
    if (argumentHits)
    {
        while (*argumentHits)
            mix(static_cast<uint16_t>(*argumentHits++));
    }
    return hash;
}

bool MarkQteCandidateLogged(uint64_t hash)
{
    if (!hash)
        return false;
    for (uint64_t existing : g_loggedQteCandidates)
    {
        if (existing == hash)
            return false;
    }
    for (uint64_t& slot : g_loggedQteCandidates)
    {
        if (slot == 0)
        {
            slot = hash;
            return true;
        }
    }
    return false;
}

void RunQteDiagnostic(uintptr_t caller,
                      int sequenceIndex,
                      uintptr_t segmentAddress,
                      const std::array<uintptr_t, 9>& arguments,
                      const SequenceInspection& inspection)
{
    InterlockedIncrement(&g_qteDiagnosticCalls);
    if (!g_enableQteDiagnosticHook)
        return;

    bool controllerMode =
        InterlockedCompareExchange(&g_controllerModeActive, 0, 0) != 0;
    if (!controllerMode)
    {
        bool directMode = false;
        if (ReadControllerModeForPlayer(0, &directMode))
            controllerMode = directMode;
    }
    if (!controllerMode)
        return;

    wchar_t argumentHits[512] = L"";
    const bool hasArgumentHit =
        InspectQteArgumentPointers(arguments, argumentHits, std::size(argumentHits));
    if (inspection.standaloneAorDCount <= 0 && !hasArgumentHit)
        return;

    InterlockedIncrement(&g_qteCandidateCalls);
    const uint64_t hash = MakeQteCandidateHash(caller,
                                               sequenceIndex,
                                               inspection,
                                               argumentHits);
    if (!MarkQteCandidateLogged(hash))
        return;
    const LONG candidateNumber = InterlockedIncrement(&g_qteUniqueCandidates);
    if (candidateNumber > g_logFirstQteCandidates)
        return;

    wchar_t cores[320] = L"";
    wchar_t textRuns[1024] = L"";
    AppendCoreList(inspection, cores, std::size(cores));
    AppendTextRunList(inspection, textRuns, std::size(textRuns));
    Log(L"[QTE候选] #%ld caller=%08X sequence=%d segment=%p standaloneAD=%d cores=%ls text=%ls args=%ls。",
        static_cast<long>(candidateNumber),
        static_cast<unsigned>(caller),
        sequenceIndex,
        reinterpret_cast<void*>(segmentAddress),
        inspection.standaloneAorDCount,
        cores,
        textRuns,
        hasArgumentHit ? argumentHits : L"<none>");

    if (g_logQteRecordHex)
    {
        wchar_t recordHex[0xACu * 2u + 1u] = L"";
        FormatSequenceRecordHex(GetSequenceRecordPointer(sequenceIndex),
                                recordHex,
                                std::size(recordHex));
        Log(L"[QTE记录] #%ld sequence=%d bytes=%ls。",
            static_cast<long>(candidateNumber),
            sequenceIndex,
            recordHex[0] ? recordHex : L"<unavailable>");
    }
}

uint64_t MakeRenderEventHash(uintptr_t parentCaller,
                             int sequenceIndex,
                             int actionId,
                             ControllerIcon icon,
                             const SequenceInspection& inspection)
{
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value)
    {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(parentCaller);
    mix(static_cast<uint32_t>(sequenceIndex));
    mix(static_cast<uint32_t>(actionId));
    mix(static_cast<uint32_t>(static_cast<int>(icon)));
    for (int index = 0; index < inspection.coreCount; ++index)
    {
        const wchar_t* core = inspection.cores[static_cast<size_t>(index)].data();
        while (*core)
            mix(static_cast<uint16_t>(*core++));
    }
    return hash;
}

bool MarkRenderEventLogged(uint64_t hash)
{
    if (!hash)
        return false;
    for (uint64_t existing : g_loggedRenderEvents)
    {
        if (existing == hash)
            return false;
    }
    for (uint64_t& slot : g_loggedRenderEvents)
    {
        if (slot == 0)
        {
            slot = hash;
            return true;
        }
    }
    return false;
}

float __fastcall HookTokenRenderer(void* self,
                                   void*,
                                   uintptr_t arg1,
                                   uintptr_t arg2,
                                   uintptr_t arg3,
                                   uintptr_t arg4,
                                   uintptr_t arg5,
                                   uintptr_t arg6,
                                   uintptr_t arg7,
                                   uintptr_t arg8,
                                   uintptr_t arg9)
{
    const LONG callNumber = InterlockedIncrement(&g_eventRenderCalls);
    const int sequenceIndex = ReadSegmentSequenceIndex(arg7);
    // 必须在 HookTokenRenderer 本体内展开；若封装成普通辅助函数，读取到的只会是
    // Hook 内部调用辅助函数后的返回地址，而不是游戏调用 E85740 的地址。
    const uintptr_t parentCaller = CAPTURE_TOKEN_RENDERER_CALLER();
    const std::array<uintptr_t, 9> arguments = {
        arg1, arg2, arg3, arg4, arg5, arg6, arg7, arg8, arg9
    };
    LogTokenRendererArgumentSnapshot(callNumber, self, parentCaller, arguments);

    SequenceInspection inspection = {};
    ReadSequenceInspection(sequenceIndex, &inspection);
    RunQteDiagnostic(parentCaller,
                     sequenceIndex,
                     arg7,
                     arguments,
                     inspection);

    ControllerIcon icon = ControllerIcon::Invalid;
    int actionId = -1;
    const wchar_t* matchSource = L"Unmatched";
    DWORD carrierAge = 0;
    int carrierRemaining = 0;

    // 掩体教程借用 SCROLLLOCK / NUMLOCK 两个无动态文字的预制载体。
    // GetPromptIcon 必须刚刚以 action 38/39 签发对应载体；普通 LS_UP/LS_DOWN、
    // 黑客 QTE 以及其他 token 均不能进入该事件纹理分支。
    const ControllerIcon carrierIcon = FindDistinctCarrierIcon(inspection);
    if (g_enableCoverStaticFix && IsCoverCarrierIcon(carrierIcon))
    {
        int pendingAction = -1;
        if (ConsumePendingCarrier(carrierIcon,
                                  &pendingAction,
                                  &carrierAge,
                                  &carrierRemaining))
        {
            const ControllerIcon coverDisplayIcon =
                CoverDisplayIconForCarrier(pendingAction, carrierIcon);
            if (coverDisplayIcon != ControllerIcon::Invalid)
            {
                icon = coverDisplayIcon;
                actionId = pendingAction;
                matchSource = L"CoverStaticCarrier";
                InterlockedIncrement(&g_coverStaticRendered);
            }
        }
        if (icon == ControllerIcon::Invalid)
        {
            matchSource = L"UnarmedCoverStatic";
            InterlockedIncrement(&g_promptCarrierUnarmed);
        }
    }

    // 旧通用事件渲染器保持默认关闭；只有显式启用时才允许处理其他载体。
    if (icon == ControllerIcon::Invalid && g_enableEventRendererHook &&
        carrierIcon != ControllerIcon::Invalid)
    {
        if (ConsumePendingCarrier(carrierIcon,
                                  &actionId,
                                  &carrierAge,
                                  &carrierRemaining))
        {
            icon = carrierIcon;
            matchSource = L"DistinctCarrier";
        }
        else
        {
            matchSource = L"UnarmedCarrier";
            InterlockedIncrement(&g_promptCarrierUnarmed);
        }
    }

    if (icon == ControllerIcon::Invalid &&
        !g_enableEventRendererHook)
    {
        return g_originalTokenRenderer(self,
                                       arg1,
                                       arg2,
                                       arg3,
                                       arg4,
                                       arg5,
                                       arg6,
                                       arg7,
                                       arg8,
                                       arg9);
    }

    // 当前实机证据尚不能证明 arg7+8 是全局 FontSequence 索引。保留该路线用于
    // 诊断，但默认关闭，避免把普通 SPACE 误判为 WDRAW/LSC。
    if (icon == ControllerIcon::Invalid &&
        g_enableEventRendererHook &&
        g_enableSequenceAliasMatching)
    {
        const EventRule* rule = FindEventRule(inspection);
        if (rule)
        {
            bool ambiguous = false;
            BindingResolution resolution = {};
            const ControllerIcon resolved = ResolveControllerIconForAction(rule->actionId,
                                                                            &resolution,
                                                                            &ambiguous);
            if (resolved != ControllerIcon::Invalid && !ambiguous)
            {
                icon = resolved;
                actionId = rule->actionId;
                matchSource = L"AliasBinding";
            }
        }
    }

    const RenderEventContext previous = CurrentRenderEventContext();
    CurrentRenderEventContext().depth = previous.depth + 1;
    CurrentRenderEventContext().icon = icon;
    CurrentRenderEventContext().actionId = actionId;
    CurrentRenderEventContext().sequenceIndex = sequenceIndex;
    CurrentRenderEventContext().parentCaller = parentCaller;
    CurrentRenderEventContext().drawCalls = 0;
    CurrentRenderEventContext().drawOverrides = 0;
    CurrentRenderEventContext().drawFailures = 0;

    if (icon != ControllerIcon::Invalid)
    {
        InterlockedIncrement(&g_eventMatchedCalls);
        CurrentDeferredEventDrawHint().icon = icon;
        CurrentDeferredEventDrawHint().actionId = actionId;
        CurrentDeferredEventDrawHint().tick = GetTickCount();
        CurrentDeferredEventDrawHint().remainingAtlasDraws = g_deferredAtlasDrawBudget;
        InterlockedIncrement(&g_deferredDrawHintsCreated);
    }

    EventTextureOverrideState textureOverride = {};
    const bool overrideApplied = BeginEventTextureOverride(icon, &textureOverride, true);

    const uint64_t eventHash = MakeRenderEventHash(parentCaller,
                                                    sequenceIndex,
                                                    actionId,
                                                    icon,
                                                    inspection);
    const bool newEvent = MarkRenderEventLogged(eventHash);
    if (g_logEveryEventRender ||
        callNumber <= g_logFirstEventRenders ||
        newEvent)
    {
        wchar_t cores[320] = L"";
        wchar_t iconName[32] = L"";
        AppendCoreList(inspection, cores, std::size(cores));
        ControllerIconName(icon, iconName, std::size(iconName));
        Log(L"[事件渲染] #%ld caller=%08X sequence=%d segment=%p cores=%ls source=%ls action=%d icon=%ls carrierAge=%lu pending=%d active=%d stage=%lu。",
            static_cast<long>(callNumber),
            static_cast<unsigned>(parentCaller),
            sequenceIndex,
            reinterpret_cast<void*>(arg7),
            cores,
            matchSource,
            actionId,
            iconName,
            static_cast<unsigned long>(carrierAge),
            carrierRemaining,
            overrideApplied ? 1 : 0,
            static_cast<unsigned long>(textureOverride.stage));
        if (newEvent && icon == ControllerIcon::Invalid)
            InterlockedIncrement(&g_eventUnmatchedLogged);
    }

    const float result = g_originalTokenRenderer(self,
                                                 arg1,
                                                 arg2,
                                                 arg3,
                                                 arg4,
                                                 arg5,
                                                 arg6,
                                                 arg7,
                                                 arg8,
                                                 arg9);

    const int eventDrawCalls = CurrentRenderEventContext().drawCalls;
    const int eventDrawOverrides = CurrentRenderEventContext().drawOverrides;
    const int eventDrawFailures = CurrentRenderEventContext().drawFailures;
    if (eventDrawOverrides > 0 &&
        CurrentDeferredEventDrawHint().actionId == actionId &&
        CurrentDeferredEventDrawHint().icon == icon)
    {
        // 已在 TokenRenderer 内真正覆盖过图集，无需把同一事件带到函数外。
        CurrentDeferredEventDrawHint() = {};
    }
    const int deferredBudget = CurrentDeferredEventDrawHint().remainingAtlasDraws;
    const bool preBindRestored = EndEventTextureOverride(&textureOverride);
    if (icon != ControllerIcon::Invalid &&
        (g_logEveryEventRender || callNumber <= g_logFirstEventRenders || newEvent))
    {
        Log(L"[事件绘制] #%ld action=%d icon=%d prebind=%d prebindRestore=%d draws=%d drawOverrides=%d drawFailures=%d deferredBudget=%d。",
            static_cast<long>(callNumber),
            actionId,
            static_cast<int>(icon),
            overrideApplied ? 1 : 0,
            preBindRestored ? 1 : 0,
            eventDrawCalls,
            eventDrawOverrides,
            eventDrawFailures,
            deferredBudget);
    }
    CurrentRenderEventContext() = previous;
    return result;
}

void CapturePadObject(void* self)
{
    if (!self)
        return;
    void* previous = InterlockedExchangePointer(&g_currentPadObject, self);
    if (previous != self)
    {
        InterlockedIncrement(&g_padCaptureChanges);
        Log(L"[输入捕获] 当前手柄对象=%p（previous=%p）。", self, previous);
    }
}

void CaptureActionManager(void* self)
{
    if (!self)
        return;
    void* previous = InterlockedExchangePointer(&g_currentActionManager, self);
    if (previous != self)
    {
        InterlockedIncrement(&g_managerCaptureChanges);
        Log(L"[输入捕获] 当前逻辑动作管理器=%p（previous=%p）。", self, previous);
    }
}

void __fastcall HookPadUpdate(void* self, void*)
{
    g_originalPadUpdate(self);
    CapturePadObject(self);
}

int __fastcall HookActionHeld(void* self, void*, int actionId)
{
    CaptureActionManager(self);
    return g_originalActionHeld(self, actionId);
}

bool WriteRelativeJumpSized(BYTE* source, const void* destination, size_t patchLength)
{
    if (!source || !destination || patchLength < 5)
        return false;

    const intptr_t displacement = reinterpret_cast<const BYTE*>(destination) - (source + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX)
        return false;

    DWORD oldProtection = 0;
    if (!VirtualProtect(source, patchLength, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;

    source[0] = 0xE9;
    *reinterpret_cast<int32_t*>(source + 1) = static_cast<int32_t>(displacement);
    for (size_t index = 5; index < patchLength; ++index)
        source[index] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), source, patchLength);

    DWORD ignoredProtection = 0;
    VirtualProtect(source, patchLength, oldProtection, &ignoredProtection);
    return true;
}

bool BuildInputTrampoline(BYTE* target, size_t patchLength, void** output)
{
    if (!target || !output || patchLength < 5)
        return false;

    const size_t trampolineSize = patchLength + 5;
    BYTE* trampoline = static_cast<BYTE*>(VirtualAlloc(nullptr,
                                                       trampolineSize,
                                                       MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE));
    if (!trampoline)
        return false;

    memcpy(trampoline, target, patchLength);
    trampoline[patchLength] = 0xE9;
    const intptr_t returnDisplacement =
        (target + patchLength) - (trampoline + patchLength + 5);
    if (returnDisplacement < INT32_MIN || returnDisplacement > INT32_MAX)
    {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return false;
    }
    *reinterpret_cast<int32_t*>(trampoline + patchLength + 1) =
        static_cast<int32_t>(returnDisplacement);

    FlushInstructionCache(GetCurrentProcess(), trampoline, trampolineSize);
    *output = trampoline;
    g_inputTrampolines.push_back(trampoline);
    return true;
}

bool FunctionHeadMatches(uintptr_t rva, const BYTE* expected, size_t length)
{
    if (!expected || length < 5)
        return false;
    BYTE* target = reinterpret_cast<BYTE*>(g_base + rva);
    __try
    {
        return memcmp(target, expected, length) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename FunctionPointer>
bool InstallInputDetour(const wchar_t* name,
                        uintptr_t rva,
                        const BYTE* expected,
                        size_t patchLength,
                        const void* hook,
                        FunctionPointer* original)
{
    BYTE* target = reinterpret_cast<BYTE*>(g_base + rva);
    if (!FunctionHeadMatches(rva, expected, patchLength))
        return false;

    void* trampoline = nullptr;
    if (!BuildInputTrampoline(target, patchLength, &trampoline))
    {
        Log(L"[错误] %ls 创建 trampoline 失败。", name);
        return false;
    }
    // 先发布原函数 trampoline，再写入目标 JMP。这样即使其他线程在 JMP 写入后
    // 立刻进入 Hook，也不会看到空的 g_original* 指针。
    *original = reinterpret_cast<FunctionPointer>(trampoline);
    MemoryBarrier();
    if (!WriteRelativeJumpSized(target, hook, patchLength))
    {
        *original = nullptr;
        Log(L"[错误] %ls 写入 JMP 失败。", name);
        return false;
    }

    Log(L"[信息] 已 Hook %ls：%08X -> %08X，Trampoline=%08X。",
        name,
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(target)),
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(hook)),
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(trampoline)));
    return true;
}

void LogRuntimeFunctionHead(const wchar_t* name,
                            uintptr_t rva,
                            size_t byteCount)
{
    if (!name || byteCount == 0 || byteCount > 24)
        return;

    BYTE bytes[24] = {};
    bool readable = true;
    for (size_t index = 0; index < byteCount; ++index)
    {
        if (!SafeReadByte(reinterpret_cast<const void*>(g_base + rva + index),
                          &bytes[index]))
        {
            readable = false;
            break;
        }
    }
    if (!readable)
    {
        Log(L"[QTE入口] %ls RVA=%08X 函数头不可读。",
            name,
            static_cast<unsigned>(rva));
        return;
    }

    wchar_t text[24 * 3 + 1] = L"";
    size_t position = 0;
    for (size_t index = 0; index < byteCount && position + 4 < std::size(text); ++index)
    {
        const int written = _snwprintf_s(text + position,
                                          std::size(text) - position,
                                          _TRUNCATE,
                                          index + 1 < byteCount ? L"%02X " : L"%02X",
                                          static_cast<unsigned>(bytes[index]));
        if (written <= 0)
            break;
        position += static_cast<size_t>(written);
    }
    Log(L"[QTE入口] %ls RVA=%08X head=%ls。",
        name,
        static_cast<unsigned>(rva),
        text);
}

bool IsPromptNameFunctionReady()
{
    return FunctionHeadMatches(kRvaGetPromptName,
                               kExpectedGetPromptNameHead,
                               std::size(kExpectedGetPromptNameHead));
}

void LogPromptNameHeadBytes()
{
    BYTE bytes[16] = {};
    __try
    {
        memcpy(bytes, reinterpret_cast<const void*>(g_base + kRvaGetPromptName), sizeof(bytes));
        Log(L"[诊断] GetPromptName 当前函数头：%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X。",
            bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5], bytes[6], bytes[7],
            bytes[8], bytes[9], bytes[10], bytes[11], bytes[12], bytes[13], bytes[14], bytes[15]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        Log(L"[诊断] GetPromptName 函数头不可读。");
    }
}

bool InstallPromptNameHook()
{
    if (!g_enablePromptNameHook)
    {
        Log(L"[信息] EnablePromptNameHook=0，未安装动态键名抑制 Hook。");
        return true;
    }
    if (!IsPromptNameFunctionReady())
        return false;

    return InstallInputDetour(L"GetPromptName",
                              kRvaGetPromptName,
                              kExpectedGetPromptNameHead,
                              std::size(kExpectedGetPromptNameHead),
                              reinterpret_cast<const void*>(&HookGetPromptName),
                              &g_originalGetPromptName);
}

bool AreInputCaptureFunctionsReady()
{
    return FunctionHeadMatches(kRvaPadUpdate,
                               kExpectedPadUpdateHead,
                               std::size(kExpectedPadUpdateHead)) &&
           FunctionHeadMatches(kRvaActionHeld,
                               kExpectedActionHeldHead,
                               std::size(kExpectedActionHeldHead));
}

bool InstallInputCaptureHooks()
{
    if (!g_useGameControllerBindings)
    {
        Log(L"[信息] UseGameControllerBindings=0，不安装只读输入对象捕获 Hook。 ");
        return true;
    }

    // 先确认两个函数头都已完成运行时解密，再一次性安装，避免只改写一半后
    // 下一轮版本检查无法通过。
    if (!AreInputCaptureFunctionsReady())
        return false;

    if (!InstallInputDetour(L"PadUpdate",
                            kRvaPadUpdate,
                            kExpectedPadUpdateHead,
                            std::size(kExpectedPadUpdateHead),
                            reinterpret_cast<const void*>(&HookPadUpdate),
                            &g_originalPadUpdate))
        return false;

    if (!InstallInputDetour(L"ActionHeld",
                            kRvaActionHeld,
                            kExpectedActionHeldHead,
                            std::size(kExpectedActionHeldHead),
                            reinterpret_cast<const void*>(&HookActionHeld),
                            &g_originalActionHeld))
        return false;

    return true;
}

bool IsEventRendererReady()
{
    return FunctionHeadMatches(kRvaTokenRenderer,
                               kExpectedTokenRendererHead,
                               std::size(kExpectedTokenRendererHead));
}

bool InstallEventRendererHook()
{
    if (!g_enableEventRendererHook && !g_enableQteDiagnosticHook)
    {
        Log(L"[信息] EventRenderer 与 QTE 诊断均关闭，不安装 TokenRenderer Hook。 ");
        return true;
    }
    if (!IsEventRendererReady())
        return false;

    return InstallInputDetour(L"TokenRenderer",
                              kRvaTokenRenderer,
                              kExpectedTokenRendererHead,
                              std::size(kExpectedTokenRendererHead),
                              reinterpret_cast<const void*>(&HookTokenRenderer),
                              &g_originalTokenRenderer);
}

bool WriteRelativeJump(void* source, const void* destination)
{
    if (!source || !destination)
        return false;

    const intptr_t displacement =
        reinterpret_cast<const BYTE*>(destination) -
        (reinterpret_cast<const BYTE*>(source) + 5);
    if (displacement < INT32_MIN || displacement > INT32_MAX)
        return false;

    DWORD oldProtection = 0;
    if (!VirtualProtect(source, 5, PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;

    BYTE* bytes = static_cast<BYTE*>(source);
    bytes[0] = 0xE9;
    *reinterpret_cast<int32_t*>(bytes + 1) = static_cast<int32_t>(displacement);
    FlushInstructionCache(GetCurrentProcess(), source, 5);

    DWORD ignoredProtection = 0;
    VirtualProtect(source, 5, oldProtection, &ignoredProtection);
    return true;
}

bool IsPromptFunctionReady()
{
    BYTE* target = reinterpret_cast<BYTE*>(g_base + kRvaGetPromptIcon);
    __try
    {
        return memcmp(target,
                      kExpectedGetPromptIconHead,
                      sizeof(kExpectedGetPromptIconHead)) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool InstallPromptHook()
{
    if (!g_enablePromptHook)
    {
        Log(L"[信息] EnablePromptHook=0，未安装 GetPromptIcon Hook。 ");
        return true;
    }

    if (!IsPromptFunctionReady())
        return false;

    g_getKeyboardValue = reinterpret_cast<BindingValueFn>(g_base + kRvaGetKeyboardValue);
    g_getMouseValue = reinterpret_cast<BindingValueFn>(g_base + kRvaGetMouseValue);
    g_keyboardIconTable = reinterpret_cast<const wchar_t**>(g_base + kRvaKeyboardIconTable);
    g_mouseIconTable = reinterpret_cast<const wchar_t**>(g_base + kRvaMouseIconTable);

    // 对 24 个专属载体做启动自检。索引只用于取得 token；图集槽必须采用
    // v1.1.0 实机校准值，绝不能由索引推算。token core 从目标 EXE 的实际字符串
    // 动态提取并逐项检查唯一性。
    for (int iconIndex = 0; iconIndex < kControllerIconCount; ++iconIndex)
    {
        const DistinctCarrierSpec& spec = kDistinctCarrierSpecs[static_cast<size_t>(iconIndex)];
        wchar_t* core = g_distinctCarrierCores[static_cast<size_t>(iconIndex)].data();
        const int tableLimit = spec.table == CarrierTable::Mouse ? 7 : 0x90;
        const wchar_t* token = ReadDistinctCarrierToken(spec);
        const bool primaryCellValid =
            spec.cellX >= 0 && spec.cellX < 8 && spec.cellY >= 0 && spec.cellY < 8;
        const bool secondaryDisabled =
            spec.secondaryCellX < 0 && spec.secondaryCellY < 0;
        const bool secondaryCellValid =
            spec.secondaryCellX >= 0 && spec.secondaryCellX < 8 &&
            spec.secondaryCellY >= 0 && spec.secondaryCellY < 8;

        if (spec.tableIndex < 0 ||
            spec.tableIndex >= tableLimit ||
            !primaryCellValid ||
            (!secondaryDisabled && !secondaryCellValid) ||
            !ExtractTokenCore(token,
                              core,
                              g_distinctCarrierCores[static_cast<size_t>(iconIndex)].size()))
        {
            Log(L"[错误] 专属载体自检失败：icon=%d table=%ls index=0x%02X token=%ls primary=(%d,%d) secondary=(%d,%d)。",
                iconIndex,
                CarrierTableName(spec.table),
                spec.tableIndex,
                token ? token : L"<NULL>",
                spec.cellX,
                spec.cellY,
                spec.secondaryCellX,
                spec.secondaryCellY);
            return false;
        }

        for (int previous = 0; previous < iconIndex; ++previous)
        {
            const DistinctCarrierSpec& oldSpec =
                kDistinctCarrierSpecs[static_cast<size_t>(previous)];
            const wchar_t* oldCore =
                g_distinctCarrierCores[static_cast<size_t>(previous)].data();
            if ((oldSpec.table == spec.table &&
                 oldSpec.tableIndex == spec.tableIndex) ||
                wcscmp(oldCore, core) == 0)
            {
                Log(L"[错误] 专属载体重复：icon=%d 与 icon=%d 共用 table/index 或 core=%ls。 ",
                    previous,
                    iconIndex,
                    core);
                return false;
            }
        }

        wchar_t iconName[32] = L"";
        ControllerIconName(static_cast<ControllerIcon>(iconIndex),
                           iconName,
                           std::size(iconName));
        Log(L"[载体校准] icon=%ls table=%ls index=0x%02X core=%ls primary=(%d,%d) secondary=(%d,%d)。",
            iconName,
            CarrierTableName(spec.table),
            spec.tableIndex,
            core,
            spec.cellX,
            spec.cellY,
            spec.secondaryCellX,
            spec.secondaryCellY);
    }

    BYTE* target = reinterpret_cast<BYTE*>(g_base + kRvaGetPromptIcon);
    if (!WriteRelativeJump(target, reinterpret_cast<const void*>(&HookGetPromptIconNaked)))
    {
        Log(L"[错误] 写入 GetPromptIcon JMP 失败。 ");
        return false;
    }

    Log(L"[信息] 已 Hook GetPromptIcon：%08X -> %08X；24 个校准载体自检通过；BACK 已迁移到 WHEELDOWN 槽。",
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(target)),
        static_cast<unsigned>(reinterpret_cast<uintptr_t>(&HookGetPromptIconNaked)));
    return true;
}

constexpr std::array<uint32_t, 256> BuildCrc32Table()
{
    std::array<uint32_t, 256> table = {};
    for (uint32_t index = 0; index < 256; ++index)
    {
        uint32_t value = index;
        for (int bit = 0; bit < 8; ++bit)
            value = (value >> 1) ^ ((value & 1u) ? 0xEDB88320u : 0u);
        table[index] = value;
    }
    return table;
}

constexpr auto kCrc32Table = BuildCrc32Table();

uint32_t ComputeCrc32(const void* data, size_t size)
{
    uint32_t crc = 0xFFFFFFFFu;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t index = 0; index < size; ++index)
        crc = kCrc32Table[(crc ^ bytes[index]) & 0xFFu] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t ComputeQuickChecksum(const void* data, size_t size)
{
    if (!data || size < sizeof(uint32_t))
        return 0;

    const uint32_t* words = static_cast<const uint32_t*>(data);
    const size_t count = size / sizeof(uint32_t);
    uint32_t sum = words[0];
    for (size_t index = 1; index < count; ++index)
    {
        uint32_t temporary = words[index];
        temporary = (sum >> 29) + temporary;
        temporary = (sum >> 17) + temporary;
        sum = (sum << 3) ^ temporary;
    }
    return sum;
}

const wchar_t* D3DFormatName(D3DFORMAT format)
{
    switch (format)
    {
    case D3DFMT_A8R8G8B8: return L"A8R8G8B8";
    case D3DFMT_X8R8G8B8: return L"X8R8G8B8";
    case D3DFMT_A8B8G8R8: return L"A8B8G8R8";
    case D3DFMT_X8B8G8R8: return L"X8B8G8R8";
    case D3DFMT_A4R4G4B4: return L"A4R4G4B4";
    case D3DFMT_A1R5G5B5: return L"A1R5G5B5";
    case D3DFMT_R5G6B5: return L"R5G6B5";
    case D3DFMT_DXT1: return L"DXT1";
    case D3DFMT_DXT2: return L"DXT2";
    case D3DFMT_DXT3: return L"DXT3";
    case D3DFMT_DXT4: return L"DXT4";
    case D3DFMT_DXT5: return L"DXT5";
    case D3DFMT_A8: return L"A8";
    case D3DFMT_L8: return L"L8";
    case D3DFMT_A8L8: return L"A8L8";
    default: return L"UNKNOWN";
    }
}

uint8_t Expand5(uint32_t value)
{
    return static_cast<uint8_t>((value << 3) | (value >> 2));
}

uint8_t Expand6(uint32_t value)
{
    return static_cast<uint8_t>((value << 2) | (value >> 4));
}

Pixel Decode565(uint16_t value)
{
    Pixel pixel = {};
    pixel.r = Expand5((value >> 11) & 0x1F);
    pixel.g = Expand6((value >> 5) & 0x3F);
    pixel.b = Expand5(value & 0x1F);
    pixel.a = 255;
    return pixel;
}

Pixel Interpolate(const Pixel& first, const Pixel& second, int firstWeight, int secondWeight, int divisor)
{
    Pixel result = {};
    result.r = static_cast<uint8_t>((first.r * firstWeight + second.r * secondWeight) / divisor);
    result.g = static_cast<uint8_t>((first.g * firstWeight + second.g * secondWeight) / divisor);
    result.b = static_cast<uint8_t>((first.b * firstWeight + second.b * secondWeight) / divisor);
    result.a = static_cast<uint8_t>((first.a * firstWeight + second.a * secondWeight) / divisor);
    return result;
}

void DecodeDxtColorBlock(const uint8_t* block, bool allowTransparent, Pixel colors[4])
{
    const uint16_t color0 = static_cast<uint16_t>(block[0] | (block[1] << 8));
    const uint16_t color1 = static_cast<uint16_t>(block[2] | (block[3] << 8));
    colors[0] = Decode565(color0);
    colors[1] = Decode565(color1);

    if (!allowTransparent || color0 > color1)
    {
        colors[2] = Interpolate(colors[0], colors[1], 2, 1, 3);
        colors[3] = Interpolate(colors[0], colors[1], 1, 2, 3);
    }
    else
    {
        colors[2] = Interpolate(colors[0], colors[1], 1, 1, 2);
        colors[3] = Pixel{0, 0, 0, 0};
    }
}

void StoreBlockPixel(std::vector<Pixel>& output,
                     UINT width,
                     UINT height,
                     UINT blockX,
                     UINT blockY,
                     UINT localX,
                     UINT localY,
                     const Pixel& pixel)
{
    const UINT x = blockX * 4 + localX;
    const UINT y = blockY * 4 + localY;
    if (x < width && y < height)
        output[static_cast<size_t>(y) * width + x] = pixel;
}

bool DecodeDxtTexture(const D3DLOCKED_RECT& locked,
                      UINT width,
                      UINT height,
                      D3DFORMAT format,
                      std::vector<Pixel>& output)
{
    const UINT blockColumns = (width + 3) / 4;
    const UINT blockRows = (height + 3) / 4;
    const UINT blockBytes = (format == D3DFMT_DXT1) ? 8u : 16u;
    const uint8_t* base = static_cast<const uint8_t*>(locked.pBits);

    for (UINT blockY = 0; blockY < blockRows; ++blockY)
    {
        const uint8_t* row = base + static_cast<size_t>(blockY) * locked.Pitch;
        for (UINT blockX = 0; blockX < blockColumns; ++blockX)
        {
            const uint8_t* block = row + static_cast<size_t>(blockX) * blockBytes;
            Pixel colors[4] = {};
            const uint8_t* colorBlock = (format == D3DFMT_DXT1) ? block : block + 8;
            DecodeDxtColorBlock(colorBlock, format == D3DFMT_DXT1, colors);

            uint8_t alphaValues[16] = {};
            if (format == D3DFMT_DXT1)
            {
                std::fill(std::begin(alphaValues), std::end(alphaValues), uint8_t{0xFF});
            }
            else if (format == D3DFMT_DXT2 || format == D3DFMT_DXT3)
            {
                uint64_t alphaBits = 0;
                memcpy(&alphaBits, block, sizeof(alphaBits));
                for (int pixelIndex = 0; pixelIndex < 16; ++pixelIndex)
                {
                    const uint8_t alpha4 = static_cast<uint8_t>((alphaBits >> (pixelIndex * 4)) & 0xF);
                    alphaValues[pixelIndex] = static_cast<uint8_t>(alpha4 * 17);
                }
            }
            else
            {
                const uint8_t alpha0 = block[0];
                const uint8_t alpha1 = block[1];
                uint8_t palette[8] = {alpha0, alpha1, 0, 0, 0, 0, 0, 0};
                if (alpha0 > alpha1)
                {
                    palette[2] = static_cast<uint8_t>((6 * alpha0 + 1 * alpha1) / 7);
                    palette[3] = static_cast<uint8_t>((5 * alpha0 + 2 * alpha1) / 7);
                    palette[4] = static_cast<uint8_t>((4 * alpha0 + 3 * alpha1) / 7);
                    palette[5] = static_cast<uint8_t>((3 * alpha0 + 4 * alpha1) / 7);
                    palette[6] = static_cast<uint8_t>((2 * alpha0 + 5 * alpha1) / 7);
                    palette[7] = static_cast<uint8_t>((1 * alpha0 + 6 * alpha1) / 7);
                }
                else
                {
                    palette[2] = static_cast<uint8_t>((4 * alpha0 + 1 * alpha1) / 5);
                    palette[3] = static_cast<uint8_t>((3 * alpha0 + 2 * alpha1) / 5);
                    palette[4] = static_cast<uint8_t>((2 * alpha0 + 3 * alpha1) / 5);
                    palette[5] = static_cast<uint8_t>((1 * alpha0 + 4 * alpha1) / 5);
                    palette[6] = 0;
                    palette[7] = 255;
                }

                uint64_t alphaIndexBits = 0;
                for (int byteIndex = 0; byteIndex < 6; ++byteIndex)
                    alphaIndexBits |= static_cast<uint64_t>(block[2 + byteIndex]) << (byteIndex * 8);
                for (int pixelIndex = 0; pixelIndex < 16; ++pixelIndex)
                    alphaValues[pixelIndex] = palette[(alphaIndexBits >> (pixelIndex * 3)) & 0x7];
            }

            uint32_t colorIndices = 0;
            memcpy(&colorIndices, colorBlock + 4, sizeof(colorIndices));
            for (UINT localY = 0; localY < 4; ++localY)
            {
                for (UINT localX = 0; localX < 4; ++localX)
                {
                    const UINT pixelIndex = localY * 4 + localX;
                    const UINT colorIndex = (colorIndices >> (pixelIndex * 2)) & 0x3;
                    Pixel pixel = colors[colorIndex];
                    if (format != D3DFMT_DXT1)
                        pixel.a = alphaValues[pixelIndex];
                    StoreBlockPixel(output,
                                    width,
                                    height,
                                    blockX,
                                    blockY,
                                    localX,
                                    localY,
                                    pixel);
                }
            }
        }
    }

    return true;
}

bool DecodeTextureLevel0(IDirect3DTexture9* texture,
                         const D3DSURFACE_DESC& description,
                         std::vector<Pixel>& output)
{
    output.assign(static_cast<size_t>(description.Width) * description.Height, Pixel{});

    D3DLOCKED_RECT locked = {};
    HRESULT result = texture->LockRect(0, &locked, nullptr, D3DLOCK_READONLY);
    if (FAILED(result))
    {
        // 某些管理纹理驱动不接受 READONLY 标志，再用普通 LockRect 尝试一次。
        result = texture->LockRect(0, &locked, nullptr, 0);
    }
    if (FAILED(result) || !locked.pBits)
        return false;
    if (locked.Pitch <= 0)
    {
        texture->UnlockRect(0);
        return false;
    }

    bool decoded = true;
    const uint8_t* base = static_cast<const uint8_t*>(locked.pBits);

    switch (description.Format)
    {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint8_t* row = base + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < description.Width; ++x)
            {
                const uint8_t* source = row + x * 4;
                Pixel& target = output[static_cast<size_t>(y) * description.Width + x];
                target.b = source[0];
                target.g = source[1];
                target.r = source[2];
                target.a = (description.Format == D3DFMT_X8R8G8B8) ? 255 : source[3];
            }
        }
        break;

    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint8_t* row = base + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < description.Width; ++x)
            {
                const uint8_t* source = row + x * 4;
                Pixel& target = output[static_cast<size_t>(y) * description.Width + x];
                target.r = source[0];
                target.g = source[1];
                target.b = source[2];
                target.a = (description.Format == D3DFMT_X8B8G8R8) ? 255 : source[3];
            }
        }
        break;

    case D3DFMT_A4R4G4B4:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * locked.Pitch);
            for (UINT x = 0; x < description.Width; ++x)
            {
                const uint16_t value = row[x];
                Pixel& target = output[static_cast<size_t>(y) * description.Width + x];
                target.a = static_cast<uint8_t>(((value >> 12) & 0xF) * 17);
                target.r = static_cast<uint8_t>(((value >> 8) & 0xF) * 17);
                target.g = static_cast<uint8_t>(((value >> 4) & 0xF) * 17);
                target.b = static_cast<uint8_t>((value & 0xF) * 17);
            }
        }
        break;

    case D3DFMT_A1R5G5B5:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * locked.Pitch);
            for (UINT x = 0; x < description.Width; ++x)
            {
                const uint16_t value = row[x];
                Pixel& target = output[static_cast<size_t>(y) * description.Width + x];
                target.a = (value & 0x8000) ? 255 : 0;
                target.r = Expand5((value >> 10) & 0x1F);
                target.g = Expand5((value >> 5) & 0x1F);
                target.b = Expand5(value & 0x1F);
            }
        }
        break;

    case D3DFMT_R5G6B5:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(base + static_cast<size_t>(y) * locked.Pitch);
            for (UINT x = 0; x < description.Width; ++x)
                output[static_cast<size_t>(y) * description.Width + x] = Decode565(row[x]);
        }
        break;

    case D3DFMT_A8:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint8_t* row = base + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < description.Width; ++x)
                output[static_cast<size_t>(y) * description.Width + x] = Pixel{255, 255, 255, row[x]};
        }
        break;

    case D3DFMT_L8:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint8_t* row = base + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < description.Width; ++x)
                output[static_cast<size_t>(y) * description.Width + x] = Pixel{row[x], row[x], row[x], 255};
        }
        break;

    case D3DFMT_A8L8:
        for (UINT y = 0; y < description.Height; ++y)
        {
            const uint8_t* row = base + static_cast<size_t>(y) * locked.Pitch;
            for (UINT x = 0; x < description.Width; ++x)
            {
                const uint8_t luminance = row[x * 2];
                const uint8_t alpha = row[x * 2 + 1];
                output[static_cast<size_t>(y) * description.Width + x] = Pixel{luminance, luminance, luminance, alpha};
            }
        }
        break;

    case D3DFMT_DXT1:
    case D3DFMT_DXT2:
    case D3DFMT_DXT3:
    case D3DFMT_DXT4:
    case D3DFMT_DXT5:
        decoded = DecodeDxtTexture(locked,
                                   description.Width,
                                   description.Height,
                                   description.Format,
                                   output);
        break;

    default:
        decoded = false;
        break;
    }

    texture->UnlockRect(0);
    return decoded;
}

uint8_t CompositeChannel(uint8_t color, uint8_t alpha, uint8_t background, int alphaMode)
{
    if (alphaMode == 0)
        return color; // 直接比较纹理 RGB。

    if (alphaMode == 1)
    {
        // Straight Alpha：RGB 尚未乘 Alpha。
        return static_cast<uint8_t>((color * alpha + background * (255 - alpha) + 127) / 255);
    }

    // Premultiplied Alpha：RGB 已乘 Alpha，只补背景项。
    const int value = color + (background * (255 - alpha) + 127) / 255;
    return static_cast<uint8_t>(std::min(value, 255));
}

double ComputeSignatureScore(const std::vector<Pixel>& pixels,
                             UINT width,
                             UINT height,
                             int alphaMode,
                             bool verticallyFlipped)
{
    if (width != 512 || height != 512)
        return 1.0e30;

    constexpr int blockSize = 512 / kAtlasReferenceSize;
    constexpr uint8_t background[3] = {64, 65, 64};
    double difference = 0.0;
    size_t referenceOffset = 0;

    for (int signatureY = 0; signatureY < kAtlasReferenceSize; ++signatureY)
    {
        for (int signatureX = 0; signatureX < kAtlasReferenceSize; ++signatureX)
        {
            uint64_t sums[3] = {0, 0, 0};
            for (int localY = 0; localY < blockSize; ++localY)
            {
                int sourceY = signatureY * blockSize + localY;
                if (verticallyFlipped)
                    sourceY = 511 - sourceY;

                for (int localX = 0; localX < blockSize; ++localX)
                {
                    const int sourceX = signatureX * blockSize + localX;
                    const Pixel& pixel = pixels[static_cast<size_t>(sourceY) * width + sourceX];
                    sums[0] += CompositeChannel(pixel.r, pixel.a, background[0], alphaMode);
                    sums[1] += CompositeChannel(pixel.g, pixel.a, background[1], alphaMode);
                    sums[2] += CompositeChannel(pixel.b, pixel.a, background[2], alphaMode);
                }
            }

            constexpr uint64_t sampleCount = blockSize * blockSize;
            for (int channel = 0; channel < 3; ++channel)
            {
                const int average = static_cast<int>((sums[channel] + sampleCount / 2) / sampleCount);
                const int reference = kAtlasReferenceRgb[referenceOffset++];
                difference += std::abs(average - reference);
            }
        }
    }

    return difference / static_cast<double>(kAtlasReferenceSize * kAtlasReferenceSize * 3);
}

SimilarityResult ComputeAtlasSimilarity(const std::vector<Pixel>& pixels, UINT width, UINT height)
{
    SimilarityResult best = {};
    for (int alphaMode = 0; alphaMode < 3; ++alphaMode)
    {
        for (int flip = 0; flip < 2; ++flip)
        {
            const double score = ComputeSignatureScore(pixels,
                                                       width,
                                                       height,
                                                       alphaMode,
                                                       flip != 0);
            if (score < best.score)
            {
                best.score = score;
                best.alphaMode = alphaMode;
                best.verticallyFlipped = flip != 0;
            }
        }
    }
    return best;
}

void OverlayIconCell(std::vector<Pixel>& pixels,
                     UINT width,
                     int iconIndex,
                     int cellX,
                     int cellY,
                     bool applyVerticalFlip)
{
    if (cellX < 0 || cellX >= 8 || cellY < 0 || cellY >= 8)
        return;
    if (IsPreservedAtlasCell(cellX, cellY))
    {
        Log(L"[保护] 已拒绝覆盖图集保留单元 (%d,%d)：无限弹药符号。", cellX, cellY);
        return;
    }

    const int destinationX = cellX * 64;
    int destinationY = cellY * 64;
    if (applyVerticalFlip)
        destinationY = 512 - destinationY - 64;

    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
            pixels[static_cast<size_t>(destinationY + y) * width + destinationX + x] = Pixel{0, 0, 0, 0};
    }

    const int styleIndex = CurrentIconStyleIndex();
    const uint8_t* icon = kEmbeddedIconRgba[styleIndex][iconIndex];
    for (int y = 0; y < 64; ++y)
    {
        for (int x = 0; x < 64; ++x)
        {
            const size_t sourceOffset = (static_cast<size_t>(y) * 64 + x) * 4;
            Pixel pixel = {
                icon[sourceOffset + 0],
                icon[sourceOffset + 1],
                icon[sourceOffset + 2],
                icon[sourceOffset + 3]
            };

            if (g_premultiplyEmbeddedIcons)
            {
                pixel.r = static_cast<uint8_t>((pixel.r * pixel.a + 127) / 255);
                pixel.g = static_cast<uint8_t>((pixel.g * pixel.a + 127) / 255);
                pixel.b = static_cast<uint8_t>((pixel.b * pixel.a + 127) / 255);
            }

            pixels[static_cast<size_t>(destinationY + y) * width + destinationX + x] = pixel;
        }
    }
}

void OverlayDistinctCarrierIcon(std::vector<Pixel>& pixels,
                                UINT width,
                                UINT height,
                                bool detectedVerticalFlip,
                                ControllerIcon icon)
{
    if (width != 512 || height != 512)
        return;
    const int iconIndex = static_cast<int>(icon);
    const DistinctCarrierSpec* spec = CarrierSpecForIcon(icon);
    if (iconIndex < 0 || iconIndex >= kControllerIconCount || !spec)
        return;

    const bool applyVerticalFlip = (g_flipAtlasVertically < 0)
                                       ? detectedVerticalFlip
                                       : (g_flipAtlasVertically != 0);
    OverlayIconCell(pixels,
                    width,
                    iconIndex,
                    spec->cellX,
                    spec->cellY,
                    applyVerticalFlip);
    if (spec->secondaryCellX >= 0 && spec->secondaryCellY >= 0)
    {
        OverlayIconCell(pixels,
                        width,
                        iconIndex,
                        spec->secondaryCellX,
                        spec->secondaryCellY,
                        applyVerticalFlip);
    }
}


void OverlayAllCarrierIcons(std::vector<Pixel>& pixels,
                            UINT width,
                            UINT height,
                            bool detectedVerticalFlip)
{
    for (int iconIndex = 0; iconIndex < kControllerIconCount; ++iconIndex)
    {
        OverlayDistinctCarrierIcon(pixels,
                                   width,
                                   height,
                                   detectedVerticalFlip,
                                   static_cast<ControllerIcon>(iconIndex));
    }
}

bool CreateReplacementTexture(IDirect3DDevice9* device,
                              const std::vector<Pixel>& pixels,
                              UINT width,
                              UINT height,
                              IDirect3DTexture9** output)
{
    if (!device || !output || pixels.size() != static_cast<size_t>(width) * height)
        return false;

    *output = nullptr;
    IDirect3DTexture9* replacement = nullptr;
    // 直接调用安装 Hook 前保存的原始 vtable 方法，避免内部创建替换纹理时
    // 再次进入 HookCreateTexture 并被误登记为新的 512x512 游戏候选。
    HRESULT result = E_FAIL;
    if (g_originalDeviceCreateTexture)
    {
        result = g_originalDeviceCreateTexture(device,
                                               width,
                                               height,
                                               1,
                                               0,
                                               D3DFMT_A8R8G8B8,
                                               D3DPOOL_MANAGED,
                                               &replacement,
                                               nullptr);
    }
    else
    {
        result = device->CreateTexture(width,
                                       height,
                                       1,
                                       0,
                                       D3DFMT_A8R8G8B8,
                                       D3DPOOL_MANAGED,
                                       &replacement,
                                       nullptr);
    }
    if (FAILED(result) || !replacement)
    {
        Log(L"[错误] 创建 A8R8G8B8 替换纹理失败：HRESULT=0x%08X。",
            static_cast<unsigned>(result));
        return false;
    }

    D3DLOCKED_RECT locked = {};
    result = replacement->LockRect(0, &locked, nullptr, 0);
    if (FAILED(result) || !locked.pBits)
    {
        Log(L"[错误] 锁定替换纹理失败：HRESULT=0x%08X。",
            static_cast<unsigned>(result));
        replacement->Release();
        return false;
    }

    for (UINT y = 0; y < height; ++y)
    {
        uint8_t* row = static_cast<uint8_t*>(locked.pBits) + static_cast<size_t>(y) * locked.Pitch;
        for (UINT x = 0; x < width; ++x)
        {
            const Pixel& pixel = pixels[static_cast<size_t>(y) * width + x];
            uint8_t* destination = row + x * 4;
            destination[0] = pixel.b;
            destination[1] = pixel.g;
            destination[2] = pixel.r;
            destination[3] = pixel.a;
        }
    }

    replacement->UnlockRect(0);
    *output = replacement;
    return true;
}

bool ShouldPatchCandidate(int candidateIndex,
                          uint32_t sourceCrc32,
                          const SimilarityResult& similarity)
{
    if (g_targetSourceCrc32 != 0)
        return sourceCrc32 == g_targetSourceCrc32;

    if (g_targetCandidateIndex > 0)
        return candidateIndex == g_targetCandidateIndex;

    return g_autoDetectTexture && similarity.score <= g_autoDetectThreshold;
}

HRESULT WINAPI HookD3DXCreateTextureFromFileInMemory(LPDIRECT3DDEVICE9 device,
                                                       LPCVOID sourceData,
                                                       UINT sourceDataSize,
                                                       LPDIRECT3DTEXTURE9* texture)
{
    InterlockedIncrement(&g_textureLoadCalls);

    const HRESULT result = g_originalCreateTextureFromMemory(device,
                                                              sourceData,
                                                              sourceDataSize,
                                                              texture);
    if (FAILED(result) || !texture || !*texture)
        return result;

    D3DSURFACE_DESC description = {};
    if (FAILED((*texture)->GetLevelDesc(0, &description)))
        return result;

    if (description.Width != 512 || description.Height != 512)
        return result;

    const int candidateIndex = static_cast<int>(InterlockedIncrement(&g_512CandidateCalls));

    // CRC 表初始化、候选解码和替换纹理创建都在同一把锁内完成，避免纹理加载线程并发
    // 首次进入时发生静态表竞争，也避免多个候选同时被判定为首个目标。
    EnterCriticalSection(&g_textureLock);

    const uint32_t sourceCrc32 = (sourceData && sourceDataSize)
                                     ? ComputeCrc32(sourceData, sourceDataSize)
                                     : 0;
    const uint32_t quickChecksum = (sourceData && sourceDataSize)
                                       ? ComputeQuickChecksum(sourceData, sourceDataSize)
                                       : 0;

    std::vector<Pixel> pixels;
    if (!DecodeTextureLevel0(*texture, description, pixels))
    {
        Log(L"[候选] #%d：512x512，Format=%ls(0x%08X)，Pool=%d，Levels=%u，SrcBytes=%u，CRC32=0x%08X，Quick=0x%08X；Lock/解码失败。",
            candidateIndex,
            D3DFormatName(description.Format),
            static_cast<unsigned>(description.Format),
            static_cast<int>(description.Pool),
            (*texture)->GetLevelCount(),
            sourceDataSize,
            sourceCrc32,
            quickChecksum);
        LeaveCriticalSection(&g_textureLock);
        return result;
    }

    const SimilarityResult similarity = ComputeAtlasSimilarity(pixels,
                                                               description.Width,
                                                               description.Height);

    if (g_logAll512Candidates || similarity.score <= g_autoDetectThreshold)
    {
        Log(L"[候选] #%d：512x512，Format=%ls(0x%08X)，Pool=%d，Levels=%u，SrcBytes=%u，CRC32=0x%08X，Quick=0x%08X，Score=%.3f，AlphaMode=%d，Flip=%d。",
            candidateIndex,
            D3DFormatName(description.Format),
            static_cast<unsigned>(description.Format),
            static_cast<int>(description.Pool),
            (*texture)->GetLevelCount(),
            sourceDataSize,
            sourceCrc32,
            quickChecksum,
            similarity.score,
            similarity.alphaMode,
            similarity.verticallyFlipped ? 1 : 0);
    }

    const bool alreadyPatched = InterlockedCompareExchange(&g_atlasPatchCount, 0, 0) > 0;
    const bool shouldPatch = ShouldPatchCandidate(candidateIndex, sourceCrc32, similarity) &&
                             (g_patchEveryMatchingAtlas || !alreadyPatched);

    if (shouldPatch)
    {
        // 事件模式禁止在加载时永久替换整张图集。真正的替换只在 TokenRenderer
        // 目标事件调用期间由 SetTexture Hook 选择，因此 D3DX 路径只记录识别结果。
        Log(L"[D3DX] 已识别按键图集候选 #%d，但事件模式不执行全局替换；等待设备 SetTexture 路径。",
            candidateIndex);
    }

    LeaveCriticalSection(&g_textureLock);
    return result;
}

void** FindImportedFunctionSlot(HMODULE module,
                                const char* importedDll,
                                const char* functionName)
{
    if (!module || !importedDll || !functionName)
        return nullptr;

    BYTE* base = reinterpret_cast<BYTE*>(module);
    auto* dosHeader = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        return nullptr;

    auto* ntHeaders = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dosHeader->e_lfanew);
    if (ntHeaders->Signature != IMAGE_NT_SIGNATURE)
        return nullptr;

    const IMAGE_DATA_DIRECTORY& importDirectory =
        ntHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!importDirectory.VirtualAddress)
        return nullptr;

    auto* descriptor = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + importDirectory.VirtualAddress);
    for (; descriptor->Name; ++descriptor)
    {
        const char* dllName = reinterpret_cast<const char*>(base + descriptor->Name);
        if (_stricmp(dllName, importedDll) != 0)
            continue;

        if (!descriptor->OriginalFirstThunk)
            return nullptr;

        const DWORD originalThunkRva = descriptor->OriginalFirstThunk;
        auto* originalThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + originalThunkRva);
        auto* firstThunk = reinterpret_cast<IMAGE_THUNK_DATA32*>(base + descriptor->FirstThunk);

        for (; originalThunk->u1.AddressOfData; ++originalThunk, ++firstThunk)
        {
            if (IMAGE_SNAP_BY_ORDINAL32(originalThunk->u1.Ordinal))
                continue;

            auto* importByName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(
                base + originalThunk->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char*>(importByName->Name), functionName) == 0)
                return reinterpret_cast<void**>(&firstThunk->u1.Function);
        }
    }

    return nullptr;
}

bool PatchPointer(void** slot, void* replacement, void** original)
{
    if (!slot || !replacement || !original)
        return false;

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtection))
        return false;

    *original = *slot;
    *slot = replacement;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    DWORD ignoredProtection = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignoredProtection);
    return true;
}


bool PatchComVtableSlot(void* object,
                        size_t methodIndex,
                        void* replacement,
                        void** original)
{
    if (!object || !replacement || !original)
        return false;

    void** vtable = *reinterpret_cast<void***>(object);
    if (!vtable)
        return false;

    void** slot = &vtable[methodIndex];
    if (*slot == replacement)
        return true;

    // 第二个 D3D9 对象可能使用另一张 vtable，但通常仍指向相同的原始实现。
    // 若槽位既不是已保存的原函数也不是我们的 Hook，说明存在第三方 Hook 冲突；
    // 此时宁可停止，避免覆盖 TexMod、ReShade 等工具的链路。
    if (*original && *slot != *original)
    {
        Log(L"[错误] COM vtable[%u] 已被其他模块修改：Current=%p，Expected=%p，Hook=%p。",
            static_cast<unsigned>(methodIndex),
            *slot,
            *original,
            replacement);
        return false;
    }

    DWORD oldProtection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtection))
        return false;

    if (!*original)
        *original = *slot;
    *slot = replacement;
    FlushInstructionCache(GetCurrentProcess(), slot, sizeof(void*));

    DWORD ignoredProtection = 0;
    VirtualProtect(slot, sizeof(void*), oldProtection, &ignoredProtection);
    return true;
}

DeviceTextureCandidate* FindDeviceTextureCandidateLocked(IDirect3DTexture9* texture)
{
    for (DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
    {
        if (candidate.original == texture)
            return &candidate;
    }
    return nullptr;
}

void ReleaseStyleDependentTexturesLocked(DeviceTextureCandidate& candidate)
{
    for (IDirect3DTexture9*& replacement : candidate.globalReplacements)
    {
        if (replacement)
        {
            replacement->Release();
            replacement = nullptr;
        }
    }

    for (auto& styleReplacements : candidate.eventReplacements)
    {
        for (IDirect3DTexture9*& replacement : styleReplacements)
        {
            if (replacement)
            {
                replacement->Release();
                replacement = nullptr;
            }
        }
    }
}

bool IsCandidateGlobalReplacementLocked(const DeviceTextureCandidate& candidate,
                                        IDirect3DBaseTexture9* texture)
{
    if (!texture)
        return false;
    for (IDirect3DTexture9* replacement : candidate.globalReplacements)
    {
        if (replacement && texture == replacement)
            return true;
    }
    return false;
}

bool IsCandidateEventReplacementLocked(const DeviceTextureCandidate& candidate,
                                       IDirect3DBaseTexture9* texture)
{
    if (!texture)
        return false;
    for (const auto& styleReplacements : candidate.eventReplacements)
    {
        for (IDirect3DTexture9* replacement : styleReplacements)
        {
            if (replacement && texture == replacement)
                return true;
        }
    }
    return false;
}

void ReleaseDeviceTextureCandidatesLocked()
{
    for (DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
    {
        ReleaseStyleDependentTexturesLocked(candidate);
        candidate.sourcePixels.clear();
        candidate.atlasConfirmed = false;
    }
    g_deviceTextureCandidates.clear();
}

bool ApplyResolvedIconStyle(const ResolvedControllerFamily& resolved,
                            IDirect3DDevice9* device)
{
    const int desiredIndex = IconStyleIndexForFamily(resolved.family);
    bool changed = false;
    bool repairedOriginalAtlas = false;
    int previousIndex = 0;

    EnterCriticalSection(&g_textureLock);
    previousIndex = CurrentIconStyleIndex();
    if (previousIndex != desiredIndex)
    {
        // 只切换当前样式索引，不释放三套全局替换纹理。游戏会通过 StateBlock/
        // 内部状态恢复旧图集；保留旧全局指针才能识别并修复这种恢复。事件变体
        // 已限制为每家族最多 LS_LEFT/LS_RIGHT 两张，不再形成大规模纹理缓存。
        InterlockedExchange(&g_iconStyleIndex, desiredIndex);
        changed = true;
    }

    // 即使 family 没有变化也执行低频自愈：图集替换链依赖 SetTexture 时
    // 把原图集换成全局混合图集，但游戏的 StateBlock 可能绕过 Hook 恢复原键盘图集。
    // Auto 模式新增的 750 ms 轮询正好可安全检查并修复该状态，不能只在 family 变化时
    // 执行，否则 Xbox→Xbox（例如 Standard/Stadia）仍可能在 Tips 中停留为键盘图标。
    if (device &&
        InterlockedCompareExchange(&g_controllerModeActive, 0, 0) != 0 &&
        g_originalDeviceSetTexture)
    {
        for (DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
        {
            if (!candidate.hasObservedStage || !candidate.atlasConfirmed)
                continue;

            IDirect3DBaseTexture9* current = nullptr;
            if (FAILED(device->GetTexture(candidate.lastObservedStage, &current)))
                continue;

            IDirect3DTexture9* desiredReplacement =
                candidate.globalReplacements[static_cast<size_t>(desiredIndex)];
            const bool currentIsOriginal = current == candidate.original;
            const bool currentIsKnownGlobal =
                IsCandidateGlobalReplacementLocked(candidate, current);
            const bool wrongGlobalStyle = currentIsKnownGlobal &&
                                          current != desiredReplacement;
            if (current)
                current->Release();

            // 事件专用纹理不在此处强行改写，避免破坏正在进行的单 token 绘制。
            if (!currentIsOriginal && !wrongGlobalStyle)
                continue;

            if (!desiredReplacement)
                desiredReplacement = EnsureGlobalReplacementLocked(device, candidate);
            if (desiredReplacement)
            {
                g_originalDeviceSetTexture(device,
                                           candidate.lastObservedStage,
                                           desiredReplacement);
                repairedOriginalAtlas = repairedOriginalAtlas || currentIsOriginal;
            }

        }
    }
    LeaveCriticalSection(&g_textureLock);

    const bool resolutionChanged =
        g_lastLoggedFamily != static_cast<int>(resolved.family) ||
        g_lastLoggedFamilySource != static_cast<int>(resolved.source) ||
        g_lastLoggedSdlDeviceId != resolved.deviceId;

    LONG repairNumber = 0;
    if (repairedOriginalAtlas)
        repairNumber = InterlockedIncrement(&g_autoAtlasRepairCount);
    const bool logRepair = repairedOriginalAtlas &&
                           (repairNumber <= 8 || (repairNumber % 64) == 0);

    if (changed || logRepair || resolutionChanged)
    {
        Log(L"[自动图标] DeviceID=%u，Family=%ls，Source=%ls，EffectiveType=%d(%hs)，RealType=%d(%hs)，VID=0x%04X，Name=%hs；Style=%ls%s%s。",
            static_cast<unsigned>(resolved.deviceId),
            ControllerPromptFamilyName(resolved.family),
            FamilyResolutionSourceName(resolved.source),
            resolved.effectiveType,
            resolved.effectiveTypeString[0] ? resolved.effectiveTypeString : "(无)",
            resolved.realType,
            resolved.realTypeString[0] ? resolved.realTypeString : "(无)",
            static_cast<unsigned>(resolved.vendor),
            resolved.deviceName[0] ? resolved.deviceName : "(无)",
            IconStyleNameFromIndex(desiredIndex),
            changed ? L"；已切换样式缓存且未释放旧纹理" : L"",
            repairedOriginalAtlas ? L"；已将状态块恢复的原键盘图集重新绑定为手柄图集" : L"");
        g_lastLoggedFamily = static_cast<int>(resolved.family);
        g_lastLoggedFamilySource = static_cast<int>(resolved.source);
        g_lastLoggedSdlDeviceId = resolved.deviceId;
    }
    return changed || repairedOriginalAtlas;
}

void MaybeApplyPublishedAutomaticIconStyle(IDirect3DDevice9* device, bool force)
{
    if (CurrentIconStyleMode() != IconStyleMode::Auto)
        return;

    const DWORD now = GetTickCount();
    if (!force && g_hasAutoStyleApplyTick &&
        !TickIntervalElapsed(now,
                             g_lastAutoStyleApplyTick,
                             kSdlFamilyPollIntervalMs))
    {
        return;
    }

    g_lastAutoStyleApplyTick = now;
    g_hasAutoStyleApplyTick = true;

    // 这里只读取 WorkerThread 已发布的 POD 快照，不调用 GetModuleHandle、
    // GetProcAddress 或任何 SDL API。这样 D3D9 SetTexture Hook 不会进入
    // JoyShim 的延迟初始化、模块扫描或 SDL joystick 锁。
    const ResolvedControllerFamily resolved =
        ReadPublishedAutomaticControllerFamily();
    ApplyResolvedIconStyle(resolved, device);
}

bool ShouldPatchDeviceCandidate(int candidateIndex,
                                uint32_t pixelCrc32,
                                const SimilarityResult& similarity)
{
    if (g_targetPixelCrc32 != 0)
        return pixelCrc32 == g_targetPixelCrc32;

    if (g_targetCandidateIndex > 0)
        return candidateIndex == g_targetCandidateIndex;

    return g_autoDetectTexture && similarity.score <= g_autoDetectThreshold;
}

bool TickIntervalElapsed(DWORD now, DWORD earlier, DWORD interval)
{
    // DWORD 无符号减法天然处理 GetTickCount 约 49.7 天回绕。
    return static_cast<DWORD>(now - earlier) >= interval;
}

bool DeviceCandidateInspectionExpired(const DeviceTextureCandidate& candidate,
                                      DWORD now)
{
    return candidate.hasFirstObservedTick &&
           TickIntervalElapsed(now,
                               candidate.firstObservedTick,
                               kDeviceInspectionTimeoutMs);
}

bool ShouldInspectDeviceTextureCandidateLocked(DeviceTextureCandidate& candidate,
                                               DWORD now)
{
    if (candidate.atlasConfirmed || candidate.permanentlyRejected)
        return false;

    if (!candidate.hasFirstObservedTick)
    {
        candidate.firstObservedTick = now;
        candidate.hasFirstObservedTick = true;
        return false;
    }

    if (DeviceCandidateInspectionExpired(candidate, now))
    {
        candidate.permanentlyRejected = true;
        Log(L"[设备候选] #%d 已排除：首次观察后 %lu ms 仍未匹配，检查次数=%d。",
            candidate.candidateIndex,
            static_cast<unsigned long>(static_cast<DWORD>(now - candidate.firstObservedTick)),
            candidate.inspectionAttempts);
        return false;
    }

    if (candidate.inspectionAttempts >= kDeviceInspectionMaximumAttempts)
    {
        candidate.permanentlyRejected = true;
        Log(L"[设备候选] #%d 已排除：达到按时间限频后的最大检查次数 %d。",
            candidate.candidateIndex,
            kDeviceInspectionMaximumAttempts);
        return false;
    }

    if (candidate.inspectionAttempts == 0)
    {
        return TickIntervalElapsed(now,
                                   candidate.firstObservedTick,
                                   kDeviceInspectionInitialGraceMs);
    }

    return TickIntervalElapsed(now,
                               candidate.lastInspectionTick,
                               kDeviceInspectionIntervalMs);
}

bool ShouldRetireDeviceTextureCandidate(const DeviceTextureCandidate& candidate,
                                        DWORD now)
{
    return candidate.inspectionAttempts >= kDeviceInspectionMaximumAttempts ||
           DeviceCandidateInspectionExpired(candidate, now);
}

IDirect3DTexture9* EnsureGlobalReplacementLocked(IDirect3DDevice9* device,
                                                  DeviceTextureCandidate& candidate)
{
    const int styleIndex = CurrentIconStyleIndex();
    IDirect3DTexture9*& cached =
        candidate.globalReplacements[static_cast<size_t>(styleIndex)];
    if (cached)
        return cached;
    if (!device || candidate.sourcePixels.empty())
        return nullptr;

    std::vector<Pixel> globalPixels = candidate.sourcePixels;
    OverlayAllCarrierIcons(globalPixels,
                           candidate.creationDesc.Width,
                           candidate.creationDesc.Height,
                           candidate.detectedVerticalFlip);
    if (!CreateReplacementTexture(device,
                                  globalPixels,
                                  candidate.creationDesc.Width,
                                  candidate.creationDesc.Height,
                                  &cached))
    {
        return nullptr;
    }

    Log(L"[图集样式] 已创建 %ls 混合图集：Candidate=%d，StyleIndex=%d，Replacement=%p。",
        IconStyleNameFromIndex(styleIndex),
        candidate.candidateIndex,
        styleIndex,
        cached);
    return cached;
}

IDirect3DTexture9* InspectDeviceTextureCandidateLocked(IDirect3DDevice9* device,
                                                        DeviceTextureCandidate& candidate,
                                                        DWORD now)
{
    if (candidate.atlasConfirmed || candidate.permanentlyRejected)
        return nullptr;

    if (g_stopScanningAfterFirstPatch &&
        InterlockedCompareExchange(&g_atlasPatchCount, 0, 0) > 0)
    {
        candidate.permanentlyRejected = true;
        return nullptr;
    }

    candidate.lastInspectionTick = now;
    ++candidate.inspectionAttempts;
    InterlockedIncrement(&g_device512Inspected);

    D3DSURFACE_DESC description = {};
    if (FAILED(candidate.original->GetLevelDesc(0, &description)))
    {
        if (ShouldRetireDeviceTextureCandidate(candidate, now))
            candidate.permanentlyRejected = true;
        Log(L"[设备候选] #%d 第 %d/%d 次检查：GetLevelDesc 失败%s。",
            candidate.candidateIndex,
            candidate.inspectionAttempts,
            kDeviceInspectionMaximumAttempts,
            candidate.permanentlyRejected ? L"；已达到限频重试上限" : L"");
        return nullptr;
    }

    std::vector<Pixel> pixels;
    if (!DecodeTextureLevel0(candidate.original, description, pixels))
    {
        if (ShouldRetireDeviceTextureCandidate(candidate, now))
            candidate.permanentlyRejected = true;
        Log(L"[设备候选] #%d 第 %d/%d 次检查：512x512，Format=%ls(0x%08X)，Usage=0x%08X，Pool=%d；Lock/解码失败%s。",
            candidate.candidateIndex,
            candidate.inspectionAttempts,
            kDeviceInspectionMaximumAttempts,
            D3DFormatName(description.Format),
            static_cast<unsigned>(description.Format),
            static_cast<unsigned>(description.Usage),
            static_cast<int>(description.Pool),
            candidate.permanentlyRejected ? L"；已达到限频重试上限" : L"");
        return nullptr;
    }

    const uint32_t pixelCrc32 = ComputeCrc32(pixels.data(), pixels.size() * sizeof(Pixel));
    const bool changedFromPrevious = candidate.lastPixelCrc32 != 0 &&
                                     candidate.lastPixelCrc32 != pixelCrc32;
    candidate.lastPixelCrc32 = pixelCrc32;

    const SimilarityResult similarity = ComputeAtlasSimilarity(pixels,
                                                               description.Width,
                                                               description.Height);
    Log(L"[设备候选] #%d 第 %d/%d 次检查：Texture=%p，Format=%ls(0x%08X)，Usage=0x%08X，Pool=%d，Levels=%u，PixelCRC32=0x%08X，Changed=%d，Score=%.3f，AlphaMode=%d，Flip=%d。",
        candidate.candidateIndex,
        candidate.inspectionAttempts,
        kDeviceInspectionMaximumAttempts,
        candidate.original,
        D3DFormatName(description.Format),
        static_cast<unsigned>(description.Format),
        static_cast<unsigned>(description.Usage),
        static_cast<int>(description.Pool),
        candidate.original->GetLevelCount(),
        pixelCrc32,
        changedFromPrevious ? 1 : 0,
        similarity.score,
        similarity.alphaMode,
        similarity.verticallyFlipped ? 1 : 0);

    if (ShouldPatchDeviceCandidate(candidate.candidateIndex, pixelCrc32, similarity))
    {
        candidate.sourcePixels = std::move(pixels);
        candidate.detectedVerticalFlip = similarity.verticallyFlipped;

        candidate.atlasConfirmed = true;
        IDirect3DTexture9* installedReplacement =
            EnsureGlobalReplacementLocked(device, candidate);
        if (!installedReplacement)
        {
            candidate.atlasConfirmed = false;
            candidate.sourcePixels.clear();
            candidate.permanentlyRejected = true;
            Log(L"[错误] 候选 #%d 已识别，但创建所选 %ls 图标样式的混合图集失败；已安全放弃该候选。",
                candidate.candidateIndex,
                SelectedIconStyleName());
            return nullptr;
        }

        const LONG patchNumber = InterlockedIncrement(&g_atlasPatchCount);
        Log(L"[成功] 已安装所选 %ls 图标样式的静态/动态混合图集：Candidate=%d，PatchNo=%ld，Original=%p，Replacement=%p，PixelCRC32=0x%08X，Score=%.3f，首次观察后=%lu ms，检查次数=%d。",
            SelectedIconStyleName(),
            candidate.candidateIndex,
            static_cast<long>(patchNumber),
            candidate.original,
            installedReplacement,
            pixelCrc32,
            similarity.score,
            static_cast<unsigned long>(static_cast<DWORD>(now - candidate.firstObservedTick)),
            candidate.inspectionAttempts);
        return installedReplacement;
    }

    // 不再因连续两次像素相同而立即永久排除。候选在首次 SetTexture 后先经历固定
    // 宽限期，之后最多每 100 ms 检查一次；只有达到 5 秒时间窗或 48 次限频检查
    // 才放弃。这样 EnableLog=0/1 走相同的功能时序，日志 I/O 不能再决定检查次数。
    if (ShouldRetireDeviceTextureCandidate(candidate, now))
    {
        candidate.permanentlyRejected = true;
        Log(L"[设备候选] #%d 已排除：限频检查窗口结束，Elapsed=%lu ms，Attempts=%d，LastCRC32=0x%08X，LastScore=%.3f。",
            candidate.candidateIndex,
            static_cast<unsigned long>(static_cast<DWORD>(now - candidate.firstObservedTick)),
            candidate.inspectionAttempts,
            pixelCrc32,
            similarity.score);
    }

    return nullptr;
}

IDirect3DTexture9* EnsureEventReplacementLocked(IDirect3DDevice9* device,
                                                DeviceTextureCandidate& candidate,
                                                ControllerIcon icon)
{
    const int iconIndex = static_cast<int>(icon);
    if (!candidate.atlasConfirmed ||
        candidate.sourcePixels.empty() ||
        iconIndex < 0 ||
        iconIndex >= kControllerIconCount)
    {
        return nullptr;
    }

    // 只有掩体 LS_LEFT/LS_RIGHT 需要把显示图标额外写入另一载体格位。
    // 其余事件纹理与全局混合图集逐像素完全相同，直接复用全局图集，避免
    // 每个图标再常驻一张 512x512 A8R8G8B8 MANAGED 纹理。
    if (!IsCoverDisplayIcon(icon))
        return EnsureGlobalReplacementLocked(device, candidate);

    const int styleIndex = CurrentIconStyleIndex();
    IDirect3DTexture9*& cached =
        candidate.eventReplacements[static_cast<size_t>(styleIndex)]
                                   [static_cast<size_t>(iconIndex)];
    if (cached)
        return cached;

    // 事件变体必须从完整的全局手柄图集语义重建，而不是从原键盘图集只改一格。
    // 这样即使引擎把同一行中的多个提示合并进相邻 draw，A/其他图标也不会在
    // 掩体事件纹理生效期间短暂回退成键盘图标。每种 family 独立缓存。
    std::vector<Pixel> pixels = candidate.sourcePixels;
    OverlayAllCarrierIcons(pixels,
                           candidate.creationDesc.Width,
                           candidate.creationDesc.Height,
                           candidate.detectedVerticalFlip);
    if (IsCoverDisplayIcon(icon))
    {
        const ControllerIcon coverCarrierIcon = CoverCarrierIconForDisplay(icon);
        const DistinctCarrierSpec* coverCarrier = CarrierSpecForIcon(coverCarrierIcon);
        if (!coverCarrier)
            return nullptr;
        const bool applyVerticalFlip = (g_flipAtlasVertically < 0)
                                           ? candidate.detectedVerticalFlip
                                           : (g_flipAtlasVertically != 0);
        OverlayIconCell(pixels,
                        candidate.creationDesc.Width,
                        iconIndex,
                        coverCarrier->cellX,
                        coverCarrier->cellY,
                        applyVerticalFlip);
    }
    if (!CreateReplacementTexture(device,
                                  pixels,
                                  candidate.creationDesc.Width,
                                  candidate.creationDesc.Height,
                                  &cached))
    {
        wchar_t iconName[32] = L"";
        ControllerIconName(icon, iconName, std::size(iconName));
        Log(L"[错误] 创建事件替换纹理失败：Candidate=%d Style=%ls icon=%ls。",
            candidate.candidateIndex,
            IconStyleNameFromIndex(styleIndex),
            iconName);
        return nullptr;
    }

    wchar_t iconName[32] = L"";
    ControllerIconName(icon, iconName, std::size(iconName));
    const ControllerIcon coverCarrierIcon = CoverCarrierIconForDisplay(icon);
    const bool coverEvent = IsCoverDisplayIcon(icon) &&
                            coverCarrierIcon != ControllerIcon::Invalid;
    const ControllerIcon reportedCarrierIcon = coverEvent ? coverCarrierIcon : icon;
    const DistinctCarrierSpec* carrier = CarrierSpecForIcon(reportedCarrierIcon);
    const int carrierIconIndex = static_cast<int>(reportedCarrierIcon);
    const wchar_t* carrierCore =
        (carrierIconIndex >= 0 && carrierIconIndex < kControllerIconCount)
            ? g_distinctCarrierCores[static_cast<size_t>(carrierIconIndex)].data()
            : L"<none>";
    const int effectiveCellX = carrier ? carrier->cellX : -1;
    const int effectiveCellY = carrier ? carrier->cellY : -1;
    Log(L"[事件纹理] 已按需创建：Candidate=%d Style=%ls icon=%ls texture=%p carrier=%ls:0x%02X core=%ls effective=(%d,%d) primary=(%d,%d) secondary=(%d,%d)。",
        candidate.candidateIndex,
        IconStyleNameFromIndex(styleIndex),
        iconName,
        cached,
        carrier ? CarrierTableName(carrier->table) : L"?",
        carrier ? carrier->tableIndex : -1,
        carrierCore && carrierCore[0] ? carrierCore : L"<none>",
        effectiveCellX,
        effectiveCellY,
        carrier ? carrier->cellX : -1,
        carrier ? carrier->cellY : -1,
        carrier ? carrier->secondaryCellX : -1,
        carrier ? carrier->secondaryCellY : -1);
    return cached;
}

bool BeginEventTextureOverride(ControllerIcon icon,
                               EventTextureOverrideState* state,
                               bool allowNullTexture)
{
    if (!state)
        return false;
    *state = {};
    if (icon == ControllerIcon::Invalid || !g_enableTextureHook ||
        !g_originalDeviceSetTexture || !g_activeD3D9Device)
    {
        return false;
    }

    InterlockedIncrement(&g_eventTextureOverrideAttempts);

    IDirect3DDevice9* device = g_activeD3D9Device;
    IDirect3DTexture9* expectedOriginal = nullptr;
    IDirect3DTexture9* expectedGlobal = nullptr;
    IDirect3DTexture9* replacement = nullptr;
    DWORD stage = 0;

    EnterCriticalSection(&g_textureLock);
    for (DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
    {
        if (!candidate.atlasConfirmed || !candidate.hasObservedStage)
            continue;
        expectedGlobal = EnsureGlobalReplacementLocked(device, candidate);
        replacement = EnsureEventReplacementLocked(device, candidate, icon);
        if (!replacement)
            continue;
        replacement->AddRef();
        expectedOriginal = candidate.original;
        stage = candidate.lastObservedStage;
        break;
    }
    LeaveCriticalSection(&g_textureLock);

    if (!replacement)
    {
        InterlockedIncrement(&g_eventTextureOverrideFailures);
        return false;
    }

    IDirect3DBaseTexture9* previous = nullptr;
    const HRESULT getResult = device->GetTexture(stage, &previous);
    if (FAILED(getResult))
    {
        replacement->Release();
        InterlockedIncrement(&g_eventTextureOverrideFailures);
        return false;
    }

    // TokenRenderer 的实机时序证明：进入单个 token 渲染器时，目标 stage 可能合法地
    // 处于 NULL，真正的键盘图集随后由引擎内部状态块恢复。只有函数边界预绑定调用
    // 允许 NULL；DrawPrimitive 最后时刻重绑定必须看到确认图集或事件变体，避免给
    // 本来不使用纹理的几何绘制强行挂上图集。返回后仍恢复调用前的精确状态。
    bool expectedTextureBound = previous == expectedOriginal ||
                                previous == expectedGlobal ||
                                (allowNullTexture && previous == nullptr);
    if (!expectedTextureBound)
    {
        EnterCriticalSection(&g_textureLock);
        for (const DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
        {
            if (previous == candidate.original ||
                IsCandidateGlobalReplacementLocked(candidate, previous) ||
                IsCandidateEventReplacementLocked(candidate, previous))
            {
                expectedTextureBound = true;
                break;
            }
        }
        LeaveCriticalSection(&g_textureLock);
    }
    if (!expectedTextureBound)
    {
        const LONG mismatchNumber = InterlockedIncrement(&g_eventTextureUnexpectedStage);
        if (mismatchNumber <= 16)
        {
            Log(L"[事件纹理] 主动绑定已跳过：stage=%lu 当前纹理=%p，不是确认原图集=%p 或全局图集=%p。",
                static_cast<unsigned long>(stage),
                previous,
                expectedOriginal,
                expectedGlobal);
        }
        if (previous)
            previous->Release();
        replacement->Release();
        InterlockedIncrement(&g_eventTextureOverrideFailures);
        return false;
    }

    const HRESULT setResult = g_originalDeviceSetTexture(device, stage, replacement);
    if (FAILED(setResult))
    {
        if (previous)
            previous->Release();
        replacement->Release();
        InterlockedIncrement(&g_eventTextureOverrideFailures);
        return false;
    }

    state->device = device;
    state->previousTexture = previous;
    state->replacementTexture = replacement;
    state->stage = stage;
    state->active = true;
    InterlockedIncrement(&g_eventTextureOverrideApplied);
    InterlockedIncrement(&g_eventTextureBindings);
    return true;
}

bool EndEventTextureOverride(EventTextureOverrideState* state)
{
    if (!state || !state->active)
        return false;

    const HRESULT restoreResult = g_originalDeviceSetTexture(state->device,
                                                              state->stage,
                                                              state->previousTexture);
    const bool restored = SUCCEEDED(restoreResult);
    if (restored)
        InterlockedIncrement(&g_eventTextureOverrideRestored);
    else
        InterlockedIncrement(&g_eventTextureOverrideFailures);

    if (state->previousTexture)
        state->previousTexture->Release();
    if (state->replacementTexture)
        state->replacementTexture->Release();
    *state = {};
    return restored;
}

void RefreshAtlasPatchCountLocked()
{
    LONG confirmed = 0;
    for (const DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
    {
        if (candidate.atlasConfirmed)
            ++confirmed;
    }
    InterlockedExchange(&g_atlasPatchCount, confirmed);
}

void ResetUnconfirmedManagedCandidateInspectionLocked(DeviceTextureCandidate& candidate)
{
    if (candidate.atlasConfirmed || candidate.creationDesc.Pool != D3DPOOL_MANAGED)
        return;

    candidate.inspectionAttempts = 0;
    candidate.lastPixelCrc32 = 0;
    candidate.firstObservedTick = 0;
    candidate.lastInspectionTick = 0;
    candidate.hasFirstObservedTick = false;
    candidate.permanentlyRejected = false;
}

void CommitSuccessfulDeviceResetLocked(size_t* preservedManaged,
                                       size_t* droppedNonManaged)
{
    size_t preserved = 0;
    size_t dropped = 0;

    for (auto it = g_deviceTextureCandidates.begin();
         it != g_deviceTextureCandidates.end();)
    {
        if (it->creationDesc.Pool == D3DPOOL_MANAGED)
        {
            ++preserved;
            ResetUnconfirmedManagedCandidateInspectionLocked(*it);
            ++it;
            continue;
        }

        // 非 managed 原纹理由游戏持有，Reset 成功后其旧指针不能再被信任。
        // 插件只释放自己拥有的 managed 替换纹理，不对 original 做 AddRef/Release。
        ReleaseStyleDependentTexturesLocked(*it);
        it->sourcePixels.clear();
        it = g_deviceTextureCandidates.erase(it);
        ++dropped;
    }

    RefreshAtlasPatchCountLocked();
    if (preservedManaged)
        *preservedManaged = preserved;
    if (droppedNonManaged)
        *droppedNonManaged = dropped;
}

HRESULT STDMETHODCALLTYPE HookDeviceReset(IDirect3DDevice9* self,
                                           D3DPRESENT_PARAMETERS* presentationParameters)
{
    size_t candidatesBefore = 0;
    size_t confirmedManagedBefore = 0;
    EnterCriticalSection(&g_textureLock);
    candidatesBefore = g_deviceTextureCandidates.size();
    for (const DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
    {
        if (candidate.creationDesc.Pool == D3DPOOL_MANAGED && candidate.atlasConfirmed)
            ++confirmedManagedBefore;
    }
    LeaveCriticalSection(&g_textureLock);

    // 关键：不要在 Reset 前释放/清空 managed 图集。目标游戏原图集和本插件替换
    // 图集均为 D3DPOOL_MANAGED，它们由 D3D9 负责跨 Reset 保留。若这里提前遗忘，
    // 游戏不会重新 CreateTexture 原图集，插件也就没有机会重新发现它。
    Log(L"[D3D9] Reset 开始：Candidates=%u，ConfirmedManaged=%u；Reset 前保留全部候选与 managed 替换纹理。",
        static_cast<unsigned>(candidatesBefore),
        static_cast<unsigned>(confirmedManagedBefore));

    const HRESULT result = g_originalDeviceReset(self, presentationParameters);

    if (SUCCEEDED(result))
    {
        size_t preservedManaged = 0;
        size_t droppedNonManaged = 0;
        LONG confirmedAfter = 0;
        EnterCriticalSection(&g_textureLock);
        CommitSuccessfulDeviceResetLocked(&preservedManaged, &droppedNonManaged);
        confirmedAfter = InterlockedCompareExchange(&g_atlasPatchCount, 0, 0);
        LeaveCriticalSection(&g_textureLock);

        Log(L"[D3D9] Reset 成功 HRESULT=0x%08X：保留 managed 候选=%u，清理非 managed 候选=%u，已确认图集=%ld。",
            static_cast<unsigned>(result),
            static_cast<unsigned>(preservedManaged),
            static_cast<unsigned>(droppedNonManaged),
            static_cast<long>(confirmedAfter));
    }
    else
    {
        // Reset 失败意味着设备尚未完成重建。保持候选、sourcePixels 和替换缓存原样，
        // 避免一次失败/试探性 Reset 就永久丢失已经确认的 managed 图集。
        Log(L"[D3D9] Reset 失败 HRESULT=0x%08X：完整保留 Reset 前候选与图集缓存，等待后续重试。",
            static_cast<unsigned>(result));
    }

    return result;
}

HRESULT STDMETHODCALLTYPE HookDeviceCreateTexture(IDirect3DDevice9* self,
                                                   UINT width,
                                                   UINT height,
                                                   UINT levels,
                                                   DWORD usage,
                                                   D3DFORMAT format,
                                                   D3DPOOL pool,
                                                   IDirect3DTexture9** returnedTexture,
                                                   HANDLE* sharedHandle)
{
    InterlockedIncrement(&g_deviceCreateTextureCalls);
    const HRESULT result = g_originalDeviceCreateTexture(self,
                                                          width,
                                                          height,
                                                          levels,
                                                          usage,
                                                          format,
                                                          pool,
                                                          returnedTexture,
                                                          sharedHandle);
    if (FAILED(result) || !returnedTexture || !*returnedTexture)
        return result;

    if (width != 512 || height != 512)
        return result;

    D3DSURFACE_DESC description = {};
    (*returnedTexture)->GetLevelDesc(0, &description);

    EnterCriticalSection(&g_textureLock);
    if (!FindDeviceTextureCandidateLocked(*returnedTexture))
    {
        DeviceTextureCandidate candidate = {};
        candidate.original = *returnedTexture;
        candidate.creationDesc = description;
        candidate.requestedLevels = levels;
        candidate.requestedUsage = usage;
        candidate.candidateIndex = static_cast<int>(InterlockedIncrement(&g_device512Created));
        g_deviceTextureCandidates.push_back(candidate);

        Log(L"[D3D9] 创建 512x512 候选 #%d：Texture=%p，Format=%ls(0x%08X)，Usage=0x%08X，Pool=%d，RequestedLevels=%u，ActualLevels=%u。",
            candidate.candidateIndex,
            candidate.original,
            D3DFormatName(description.Format),
            static_cast<unsigned>(description.Format),
            static_cast<unsigned>(usage),
            static_cast<int>(pool),
            levels,
            candidate.original->GetLevelCount());
    }
    LeaveCriticalSection(&g_textureLock);
    return result;
}

HRESULT STDMETHODCALLTYPE HookDeviceSetTexture(IDirect3DDevice9* self,
                                                DWORD stage,
                                                IDirect3DBaseTexture9* texture)
{
    InterlockedIncrement(&g_deviceSetTextureCalls);
    IDirect3DBaseTexture9* textureToBind = texture;

    // 配置未完成或事件纹理链关闭时完全透传。
    if (InterlockedCompareExchange(&g_configurationReady, 0, 0) == 0 ||
        !g_enableTextureHook)
    {
        return g_originalDeviceSetTexture(self, stage, texture);
    }

    // 自动模式只在已有 D3D9 活动时低频轮询；固定模式仅执行一次轻量分支。
    MaybeApplyPublishedAutomaticIconStyle(self, false);

    if (texture && texture->GetType() == D3DRTYPE_TEXTURE)
    {
        auto* texture2D = static_cast<IDirect3DTexture9*>(texture);
        EnterCriticalSection(&g_textureLock);
        DeviceTextureCandidate* candidate = FindDeviceTextureCandidateLocked(texture2D);
        if (candidate)
        {
            candidate->lastObservedStage = stage;
            candidate->hasObservedStage = true;
            if (!candidate->atlasConfirmed)
            {
                const DWORD now = GetTickCount();
                if (ShouldInspectDeviceTextureCandidateLocked(*candidate, now))
                    InspectDeviceTextureCandidateLocked(self, *candidate, now);
            }

            // 游戏不支持键鼠/手柄热切换；手柄模式直接使用当前 IconStyle 的单套混合图集，键鼠模式
            // 继续使用原图集。事件本身在 GetPromptIcon/GetPromptName 阶段选择目标槽并
            // 清除动态键名，SetTexture 层不再猜测当前事件身份。
            if (candidate->atlasConfirmed &&
                InterlockedCompareExchange(&g_controllerModeActive, 0, 0) != 0)
            {
                IDirect3DTexture9* replacement =
                    EnsureGlobalReplacementLocked(self, *candidate);
                if (replacement)
                {
                    textureToBind = replacement;
                    InterlockedIncrement(&g_eventTextureBindings);
                }
            }
        }
        LeaveCriticalSection(&g_textureLock);
    }

    return g_originalDeviceSetTexture(self, stage, textureToBind);
}

bool IsConfirmedEventAtlasCurrentlyBound()
{
    IDirect3DDevice9* device = g_activeD3D9Device;
    if (!device)
        return false;

    bool matched = false;
    EnterCriticalSection(&g_textureLock);
    for (const DeviceTextureCandidate& candidate : g_deviceTextureCandidates)
    {
        if (!candidate.atlasConfirmed || !candidate.hasObservedStage)
            continue;

        IDirect3DBaseTexture9* current = nullptr;
        if (SUCCEEDED(device->GetTexture(candidate.lastObservedStage, &current)))
        {
            matched = current == candidate.original ||
                      IsCandidateGlobalReplacementLocked(candidate, current) ||
                      IsCandidateEventReplacementLocked(candidate, current);
            if (current)
                current->Release();
        }
        if (matched)
            break;
    }
    LeaveCriticalSection(&g_textureLock);
    return matched;
}

bool BeginDrawEventTextureOverride(EventTextureOverrideState* state)
{
    const bool activeContext = CurrentRenderEventContext().depth > 0 &&
                               CurrentRenderEventContext().icon != ControllerIcon::Invalid;
    ControllerIcon icon = ControllerIcon::Invalid;
    bool deferred = false;

    if (activeContext)
    {
        icon = CurrentRenderEventContext().icon;
        InterlockedIncrement(&g_eventDrawCalls);
        ++CurrentRenderEventContext().drawCalls;
    }
    else if (CurrentDeferredEventDrawHint().icon != ControllerIcon::Invalid &&
             CurrentDeferredEventDrawHint().remainingAtlasDraws > 0)
    {
        const DWORD age = GetTickCount() - CurrentDeferredEventDrawHint().tick;
        if (age <= static_cast<DWORD>(g_deferredDrawLifetimeMs))
        {
            icon = CurrentDeferredEventDrawHint().icon;
            deferred = true;
        }
        else
        {
            CurrentDeferredEventDrawHint() = {};
            InterlockedIncrement(&g_deferredDrawHintsExpired);
        }
    }

    if (icon == ControllerIcon::Invalid)
        return false;

    // 不把“TokenRenderer 内存在普通无纹理/其他材质 draw”记作覆盖失败；只有当前
    // stage 确实绑定确认图集或事件变体时，才进入保存—替换—恢复链。
    if (!IsConfirmedEventAtlasCurrentlyBound())
        return false;

    const bool applied = BeginEventTextureOverride(icon, state, false);
    if (applied)
    {
        InterlockedIncrement(&g_eventDrawOverrideApplied);
        if (activeContext)
        {
            ++CurrentRenderEventContext().drawOverrides;
        }
        else if (deferred)
        {
            InterlockedIncrement(&g_deferredDrawOverrideApplied);
            --CurrentDeferredEventDrawHint().remainingAtlasDraws;
            if (CurrentDeferredEventDrawHint().remainingAtlasDraws <= 0)
                CurrentDeferredEventDrawHint() = {};
        }
    }
    else
    {
        InterlockedIncrement(&g_eventDrawOverrideFailures);
        if (activeContext)
            ++CurrentRenderEventContext().drawFailures;
    }
    return applied;
}

void EndDrawEventTextureOverride(EventTextureOverrideState* state)
{
    if (!state || !state->active)
        return;

    if (EndEventTextureOverride(state))
        InterlockedIncrement(&g_eventDrawOverrideRestored);
    else
        InterlockedIncrement(&g_eventDrawOverrideFailures);
}

HRESULT STDMETHODCALLTYPE HookDeviceDrawPrimitive(IDirect3DDevice9* self,
                                                   D3DPRIMITIVETYPE primitiveType,
                                                   UINT startVertex,
                                                   UINT primitiveCount)
{
    InterlockedIncrement(&g_deviceDrawPrimitiveCalls);
    EventTextureOverrideState textureOverride = {};
    BeginDrawEventTextureOverride(&textureOverride);
    const HRESULT result = g_originalDeviceDrawPrimitive(self,
                                                          primitiveType,
                                                          startVertex,
                                                          primitiveCount);
    EndDrawEventTextureOverride(&textureOverride);
    return result;
}

HRESULT STDMETHODCALLTYPE HookDeviceDrawIndexedPrimitive(IDirect3DDevice9* self,
                                                          D3DPRIMITIVETYPE primitiveType,
                                                          INT baseVertexIndex,
                                                          UINT minVertexIndex,
                                                          UINT numVertices,
                                                          UINT startIndex,
                                                          UINT primitiveCount)
{
    InterlockedIncrement(&g_deviceDrawIndexedPrimitiveCalls);
    EventTextureOverrideState textureOverride = {};
    BeginDrawEventTextureOverride(&textureOverride);
    const HRESULT result = g_originalDeviceDrawIndexedPrimitive(self,
                                                                 primitiveType,
                                                                 baseVertexIndex,
                                                                 minVertexIndex,
                                                                 numVertices,
                                                                 startIndex,
                                                                 primitiveCount);
    EndDrawEventTextureOverride(&textureOverride);
    return result;
}

HRESULT STDMETHODCALLTYPE HookDeviceDrawPrimitiveUP(IDirect3DDevice9* self,
                                                     D3DPRIMITIVETYPE primitiveType,
                                                     UINT primitiveCount,
                                                     const void* vertexStreamZeroData,
                                                     UINT vertexStreamZeroStride)
{
    InterlockedIncrement(&g_deviceDrawPrimitiveUPCalls);
    EventTextureOverrideState textureOverride = {};
    BeginDrawEventTextureOverride(&textureOverride);
    const HRESULT result = g_originalDeviceDrawPrimitiveUP(self,
                                                            primitiveType,
                                                            primitiveCount,
                                                            vertexStreamZeroData,
                                                            vertexStreamZeroStride);
    EndDrawEventTextureOverride(&textureOverride);
    return result;
}

HRESULT STDMETHODCALLTYPE HookDeviceDrawIndexedPrimitiveUP(IDirect3DDevice9* self,
                                                            D3DPRIMITIVETYPE primitiveType,
                                                            UINT minVertexIndex,
                                                            UINT numVertices,
                                                            UINT primitiveCount,
                                                            const void* indexData,
                                                            D3DFORMAT indexDataFormat,
                                                            const void* vertexStreamZeroData,
                                                            UINT vertexStreamZeroStride)
{
    InterlockedIncrement(&g_deviceDrawIndexedPrimitiveUPCalls);
    EventTextureOverrideState textureOverride = {};
    BeginDrawEventTextureOverride(&textureOverride);
    const HRESULT result = g_originalDeviceDrawIndexedPrimitiveUP(self,
                                                                   primitiveType,
                                                                   minVertexIndex,
                                                                   numVertices,
                                                                   primitiveCount,
                                                                   indexData,
                                                                   indexDataFormat,
                                                                   vertexStreamZeroData,
                                                                   vertexStreamZeroStride);
    EndDrawEventTextureOverride(&textureOverride);
    return result;
}

bool HookD3D9Device(IDirect3DDevice9* device)
{
    if (!device)
        return false;

    bool success = true;
    success = PatchComVtableSlot(device,
                                 kIDirect3DDevice9ResetIndex,
                                 reinterpret_cast<void*>(&HookDeviceReset),
                                 reinterpret_cast<void**>(&g_originalDeviceReset)) && success;
    success = PatchComVtableSlot(device,
                                 kIDirect3DDevice9CreateTextureIndex,
                                 reinterpret_cast<void*>(&HookDeviceCreateTexture),
                                 reinterpret_cast<void**>(&g_originalDeviceCreateTexture)) && success;
    success = PatchComVtableSlot(device,
                                 kIDirect3DDevice9SetTextureIndex,
                                 reinterpret_cast<void*>(&HookDeviceSetTexture),
                                 reinterpret_cast<void**>(&g_originalDeviceSetTexture)) && success;

    const bool needDrawHooks = g_enableEventRendererHook;
    if (needDrawHooks)
    {
        success = PatchComVtableSlot(device,
                                     kIDirect3DDevice9DrawPrimitiveIndex,
                                     reinterpret_cast<void*>(&HookDeviceDrawPrimitive),
                                     reinterpret_cast<void**>(&g_originalDeviceDrawPrimitive)) && success;
        success = PatchComVtableSlot(device,
                                     kIDirect3DDevice9DrawIndexedPrimitiveIndex,
                                     reinterpret_cast<void*>(&HookDeviceDrawIndexedPrimitive),
                                     reinterpret_cast<void**>(&g_originalDeviceDrawIndexedPrimitive)) && success;
        success = PatchComVtableSlot(device,
                                     kIDirect3DDevice9DrawPrimitiveUPIndex,
                                     reinterpret_cast<void*>(&HookDeviceDrawPrimitiveUP),
                                     reinterpret_cast<void**>(&g_originalDeviceDrawPrimitiveUP)) && success;
        success = PatchComVtableSlot(device,
                                     kIDirect3DDevice9DrawIndexedPrimitiveUPIndex,
                                     reinterpret_cast<void*>(&HookDeviceDrawIndexedPrimitiveUP),
                                     reinterpret_cast<void**>(&g_originalDeviceDrawIndexedPrimitiveUP)) && success;
    }

    if (success)
    {
        g_activeD3D9Device = device;
        Log(L"[D3D9] 已 Hook Device=%p：Reset=%p，CreateTexture=%p，SetTexture=%p，DrawHooks=%d。",
            device,
            reinterpret_cast<void*>(g_originalDeviceReset),
            reinterpret_cast<void*>(g_originalDeviceCreateTexture),
            reinterpret_cast<void*>(g_originalDeviceSetTexture),
            needDrawHooks ? 1 : 0);
    }
    return success;
}

HRESULT STDMETHODCALLTYPE HookD3D9CreateDevice(IDirect3D9* self,
                                                UINT adapter,
                                                D3DDEVTYPE deviceType,
                                                HWND focusWindow,
                                                DWORD behaviorFlags,
                                                D3DPRESENT_PARAMETERS* presentationParameters,
                                                IDirect3DDevice9** returnedDevice)
{
    InterlockedIncrement(&g_createDeviceCallCount);
    const HRESULT result = g_originalD3D9CreateDevice(self,
                                                      adapter,
                                                      deviceType,
                                                      focusWindow,
                                                      behaviorFlags,
                                                      presentationParameters,
                                                      returnedDevice);
    Log(L"[D3D9] CreateDevice：HRESULT=0x%08X，Device=%p，Adapter=%u，Type=%d，Behavior=0x%08X。",
        static_cast<unsigned>(result),
        (SUCCEEDED(result) && returnedDevice) ? *returnedDevice : nullptr,
        adapter,
        static_cast<int>(deviceType),
        static_cast<unsigned>(behaviorFlags));

    if (SUCCEEDED(result) && returnedDevice && *returnedDevice && g_enableTextureHook)
        HookD3D9Device(*returnedDevice);
    return result;
}

bool HookD3D9Object(IDirect3D9* object)
{
    if (!object)
        return false;

    const bool success = PatchComVtableSlot(object,
                                            kIDirect3D9CreateDeviceIndex,
                                            reinterpret_cast<void*>(&HookD3D9CreateDevice),
                                            reinterpret_cast<void**>(&g_originalD3D9CreateDevice));
    if (success)
        Log(L"[D3D9] 已 Hook IDirect3D9::CreateDevice：Object=%p，Original=%p。",
            object,
            reinterpret_cast<void*>(g_originalD3D9CreateDevice));
    return success;
}

IDirect3D9* WINAPI HookDirect3DCreate9(UINT sdkVersion)
{
    InterlockedIncrement(&g_factoryHookCallCount);
    IDirect3D9* object = g_originalDirect3DCreate9(sdkVersion);
    Log(L"[D3D9] Direct3DCreate9(%u) -> %p。", sdkVersion, object);
    if (object && g_enableTextureHook)
        HookD3D9Object(object);
    return object;
}

bool InstallEarlyDirect3DCreate9Hook()
{
    if (g_originalDirect3DCreate9)
        return true;

    if (!g_mainModule)
        g_mainModule = GetModuleHandleW(nullptr);
    if (!g_mainModule)
        return false;
    if (!g_base)
        g_base = reinterpret_cast<uintptr_t>(g_mainModule);

    void** slot = FindImportedFunctionSlot(g_mainModule,
                                           "d3d9.dll",
                                           "Direct3DCreate9");
    if (!slot)
    {
        void** fallback = reinterpret_cast<void**>(g_base + kRvaDirect3DCreate9Iat);
        __try
        {
            if (*fallback)
                slot = fallback;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            slot = nullptr;
        }
    }

    if (!slot)
        return false;

    if (!PatchPointer(slot,
                      reinterpret_cast<void*>(&HookDirect3DCreate9),
                      reinterpret_cast<void**>(&g_originalDirect3DCreate9)))
        return false;

    g_direct3DCreate9IatSlot = slot;
    return g_originalDirect3DCreate9 != nullptr;
}

bool InstallD3D9FactoryHook()
{
    if (!g_enableTextureHook)
    {
        Log(L"[信息] EnableTextureHook=0，不安装 D3D9 设备级纹理 Hook。 ");
        return true;
    }

    if (!InstallEarlyDirect3DCreate9Hook())
    {
        Log(L"[错误] 无法 Hook Direct3DCreate9 IAT。 ");
        return false;
    }

    Log(L"[信息] 已 Hook Direct3DCreate9：IAT=%p，Original=%p，Hook=%p。",
        g_direct3DCreate9IatSlot,
        reinterpret_cast<void*>(g_originalDirect3DCreate9),
        &HookDirect3DCreate9);
    return true;
}

bool InstallTextureHook()
{
    if (!g_enableD3DXFallbackHook)
    {
        Log(L"[信息] EnableD3DXFallbackHook=0，不安装无调用记录的 D3DX 后备 Hook。 ");
        return true;
    }

    void** slot = FindImportedFunctionSlot(g_mainModule,
                                           "d3dx9_42.dll",
                                           "D3DXCreateTextureFromFileInMemory");
    if (!slot)
    {
        // 已知版本的后备 IAT 地址。先确认指针可读并且非空，避免在错误版本上盲写。
        void** fallback = reinterpret_cast<void**>(g_base + kRvaD3DXCreateTextureFromFileInMemoryIat);
        __try
        {
            if (*fallback)
                slot = fallback;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            slot = nullptr;
        }
    }

    if (!slot)
        return false;

    static_assert(sizeof(g_originalCreateTextureFromMemory) == sizeof(void*),
                  "x86 函数指针大小必须与普通指针一致");
    if (!PatchPointer(slot,
                      reinterpret_cast<void*>(&HookD3DXCreateTextureFromFileInMemory),
                      reinterpret_cast<void**>(&g_originalCreateTextureFromMemory)))
    {
        Log(L"[错误] 写入 D3DXCreateTextureFromFileInMemory IAT 失败。 ");
        return false;
    }

    g_createTextureIatSlot = slot;
    if (!g_originalCreateTextureFromMemory)
    {
        Log(L"[错误] 原 D3DXCreateTextureFromFileInMemory 指针为空。 ");
        return false;
    }

    Log(L"[信息] 已 Hook D3DXCreateTextureFromFileInMemory：IAT=%p，Original=%p，Hook=%p。",
        slot,
        reinterpret_cast<void*>(g_originalCreateTextureFromMemory),
        &HookD3DXCreateTextureFromFileInMemory);
    return true;
}

bool InstallHooksWithRetry()
{
    const DWORD startTick = GetTickCount();
    bool promptInstalled = !g_enablePromptHook;
    bool promptNameInstalled = !g_enablePromptNameHook;
    bool inputCaptureInstalled = !g_useGameControllerBindings;
    bool eventRendererInstalled = !(g_enableEventRendererHook || g_enableQteDiagnosticHook);
    bool d3d9FactoryInstalled = !g_enableTextureHook;
    bool d3dxFallbackInstalled = !g_enableD3DXFallbackHook;
    bool wroteWaitingLog = false;

    while (!promptInstalled || !promptNameInstalled || !inputCaptureInstalled || !eventRendererInstalled ||
           !d3d9FactoryInstalled || !d3dxFallbackInstalled)
    {
        if (!inputCaptureInstalled && AreInputCaptureFunctionsReady())
            inputCaptureInstalled = InstallInputCaptureHooks();
        if (!eventRendererInstalled && IsEventRendererReady())
            eventRendererInstalled = InstallEventRendererHook();
        if (!promptInstalled && IsPromptFunctionReady())
            promptInstalled = InstallPromptHook();
        if (!promptNameInstalled && IsPromptNameFunctionReady())
            promptNameInstalled = InstallPromptNameHook();
        if (!d3d9FactoryInstalled)
            d3d9FactoryInstalled = InstallD3D9FactoryHook();
        if (!d3dxFallbackInstalled)
            d3dxFallbackInstalled = InstallTextureHook();

        if (promptInstalled && promptNameInstalled && inputCaptureInstalled && eventRendererInstalled &&
            d3d9FactoryInstalled && d3dxFallbackInstalled)
            return true;

        if (!wroteWaitingLog)
        {
            Log(L"[等待] Hook 目标尚未就绪：PromptIcon=%d，PromptName=%d，InputCapture=%d，EventRenderer=%d，D3D9=%d，D3DXFallback=%d。",
                promptInstalled ? 1 : 0,
                promptNameInstalled ? 1 : 0,
                inputCaptureInstalled ? 1 : 0,
                eventRendererInstalled ? 1 : 0,
                d3d9FactoryInstalled ? 1 : 0,
                d3dxFallbackInstalled ? 1 : 0);
            wroteWaitingLog = true;
        }

        if (GetTickCount() - startTick >= static_cast<DWORD>(g_retryDurationMs))
            break;
        Sleep(static_cast<DWORD>(g_retryIntervalMs));
    }

    if (!promptNameInstalled)
        LogPromptNameHeadBytes();
    Log(L"[错误] 超过重试时间仍未完成 Hook：PromptIcon=%d，PromptName=%d，InputCapture=%d，EventRenderer=%d，D3D9=%d，D3DXFallback=%d。",
        promptInstalled ? 1 : 0,
        promptNameInstalled ? 1 : 0,
        inputCaptureInstalled ? 1 : 0,
        eventRendererInstalled ? 1 : 0,
        d3d9FactoryInstalled ? 1 : 0,
        d3dxFallbackInstalled ? 1 : 0);
    return false;
}

void LoadConfiguration()
{
    const int defaultEnableLog = g_iniPresent ? 1 : 0;
    const wchar_t* defaultIconStyle = g_iniPresent ? L"Xbox" : L"Auto";

    g_enable = ReadIniInt(L"General", L"Enable", 1);
#ifdef BSCN_SINGLE_DLL
    g_enableLog = 1;
#else
    g_enableLog = ReadIniInt(L"General", L"EnableLog", defaultEnableLog) ? 1 : 0;
#endif
    g_requestedIconStyle = ReadIniString(L"General", L"IconStyle", defaultIconStyle);
    TrimWhitespace(&g_requestedIconStyle);
    IconStyleMode configuredMode = ParseIconStyleMode(g_requestedIconStyle,
                                                      &g_iconStyleValueValid);

    const CommandLineStyleSelection commandLine = ParseCommandLineStyleSelection();
    if (commandLine.present)
    {
        g_commandLineStyleOverride = true;
        g_commandLineStyleArgument = commandLine.argument;
        configuredMode = commandLine.mode;
        g_requestedIconStyle = commandLine.argument;
        g_iconStyleValueValid = true;
    }
    else
    {
        g_commandLineStyleOverride = false;
        g_commandLineStyleArgument.clear();
    }

    InterlockedExchange(&g_iconStyleMode, static_cast<LONG>(configuredMode));
    InterlockedExchange(&g_iconStyleIndex, IconStyleIndexForMode(configuredMode));
    g_enablePromptHook = ReadIniInt(L"General", L"EnablePromptHook", 1);
    g_enablePromptNameHook = ReadIniInt(L"General", L"EnablePromptNameHook", 1);
    g_enableTextureHook = ReadIniInt(L"General", L"EnableTextureHook", 1);
    g_enableD3DXFallbackHook = ReadIniInt(L"General", L"EnableD3DXFallbackHook", 0);
    g_fallbackToKeyboardMouse = ReadIniInt(L"General", L"FallbackToKeyboardMouse", 1);
    g_useGameControllerBindings = ReadIniInt(L"General", L"UseGameControllerBindings", 1);
    g_enableEventRendererHook = ReadIniInt(L"General", L"EnableEventRendererHook", 0);
    g_enableQteDiagnosticHook = ReadIniInt(L"General", L"EnableQteDiagnosticHook", 0);
    g_forceControllerMode = ReadIniInt(L"General", L"ForceControllerMode", -1);

    g_enableSequenceAliasMatching = ReadIniInt(L"EventMatching", L"EnableSequenceAliasMatching", 0);
    g_promptHintLifetimeMs = ReadIniInt(L"EventMatching", L"PromptHintLifetimeMs", 1000);
    g_deferredDrawLifetimeMs = ReadIniInt(L"EventMatching", L"DeferredDrawLifetimeMs", 100);
    g_deferredAtlasDrawBudget = ReadIniInt(L"EventMatching", L"DeferredAtlasDrawBudget", 4);

    g_initialDelayMs = ReadIniInt(L"Timing", L"InitialDelayMs", 0);
    g_retryDurationMs = ReadIniInt(L"Timing", L"InstallRetryDurationMs", 30000);
    g_retryIntervalMs = ReadIniInt(L"Timing", L"InstallRetryIntervalMs", 10);

    g_autoDetectTexture = ReadIniInt(L"Texture", L"AutoDetect", 1);
    g_autoDetectThreshold = ReadIniDouble(L"Texture", L"AutoDetectThreshold", 18.0);
    g_targetCandidateIndex = ReadIniInt(L"Texture", L"TargetCandidateIndex", 0);
    g_targetSourceCrc32 = ReadIniUInt32(L"Texture", L"TargetSourceCrc32", 0);
    g_targetPixelCrc32 = ReadIniUInt32(L"Texture", L"TargetPixelCrc32", 0);
    g_stopScanningAfterFirstPatch = ReadIniInt(L"Texture", L"StopScanningAfterFirstPatch", 1);
    g_logAll512Candidates = ReadIniInt(L"Texture", L"LogAll512Candidates", 1);
    g_patchEveryMatchingAtlas = ReadIniInt(L"Texture", L"PatchEveryMatchingAtlas", 1);
    g_premultiplyEmbeddedIcons = ReadIniInt(L"Texture", L"PremultiplyEmbeddedIcons", 0);
    g_flipAtlasVertically = ReadIniInt(L"Texture", L"FlipAtlasVertically", -1);

    g_logFirstPromptCalls = ReadIniInt(L"Debug", L"LogFirstPromptCalls", 128);
    g_logEveryPromptCall = ReadIniInt(L"Debug", L"LogEveryPromptCall", 0);
    g_logFirstBindingResolutions = ReadIniInt(L"Debug", L"LogFirstBindingResolutions", 256);
    g_logEveryBindingResolution = ReadIniInt(L"Debug", L"LogEveryBindingResolution", 0);
    g_logUnknownBindingNodes = ReadIniInt(L"Debug", L"LogUnknownBindingNodes", 1);
    g_logFirstEventRenders = ReadIniInt(L"Debug", L"LogFirstEventRenders", 256);
    g_logEveryEventRender = ReadIniInt(L"Debug", L"LogEveryEventRender", 0);
    g_logFirstEventArgumentSnapshots = ReadIniInt(L"Debug", L"LogFirstEventArgumentSnapshots", 0);
    g_logFirstQteCandidates = ReadIniInt(L"Debug", L"LogFirstQteCandidates", 64);
    g_logQteRecordHex = ReadIniInt(L"Debug", L"LogQteRecordHex", 1);

    g_initialDelayMs = std::max(g_initialDelayMs, 0);
    g_retryDurationMs = std::max(g_retryDurationMs, 1000);
    g_retryIntervalMs = std::clamp(g_retryIntervalMs, 1, 1000);
    g_autoDetectThreshold = std::clamp(g_autoDetectThreshold, 0.0, 255.0);
    g_targetCandidateIndex = std::max(g_targetCandidateIndex, 0);
    g_flipAtlasVertically = std::clamp(g_flipAtlasVertically, -1, 1);
    g_forceControllerMode = std::clamp(g_forceControllerMode, -1, 1);
    g_logFirstPromptCalls = std::max(g_logFirstPromptCalls, 0);
    g_logFirstBindingResolutions = std::max(g_logFirstBindingResolutions, 0);
    g_logFirstEventRenders = std::max(g_logFirstEventRenders, 0);
    g_logFirstEventArgumentSnapshots = std::clamp(g_logFirstEventArgumentSnapshots, 0, 256);
    g_logFirstQteCandidates = std::clamp(g_logFirstQteCandidates, 0, 256);
    g_logQteRecordHex = g_logQteRecordHex ? 1 : 0;
    g_promptHintLifetimeMs = std::clamp(g_promptHintLifetimeMs, 100, 10000);
    g_deferredDrawLifetimeMs = std::clamp(g_deferredDrawLifetimeMs, 1, 500);
    g_deferredAtlasDrawBudget = std::clamp(g_deferredAtlasDrawBudget, 1, 32);
    g_lastLoggedBindingRoot.fill(0);
    g_lastLoggedBindingResult.fill(static_cast<int>(ControllerIcon::Invalid));
    g_lastLoggedBindingSeen.fill(0);
    g_loggedRenderEvents.fill(0);
    g_loggedQteCandidates.fill(0);
    CurrentPendingCarrierStates() = {};
}

bool HideOwnRuntimeImportDirectories(HMODULE module)
{
    if (!module)
        return false;

    __try
    {
        auto* base = reinterpret_cast<BYTE*>(module);
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return false;

        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        {
            return false;
        }

        IMAGE_DATA_DIRECTORY* directories = nt->OptionalHeader.DataDirectory;
        IMAGE_DATA_DIRECTORY& imports = directories[IMAGE_DIRECTORY_ENTRY_IMPORT];

        IMAGE_DATA_DIRECTORY* delayImports = nullptr;
        if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
            delayImports = &directories[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];

        g_runtimeImportShield.originalImportRva = imports.VirtualAddress;
        g_runtimeImportShield.originalImportSize = imports.Size;
        if (delayImports)
        {
            g_runtimeImportShield.originalDelayImportRva = delayImports->VirtualAddress;
            g_runtimeImportShield.originalDelayImportSize = delayImports->Size;
        }

        if (imports.VirtualAddress == 0 && imports.Size == 0 &&
            (!delayImports ||
             (delayImports->VirtualAddress == 0 && delayImports->Size == 0)))
        {
            g_runtimeImportShield.applied = true;
            g_runtimeImportShield.alreadyHidden = true;
            return true;
        }

        // 只改变 Optional Header 所在内存页的保护属性。Windows loader 已经完成
        // IAT 解析；清空目录项不会擦除 FirstThunk，也不会改变任何已解析函数地址。
        DWORD oldProtect = 0;
        if (!VirtualProtect(&nt->OptionalHeader,
                            sizeof(nt->OptionalHeader),
                            PAGE_READWRITE,
                            &oldProtect))
        {
            return false;
        }

        imports.VirtualAddress = 0;
        imports.Size = 0;
        if (delayImports)
        {
            delayImports->VirtualAddress = 0;
            delayImports->Size = 0;
        }

        DWORD ignored = 0;
        const BOOL restored = VirtualProtect(&nt->OptionalHeader,
                                              sizeof(nt->OptionalHeader),
                                              oldProtect,
                                              &ignored);
        FlushInstructionCache(GetCurrentProcess(),
                              &nt->OptionalHeader,
                              sizeof(nt->OptionalHeader));

        g_runtimeImportShield.applied = restored != FALSE;
        g_runtimeImportShield.alreadyHidden = false;
        return restored != FALSE;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool RuntimeImportDirectoriesAreHidden(HMODULE module)
{
    if (!module)
        return false;
    __try
    {
        const auto* base = reinterpret_cast<const BYTE*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR32_MAGIC ||
            nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_IMPORT)
        {
            return false;
        }
        const auto& imports =
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (imports.VirtualAddress != 0 || imports.Size != 0)
            return false;
        if (nt->OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT)
        {
            const auto& delayImports =
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
            if (delayImports.VirtualAddress != 0 || delayImports.Size != 0)
                return false;
        }
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

DWORD WINAPI WorkerThread(LPVOID)
{
    LoadConfiguration();
    Log(L"[信息] %ls.asi v%ls 已加载。INI=%ls，IniPresent=%d，Mode=%ls，ActiveStyle=%ls，CommandLineOverride=%d(%ls)，EnableLog=%d",
        kPluginName,
        kPluginVersion,
        g_iniPath,
        g_iniPresent ? 1 : 0,
        IconStyleModeName(CurrentIconStyleMode()),
        SelectedIconStyleName(),
        g_commandLineStyleOverride ? 1 : 0,
        g_commandLineStyleOverride ? g_commandLineStyleArgument.c_str() : L"(无)",
        g_enableLog);
    Log(L"[信息] ImportShield=%d，ImportDirectoryHidden=%d，OriginalImport=0x%08X/0x%X，OriginalDelayImport=0x%08X/0x%X。",
        g_runtimeImportShield.applied ? 1 : 0,
        RuntimeImportDirectoriesAreHidden(g_selfModule) ? 1 : 0,
        static_cast<unsigned>(g_runtimeImportShield.originalImportRva),
        static_cast<unsigned>(g_runtimeImportShield.originalImportSize),
        static_cast<unsigned>(g_runtimeImportShield.originalDelayImportRva),
        static_cast<unsigned>(g_runtimeImportShield.originalDelayImportSize));
    if (!g_iconStyleValueValid)
    {
        Log(L"[警告] IconStyle=%ls 无效，已回退为 Xbox。支持：Auto、Xbox、PS5、Switch。",
            g_requestedIconStyle.c_str());
    }
    Log(L"[信息] 当前策略：保留原版 Action/图集修复；Auto 读取本 DLL 的 JoyShim SDL 已发布槽位；无独立 SDL3.dll；命令行 -ps/-ns/-xb/-auto 优先；Reset 保留 managed 图集。轮询=%lu ms，确认次数=%d；首次图集宽限=%lu ms，检查间隔=%lu ms，超时=%lu ms，最大检查=%d。",
        static_cast<unsigned long>(kSdlFamilyPollIntervalMs),
        kSdlFamilyStablePollCount,
        static_cast<unsigned long>(kDeviceInspectionInitialGraceMs),
        static_cast<unsigned long>(kDeviceInspectionIntervalMs),
        static_cast<unsigned long>(kDeviceInspectionTimeoutMs),
        kDeviceInspectionMaximumAttempts);

    if (!g_enable)
    {
        Log(L"[信息] Enable=0，插件不执行提示或纹理替换。早期 D3D9 IAT Hook 仅保持透传。 ");
        return 0;
    }
    InterlockedExchange(&g_configurationReady, 1);
    ResolvedControllerFamily automaticFallback = {};
    automaticFallback.family = ControllerPromptFamily::Xbox;
    automaticFallback.source = FamilyResolutionSource::NoSdl;
    PublishAutomaticControllerFamily(automaticFallback);
    MaybeApplyPublishedAutomaticIconStyle(nullptr, true);
    Log(L"[信息] UPX 兼容：ImportShield=%d，StaticTLS=0，DynamicTLS=%d，ThreadNotificationsDisabled=%d，BackgroundThreads=1。",
        RuntimeImportDirectoriesAreHidden(g_selfModule) ? 1 : 0,
        g_threadRuntimeTlsIndex != TLS_OUT_OF_INDEXES ? 1 : 0,
        g_threadLibraryCallsDisabled ? 1 : 0);

    if (g_initialDelayMs > 0)
        Sleep(static_cast<DWORD>(g_initialDelayMs));

    g_mainModule = GetModuleHandleW(nullptr);
    g_base = reinterpret_cast<uintptr_t>(g_mainModule);
    if (!VerifyMainExecutable())
        return 0;

    LogRuntimeFunctionHead(L"GetCurrentControlConfigEmptyButtons",
                           kRvaGetCurrentControlConfigEmptyButtons,
                           16);
    LogRuntimeFunctionHead(L"InputGadgetSetGameButton",
                           kRvaInputGadgetSetGameButton,
                           16);

    if (!InstallHooksWithRetry())
        return 0;

    Log(L"[信息] Hook 安装完成。自动识别=%d，阈值=%.3f，TargetCandidate=%d，TargetPixelCRC32=0x%08X，D3DXFallback=%d，EventRenderer=%d，LegacyCoverStatic=%d，QteDiagnostic=%d。",
        g_autoDetectTexture,
        g_autoDetectThreshold,
        g_targetCandidateIndex,
        g_targetPixelCrc32,
        g_enableD3DXFallbackHook,
        g_enableEventRendererHook,
        g_enableCoverStaticFix,
        g_enableQteDiagnosticHook);
    Log(L"[信息] 混合图标槽：Mode=%ls，ActiveStyle=%ls，AvailableCarrierCount=24，PromptNameHook=%d，ForceControllerMode=%d；SPACE=Y，ESCAPE=START，ARROWLEFT=LT，ARROWRIGHT=RT，LS_LEFT=SCROLLLOCK，LS_RIGHT=NUMLOCK，LS_UP=k1，LS_DOWN=k4，MOUSERIGHT=RS_ALL；无限弹药单元 (3,7) 保持原样。",
        IconStyleModeName(CurrentIconStyleMode()),
        SelectedIconStyleName(),
        g_enablePromptNameHook,
        g_forceControllerMode);
    Log(L"[信息] binding 解析器已启用：事件只决定查询哪个动作，实际按钮仍从当前游戏配置读取。 ");

    if (CurrentIconStyleMode() == IconStyleMode::Auto)
        RunAutomaticFamilyObserverLoop();
    return 0;
}

void WriteFinalStatistics()
{
    Log(L"[统计] SDLObserverQueryFaults=%ld。",
        static_cast<long>(g_sdlObserverQueryFaults));
    Log(L"[统计] PromptIconCalls=%ld，PromptNameCalls=%ld，NamesSuppressed=%ld，Resolved=%ld，Unresolved=%ld，Ambiguous=%ld，Fallback=%ld，Null=%ld，BindingResolutions=%ld，ControllerMode=%ld，ModeReadFailures=%ld，PadCaptureChanges=%ld，ManagerCaptureChanges=%ld，QteProbeCalls=%ld，QteCandidates=%ld，QteUnique=%ld，QteOverrides=%ld，CoverDirectionOverrides=%ld，CoverArmed=%ld，CoverRendered=%ld，Direct3DCreate9=%ld，CreateDevice=%ld，DeviceCreateTexture=%ld，DeviceSetTexture=%ld，Device512Created=%ld，Device512Inspected=%ld，D3DXLoads=%ld，D3DX512=%ld，AtlasInstalled=%ld，GlobalBindings=%ld。",
        static_cast<long>(g_totalPromptCalls),
        static_cast<long>(g_totalPromptNameCalls),
        static_cast<long>(g_suppressedPromptNameCalls),
        static_cast<long>(g_resolvedPromptCalls),
        static_cast<long>(g_unresolvedPromptCalls),
        static_cast<long>(g_ambiguousPromptCalls),
        static_cast<long>(g_fallbackPromptCalls),
        static_cast<long>(g_nullPromptCalls),
        static_cast<long>(g_bindingResolutionCalls),
        static_cast<long>(g_controllerModeActive),
        static_cast<long>(g_controllerModeReadFailures),
        static_cast<long>(g_padCaptureChanges),
        static_cast<long>(g_managerCaptureChanges),
        static_cast<long>(g_qteDiagnosticCalls),
        static_cast<long>(g_qteCandidateCalls),
        static_cast<long>(g_qteUniqueCandidates),
        static_cast<long>(g_qtePromptOverrides),
        static_cast<long>(g_coverDirectionOverrides),
        static_cast<long>(g_coverStaticArmed),
        static_cast<long>(g_coverStaticRendered),
        static_cast<long>(g_factoryHookCallCount),
        static_cast<long>(g_createDeviceCallCount),
        static_cast<long>(g_deviceCreateTextureCalls),
        static_cast<long>(g_deviceSetTextureCalls),
        static_cast<long>(g_device512Created),
        static_cast<long>(g_device512Inspected),
        static_cast<long>(g_textureLoadCalls),
        static_cast<long>(g_512CandidateCalls),
        static_cast<long>(g_atlasPatchCount),
        static_cast<long>(g_eventTextureBindings));
}

} // namespace

namespace {

void InitializeBondIconRuntime(HMODULE module)
{
    g_selfModule = module;

    // 独立 ASI 需要保护自己的导入表，避免其他代理扫描时改写 UPX 存根的导入槽。
    // 单 DLL 专版的 JoyShim 与图标代码共享模块，因此不能隐藏其代理所需的导入表。
#ifndef BSCN_SINGLE_DLL
    HideOwnRuntimeImportDirectories(module);
#endif

    // 初始化线程状态与互斥锁，使早期 D3D9 Hook 可安全进入图标运行时。
    g_threadRuntimeTlsIndex = TlsAlloc();
    g_threadLibraryCallsDisabled = DisableThreadLibraryCalls(module) != FALSE;
    InterlockedExchange(&g_workerStopRequested, 0);
    InitializeCriticalSection(&g_logLock);
    InitializeCriticalSection(&g_textureLock);
    InitializePaths();

    // 专版的同名日志由统一入口创建并截断，这里只启用图标日志转发。
    // 独立 ASI 仍尊重可选 INI 和无 INI 默认关闭日志的既有规则。
#ifdef BSCN_SINGLE_DLL
    g_enableLog = 1;
#else
    g_enableLog = g_iniPresent
                      ? (GetPrivateProfileIntW(L"General", L"EnableLog", 1, g_iniPath) ? 1 : 0)
                      : 0;
    if (g_enableLog) OpenLog();
#endif

    // 主线程可能很早创建 D3D9 工厂，所以在启动 Worker 之前先安装轻量 IAT Hook。
    // 复杂初始化放到 Worker 执行，不能在加载器锁内等待线程完成。
    g_mainModule = GetModuleHandleW(nullptr);
    g_base = reinterpret_cast<uintptr_t>(g_mainModule);
    if (!InstallEarlyDirect3DCreate9Hook())
        Log(L"[警告] DllMain 早期 Direct3DCreate9 Hook 未安装；Worker 将继续重试。 ");

    HANDLE thread = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
    if (thread)
        CloseHandle(thread);
}

} // namespace

#ifdef BSCN_SINGLE_DLL

extern "C" void BondIconsProcessAttach(HMODULE module)
{
    // JoyShim 是唯一 DLL 入口，此函数只启动图标模块，不重复定义 DllMain。
    InitializeBondIconRuntime(module);
}

extern "C" void BondIconsProcessDetach()
{
    // 卸载时只通知 Worker 退出；不能在加载器锁内等待它或释放仍在使用的锁。
    InterlockedExchange(&g_workerStopRequested, 1);
}

#else

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        InitializeBondIconRuntime(module);
    }
    else if (reason == DLL_PROCESS_DETACH)
    {
        // 独立 ASI 的退出清理只在这里执行；专版交由统一入口管理同名日志。
        InterlockedExchange(&g_workerStopRequested, 1);
        WriteFinalStatistics();
        CloseLog();
        DeleteCriticalSection(&g_textureLock);
        DeleteCriticalSection(&g_logLock);
    }
    return TRUE;
}

#endif
