#include "proxy_system.hpp"
#include "win32_proc.hpp"
#include "directinput_filter.hpp"

#include <cstdint>

namespace {

#define JOYSHIM_PROXY_0(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name() noexcept { \
        using Function = std::uintptr_t (WINAPI*)(); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function() : 0; \
    }

#define JOYSHIM_PROXY_1(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1) : 0; \
    }

#define JOYSHIM_PROXY_2(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1, std::uintptr_t a2) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1, std::uintptr_t a2); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1, a2) : 0; \
    }

#define JOYSHIM_PROXY_3(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1, a2, a3) : 0; \
    }

#define JOYSHIM_PROXY_4(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1, a2, a3, a4) : 0; \
    }

#define JOYSHIM_PROXY_5(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1, a2, a3, a4, a5) : 0; \
    }

#define JOYSHIM_PROXY_6(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1, a2, a3, a4, a5, a6) : 0; \
    }

#define JOYSHIM_PROXY_7(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1, a2, a3, a4, a5, a6, a7) : 0; \
    }

#define JOYSHIM_PROXY_8(kind, name) \
    extern "C" std::uintptr_t WINAPI JOYSHIM_System_##name(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8) noexcept { \
        using Function = std::uintptr_t (WINAPI*)(std::uintptr_t a1, std::uintptr_t a2, std::uintptr_t a3, std::uintptr_t a4, std::uintptr_t a5, std::uintptr_t a6, std::uintptr_t a7, std::uintptr_t a8); \
        static const auto function = joyshim::FarProcToFunction<Function>( \
            joyshim::ResolveSystemExport(joyshim::SystemDllKind::kind, #name)); \
        return function != nullptr ? function(a1, a2, a3, a4, a5, a6, a7, a8) : 0; \
    }

}  // namespace

extern "C" HRESULT WINAPI JOYSHIM_System_DirectInput8Create(
    HINSTANCE instance,
    DWORD version,
    const GUID& interfaceId,
    void** output,
    void* outer) noexcept {
    return joyshim::ConfiguredDirectInput8Create(
        instance, version, interfaceId, output, outer);
}
JOYSHIM_PROXY_0(DInput8, DllCanUnloadNow)
JOYSHIM_PROXY_3(DInput8, DllGetClassObject)
JOYSHIM_PROXY_0(DInput8, DllRegisterServer)
JOYSHIM_PROXY_0(DInput8, DllUnregisterServer)
JOYSHIM_PROXY_0(DInput8, GetdfDIJoystick)
JOYSHIM_PROXY_3(WinMM, CloseDriver)
JOYSHIM_PROXY_5(WinMM, DefDriverProc)
JOYSHIM_PROXY_7(WinMM, DriverCallback)
JOYSHIM_PROXY_3(WinMM, DrvClose)
JOYSHIM_PROXY_5(WinMM, DrvDefDriverProc)
JOYSHIM_PROXY_1(WinMM, DrvGetModuleHandle)
JOYSHIM_PROXY_3(WinMM, DrvOpen)
JOYSHIM_PROXY_3(WinMM, DrvOpenA)
JOYSHIM_PROXY_4(WinMM, DrvSendMessage)
JOYSHIM_PROXY_1(WinMM, GetDriverFlags)
JOYSHIM_PROXY_1(WinMM, GetDriverModuleHandle)
JOYSHIM_PROXY_3(WinMM, OpenDriver)
JOYSHIM_PROXY_3(WinMM, OpenDriverA)
JOYSHIM_PROXY_3(WinMM, PlaySound)
JOYSHIM_PROXY_3(WinMM, PlaySoundA)
JOYSHIM_PROXY_3(WinMM, PlaySoundW)
JOYSHIM_PROXY_4(WinMM, SendDriverMessage)
JOYSHIM_PROXY_3(WinMM, auxGetDevCapsA)
JOYSHIM_PROXY_3(WinMM, auxGetDevCapsW)
JOYSHIM_PROXY_0(WinMM, auxGetNumDevs)
JOYSHIM_PROXY_2(WinMM, auxGetVolume)
JOYSHIM_PROXY_4(WinMM, auxOutMessage)
JOYSHIM_PROXY_2(WinMM, auxSetVolume)
JOYSHIM_PROXY_1(WinMM, joyConfigChanged)
JOYSHIM_PROXY_3(WinMM, joyGetDevCapsA)
JOYSHIM_PROXY_3(WinMM, joyGetDevCapsW)
JOYSHIM_PROXY_0(WinMM, joyGetNumDevs)
JOYSHIM_PROXY_2(WinMM, joyGetPos)
JOYSHIM_PROXY_2(WinMM, joyGetPosEx)
JOYSHIM_PROXY_2(WinMM, joyGetThreshold)
JOYSHIM_PROXY_1(WinMM, joyReleaseCapture)
JOYSHIM_PROXY_4(WinMM, joySetCapture)
JOYSHIM_PROXY_2(WinMM, joySetThreshold)
JOYSHIM_PROXY_3(WinMM, mciDriverNotify)
JOYSHIM_PROXY_1(WinMM, mciDriverYield)
JOYSHIM_PROXY_1(WinMM, mciExecute)
JOYSHIM_PROXY_1(WinMM, mciFreeCommandResource)
JOYSHIM_PROXY_1(WinMM, mciGetCreatorTask)
JOYSHIM_PROXY_1(WinMM, mciGetDeviceIDA)
JOYSHIM_PROXY_2(WinMM, mciGetDeviceIDFromElementIDA)
JOYSHIM_PROXY_2(WinMM, mciGetDeviceIDFromElementIDW)
JOYSHIM_PROXY_1(WinMM, mciGetDeviceIDW)
JOYSHIM_PROXY_1(WinMM, mciGetDriverData)
JOYSHIM_PROXY_3(WinMM, mciGetErrorStringA)
JOYSHIM_PROXY_3(WinMM, mciGetErrorStringW)
JOYSHIM_PROXY_2(WinMM, mciGetYieldProc)
JOYSHIM_PROXY_3(WinMM, mciLoadCommandResource)
JOYSHIM_PROXY_4(WinMM, mciSendCommandA)
JOYSHIM_PROXY_4(WinMM, mciSendCommandW)
JOYSHIM_PROXY_4(WinMM, mciSendStringA)
JOYSHIM_PROXY_4(WinMM, mciSendStringW)
JOYSHIM_PROXY_2(WinMM, mciSetDriverData)
JOYSHIM_PROXY_3(WinMM, mciSetYieldProc)
JOYSHIM_PROXY_3(WinMM, midiConnect)
JOYSHIM_PROXY_3(WinMM, midiDisconnect)
JOYSHIM_PROXY_3(WinMM, midiInAddBuffer)
JOYSHIM_PROXY_1(WinMM, midiInClose)
JOYSHIM_PROXY_3(WinMM, midiInGetDevCapsA)
JOYSHIM_PROXY_3(WinMM, midiInGetDevCapsW)
JOYSHIM_PROXY_3(WinMM, midiInGetErrorTextA)
JOYSHIM_PROXY_3(WinMM, midiInGetErrorTextW)
JOYSHIM_PROXY_2(WinMM, midiInGetID)
JOYSHIM_PROXY_0(WinMM, midiInGetNumDevs)
JOYSHIM_PROXY_4(WinMM, midiInMessage)
JOYSHIM_PROXY_5(WinMM, midiInOpen)
JOYSHIM_PROXY_3(WinMM, midiInPrepareHeader)
JOYSHIM_PROXY_1(WinMM, midiInReset)
JOYSHIM_PROXY_1(WinMM, midiInStart)
JOYSHIM_PROXY_1(WinMM, midiInStop)
JOYSHIM_PROXY_3(WinMM, midiInUnprepareHeader)
JOYSHIM_PROXY_4(WinMM, midiOutCacheDrumPatches)
JOYSHIM_PROXY_4(WinMM, midiOutCachePatches)
JOYSHIM_PROXY_1(WinMM, midiOutClose)
JOYSHIM_PROXY_3(WinMM, midiOutGetDevCapsA)
JOYSHIM_PROXY_3(WinMM, midiOutGetDevCapsW)
JOYSHIM_PROXY_3(WinMM, midiOutGetErrorTextA)
JOYSHIM_PROXY_3(WinMM, midiOutGetErrorTextW)
JOYSHIM_PROXY_2(WinMM, midiOutGetID)
JOYSHIM_PROXY_0(WinMM, midiOutGetNumDevs)
JOYSHIM_PROXY_2(WinMM, midiOutGetVolume)
JOYSHIM_PROXY_3(WinMM, midiOutLongMsg)
JOYSHIM_PROXY_4(WinMM, midiOutMessage)
JOYSHIM_PROXY_5(WinMM, midiOutOpen)
JOYSHIM_PROXY_3(WinMM, midiOutPrepareHeader)
JOYSHIM_PROXY_1(WinMM, midiOutReset)
JOYSHIM_PROXY_2(WinMM, midiOutSetVolume)
JOYSHIM_PROXY_2(WinMM, midiOutShortMsg)
JOYSHIM_PROXY_3(WinMM, midiOutUnprepareHeader)
JOYSHIM_PROXY_1(WinMM, midiStreamClose)
JOYSHIM_PROXY_6(WinMM, midiStreamOpen)
JOYSHIM_PROXY_3(WinMM, midiStreamOut)
JOYSHIM_PROXY_1(WinMM, midiStreamPause)
JOYSHIM_PROXY_3(WinMM, midiStreamPosition)
JOYSHIM_PROXY_3(WinMM, midiStreamProperty)
JOYSHIM_PROXY_1(WinMM, midiStreamRestart)
JOYSHIM_PROXY_1(WinMM, midiStreamStop)
JOYSHIM_PROXY_1(WinMM, mixerClose)
JOYSHIM_PROXY_3(WinMM, mixerGetControlDetailsA)
JOYSHIM_PROXY_3(WinMM, mixerGetControlDetailsW)
JOYSHIM_PROXY_3(WinMM, mixerGetDevCapsA)
JOYSHIM_PROXY_3(WinMM, mixerGetDevCapsW)
JOYSHIM_PROXY_3(WinMM, mixerGetID)
JOYSHIM_PROXY_3(WinMM, mixerGetLineControlsA)
JOYSHIM_PROXY_3(WinMM, mixerGetLineControlsW)
JOYSHIM_PROXY_3(WinMM, mixerGetLineInfoA)
JOYSHIM_PROXY_3(WinMM, mixerGetLineInfoW)
JOYSHIM_PROXY_0(WinMM, mixerGetNumDevs)
JOYSHIM_PROXY_4(WinMM, mixerMessage)
JOYSHIM_PROXY_5(WinMM, mixerOpen)
JOYSHIM_PROXY_3(WinMM, mixerSetControlDetails)
JOYSHIM_PROXY_0(WinMM, mmGetCurrentTask)
JOYSHIM_PROXY_1(WinMM, mmTaskBlock)
JOYSHIM_PROXY_3(WinMM, mmTaskCreate)
JOYSHIM_PROXY_1(WinMM, mmTaskSignal)
JOYSHIM_PROXY_0(WinMM, mmTaskYield)
JOYSHIM_PROXY_3(WinMM, mmioAdvance)
JOYSHIM_PROXY_3(WinMM, mmioAscend)
JOYSHIM_PROXY_2(WinMM, mmioClose)
JOYSHIM_PROXY_3(WinMM, mmioCreateChunk)
JOYSHIM_PROXY_4(WinMM, mmioDescend)
JOYSHIM_PROXY_2(WinMM, mmioFlush)
JOYSHIM_PROXY_3(WinMM, mmioGetInfo)
JOYSHIM_PROXY_3(WinMM, mmioInstallIOProcA)
JOYSHIM_PROXY_3(WinMM, mmioInstallIOProcW)
JOYSHIM_PROXY_3(WinMM, mmioOpenA)
JOYSHIM_PROXY_3(WinMM, mmioOpenW)
JOYSHIM_PROXY_3(WinMM, mmioRead)
JOYSHIM_PROXY_4(WinMM, mmioRenameA)
JOYSHIM_PROXY_4(WinMM, mmioRenameW)
JOYSHIM_PROXY_3(WinMM, mmioSeek)
JOYSHIM_PROXY_4(WinMM, mmioSendMessage)
JOYSHIM_PROXY_4(WinMM, mmioSetBuffer)
JOYSHIM_PROXY_3(WinMM, mmioSetInfo)
JOYSHIM_PROXY_2(WinMM, mmioStringToFOURCCA)
JOYSHIM_PROXY_2(WinMM, mmioStringToFOURCCW)
JOYSHIM_PROXY_3(WinMM, mmioWrite)
JOYSHIM_PROXY_0(WinMM, mmsystemGetVersion)
JOYSHIM_PROXY_2(WinMM, sndPlaySoundA)
JOYSHIM_PROXY_2(WinMM, sndPlaySoundW)
JOYSHIM_PROXY_1(WinMM, timeBeginPeriod)
JOYSHIM_PROXY_1(WinMM, timeEndPeriod)
JOYSHIM_PROXY_2(WinMM, timeGetDevCaps)
JOYSHIM_PROXY_2(WinMM, timeGetSystemTime)
JOYSHIM_PROXY_0(WinMM, timeGetTime)
JOYSHIM_PROXY_1(WinMM, timeKillEvent)
JOYSHIM_PROXY_5(WinMM, timeSetEvent)
JOYSHIM_PROXY_3(WinMM, waveInAddBuffer)
JOYSHIM_PROXY_1(WinMM, waveInClose)
JOYSHIM_PROXY_3(WinMM, waveInGetDevCapsA)
JOYSHIM_PROXY_3(WinMM, waveInGetDevCapsW)
JOYSHIM_PROXY_3(WinMM, waveInGetErrorTextA)
JOYSHIM_PROXY_3(WinMM, waveInGetErrorTextW)
JOYSHIM_PROXY_2(WinMM, waveInGetID)
JOYSHIM_PROXY_0(WinMM, waveInGetNumDevs)
JOYSHIM_PROXY_3(WinMM, waveInGetPosition)
JOYSHIM_PROXY_4(WinMM, waveInMessage)
JOYSHIM_PROXY_6(WinMM, waveInOpen)
JOYSHIM_PROXY_3(WinMM, waveInPrepareHeader)
JOYSHIM_PROXY_1(WinMM, waveInReset)
JOYSHIM_PROXY_1(WinMM, waveInStart)
JOYSHIM_PROXY_1(WinMM, waveInStop)
JOYSHIM_PROXY_3(WinMM, waveInUnprepareHeader)
JOYSHIM_PROXY_1(WinMM, waveOutBreakLoop)
JOYSHIM_PROXY_1(WinMM, waveOutClose)
JOYSHIM_PROXY_3(WinMM, waveOutGetDevCapsA)
JOYSHIM_PROXY_3(WinMM, waveOutGetDevCapsW)
JOYSHIM_PROXY_3(WinMM, waveOutGetErrorTextA)
JOYSHIM_PROXY_3(WinMM, waveOutGetErrorTextW)
JOYSHIM_PROXY_2(WinMM, waveOutGetID)
JOYSHIM_PROXY_0(WinMM, waveOutGetNumDevs)
JOYSHIM_PROXY_2(WinMM, waveOutGetPitch)
JOYSHIM_PROXY_2(WinMM, waveOutGetPlaybackRate)
JOYSHIM_PROXY_3(WinMM, waveOutGetPosition)
JOYSHIM_PROXY_2(WinMM, waveOutGetVolume)
JOYSHIM_PROXY_4(WinMM, waveOutMessage)
JOYSHIM_PROXY_6(WinMM, waveOutOpen)
JOYSHIM_PROXY_1(WinMM, waveOutPause)
JOYSHIM_PROXY_3(WinMM, waveOutPrepareHeader)
JOYSHIM_PROXY_1(WinMM, waveOutReset)
JOYSHIM_PROXY_1(WinMM, waveOutRestart)
JOYSHIM_PROXY_2(WinMM, waveOutSetPitch)
JOYSHIM_PROXY_2(WinMM, waveOutSetPlaybackRate)
JOYSHIM_PROXY_2(WinMM, waveOutSetVolume)
JOYSHIM_PROXY_3(WinMM, waveOutUnprepareHeader)
JOYSHIM_PROXY_3(WinMM, waveOutWrite)
