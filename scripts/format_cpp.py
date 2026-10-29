#!/usr/bin/env python3
"""Cross-platform C++ format gate. Requires Python 3.10+, Git and clang-format.

Default scope is tracked changes against HEAD plus untracked sources. Explicit
--paths and --all format whole files. Check never writes sources. Exit codes:
0 passed, 1 drift/unsafe/non-convergent, 2 usage/tool failure.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
EXTENSIONS = {'.cpp', '.cppm', '.h'}
EXCLUDED = re.compile(r'(^|/)(out|build|cmake-build[^/]*|Temp|Testing|_deps|vcpkg_installed|\.slim|\.idea|\.git|\.codegraph|\.windbg-symbol-cache)(/|$)')


def run(args, root=ROOT):
    return subprocess.run(args, cwd=root, check=True, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE).stdout


def sources(directory):
    for current, dirs, files in os.walk(directory):
        dirs[:] = [d for d in dirs if not EXCLUDED.search((Path(current) / d).as_posix())]
        for name in files:
            path = Path(current) / name
            if path.suffix in EXTENSIONS:
                yield path.resolve()


def select(root, base, paths, all_sources):
    if all_sources:
        return {p: [] for p in sources(root)}
    if paths:
        work = {}
        for name in paths:
            path = (root / name).resolve()
            if not path.exists():
                raise ValueError(f'Path not found: {name}')
            if path.is_dir():
                work.update((p, []) for p in sources(path))
            else:
                work[path] = []
        return work

    work = {}
    names = run(['git', 'diff', '--name-only', '-z', '--diff-filter=ACMR', base, '--'], root)
    for name in names.decode('utf-8').split('\0'):
        path = root / name
        if path.suffix not in EXTENSIONS or not path.is_file():
            continue
        diff = run(['git', 'diff', '-U0', '--no-color', base, '--', name], root).decode('utf-8')
        ranges = []
        for match in re.finditer(r'^@@ -\S+ \+(\d+)(?:,(\d+))? @@', diff, re.M):
            start = int(match[1])
            count = int(match[2]) if match[2] is not None else 1
            if count:
                ranges.append(f'{start}:{start + count - 1}')
        if ranges:
            work[path] = ranges

    names = run(['git', 'ls-files', '--others', '--exclude-standard', '-z', '--'], root)
    for name in names.decode('utf-8').split('\0'):
        path = root / name
        if path.suffix in EXTENSIONS and path.is_file():
            work[path] = []
    return work


# Lex literals before comments, preserving literal bytes and token boundaries.
# Fail closed for malformed literals instead of hiding possible code changes.
LEX = re.compile(r'''(?P<raw>(?:u8|u|U|L)?R"(?P<delimiter>[^()\s\\]{0,16})\(.*?\)(?P=delimiter)")|(?P<string>(?:u8|u|U|L)?"(?:[^"\\\r\n]|\\.)*")|(?P<char>(?:u8|u|U|L)?'(?:[^'\\\r\n]|\\.)*')|(?P<comment>/\*.*?\*/|//[^\r\n]*)|(?P<space>\s+)|(?P<word>[0-9](?:[A-Za-z_0-9.']|[eEpP][+-])*|[A-Za-z_][A-Za-z_0-9]*)|(?P<operator><=>|>>=|<<=|->\*|\.\.\.|::|->|\+\+|--|&&|\|\||==|!=|<=|>=|<<|>>|\+=|-=|\*=|/=|%=|&=|\|=|\^=|##)|(?P<other>.)''', re.S)


SPLICE = re.compile(r'\\(?:\r\n|\n|\r)')
RAW_OPENER = re.compile(r'(?:u8|u|U|L)?R"')


def token_stream(text, ignore_splices=False):
    tokens = []
    pos = 0
    while pos < len(text):
        if ignore_splices:
            splice = SPLICE.match(text, pos)
            if splice:
                pos += len(splice[0])
                continue
        if RAW_OPENER.match(text, pos):
            raw = LEX.match(text, pos)
            if raw.lastgroup != 'raw':
                raise ValueError('Unrecognized raw string literal')
        match = LEX.match(text, pos)
        kind = match.lastgroup
        if kind == 'other' and match[0] in {'"', "'"}:
            raise ValueError('Unrecognized string/character literal')
        if kind not in {'space', 'comment'}:
            # C++11 permits adjacent template closers as either `> >` or `>>`.
            # clang-format coalesces these; normalize only that pair, retaining
            # all other operator boundaries (including >>= and + + versus ++).
            tokens.extend(['>', '>'] if match[0] == '>>' else [match[0]])
        pos = match.end()
    return tokens


def normalize(data):
    return data.replace(b'\r\n', b'\n').replace(b'\r', b'\n')


def format_one(path, ranges, formatter, style, budget, fix):
    before = path.read_bytes()
    with tempfile.TemporaryDirectory(prefix='ppr-format-') as temporary:
        scratch = Path(temporary) / path.name
        scratch.write_bytes(before)
        previous = before
        for _ in range(budget):
            command = [formatter, f'--style=file:{style}', '-i']
            command.extend(f'--lines={r}' for r in ranges)
            run(command + [str(scratch)])
            after = scratch.read_bytes()
            if normalize(after) == normalize(previous):
                break
            previous = after
        else:
            return 'UNSTABLE', path

        try:
            original = before.decode('utf-8-sig')
            formatted = after.decode('utf-8-sig')
            if token_stream(original, True) != token_stream(formatted, True):
                return 'UNSAFE', path
        except (ValueError, UnicodeError):
            return 'UNSAFE', path

        if normalize(before) == normalize(after):
            return 'PASS', path
        if not fix:
            return 'DRIFT', path
        # Do not overwrite edits made while the formatter was running.
        if path.read_bytes() != before:
            raise ValueError(f'Source changed during formatting: {path}')
        path.write_bytes(after)
        return 'FIXED', path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=['check', 'fix'], default='check')
    parser.add_argument('--base', default='HEAD')
    scope = parser.add_mutually_exclusive_group()
    scope.add_argument('--all', action='store_true')
    scope.add_argument('--paths', nargs='+')
    parser.add_argument('--max-passes', type=int, choices=range(1, 11), default=4)
    parser.add_argument('--jobs', type=int, default=1, help='Parallel formatter workers (1–128)')
    args = parser.parse_args()
    if not 1 <= args.jobs <= 128:
        parser.error('--jobs must be between 1 and 128')

    try:
        style = ROOT / '.clang-format'
        if not style.is_file() or not style.stat().st_size:
            raise ValueError('Repository .clang-format missing or empty')
        formatter = shutil.which('clang-format')
        if not formatter:
            raise ValueError('clang-format not found on PATH')
        print(run([formatter, '--version']).decode().strip())
        work = select(ROOT, args.base, args.paths, args.all)
        print(f'mode: {args.mode}; files: {len(work)}; max passes: {args.max_passes}')
        with ThreadPoolExecutor(max_workers=args.jobs) as pool:
            results = list(pool.map(lambda item: format_one(*item, formatter, style,
                                args.max_passes, args.mode == 'fix'), sorted(work.items())))
        for status, path in results:
            print(f'{status} {path}')
        return int(any(status in {'DRIFT', 'UNSAFE', 'UNSTABLE'} for status, _ in results))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        if isinstance(error, subprocess.CalledProcessError):
            print(error.stderr.decode(errors='replace'), file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
