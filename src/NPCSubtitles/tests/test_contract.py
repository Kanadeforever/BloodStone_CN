import pathlib,re,unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
SRC=ROOT/'src'
ALL='\n'.join(p.read_text(encoding='utf-8') for p in SRC.glob('*.*') if p.suffix in {'.cpp','.h'})
class Contract(unittest.TestCase):
    def test_formal_name(self):
        cm=(ROOT/'CMakeLists.txt').read_text()
        self.assertIn('OUTPUT_NAME "NPCSubtitles"',cm)
        self.assertNotIn('zzzz_BloodstoneNpcSubtitles',ALL+cm)
    def test_single_dialogue_callsite(self):
        self.assertIn('0x0093A2A1u',ALL)
        self.assertIn('0x00F12820u',ALL)
        self.assertNotIn('0x009302B5',ALL)
        self.assertNotIn('0x00A706E8',ALL)
    def test_no_submittext_npc_route(self):
        self.assertNotIn('0x00E85900',ALL)
        self.assertNotIn('SubmitText',ALL)
    def test_battle_sealed(self):
        self.assertIn('battle_subtitles=sealed',ALL)
        self.assertNotIn('BattleChatter',ALL)
    def test_auto_fallback(self):
        r=(SRC/'resource_mode.cpp').read_text()
        self.assertIn('ResourceMode::EnglishNative',r)
        self.assertIn('d.bfa_valid&&d.bli_valid&&d.loc_present',r)
    def test_bli3_validation(self):
        r=(SRC/'resource_mode.cpp').read_text()
        self.assertIn("'B','S','C','N','L','I','3',0",r)
        self.assertIn('U32(h+8)==3u',r)
        self.assertIn('U32(h+12)==32u',r)
    def test_english_uses_runtime_object_not_text_table(self):
        e=(SRC/'english_runtime_resolver.cpp').read_text()
        self.assertIn('ReadProcessMemory',e)
        self.assertIn('ScanRegion',e)
        self.assertNotRegex(ALL,r'L"[^"\n]{40,}"')
    def test_native_game_font_capture(self):
        n=(SRC/'native_font_capture.cpp').read_text()
        self.assertIn('0x011A02D8u',n)
        self.assertIn('0x00E42C21u',n)
        self.assertIn('kOwnerResourceHandleOffset=0x12Cu',n)
        self.assertIn('result)+8',n)
    def test_reset_lifecycle_hook(self):
        rr=(SRC/'renderer.cpp').read_text()
        self.assertIn('&vt[42]',rr)
        self.assertIn('&vt[16]',rr)
        self.assertIn('HookReset',rr)
        self.assertIn('NPC_SUBTITLE_DEVICE_RESET_BEGIN',rr)
        self.assertIn('NPC_SUBTITLE_DEVICE_RESET_OK',rr)
        self.assertIn('NPC_SUBTITLE_POST_RESET_DRAW_OK',rr)
        self.assertIn('InvalidateGpuTexture',rr)
        self.assertIn('InvalidateGpuResources',rr)
    def test_d3d9_bootstrap_recovers_late_install(self):
        rr=(SRC/'renderer.cpp').read_text()
        self.assertIn('BootstrapDeviceHooks',rr)
        self.assertIn('RECOVER_ALREADY_CREATED_DEVICE',rr)
        self.assertIn('NPC_SUBTITLE_D3D9_BOOTSTRAP_OK',rr)
        self.assertIn('GetProcAddress(d3d_module,"Direct3DCreate9")',rr)
        self.assertIn('raw_create(d3d',rr)
        self.assertIn('HookDevice(dev,false)',rr)
        self.assertNotIn('previous_create_(d3d',rr)
        self.assertNotIn('HookD3D9(d3d,"bootstrap")',rr)
        self.assertIn('NPC_SUBTITLE_D3D9_BOOTSTRAP_SKIP_FOREIGN_CREATEDEVICE',rr)
        self.assertIn('NPC_SUBTITLE_ENDSCENE_ACTIVE',rr)


    def test_renderer_installs_before_dialogue_index(self):
        p=(SRC/'plugin.cpp').read_text()
        self.assertLess(p.index('g_renderer.Install()'),p.index('g_index.Initialize'))
        self.assertIn('NPC_SUBTITLE_RENDERER_EARLY_INSTALL',p)

    def test_layout_tuning(self):
        c=(SRC/'config.cpp').read_text()
        self.assertIn('BaseY",0.120f',c)
        self.assertIn('LineSpacing",0.82f',c)
    def test_chinese_assets_external(self):
        self.assertNotIn('BSCNATL1',ALL.replace("'B','S','C','N','A','T','L','1'",''))
        self.assertFalse(any(p.suffix.lower() in {'.bfa','.bli','.loc'} for p in ROOT.rglob('*') if p.is_file()))
    def test_human_docs_chinese_filenames(self):
        expected={'完整接档.md','使用说明.md','编译说明.md','技术说明.md','测试清单.md','已知问题与下一步.md','发布包规范.md'}
        self.assertTrue(expected.issubset({p.name for p in ROOT.iterdir()}))
        forbidden=[p.name for p in ROOT.iterdir() if p.suffix.lower()=='.md' and p.name not in expected]
        self.assertEqual(forbidden,[])
    def test_x86_guard(self):
        self.assertIn('static_assert(sizeof(void*)==4',ALL.replace(' ',''))
        self.assertIn('-A Win32',(ROOT/'build_msvc_x86.bat').read_bytes().decode('gbk'))
if __name__=='__main__':unittest.main()
