#pragma once
#include "common.h"
#include "interop_api.h"

namespace bscn::interop {
void SetHostReady(bool ready);
void NotifyDeviceCreated(IDirect3DDevice9* device);
void NotifyBeforeReset(IDirect3DDevice9* device,
                       const D3DPRESENT_PARAMETERS* params);
void NotifyAfterReset(IDirect3DDevice9* device, HRESULT result);
void NotifyEndScene(IDirect3DDevice9* device);
void ObserveSubmittedText(const wchar_t* text);
void Shutdown();
}
