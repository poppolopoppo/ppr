"""Commit planner regression cases; all Git mutations use disposable repositories."""
from contextlib import redirect_stdout, redirect_stderr
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / '.opencode/skills/git-commit-planner/scripts/commit_plan.py'
SPEC = importlib.util.spec_from_file_location('commit_plan', SCRIPT)
planner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(planner)


class CommitPlanTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='ppr-git-tests-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.original_directory = Path.cwd()
        self.addCleanup(os.chdir, self.original_directory)
        os.chdir(self.root)
        self.environment = patch.dict(os.environ, {'GIT_CONFIG_NOSYSTEM': '1', 'GIT_CONFIG_GLOBAL': os.devnull})
        self.environment.start()
        self.addCleanup(self.environment.stop)
        for name in ['GIT_INDEX_FILE', 'GIT_DIR', 'GIT_WORK_TREE']:
            if name in os.environ:
                os.environ.pop(name)
        self.git('init', '-q')
        for name, value in [('user.name', 'Test'), ('user.email', 'test@example.invalid'),
                            ('commit.gpgsign', 'false'), ('core.autocrlf', 'false'), ('core.hooksPath', '.git/hooks')]:
            self.git('config', name, value)
        for name in ['first.txt', 'second.txt', 'untouched.txt']:
            self.write(name, 'one\ntwo\nthree\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'fixture')
        self.head = self.git('rev-parse', 'HEAD')

    def git(self, *args, **kwargs):
        return planner.git(*args, **kwargs)

    def write(self, name, contents):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents, encoding='utf-8', newline='\n')
        return path

    def stage(self, name, contents):
        path = self.write(name, contents)
        self.git('add', '--', name)
        return path

    def group(self, paths, subject='update fixture', **extra):
        return {'files': paths, 'component': 'fixtures', 'subject': subject,
                'body': 'Keep the fixture useful for regression testing.', **extra}

    def build(self, groups=None):
        groups = groups or [self.group(['first.txt'])]
        self.write('.slim/grouping.json', json.dumps({'commits': groups}))
        plan_id, _ = planner.build('.slim/grouping.json', '.slim/commit-plan.json')
        return plan_id, json.loads(Path('.slim/commit-plan.json').read_text())

    def replay(self, plan_id=None, dry_run=False):
        with redirect_stdout(io.StringIO()), redirect_stderr(io.StringIO()):
            planner.replay('.slim/commit-plan.json', plan_id, dry_run)

    def test_staged_partial_file_and_unrelated_index_are_preserved(self):
        self.stage('first.txt', 'staged\ntwo\nthree\n')
        self.write('first.txt', 'staged\ntwo\nunstaged\n')
        self.stage('second.txt', 'independent staged content\n')
        original_index = Path('.git/index').read_bytes()
        worktree = Path('first.txt').read_bytes()
        plan_id, plan = self.build()
        self.assertEqual(Path('.git/index').read_bytes(), original_index)
        self.assertEqual(plan['steps'][0]['content_mode'], 'staged')
        self.replay(dry_run=True)
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.head)
        self.assertEqual(Path('.git/index').read_bytes(), original_index)
        with self.assertRaisesRegex(ValueError, 'confirmed-plan-id'):
            self.replay('incorrect')
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.head)
        self.replay(plan_id)
        self.assertEqual(self.git('show', 'HEAD:first.txt'), b'staged\ntwo\nthree\n')
        self.assertEqual(Path('first.txt').read_bytes(), worktree)
        self.assertEqual(self.git('show', ':second.txt'), b'independent staged content\n')
        self.assertEqual(self.git('diff', '--cached', '--name-only'), b'second.txt\n')

    def test_discovery_limits_literals_unicode_and_indivisible_rename(self):
        name = 'folder/space [x] café.txt'
        self.stage(name, 'literal path\n')
        self.git('mv', 'first.txt', 'renamed.txt')
        before = Path('.git/index').read_bytes()
        discovered = planner.discover(['first.txt'])
        self.assertEqual(discovered['candidates'][0]['files'], ['first.txt', 'renamed.txt'])
        self.assertEqual(Path('.git/index').read_bytes(), before)
        with self.assertRaisesRegex(ValueError, 'Single change unit'):
            planner.discover(['first.txt'], max_files=1)
        candidate = planner.discover([name])['candidates'][0]
        self.assertEqual(candidate['files'], [name])
        with self.assertRaisesRegex(ValueError, 'No applicable'):
            planner.discover(['folder/*'])
        self.write('new untracked.txt', 'not staged\n')
        with self.assertRaisesRegex(ValueError, 'Untracked'):
            planner.discover([], all_local=True)

    def test_builder_rejects_split_rename_and_preserves_trailers(self):
        self.git('mv', 'first.txt', 'renamed.txt')
        for name in ['first.txt', 'renamed.txt']:
            with self.assertRaisesRegex(ValueError, 'both literal endpoints'):
                self.build([self.group([name])])
        body = ('Keep the authored paragraphs separate.\n\n'
                'Signed-off-by: Test <test@example.invalid>\n'
                'Reviewed-by: Other <other@example.invalid>')
        plan_id, plan = self.build([self.group(['first.txt', 'renamed.txt'], body=body)])
        self.assertEqual(plan['steps'][0]['body'], body)
        self.replay(plan_id)
        self.assertIn(body.encode(), self.git('log', '-1', '--format=%B'))

    def test_rename_binary_and_deletion_replay(self):
        self.git('mv', 'first.txt', 'renamed.txt')
        Path('second.txt').unlink()
        self.git('add', '-u')
        binary = Path('binary.dat')
        binary.write_bytes(bytes(range(256)))
        self.git('add', '--', 'binary.dat')
        plan_id, _ = self.build([self.group(['first.txt', 'renamed.txt']),
                                 self.group(['second.txt'], 'remove obsolete fixture'),
                                 self.group(['binary.dat'], 'add binary fixture')])
        self.replay(plan_id)
        self.assertEqual(self.git('status', '--porcelain', '--untracked-files=no'), b'')
        self.assertEqual(self.git('show', 'HEAD:binary.dat'), bytes(range(256)))
        self.assertEqual(int(self.git('rev-list', '--count', 'HEAD')), 4)

    def test_freshness_checks_head_branch_index_and_all_local_worktree(self):
        self.stage('first.txt', 'staged\n')
        _, plan = self.build()
        self.git('checkout', '-qb', 'other-branch')
        with self.assertRaisesRegex(ValueError, 'branch differs'):
            planner.assert_fresh(plan)
        self.git('checkout', '-q', plan['pre_execution_snapshot']['branch'])
        self.stage('second.txt', 'later staged edit\n')
        with self.assertRaisesRegex(ValueError, 'index_fingerprint differs'):
            planner.assert_fresh(plan)
        self.git('restore', '--staged', '--source=HEAD', '--', 'second.txt')
        self.write('first.txt', 'all local content\n')
        _, plan = self.build([self.group(['first.txt'], content_mode='all-local')])
        self.write('first.txt', 'changed after review\n')
        with self.assertRaisesRegex(ValueError, 'content differs'):
            planner.assert_fresh(plan)
        self.git('commit', '-qm', 'advance HEAD')
        with self.assertRaisesRegex(ValueError, 'head differs'):
            planner.assert_fresh(plan)

    def test_all_local_syncs_only_selected_index_paths(self):
        self.stage('first.txt', 'staged\n')
        self.write('first.txt', 'staged and unstaged\n')
        self.stage('second.txt', 'unrelated staged change\n')
        plan_id, _ = self.build([self.group(['first.txt'], content_mode='all-local')])
        self.replay(plan_id)
        self.assertEqual(self.git('show', 'HEAD:first.txt'), b'staged and unstaged\n')
        self.assertEqual(self.git('diff', '--', 'first.txt'), b'')
        self.assertEqual(self.git('diff', '--cached', '--name-only'), b'second.txt\n')

    def test_tampered_payload_hash_approval_paths_and_duplicate_steps_are_rejected(self):
        self.stage('first.txt', 'staged\n')
        _, plan = self.build()
        for update, expected in [({'payload_sha256': '0' * 64}, 'payload hash'),
                                 ({'add_paths': ['other.txt'], 'touched_paths': ['other.txt']}, 'unapproved'),
                                 ({'message': 'unreviewed'}, 'message mismatch')]:
            candidate = copy.deepcopy(plan)
            candidate['steps'][0].update(update)
            with self.assertRaisesRegex(ValueError, expected):
                planner.verify(candidate)
        candidate = copy.deepcopy(plan)
        duplicate = copy.deepcopy(candidate['steps'][0])
        duplicate['index'] = 2
        candidate['steps'].append(duplicate)
        with self.assertRaisesRegex(ValueError, 'overlapping'):
            planner.verify(candidate)
        for name in ['../outside.txt', '/tmp/outside.txt', 'C:/outside.txt', '.git/config', 'out/generated.txt']:
            with self.assertRaises(ValueError):
                planner.validate_path(name)
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.head)

    @unittest.skipIf(os.name == 'nt', 'POSIX executable-hook fixture; Windows hook execution needs native verification')
    def test_hook_failure_stops_sequence_and_retains_partial_progress(self):
        self.stage('first.txt', 'first commit\n')
        self.stage('second.txt', 'second commit\n')
        self.stage('untouched.txt', 'third commit\n')
        plan_id, _ = self.build([self.group(['first.txt']), self.group(['second.txt']), self.group(['untouched.txt'])])
        hook = self.write('.git/hooks/pre-commit', '#!/bin/sh\nif git diff --cached --name-only | grep -q second.txt; then exit 1; fi\n')
        hook.chmod(0o755)
        before_index = Path('.git/index').read_bytes()
        captured = io.StringIO()
        with redirect_stdout(captured), redirect_stderr(captured):
            with self.assertRaisesRegex(ValueError, 'git commit failed'):
                planner.replay('.slim/commit-plan.json', plan_id)
        self.assertEqual(int(self.git('rev-list', '--count', 'HEAD')), 2)
        self.assertEqual(self.git('show', 'HEAD:first.txt'), b'first commit\n')
        self.assertEqual(self.git('show', 'HEAD:second.txt'), b'one\ntwo\nthree\n')
        self.assertEqual(Path('.git/index').read_bytes(), before_index)
        retained = next(line.split(' at ', 1)[1] for line in captured.getvalue().splitlines() if line.startswith('Replay index retained'))
        index = Path(retained)
        self.assertTrue(index.exists())
        for file in index.parent.iterdir():
            file.unlink()
        index.parent.rmdir()

    def test_cli_dry_run_errors_and_json_output(self):
        self.stage('first.txt', 'staged\n')
        self.build()
        result = subprocess.run([sys.executable, str(SCRIPT), 'discover'], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)['schema_version'], 1)
        for args, code in [(['verify', '--plan-json', '.slim/commit-plan.json'], 0),
                           (['replay', '--plan-json', '.slim/commit-plan.json', '--dry-run'], 0),
                           (['replay', '--plan-json', '.slim/commit-plan.json'], 1),
                           (['verify', '--plan-json', 'missing.json'], 1), (['replay'], 2)]:
            result = subprocess.run([sys.executable, str(SCRIPT), *args], capture_output=True)
            self.assertEqual(result.returncode, code, result.stderr)
        self.assertEqual(self.git('rev-parse', 'HEAD'), self.head)

    def test_detached_head_conflict_and_atomic_builder_failure(self):
        self.stage('first.txt', 'staged\n')
        self.build()
        before = Path('.slim/commit-plan.json').read_bytes()
        self.write('.slim/grouping.json', json.dumps({'commits': [self.group(['missing.txt'])]}))
        with self.assertRaises(ValueError):
            planner.build('.slim/grouping.json', '.slim/commit-plan.json')
        self.assertEqual(Path('.slim/commit-plan.json').read_bytes(), before)
        self.git('checkout', '--detach', '-q')
        with self.assertRaises(ValueError):
            planner.discover([])
        self.git('checkout', '-q', '-')
        oid = self.git('rev-parse', 'HEAD:first.txt').decode().strip()
        self.git('update-index', '--index-info', input_data=f'0 {"0" * 40}\tfirst.txt\n100644 {oid} 1\tfirst.txt\n100644 {oid} 2\tfirst.txt\n'.encode())
        with self.assertRaisesRegex(ValueError, 'Merge conflicts'):
            planner.discover([])


if __name__ == '__main__':
    unittest.main()
