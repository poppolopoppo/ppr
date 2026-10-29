#!/usr/bin/env python3
"""Read-only C++ alternative-token/style tripwire (Python 3.10+).

A-STRANDED, B-OP and C-LEADING block. B-CMP, C-DETACHED and D-DENSE
are review advisories; --strict makes advisories fail too. This lexical
heuristic does not establish logical paragraph or section-header correctness.
Exit codes: 0 clean/advisory-only, 1 findings, 2 invalid input/tool error.
"""
import argparse
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
import re
import sys

from format_cpp import ROOT, sources

BLOCKING = {'A-STRANDED', 'B-OP', 'C-LEADING'}
EXCLUDED = re.compile(r'(^|/)(\.vscode|local-services/pix|captures|_staging)(/|$)')
GENERATED = re.compile(r'(\.generated|\.pb)\.(cpp|cppm|h)$')


@dataclass
class Line:
    text: str
    code: str
    blank: bool
    comment: bool
    preproc: bool


def split_lines(text):
    """Match the legacy scanner's deliberately conservative line heuristics.

    Raw/multiline strings and macro continuations can confuse this model;
    review such findings in context rather than treating it as a C++ parser.
    """
    result = []
    in_block = False
    for original in text.splitlines():
        code = original
        if in_block:
            end = code.find('*/')
            if end < 0:
                result.append(Line(original, '', not original.strip(), bool(original.strip()), False))
                continue
            code = code[end + 2:]
            in_block = False

        code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)
        code = re.sub(r"'(?:\\.|[^'\\])*'", "''", code)
        while '/*' in code:
            start = code.index('/*')
            end = code.find('*/', start + 2)
            if end < 0:
                code = code[:start]
                in_block = True
                break
            code = code[:start] + ' ' + code[end + 2:]
        code = code.split('//', 1)[0]
        stripped = original.strip()
        comment = not code.strip() and (stripped.startswith(('//', '/*', '*')) or in_block)
        result.append(Line(original, code, not stripped, comment, bool(re.match(r'^\s*#', code))))
    return result


def scan(text, extension='.cpp', dense_min=30):
    lines = split_lines(text)
    hits = []

    def hit(index, kind, suffix=''):
        excerpt = lines[index].text.strip()
        hits.append((index + 1, kind, excerpt[:140] + ('...' if len(excerpt) > 140 else '') + suffix))

    for i, line in enumerate(lines):
        code = line.code
        if line.blank or line.comment or line.preproc or not code.strip():
            continue
        stranded = bool(re.search(r'^\s*(and|or)\s*$', code))
        if stranded or re.search(r'\bnot\s*$', code):
            hit(i, 'A-STRANDED')
        if re.search(r'\b(and|or|not)\(', code):
            hit(i, 'B-OP')
        condition = re.search(r'\b(if|else|for|while|return|and|or|not|assert|EXPECT|CHECK|static_assert|requires)\b', code, re.I)
        if condition and not re.search(r'^\s*template\b|\boperator\s*<', code):
            if re.search(r'''[\w\)\]"']<(?![<=>])''', code):
                hit(i, 'B-CMP')
        if not stranded and re.search(r'^\s*(and|or)\b', code):
            hit(i, 'C-LEADING')
        if re.search(r'\b(and|or)\s*$', code):
            seen_close = ')' in code
            for j in range(i + 1, min(len(lines), i + 9)):
                next_line = lines[j]
                if next_line.blank or next_line.comment:
                    continue
                if next_line.preproc:
                    break
                seen_close |= ')' in next_line.code
                if re.search(r'^\s*\{\s*$', next_line.code):
                    if seen_close:
                        hit(j, 'C-DETACHED')
                    break
                if re.search(r'[{;]\s*$', next_line.code):
                    break

    if extension != '.cpp':
        return hits

    def braces(code):
        return code.count('{') - code.count('}')

    depth = 0
    i = 0
    while i < len(lines):
        line = lines[i]
        code = line.code
        candidate = False
        paren = code.find('(')
        if not (line.blank or line.comment or line.preproc) and paren >= 0 and ')' in code and not re.search(r';\s*$', code):
            prefix = code[:paren]
            if (re.search(r'[A-Za-z_][\w:]*\s*$', prefix) and
                    not re.search(r'^\s*(if|for|while|switch|catch|else|class|struct|enum|namespace)\b', code) and
                    '=' not in prefix and depth <= 2):
                candidate = bool(re.search(r'\{\s*$', code))
                if not candidate:
                    for following in lines[i + 1:i + 4]:
                        if following.blank or following.comment:
                            continue
                        candidate = bool(re.search(r'^\s*\{\s*$', following.code))
                        break
        if not candidate:
            depth += braces(code)
            i += 1
            continue

        body_depth = depth
        opening = None
        j = i
        while j < len(lines):
            body_depth += braces(lines[j].code)
            if opening is None and '{' in lines[j].code:
                opening = j
            if opening is not None and body_depth <= depth:
                break
            j += 1
            if j - i > 3000:
                break
        if opening is not None and j < len(lines):
            body = lines[opening:j + 1]
            code_count = sum(not item.blank and not item.comment and bool(item.code.strip()) for item in body)
            if code_count >= dense_min and not any(item.blank for item in body):
                hit(i, 'D-DENSE', f' [{code_count} code lines, 0 blanks]')
            depth = body_depth
            i = j + 1
        else:
            depth += braces(code)
            i += 1
    return hits


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--files', nargs='+', help='Explicit paths; comma/semicolon lists also accepted')
    parser.add_argument('--repo-root', type=Path, default=ROOT)
    parser.add_argument('--path-filter', default='')
    parser.add_argument('--dense-min-code-lines', type=int, default=30)
    parser.add_argument('--strict', action='store_true')
    parser.add_argument('--show-advisories', action='store_true')
    args = parser.parse_args()

    try:
        root = args.repo_root.resolve(strict=True)
        if args.files:
            files = []
            for entry in args.files:
                for name in re.split('[,;]', entry):
                    if not name.strip():
                        continue
                    path = (root / name.strip()).resolve(strict=True)
                    if path.is_file() and path.suffix in {'.cpp', '.cppm', '.h'}:
                        files.append(path)
                    else:
                        print(f'Skipped non-C++ file: {path}')
            if not files:
                raise ValueError('No C++ sources resolved from --files')
        else:
            files = [p for p in sources(root) if not EXCLUDED.search(p.as_posix()) and
                     not GENERATED.search(p.name) and args.path_filter.casefold() in p.as_posix().casefold()]

        findings = []
        for path in sorted(set(files)):
            try:
                relative = str(path.relative_to(root))
            except ValueError:
                relative = str(path)
            for line, kind, excerpt in scan(path.read_text(encoding='utf-8-sig'), path.suffix, args.dense_min_code_lines):
                findings.append((relative, line, kind, excerpt))
        for relative, line, kind, excerpt in sorted(findings):
            if kind in BLOCKING or args.strict or args.show_advisories:
                print(f'{relative}:{line}:{kind}: {excerpt}')
        counts = Counter(kind for _, _, kind, _ in findings)
        blockers = sum(counts[kind] for kind in BLOCKING)
        print(f'Files scanned: {len(set(files))}   Blocking: {blockers}   Advisory: {len(findings) - blockers}')
        for kind, count in sorted(counts.items()):
            print(f'  {kind}: {count}')
        if args.strict or args.show_advisories:
            directories = Counter(str(Path(relative).parent) for relative, _, _, _ in findings)
            for directory, count in directories.most_common():
                print(f'  {directory}: {count}')
        return int(bool(blockers or (args.strict and findings)))
    except (OSError, ValueError, UnicodeError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
