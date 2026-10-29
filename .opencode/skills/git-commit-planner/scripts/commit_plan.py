#!/usr/bin/env python3
"""Portable commit-plan discovery, construction, verification and explicit replay.

Requires Python 3.10+ and Git. Run from the repository root. Discovery is
read-only; build writes only a plan artifact. Replay requires --dry-run or the
SHA-256 plan ID supplied by --confirmed-plan-id. Exit 1 means a refused plan or
Git failure; argparse usage errors exit 2. Never pushes or bypasses hooks.
"""
import argparse
import base64
from collections import defaultdict
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import tempfile
import textwrap

ARTIFACT_LIMIT = 1048576
FORBIDDEN = {'out', '_deps', 'vcpkg_installed', 'build', 'imgui_module_bindings', '.slim', '.git'}


def git(*arguments, index=None, input_data=None):
    environment = os.environ.copy()
    environment['GIT_OPTIONAL_LOCKS'] = '0'
    if index is not None:
        environment['GIT_INDEX_FILE'] = str(index)
    result = subprocess.run(['git', '--literal-pathspecs', *arguments], input=input_data,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=environment)
    if result.returncode:
        raise ValueError(f'git {arguments[0]} failed ({result.returncode}): '
                         + result.stderr.decode('utf-8', errors='replace').strip())
    return result.stdout


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def nul_paths(data):
    return [name.decode('utf-8') for name in data.split(b'\0') if name]


def validate_path(path):
    if not isinstance(path, str) or not path or '\0' in path:
        raise ValueError('Expected a nonempty literal repository-relative path')
    # Git reports forward slashes on both hosts. Reject drive paths on Linux
    # too, so a reviewed payload cannot become absolute when moved to Windows.
    if path.startswith('/') or '\\' in path or re.match(r'^[A-Za-z]:', path):
        raise ValueError(f'Not a portable repository-relative path: {path!r}')
    parts = path.split('/')
    if any(part in {'', '.', '..'} for part in parts):
        raise ValueError(f'Noncanonical repository path: {path!r}')
    if any(part.casefold() in FORBIDDEN or part.casefold().startswith('cmake-build-') for part in parts):
        raise ValueError(f'Forbidden commit artifact path: {path!r}')
    return path


def repository_preflight():
    root = Path(git('rev-parse', '--show-toplevel').decode().strip()).resolve()
    if Path.cwd().resolve() != root:
        raise ValueError('Run the helper from the repository root')
    if git('ls-files', '-u', '-z'):
        raise ValueError('Merge conflicts present; resolve before planning or replay')
    git('symbolic-ref', '--quiet', '--short', 'HEAD')
    git('rev-parse', '--verify', 'HEAD')


def snapshot():
    return {
        'head': git('rev-parse', 'HEAD').decode().strip(),
        'branch': git('symbolic-ref', '--quiet', '--short', 'HEAD').decode().strip(),
        'index_fingerprint': sha256(git('ls-files', '-s', '-z')),
    }


def diff_args(mode):
    if mode not in {'staged', 'all-local'}:
        raise ValueError(f'Unsupported content mode: {mode}')
    return ['diff', *(['--cached'] if mode == 'staged' else [])]


def patch(mode, paths):
    return git(*diff_args(mode), '--binary', 'HEAD', '--', *paths)


def change_units(mode, paths=()):
    fields = nul_paths(git(*diff_args(mode), '--name-status', '--find-renames', '-z', 'HEAD', '--', *paths))
    units = []
    i = 0
    while i < len(fields):
        status = fields[i]
        count = 2 if status.startswith(('R', 'C')) else 1
        names = fields[i + 1:i + 1 + count]
        if len(names) != count:
            raise ValueError('Malformed NUL-delimited Git status')
        units.append({'status': status, 'paths': names})
        i += count + 1
    return units


def discover(selections, all_local=False, max_files=15, max_bytes=204800):
    repository_preflight()
    if not 1 <= max_files <= 15 or not 1 <= max_bytes <= 204800:
        raise ValueError('Batch limits: --max-files 1–15, --max-bytes 1–204800')
    selections = [validate_path(path.rstrip('/')) for path in selections]
    def selected(path):
        return not selections or any(path == name or path.startswith(name + '/') for name in selections)

    mode = 'all-local' if all_local else 'staged'
    if all_local and any(selected(path) for path in nul_paths(git('ls-files', '--others', '--exclude-standard', '-z'))):
        raise ValueError('Untracked files match the literal selection; stage them before discovery')
    units = [unit for unit in change_units(mode) if any(selected(path) for path in unit['paths'])]
    if not units:
        raise ValueError('No applicable tracked changes')

    groups = defaultdict(list)
    for unit in units:
        for path in unit['paths']:
            validate_path(path)
        groups[str(PurePosixPath(unit['paths'][-1]).parent)].append(unit)
    candidates = []
    for scope, group in sorted(groups.items()):
        chunk, file_count, byte_count = [], 0, 0
        def append_chunk():
            candidates.append({'scope': scope, 'files': [p for unit in chunk for p in unit['paths']],
                               'file_count': file_count, 'estimated_patch_bytes': byte_count, 'units': chunk})

        for unit in sorted(group, key=lambda unit: unit['paths'][-1]):
            size = len(patch(mode, unit['paths']))
            count = len(unit['paths'])
            if count > max_files or size > max_bytes:
                raise ValueError('Single change unit exceeds artifact gate: ' + repr(unit['paths']))
            if chunk and (file_count + count > max_files or byte_count + size > max_bytes):
                append_chunk()
                chunk, file_count, byte_count = [], 0, 0
            chunk.append({**unit, 'estimated_patch_bytes': size})
            file_count += count
            byte_count += size
        if chunk:
            append_chunk()

    total = len(candidates)
    for candidate in candidates[:8]:
        candidate.update(total_batch_count=total, remaining_batch_count=total - 1)
    return {'schema_version': 1, 'mode': mode, 'selection': selections, 'candidates': candidates[:8],
            'total_batch_count': total, 'remaining_batch_count': total - min(total, 8),
            'grouping_schema': 'Selected groupings carry discovery_selection.total_batch_count; '
                               'the builder reports total_batch_count minus one.'}


def require_complete_units(mode, paths):
    selected = set(paths)
    for unit in change_units(mode):
        names = set(unit['paths'])
        if names & selected and not names <= selected:
            raise ValueError('Rename/copy must include both literal endpoints: ' + repr(unit['paths']))


def wrap_body(body):
    if not isinstance(body, str):
        raise ValueError('Message body must be a string')
    paragraphs = re.split(r'\n[ \t]*\n', body.replace('\r\n', '\n').strip())
    wrapped = []
    for paragraph in paragraphs:
        lines = paragraph.splitlines()
        # Trailer records must remain separate lines after a blank separator.
        # Reject overlong records in message() rather than split their values.
        if lines and all(re.match(r'^[A-Za-z0-9-]+: ', line) for line in lines):
            wrapped.append('\n'.join(lines))
        else:
            wrapped.append(textwrap.fill(paragraph, width=72, break_long_words=False, break_on_hyphens=False))
    return '\n\n'.join(wrapped)


def message(component, subject, body):
    if not all(isinstance(value, str) for value in (component, subject, body)):
        raise ValueError('Message component, subject and body must be strings')
    title = f'{component}: {subject}'
    if (not component.strip() or not subject.strip() or len(title) > 72 or
            '\n' in title or '\r' in title or '\0' in title or
            re.match(r'^(feat|fix|chore|refactor|docs|test|style)(\(.+\))?:', subject, re.I) or
            subject.endswith('.') or re.match(r'^[A-Z]', subject)):
        raise ValueError('Invalid component/subject under the commit-message convention')
    if '\0' in body or '\r' in body or any(len(line) > 72 for line in body.splitlines()):
        raise ValueError('Message body must use LF lines of at most 72 characters')
    return title + ('\n\n' + body if body else '')


@contextmanager
def temporary_index(head, preserve_on_error=False):
    directory = Path(tempfile.mkdtemp(prefix='ppr-commit-plan-'))
    index = directory / 'index'
    try:
        git('read-tree', head, index=index)
        yield index
    except BaseException:
        if preserve_on_error:
            print(f'Replay index retained at {index}', file=sys.stderr)
            raise
        raise
    finally:
        # Preserve a failed execution index for diagnosis. Verification indexes
        # always disappear, including failure paths. No caller index is removed.
        if not preserve_on_error or sys.exc_info()[0] is None:
            for path in directory.iterdir():
                path.unlink()
            directory.rmdir()


def verify(plan):
    if not isinstance(plan, dict) or type(plan.get('schema_version')) is not int or plan['schema_version'] != 4:
        raise ValueError('Unsupported plan schema; rebuild with commit_plan.py')
    before = plan.get('pre_execution_snapshot')
    if (not isinstance(before, dict) or not isinstance(before.get('head'), str) or
            not re.fullmatch(r'[0-9a-f]{40}|[0-9a-f]{64}', before['head']) or
            not isinstance(before.get('branch'), str) or not before['branch'] or
            not isinstance(before.get('index_fingerprint'), str) or
            not re.fullmatch(r'[0-9a-f]{64}', before['index_fingerprint'])):
        raise ValueError('Missing complete HEAD, branch or index fingerprint')
    steps = plan.get('steps')
    if not isinstance(steps, list) or not steps:
        raise ValueError('Plan has no steps')
    seen = set()
    for number, step in enumerate(steps, 1):
        if not isinstance(step, dict) or type(step.get('index')) is not int or step['index'] != number:
            raise ValueError(f'Invalid ordered step {number}')
        diff_args(step.get('content_mode'))
        for field in ['add_paths', 'touched_paths']:
            paths = step.get(field)
            if not isinstance(paths, list) or not paths:
                raise ValueError(f'Step {number}: missing {field}')
            for path in paths:
                validate_path(path)
            if len(set(paths)) != len(paths):
                raise ValueError(f'Step {number}: duplicate {field}')
        approved = set(step['add_paths'])
        if seen & approved or not set(step['touched_paths']) <= approved:
            raise ValueError(f'Step {number}: overlapping or unapproved paths')
        seen.update(approved)
        expected_message = message(step.get('component'), step.get('subject'), step.get('body'))
        # Schema-v4 PowerShell plans used an empty paragraph even for empty bodies.
        if step.get('message') not in {expected_message, expected_message + ('\n\n' if not step['body'] else '')}:
            raise ValueError(f'Step {number}: message mismatch')
        data = base64.b64decode(step['patch_b64'], validate=True)
        if not data or sha256(data) != step.get('payload_sha256'):
            raise ValueError(f'Step {number}: payload hash mismatch')
        with temporary_index(before['head']) as index:
            git('apply', '--cached', '--binary', '--whitespace=nowarn', '-', index=index, input_data=data)
            actual = set(nul_paths(git('diff', '--cached', '--name-only', '-z', before['head'], index=index)))
            if not actual or not actual <= set(step['touched_paths']):
                raise ValueError(f'Step {number}: payload touches unapproved paths')
    return plan


def load_json(path):
    data = Path(path).read_bytes()
    if len(data) > ARTIFACT_LIMIT:
        raise ValueError(f'JSON artifact exceeds {ARTIFACT_LIMIT} bytes')
    return data, json.loads(data.decode('utf-8-sig'))


def build(grouping_path, output):
    repository_preflight()
    if Path(grouping_path).resolve() == Path(output).resolve():
        raise ValueError('Grouping input and plan output must differ')
    _, grouping = load_json(grouping_path)
    before = snapshot()
    commits = grouping.get('commits')
    if not isinstance(commits, list) or not commits:
        raise ValueError('Grouping must contain a nonempty commits array')
    remaining = 'unknown'
    if 'discovery_selection' in grouping:
        total = grouping['discovery_selection']['total_batch_count']
        if type(total) is not int or total < 1:
            raise ValueError('Invalid discovery_selection.total_batch_count')
        remaining = total - 1
    steps = []
    for number, group in enumerate(commits, 1):
        paths = group.get('files')
        if not isinstance(paths, list) or not paths:
            raise ValueError(f'Empty commit group {number}')
        for path in paths:
            validate_path(path)
        mode = group.get('content_mode', 'staged')
        if mode == 'all-local' and nul_paths(git('ls-files', '--others', '--exclude-standard', '-z', '--', *paths)):
            raise ValueError('Untracked files match an all-local group; stage them first')
        require_complete_units(mode, paths)
        data = patch(mode, paths)
        touched = list(dict.fromkeys(paths + [p for unit in change_units(mode, paths) for p in unit['paths']]))
        body = wrap_body(group.get('body', ''))
        component, subject = group.get('component'), group.get('subject')
        steps.append({'index': number, 'component': component, 'subject': subject, 'body': body,
                      'message': message(component, subject, body), 'add_paths': paths, 'touched_paths': touched,
                      'content_mode': mode, 'patch_b64': base64.b64encode(data).decode('ascii'),
                      'payload_sha256': sha256(data), 'notes': group.get('notes', '')})
    plan = {'schema_version': 4, 'pre_execution_snapshot': before, 'steps': steps,
            'summary': {'total_steps': len(steps), 'selected_scope': [p for step in steps for p in step['add_paths']],
                        'selected_mode': list(dict.fromkeys(step['content_mode'] for step in steps)),
                        'remaining_batch_count': remaining, 'remainder_guidance': 'Run batch discovery again before planning another scope.',
                        'replay': 'isolated temporary index'}}
    verify(plan)
    assert_fresh(plan)
    data = (json.dumps(plan, ensure_ascii=True, indent=2) + '\n').encode('utf-8')
    if len(data) > ARTIFACT_LIMIT:
        raise ValueError(f'Serialized plan exceeds {ARTIFACT_LIMIT} bytes')
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    # Replace atomically on the same volume, including on Windows. The input
    # grouping is retained, so failed or successful builds never consume it.
    handle, temporary = tempfile.mkstemp(prefix=output.name + '.', dir=output.parent)
    try:
        with os.fdopen(handle, 'wb') as stream:
            stream.write(data)
        os.replace(temporary, output)
    finally:
        Path(temporary).unlink(missing_ok=True)
    return sha256(data), len(steps)


def assert_fresh(plan):
    repository_preflight()
    current = snapshot()
    for field in ['head', 'branch', 'index_fingerprint']:
        if current[field] != plan['pre_execution_snapshot'][field]:
            raise ValueError(f'{field} differs from the reviewed plan; re-plan')
    for step in plan['steps']:
        require_complete_units(step['content_mode'], step['add_paths'])
        if sha256(patch(step['content_mode'], step['add_paths'])) != step['payload_sha256']:
            raise ValueError(f'Step {step["index"]} content differs from reviewed plan; re-plan')


def replay(plan_path, confirmed_plan_id=None, dry_run=False):
    raw, plan = load_json(plan_path)
    verify(plan)
    assert_fresh(plan)
    plan_id = sha256(raw)
    print(f'verified plan_id={plan_id} with {len(plan["steps"])} ordered commit(s)')
    for step in plan['steps']:
        print(f'step {step["index"]}: {step["message"]!r} [approved: {step["add_paths"]!r}]')
    if dry_run:
        print('Dry run complete; no mutation was attempted.')
        return
    if confirmed_plan_id != plan_id:
        raise ValueError('Mutation requires --confirmed-plan-id matching the verified plan_id')
    assert_fresh(plan)
    completed = []
    try:
        with temporary_index(plan['pre_execution_snapshot']['head'], preserve_on_error=True) as index:
            for step in plan['steps']:
                data = base64.b64decode(step['patch_b64'], validate=True)
                git('apply', '--cached', '--binary', '--whitespace=nowarn', '-', index=index, input_data=data)
                git('commit', '-F', '-', index=index, input_data=step['message'].encode('utf-8'))
                commit = git('rev-parse', 'HEAD').decode().strip()
                completed.append({'step': step['index'], 'hash': commit})
                print(f'committed step {step["index"]}: {commit}')
                if step['content_mode'] == 'all-local':
                    git('restore', '--staged', '--source=HEAD', '--', *step['touched_paths'])
    finally:
        print('completed commit(s): ' + json.dumps(completed))
        # Reporting must not obscure the original hook/commit error.
        try:
            print(git('status', '--short', '--branch').decode('utf-8', errors='replace'))
        except ValueError as error:
            print(f'Final status unavailable: {error}', file=sys.stderr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    discovery = commands.add_parser('discover', help='Read staged changes and emit candidate batches as JSON')
    discovery.add_argument('--paths', nargs='+', default=[])
    discovery.add_argument('--all-local', action='store_true')
    discovery.add_argument('--max-files', type=int, default=15)
    discovery.add_argument('--max-bytes', type=int, default=204800)
    builder = commands.add_parser('build', help='Build and verify a schema-v4 replay plan from grouping JSON')
    builder.add_argument('--grouping-json', required=True)
    builder.add_argument('--out', default='.slim/commit-plan.json')
    verifier = commands.add_parser('verify', help='Verify an existing plan without committing')
    verifier.add_argument('--plan-json', required=True)
    executor = commands.add_parser('replay', help='Dry-run or explicitly execute a verified plan')
    executor.add_argument('--plan-json', required=True)
    executor.add_argument('--confirmed-plan-id')
    executor.add_argument('--dry-run', action='store_true')
    args = parser.parse_args()
    try:
        if args.command == 'discover':
            print(json.dumps(discover(args.paths, args.all_local, args.max_files, args.max_bytes), indent=2))
        elif args.command == 'build':
            plan_id, count = build(args.grouping_json, args.out)
            print(f'wrote {args.out} ({count} exact patch steps, plan_id={plan_id})')
        elif args.command == 'verify':
            repository_preflight()
            raw, plan = load_json(args.plan_json)
            verify(plan)
            print(f'gate: schema-v4 plan ID={sha256(raw)} and approved payload paths OK')
        else:
            replay(args.plan_json, args.confirmed_plan_id, args.dry_run)
        return 0
    except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
