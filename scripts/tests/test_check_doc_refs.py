#!/usr/bin/env python3
"""Isolated documentation reference audit tests (standard library only)."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import check_doc_refs as audit


class DocReferenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def write(self, name, text=''):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding='utf-8')
        return path

    def invoke(self, *args):
        return subprocess.run([sys.executable, '-B', audit.__file__, '--root', str(self.root), *args],
                              capture_output=True, text=True)

    def test_relative_links_and_ignored_urls(self):
        self.write('docs/target.md')
        self.write('docs/guide.md', '[ok](target.md#part) [no](missing.md)\n'
                   '[anchor](#part) [web](https://example.org/no.md) [mail](mailto:a@example.org)\n')
        self.assertEqual(audit.audit(self.root)['findings'], [
            {'path': 'docs/guide.md', 'line': 1, 'kind': 'missing', 'target': 'missing.md'}])

    def test_root_paths_and_skill_relative_resources(self):
        self.write('scripts/check.py')
        self.write('.opencode/skills/local/references/help.md')
        self.write('.opencode/skills/local/SKILL.md', '`scripts/check.py` "references/help.md"\n'
                   '[help](references/help.md)\n')
        self.assertEqual(audit.audit(self.root, include_quoted_paths=True)['findings'], [])

    def test_external_and_missing_local_skill(self):
        self.write('.opencode/skills/broken/other.md')
        self.write('AGENTS.md', 'Load the `reflect` skill.\nUse broken.\nLoad the `removed` skill.\n')
        findings = audit.audit(self.root)['findings']
        self.assertEqual([(item['kind'], item['target']) for item in findings],
                         [('external', 'reflect'), ('missing-skill', 'broken'), ('missing-skill', 'removed')])
        self.write('.opencode/skills/reflect/SKILL.md')
        self.assertNotIn('external', [item['kind'] for item in audit.audit(self.root)['findings']])

    def test_foreign_host_fence_and_line_numbers(self):
        self.write('AGENTS.md', '```powershell\ncd C:\\work\\ppr\n"scripts/foreign.py"\n```\n'
                   '~~~bash\ncd /home/elsewhere/ppr\n"scripts/other.py"\n~~~\n'
                   '`scripts/missing.py`\n```sh\n"scripts/local.py"\n```\n')
        self.assertEqual([(item['line'], item['target']) for item in audit.audit(self.root)['findings']],
                         [(9, 'scripts/missing.py'), (11, 'scripts/local.py')])

    def test_placeholders_outputs_scratch_and_non_paths(self):
        self.write('AGENTS.md', '`scripts/<name>.py` `scripts/$NAME.py` `out/report.json`\n'
                   '[diagram](docs/diagrams/one.svg) `.slim/log.md` `lib/engine/*.cppm`\n'
                   '`https://example.org/a.py` `and/or` `python --version`\n')
        self.assertEqual(audit.audit(self.root, include_quoted_paths=True)['findings'], [])

    def test_markdown_title_encoded_space_and_parentheses(self):
        self.write('docs/a b.md')
        self.write('docs/a(b).md')
        self.write('AGENTS.md', '[one](docs/a%20b.md "title") [two](docs/a(b).md)\n')
        self.assertEqual(audit.audit(self.root)['findings'], [])

    def test_examples_include_spellings_and_generic_skill_prose(self):
        self.write('include/pP/Macros.h')
        self.write('AGENTS.md', 'Use the named skill.\n`<content_dir>/shaders/mesh.slang`\n'
                   '`%LOCALAPPDATA%/PPR/report.json` `path/to/file.cpp` `pP/Macros.h`\n'
                   '`lib/example.cppm/.cpp` `game/CMakeFiles/test.cpp.obj`\n'
                   '```toml\nsource = "images/rock.png"\n```\n'
                   '````markdown\n![Example](images/example.png)\n````\n')
        self.assertEqual(audit.audit(self.root, include_quoted_paths=True)['findings'], [])

    def test_documented_path_alias(self):
        self.write('lib/renderer/Pass.cpp')
        self.write('docs/guide.md', '`R/` below is `lib/renderer/`.\n`R/Pass.cpp`\n')
        self.assertEqual(audit.audit(self.root, include_quoted_paths=True)['findings'], [])

    def test_default_scope_and_historical_flag(self):
        excluded = ['superpowers/plans', 'plans', 'diagrams', 'archive', 'old-notes', 'historical']
        for directory in excluded:
            self.write(f'docs/{directory}/guide.md', '[missing](absent.md)\n')
        self.write('AGENTS.md')
        self.write('codemap.md')
        self.write('docs/current/nested/guide.md')
        self.write('.opencode/skills/local/SKILL.md')
        self.write('docs/README.md', '[Plan](plans/guide.md)\n')
        self.assertEqual(audit.audit(self.root)['summary']['files'], 5)
        self.assertEqual(self.invoke().returncode, 0)

        result = self.invoke('--include-historical', '--json')
        self.assertEqual(result.returncode, 1, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data['summary']['missing'], len(excluded))
        self.assertEqual(data['summary']['files'], 5 + len(excluded))

    def test_readme_directory_labels_and_incidental_history(self):
        self.write('docs/README.md', '# Documentation guide\n\n'
                   'Historical incidents do not define current policy.\n\n'
                   '`clone/` and `physics/` contain designs and proposals.\n')
        self.write('docs/clone/guide.md', '[missing](absent.md)\n')
        self.write('docs/physics/guide.md', '[missing](absent.md)\n')
        self.write('docs/records/README.md', '# Historical documentation\n')
        self.write('docs/records/nested/guide.md', '[missing](absent.md)\n')
        self.write('docs/evidence/README.md', '# Evidence\n\nThis directory contains\nhistorical records.\n')
        self.write('docs/evidence/guide.md', '[missing](absent.md)\n')
        self.write('docs/current/README.md', '# Current procedures\n\nSee historical records for background.\n')
        self.write('docs/current/guide.md', '[missing](current-missing.md)\n')
        findings = audit.audit(self.root)['findings']
        self.assertEqual([(item['path'], item['target']) for item in findings],
                         [('docs/current/guide.md', 'current-missing.md')])
        self.assertEqual(audit.audit(self.root, include_historical=True)['summary']['missing'], 5)

    def test_quoted_path_flag_is_independent_of_historical_scope(self):
        self.write('docs/current.md', '`lib/current.cpp`\n')
        self.write('docs/plans/future.md', '"lib/planned.cpp"\n')
        self.assertEqual(self.invoke().returncode, 0)
        self.assertEqual(self.invoke('--include-historical').returncode, 0)
        quoted = self.invoke('--include-quoted-paths', '--json')
        self.assertEqual(quoted.returncode, 1)
        self.assertEqual([item['target'] for item in json.loads(quoted.stdout)['findings']], ['lib/current.cpp'])
        both = self.invoke('--include-quoted-paths', '--include-historical', '--json')
        self.assertEqual(both.returncode, 1)
        self.assertEqual(json.loads(both.stdout)['summary']['missing'], 2)

    def test_default_retains_helpers_links_and_skill_breakage(self):
        self.write('docs/current.md', '`scripts/Removed.ps1` Check: PASS.\n'
                   '`python3 scripts/removed.py --check`\n'
                   '[broken](missing.md)\nLoad the `removed` skill.\n'
                   '`https://example.org/scripts/remote.py`\n')
        findings = audit.audit(self.root)['findings']
        self.assertEqual([(item['line'], item['kind'], item['target']) for item in findings], [
            (1, 'missing', 'scripts/Removed.ps1'), (2, 'missing', 'scripts/removed.py'),
            (3, 'missing', 'missing.md'), (4, 'missing-skill', 'removed')])
        self.assertEqual(self.invoke().returncode, 1)

    def test_opencode_is_scanned_even_in_named_historical_directories(self):
        self.write('.opencode/archive/guide.md', '[missing](absent.md)\n')
        self.assertEqual(audit.audit(self.root)['summary']['missing'], 1)
        self.assertEqual(self.invoke().returncode, 1)

    def test_json_and_exit_codes(self):
        self.write('AGENTS.md', 'Use reflect.\n')
        result = self.invoke('--json')
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(set(data), {'findings', 'summary'})
        self.assertEqual(data['summary'], {'files': 1, 'missing': 0, 'missing-skill': 0, 'external': 1})
        self.write('AGENTS.md', '[broken](missing.md)\n')
        result = self.invoke()
        self.assertEqual(result.returncode, 1)
        self.assertIn('AGENTS.md:1: missing: missing.md', result.stdout)
        self.assertIn('Summary:', result.stdout)
        self.assertEqual(self.invoke('--json').returncode, 1)

    def test_usage_and_internal_errors(self):
        self.assertEqual(self.invoke('--unknown').returncode, 2)
        self.assertEqual(self.invoke('--root', str(self.root / 'absent')).returncode, 2)
        file = self.write('not-a-directory')
        self.assertEqual(self.invoke('--root', str(file)).returncode, 2)
        self.write('AGENTS.md').write_bytes(b'\xff')
        self.assertEqual(self.invoke().returncode, 2)


if __name__ == '__main__':
    unittest.main()
