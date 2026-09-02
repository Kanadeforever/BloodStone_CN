import pathlib, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
SRC=ROOT/'src'
ALL='\n'.join(p.read_text(encoding='utf-8') for p in SRC.glob('*.*') if p.suffix in {'.cpp','.h'})

class Contract(unittest.TestCase):
    def test_independent_asi(self):
        cm=(ROOT/'CMakeLists.txt').read_text(encoding='utf-8')
        self.assertIn('OUTPUT_NAME "BloodstoneCN"',cm)
        self.assertNotIn('NPCSubtitles',cm)
        self.assertNotIn('BondGamepadIcons',cm)
        self.assertNotIn('BSCN_INTEGRATED_BUILD',ALL)

    def test_text_addresses_unchanged(self):
        self.assertIn('0x00E87740u',ALL)
        self.assertIn('0x00E85900u',ALL)
        self.assertNotIn('0x0093A2A1u',ALL)

    def test_coverage_strategy_unchanged(self):
        p=(SRC/'plugin.cpp').read_text(encoding='utf-8')
        e=(SRC/'external_loc.cpp').read_text(encoding='utf-8')
        self.assertIn('SOURCE_FIRST_HASH_SECOND',p)
        self.assertIn('PRINTF_TOKEN_ONLY',e)
        self.assertIn('CG_DEFER_GETTEXT',e)
        self.assertIn('CG_DEFER_SUBMIT',e)

    def test_frozen_gamepad_anchor_is_observed_not_modified(self):
        h=(SRC/'hooks.cpp').read_text(encoding='utf-8')
        self.assertIn('BondGamepadIcons.asi',h)
        self.assertIn('FROZEN_UNMODIFIED_CHAIN_CURRENT_IAT_NO_WAIT',h)
        self.assertIn('CHAIN_CURRENT_IAT_NO_WAIT',h)
        self.assertNotIn('LoadLibrary',h)
        self.assertNotIn('Sleep(10)',h)

    def test_host_owns_cn_npc_d3d_lifecycle(self):
        h=(SRC/'hooks.cpp').read_text(encoding='utf-8')
        self.assertIn('&vt[16]',h)
        self.assertIn('&vt[42]',h)
        self.assertIn('interop::NotifyDeviceCreated',h)
        self.assertIn('interop::NotifyBeforeReset',h)
        self.assertIn('interop::NotifyAfterReset',h)
        self.assertIn('interop::NotifyEndScene',h)
        self.assertIn('g_orig_reset(device,pp)',h)
        self.assertIn('g_orig_end_scene(device)',h)

    def test_no_dummy_device_or_bootstrap(self):
        h=(SRC/'hooks.cpp').read_text(encoding='utf-8')
        self.assertNotIn('CreateWindowExW',h)
        self.assertNotIn('raw_create',h)
        self.assertNotIn('D3D9Bootstrap',h)

    def test_cross_asi_c_abi(self):
        api=(SRC/'interop_api.h').read_text(encoding='utf-8')
        host=(SRC/'interop_host.cpp').read_text(encoding='utf-8')
        self.assertIn('BSCND3D9ClientV1',api)
        self.assertNotIn('__declspec(dllexport)',api)
        self.assertNotIn('std::',api)
        self.assertIn('extern "C" __declspec(dllexport)',host)
        self.assertIn('BSCN_D3D9_RegisterClientV1',host)
        self.assertIn('BSCN_D3D9_IsHostReadyV1',host)
        self.assertIn('BSCN_Text_WasRecentlySubmittedV1',host)
        self.assertIn('D3D9_INTEROP_CLIENT_REGISTERED',host)

    def test_duplicate_observer_does_not_rewrite_submit(self):
        s=(SRC/'submit_text_bridge.cpp').read_text(encoding='utf-8')
        self.assertIn('interop::ObserveSubmittedText(effective)',s)
        self.assertIn('original_submit_text_(',s)
        self.assertIn('manager,context_a8,position,layer_a0,effective,vertex_colors',s)


    def test_w1_1_d3d_is_installed_before_heavy_text_parse(self):
        p=(SRC/'plugin.cpp').read_text(encoding='utf-8')
        self.assertIn('R10_4_W1_2b_EARLY_D3D_READY',p)
        self.assertLess(p.index('hooks::Install('), p.index('external_loc_.Initialize('))
        self.assertLess(p.index('hooks::Install('), p.index('coverage_.Initialize('))

    def test_font_gate_controls_text_hook_installation(self):
        h=(SRC/'hooks.cpp').read_text(encoding='utf-8')
        self.assertIn('FontRenderReady()',h)
        self.assertIn('if (g_gate_text_until_font_ready && !FontRenderReady()) return;',h)
        self.assertLess(h.index('PublishFontReady(true);'), h.index('g_loc->TryInstall()'))

    def test_submit_stage_has_font_gate(self):
        s=(SRC/'submit_text_bridge.cpp').read_text(encoding='utf-8')
        self.assertIn('FONT_GATE_SKIP_SUBMIT',s)
        self.assertIn('!overrides_->rendering_ready()',s)
        gate=s.index('!overrides_->rendering_ready()')
        self.assertLess(gate,s.index('ResolveSubmittedText(',gate))
        self.assertLess(gate,s.index('coverage_->Resolve(',gate))

    def test_d3d_iat_chain_is_atomic(self):
        c=(SRC/'common.cpp').read_text(encoding='utf-8')
        self.assertIn('PatchPointerChain',c)
        self.assertIn('InterlockedCompareExchangePointer',c)
        self.assertIn('return PatchPointerChain(slot, replacement, original);',c)

    def test_x86_build(self):
        self.assertIn('-A Win32',(ROOT/'构建_MSVC_x86.bat').read_bytes().decode('gbk'))

if __name__=='__main__':unittest.main()
