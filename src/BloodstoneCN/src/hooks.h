#pragma once
#include "common.h"

namespace bscn {
class ExternalFontAtlas;
class ExternalLocOverrides;
class NativeFontBridge;
class LocTextBridge;
class SubmitTextBridge;
namespace hooks {
bool Install(HMODULE main_module, ExternalFontAtlas* atlas,
             NativeFontBridge* native_font, ExternalLocOverrides* overrides,
             LocTextBridge* loc, SubmitTextBridge* submit,
             bool gate_text_until_font_ready);
bool FontRenderReady();
void MaintenanceTick();
void Shutdown();
}
}
