from pathlib import Path
import json,re,subprocess,sys
ROOT=Path(__file__).resolve().parents[1]
sources='\n'.join(p.read_text(encoding='utf-8') for p in (ROOT/'src').glob('*.*') if p.suffix in {'.cpp','.h'})
docs=[p.name for p in ROOT.iterdir() if p.suffix.lower()=='.md']
checks={
 'formal_name':'OUTPUT_NAME "NPCSubtitles"' in (ROOT/'CMakeLists.txt').read_text(),
 'dialogue_site_only':'0x0093A2A1u' in sources and '0x009302B5' not in sources and '0x00A706E8' not in sources,
 'no_submittext':'0x00E85900' not in sources and 'SubmitText' not in sources,
 'battle_sealed':'battle_subtitles=sealed' in sources and 'BattleChatter' not in sources,
 'auto_fallback':'ENGLISH_NATIVE_FALLBACK' in sources and 'CHINESE_BSCN' in sources,
 'external_assets':not any(p.suffix.lower() in {'.bfa','.bli','.loc'} for p in ROOT.rglob('*') if p.is_file()),
 'native_font_addresses':all(x in sources for x in ['0x011A02D8u','0x00E42C21u','0x12Cu']),
 'reset_lifecycle_hook':all(x in (ROOT/'src/renderer.cpp').read_text() for x in ['HookReset','NPC_SUBTITLE_DEVICE_RESET_BEGIN','NPC_SUBTITLE_DEVICE_RESET_OK','NPC_SUBTITLE_POST_RESET_DRAW_OK']),
 'layout_tuning':all(x in sources for x in ['line_spacing','0.120f','0.82f']),
 'interop_safe_bootstrap':all(x in (ROOT/'src/renderer.cpp').read_text() for x in ['BootstrapDeviceHooks','INTEROP_SAFE_RAW_EXPORT','GetProcAddress(d3d_module,"Direct3DCreate9")','raw_create(d3d','NPC_SUBTITLE_D3D9_BOOTSTRAP_SKIP_FOREIGN_CREATEDEVICE','create_hook_side_effect=0']),
 'renderer_early_install':(ROOT/'src/plugin.cpp').read_text().find('g_renderer.Install()') < (ROOT/'src/plugin.cpp').read_text().find('g_index.Initialize'),
 'x86_guard':'static_assert(sizeof(void*)==4' in sources.replace(' ',''),
 'chinese_human_docs':all(name in docs for name in ['完整接档.md','使用说明.md','编译说明.md','技术说明.md','测试清单.md','已知问题与下一步.md','发布包规范.md']),
 'no_english_named_md':all(re.search(r'[\u4e00-\u9fff]',name) for name in docs),
}
proc=subprocess.run([sys.executable,'-m','unittest','discover','-s',str(ROOT/'tests'),'-v'],text=True,capture_output=True)
out={'status':'PASS' if all(checks.values()) and proc.returncode==0 else 'FAIL','checks':checks,'tests_returncode':proc.returncode,'tests_stdout':proc.stdout,'tests_stderr':proc.stderr}
print(json.dumps(out,ensure_ascii=False,indent=2))
sys.exit(0 if out['status']=='PASS' else 1)
