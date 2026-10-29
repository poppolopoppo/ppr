"""Behavioral regression tests for the native cross-platform C++ gates."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import format_cpp as formatter
import check_semantic_style as style


class GateTests(unittest.TestCase):
    def test_literal_and_token_safety(self):
        before = 'auto s = u8R"tag(a // b\n c)tag"; char c = \'x\'; int x = 1\'000;'
        self.assertEqual(formatter.token_stream(before), formatter.token_stream('// comment\n' + before))
        for after in [before.replace('a // b', 'a // c'), before.replace("'x'", "'y'"), before.replace('int x', 'int X')]:
            self.assertNotEqual(formatter.token_stream(before), formatter.token_stream(after))
        self.assertNotEqual(formatter.token_stream('int x;'), formatter.token_stream('intx;'))
        self.assertNotEqual(formatter.token_stream('x + +y'), formatter.token_stream('x ++y'))
        self.assertEqual(formatter.token_stream('vector<vector<int> > x;'),
                         formatter.token_stream('vector<vector<int>> x;'))
        self.assertEqual(formatter.token_stream('#define X a + \\\n b', True), formatter.token_stream('#define X a + b', True))
        with self.assertRaises(ValueError):
            formatter.token_stream('auto x = R"bad(unclosed;')

    @unittest.skipUnless(shutil.which('clang-format'), 'clang-format required')
    def test_real_formatter_check_fix_and_fixed_point(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'space in name.cpp'
            source.write_text('int  f( ){return  1;}\nstd::vector<std::vector<int> > x;\n')
            original = source.read_bytes()
            arguments = (source, [], shutil.which('clang-format'), formatter.ROOT / '.clang-format', 4)
            self.assertEqual(formatter.format_one(*arguments, False)[0], 'DRIFT')
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(formatter.format_one(*arguments, True)[0], 'FIXED')
            self.assertEqual(formatter.format_one(*arguments, False)[0], 'PASS')

    @unittest.skipUnless(shutil.which('clang-format'), 'clang-format required')
    def test_format_cli_multiple_paths_and_parallel_fix(self):
        with tempfile.TemporaryDirectory() as temporary:
            paths = [Path(temporary) / 'one.cpp', Path(temporary) / 'two with spaces.h']
            for path in paths:
                path.write_bytes(b'int  f( ){return  1;}\r\n')
            script = formatter.ROOT / 'scripts/format_cpp.py'
            command = [sys.executable, str(script), '--jobs', '2', '--paths', *map(str, paths)]
            original = [path.read_bytes() for path in paths]
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 1)
            self.assertEqual([path.read_bytes() for path in paths], original)
            self.assertEqual(subprocess.run(command + ['--mode', 'fix'], capture_output=True).returncode, 0)
            self.assertEqual(subprocess.run(command, capture_output=True).returncode, 0)

    def test_unsafe_unstable_and_tool_failure_preserve_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / 'test.cpp'
            source.write_text('int x;\n')
            original = source.read_bytes()
            arguments = (source, [], 'fake-formatter', formatter.ROOT / '.clang-format', 4, True)

            def unsafe(command):
                Path(command[-1]).write_text('int X;\n')

            with patch.object(formatter, 'run', side_effect=unsafe):
                self.assertEqual(formatter.format_one(*arguments)[0], 'UNSAFE')
            self.assertEqual(source.read_bytes(), original)

            def unstable(command):
                scratch = Path(command[-1])
                scratch.write_bytes(scratch.read_bytes() + b'\n')

            with patch.object(formatter, 'run', side_effect=unstable):
                self.assertEqual(formatter.format_one(*arguments)[0], 'UNSTABLE')
            self.assertEqual(source.read_bytes(), original)
            with patch.object(formatter, 'run', side_effect=subprocess.CalledProcessError(1, 'fake', stderr=b'failed')):
                with self.assertRaises(subprocess.CalledProcessError):
                    formatter.format_one(*arguments)
            self.assertEqual(source.read_bytes(), original)

    def test_git_selection_staged_unstaged_untracked_and_deletions(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            def git(*args):
                return formatter.run(['git', *args], root)
            git('init', '-q')
            source = root / 'space in name.cpp'
            source.write_text('int a;\nint b;\nint c;\n')
            git('add', '.')
            git('-c', 'user.name=Test', '-c', 'user.email=test@example.invalid', 'commit', '-qm', 'fixture')
            source.write_text('int a = 1;\nint b;\nint c;\n')
            git('add', '.')
            source.write_text('int a = 1;\nint b;\nint c = 2;\n')
            untracked = root / 'new.cppm'
            untracked.write_text('export module test;\n')
            work = formatter.select(root, 'HEAD', None, False)
            self.assertEqual(work[source], ['1:1', '3:3'])
            self.assertEqual(work[untracked], [])
            source.write_text('int b;\nint c;\n')
            self.assertNotIn(source, formatter.select(root, 'HEAD', None, False))
            with self.assertRaises(ValueError):
                formatter.select(root, 'HEAD', ['missing.cpp'], False)
            with self.assertRaises(subprocess.CalledProcessError):
                formatter.select(root, 'invalid-revision', None, False)

    def test_format_cli_errors_and_selection_exclusions(self):
        script = formatter.ROOT / 'scripts/format_cpp.py'
        for args in [['--paths', 'missing.cpp'], ['--max-passes', '0'],
                     ['--jobs', '0'], ['--all', '--paths', 'game/main.cpp']]:
            result = subprocess.run([sys.executable, str(script), *args], capture_output=True)
            self.assertEqual(result.returncode, 2)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in ['source.h', 'out/ignored.cpp', '.git/ignored.cpp', 'build/ignored.h']:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('int x;\n')
            self.assertEqual(set(formatter.select(root, 'HEAD', None, True)), {root / 'source.h'})

    def test_style_classes_and_masking(self):
        text = '''// and(foo)
const char *s = "not(x)";
/* or(x)
not(x) */
#if not(x)
#endif
if (x and
    y)
{
}
and
or z;
if (not(x)) {}
return not
if (x<max) {}
'''
        findings = style.scan(text)
        self.assertEqual([(line, kind) for line, kind, _ in findings], [
            (9, 'C-DETACHED'), (11, 'A-STRANDED'), (12, 'C-LEADING'),
            (13, 'B-OP'), (14, 'A-STRANDED'), (15, 'B-CMP')])
        dense = 'void f() {\n' + '    x();\n' * 30 + '}\n'
        self.assertIn('D-DENSE', [kind for _, kind, _ in style.scan(dense)])
        self.assertNotIn('D-DENSE', [kind for _, kind, _ in style.scan(dense.replace('    x();', '\n    x();', 1))])
        self.assertNotIn('D-DENSE', [kind for _, kind, _ in style.scan(dense, '.cppm')])

    def test_style_cli_severity_and_invalid_paths(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            source = root / 'test.cpp'
            def invoke(*extra):
                return subprocess.run([sys.executable, str(formatter.ROOT / 'scripts/check_semantic_style.py'),
                    '--repo-root', str(root), '--files', 'test.cpp', *extra], capture_output=True).returncode
            source.write_text('if (x<max) {}\n')
            self.assertEqual(invoke(), 0)
            self.assertEqual(invoke('--strict'), 1)
            source.write_text('if (not(x)) {}\n')
            self.assertEqual(invoke(), 1)
            source.unlink()
            self.assertEqual(invoke(), 2)


if __name__ == '__main__':
    unittest.main()
