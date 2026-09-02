from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
COMMON_H = (ROOT/'src/common.h').read_text(encoding='utf-8')
COMMON = (ROOT/'src/common.cpp').read_text(encoding='utf-8')
EXT = (ROOT/'src/external_loc.cpp').read_text(encoding='utf-8')
LOC = (ROOT/'src/loc_text_bridge.cpp').read_text(encoding='utf-8')
SUBMIT = (ROOT/'src/submit_text_bridge.cpp').read_text(encoding='utf-8')
COVERAGE = (ROOT/'src/text_coverage_resolver.cpp').read_text(encoding='utf-8')
PLUGIN = (ROOT/'src/plugin.cpp').read_text(encoding='utf-8')
INI = (ROOT/'config/BloodstoneCN.ini').read_text(encoding='utf-8')


def dynamic_scalar(text: str) -> bool:
    if not text or len(text) > 64:
        return False
    text = text.strip(' \t')
    if not text or not any('0' <= c <= '9' for c in text):
        return False
    allowed = set('0123456789 \t+-.,/:%()[]|')
    return all(c in allowed for c in text)


def identity_spans(text: str):
    # Matches the intended C++ tokenizer semantics closely enough for contract tests.
    parts = re.findall(r'[A-Za-z0-9._/\-]+', text)
    result = []
    for token in parts:
        if not (2 <= len(token) <= 16):
            continue
        alpha = [c for c in token if c.isalpha() and ord(c) < 128]
        digit = any(c.isdigit() and ord(c) < 128 for c in token)
        punctuation = any(c in '._/-' for c in token)
        if alpha and digit:
            result.append(token)
        elif len(alpha) >= 2 and punctuation and all(c.isupper() for c in alpha):
            result.append(token)
    return result


def should_defer(source: str, replacement: str, hash_disagreement: bool) -> bool:
    if not hash_disagreement or source == replacement:
        return False
    toks = identity_spans(source)
    stripped = source.strip(' \t')
    if len(toks) == 1 and stripped == toks[0]:
        return False
    if not toks:
        return False
    lower_repl = replacement.lower()
    return all(tok.lower() in lower_repl for tok in toks)


class W1HudLifecycle(unittest.TestCase):
    def test_dynamic_scalar_model(self):
        for value in ('10', '9', '10 / 30', '100%', '01:23', '[ 7 ]', '10|30', '-1'):
            self.assertTrue(dynamic_scalar(value), value)
        for value in ('P99', 'MP7', 'G36C', 'Ammo 10', 'Silenced P99', 'Focus Aim', '49m'):
            self.assertFalse(dynamic_scalar(value), value)

    def test_dynamic_scalar_bypasses_before_resolver_and_stable_storage(self):
        self.assertIn('const bool dynamic_runtime_scalar', SUBMIT)
        pos_guard = SUBMIT.index('const bool dynamic_runtime_scalar')
        pos_resolver = SUBMIT.index('ResolveSubmittedText(', pos_guard)
        pos_stable = SUBMIT.index('StabilizeEffectiveText(', pos_guard)
        self.assertIn('if (overrides_ && text && !dynamic_runtime_scalar)', SUBMIT)
        self.assertLess(pos_guard, pos_resolver)
        self.assertLess(pos_guard, pos_stable)
        self.assertIn('stable=SKIPPED', SUBMIT)
        self.assertIn('if (IsDynamicRuntimeScalar(submitted, units)) return {};', COVERAGE)

    def test_gettext_dynamic_scalar_keeps_game_pointer(self):
        self.assertIn('HUD_DYNAMIC_SCALAR_GETTEXT_BYPASS', EXT)
        self.assertIn('return nullptr;', EXT[EXT.index('HUD_DYNAMIC_SCALAR_GETTEXT_BYPASS'):])
        self.assertIn('pointer_owner=%s', LOC)

    def test_technical_identity_is_generic_not_weapon_list(self):
        self.assertIn('HasTechnicalIdentityToken', COMMON_H + COMMON)
        self.assertIn('PreservesTechnicalIdentityTokens', COMMON_H + COMMON)
        runtime = COMMON + EXT + LOC + SUBMIT + COVERAGE + PLUGIN
        # Examples may appear in comments, but no runtime wide-literal model table is allowed.
        for model in ('P99', 'MP7', 'M16', 'AK47', 'G36C'):
            self.assertNotIn('L"' + model + '"', runtime)

    def test_identity_model(self):
        self.assertEqual(identity_spans('P99'), ['P99'])
        self.assertEqual(identity_spans('Silenced P99'), ['P99'])
        self.assertEqual(identity_spans('G36C Assault Rifle'), ['G36C'])
        self.assertEqual(identity_spans('Combat A.R.'), ['A.R.'])
        self.assertEqual(identity_spans("Stop Greco's attack"), [])

    def test_defer_only_on_source_first_hash_disagreement(self):
        self.assertIn('source_first_hash_disagreement', EXT)
        self.assertIn('hud_defer_technical_labels_ && source_first_hash_disagreement', EXT)
        self.assertIn('reason=SOURCE_FIRST_HASH_DISAGREEMENT', EXT)
        self.assertTrue(should_defer('Silenced P99', '消音P99', True))
        self.assertFalse(should_defer('Silenced P99', '消音P99', False))
        self.assertFalse(should_defer('P99', 'P99', True))
        self.assertFalse(should_defer('Silenced P99', '错误型号MP7', True))

    def test_chinese_is_delayed_not_dropped(self):
        # W1 must preserve R10.4 selection and put the already-selected authoritative LOC
        # value into the existing precise/deferred mechanism; it must not KEEP_ORIGINAL globally.
        section = EXT[EXT.index('const wchar_t* ExternalLocOverrides::ResolveCandidate'):EXT.index('const wchar_t* ExternalLocOverrides::Resolve(', EXT.index('const wchar_t* ExternalLocOverrides::ResolveCandidate'))]
        self.assertIn('RememberPreciseMapping(candidate)', section)
        self.assertIn('context.replacement = *authoritative', section)
        self.assertIn('RememberGlobalDeferred(', section)
        self.assertIn('delivery=SUBMIT_EXACT_CONTEXT', section)
        self.assertNotIn('action=KEEP_ORIGINAL', section)

    def test_source_first_hash_second_is_not_rolled_back(self):
        self.assertIn('SOURCE_FIRST_HASH_SECOND', LOC)
        self.assertIn('SelectSourceFirstCandidate', EXT)
        self.assertIn('source family', EXT)
        self.assertIn('source_first_hash_second=UNCHANGED', PLUGIN)
        self.assertNotIn('short_code_guard=ACTIVE', PLUGIN)

    def test_hash_disagreement_does_not_globally_fallback_to_english(self):
        # Valid R10.4 Chinese paths such as city/loading labels can also have stale hashes.
        # The new defer gate therefore also requires a technical identity label.
        self.assertIn('ShouldDeferTechnicalIdentityLabel', EXT)
        self.assertIn('hud_defer_technical_labels_ && source_first_hash_disagreement', EXT)
        self.assertNotRegex(EXT, r'source_first_hash_disagreement\s*\)\s*return nullptr')

    def test_identical_candidate_preserves_pointer_without_visual_change(self):
        self.assertIn('authoritative->size() == original_units', EXT)
        self.assertIn('std::wmemcmp(original, authoritative->c_str()', EXT)

    def test_w1_is_runtime_configurable_for_ab(self):
        for key in ('BypassDynamicScalars=1', 'DeferTechnicalIdentityLabels=1', 'LifecycleProbe=0'):
            self.assertIn(key, INI)

    def test_lifecycle_probe_has_self_and_pointer_evidence(self):
        for marker in ('HUD_GETTEXT_TRACE', 'HUD_GETTEXT_SELF_TRANSITION', 'original_ptr=%p', 'returned_ptr=%p', 'pointer_owner=%s'):
            self.assertIn(marker, LOC)
        for marker in ('HUD_SUBMIT_TRACE', 'source_ptr=%p', 'effective_ptr=%p'):
            self.assertIn(marker, SUBMIT)

    def test_r10_4_coverage_pipeline_is_still_present(self):
        # W1 is not a new resolver rewrite.
        for marker in ('ResolveSubmittedText(', 'coverage_->Resolve(', 'StabilizeEffectiveText('):
            self.assertIn(marker, SUBMIT)
        for marker in ('ResolveExact', 'ResolveCanonical', 'ResolvePrintf', 'ResolveCompound', 'ResolveActionPrompt', 'ResolveTutorial', 'ResolveSpeaker', 'ResolveMixedLocalizedField', 'ResolveNormalized'):
            self.assertIn(marker, COVERAGE)

    def test_w1_1_early_d3d_precedes_loc_and_coverage(self):
        self.assertLess(PLUGIN.index('hooks::Install('), PLUGIN.index('external_loc_.Initialize('))
        self.assertLess(PLUGIN.index('hooks::Install('), PLUGIN.index('coverage_.Initialize('))
        self.assertIn('R10_4_W1_2b_EARLY_D3D_READY', PLUGIN)

    def test_submit_cannot_emit_chinese_while_font_gate_is_closed(self):
        self.assertIn('FONT_GATE_SKIP_SUBMIT', SUBMIT)
        gate=SUBMIT.index('!overrides_->rendering_ready()')
        self.assertLess(gate, SUBMIT.index('ResolveSubmittedText(', gate))
        self.assertLess(gate, SUBMIT.index('coverage_->Resolve(', gate))


if __name__ == '__main__':
    unittest.main()
