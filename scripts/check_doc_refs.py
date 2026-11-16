#!/usr/bin/env python3
"""Audit local documentation references without network or plugin-cache access.

Findings are missing paths, missing-skill targets, and informational external
skills. JSON contains findings and summary (files, missing, missing-skill,
external). Only missing local targets fail the audit. This is a conservative
reference scanner, not a Markdown renderer or a command-line flag validator.
By default, scan current documents for Markdown links, skill references, and
explicit scripts/ helper references. Historical trees and general quoted paths
are independent opt-ins.
"""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import sys
from urllib.parse import unquote

ROOT = Path(__file__).resolve().parent.parent
# Provider identities, not installation paths. A repository-local installation
# takes precedence; availability in a particular agent's catalog is not inferred.
PROVIDER_SKILLS = set('''brainstorming diagnosing-superpowers dispatching-parallel-agents
executing-plans finishing-a-development-branch receiving-code-review requesting-code-review
subagent-driven-development systematic-debugging test-driven-development using-git-worktrees
using-superpowers verification-before-completion writing-plans writing-skills reflect
verification-planning'''.split())
TOP_LEVEL = {'.opencode', 'scripts', 'docs', 'lib', 'game', 'include', 'assets', 'cmake', 'third_party'}
EXTENSIONS = {'.md', '.py', '.ps1', '.sh', '.json', '.toml', '.yaml', '.yml', '.txt', '.cmake',
              '.cpp', '.cppm', '.h', '.hpp', '.slang', '.puml', '.png', '.svg'}
QUOTED = re.compile(r'`([^`\n]+)`|"([^"\n]+)"|\'([^\'\n]+)\'')
LINK = re.compile(r'!?\[[^\]\n]*\]\(\s*(<[^>\n]+>|[^\s()]+(?:\([^()]*\)[^\s()]*)*)(?:\s+["\'][^\n]*?["\'])?\s*\)')
FENCE = re.compile(r'^\s{0,3}(`{3,}|~{3,})(.*)$')
FOREIGN_HOST = re.compile(r'(?i)(?:[a-z]:[\\/]|/(?:home|Users|mnt)/|copied from another host)')
SHELLS = {'sh', 'bash', 'shell', 'console', 'powershell', 'pwsh', 'bat', 'cmd'}
HISTORICAL_NAMES = {'historical', 'history', 'archive', 'archives', 'archived', 'legacy',
                    'retired', 'deprecated', 'old', 'plans', 'proposals'}
HISTORICAL_WORDS = re.compile(r'\b(?:historical|archives?|archived|legacy|retired|superseded|designs|proposals)\b', re.I)
HELPER = re.compile(r'(?<![\w/.:\\-])(?:\./)?scripts/[\w./-]+\.(?:py|ps1|sh|bat|cmd)\b')


def historical_directories(docs):
    """Recognize explicit directory labels, not incidental historical discussion.

    A README may label its own directory in its title or a "this directory"
    declaration, or label children using "`name/` ... contains/are ...".
    Merely linking to an archive does not mark the README's directory historical.
    """
    excluded = {docs / name for name in ('superpowers', 'plans', 'diagrams')}
    for directory in docs.rglob('*'):
        if directory.is_dir() and set(re.split(r'[-_\s]+', directory.name.lower())) & HISTORICAL_NAMES:
            excluded.add(directory)
    for readme in docs.rglob('*'):
        if not readme.is_file() or readme.name.lower() != 'readme.md':
            continue
        text = readme.read_text(encoding='utf-8-sig')
        title = re.search(r'^#\s+(.+)$', text, re.M)
        own_label = re.search(r'\b(?:this directory|this folder|these documents)\s+[^.]+', text, re.I)
        if ((title and HISTORICAL_WORDS.search(title[1])) or
                (own_label and HISTORICAL_WORDS.search(own_label[0]))):
            excluded.add(readme.parent)
        for paragraph in re.split(r'\n\s*\n', text):
            declaration = re.match(r'^\s*(`.*?)(?:\bcontains?\b|\bholds?\b|\bare\b|\bis\b)(.*)', paragraph, re.S)
            if declaration and HISTORICAL_WORDS.search(declaration[2]):
                for name in re.findall(r'`([^`]+/)`', declaration[1]):
                    candidate = readme.parent / name
                    if not Path(name).is_absolute() and '..' not in Path(name).parts:
                        excluded.add(candidate)
    return excluded


def documents(root, include_historical=False):
    files = {root / name for name in ('AGENTS.md', 'codemap.md') if (root / name).is_file()}
    files.update((root / '.opencode').rglob('*.md'))
    docs = root / 'docs'
    excluded = set() if include_historical else historical_directories(docs)
    files.update(path for path in docs.rglob('*.md')
                 if not any(path.is_relative_to(directory) for directory in excluded))
    return files


def prose_lines(text):
    """Keep line numbers; suppress host-specific shell examples as a unit."""
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        opening = FENCE.match(lines[index])
        if not opening:
            yield index + 1, lines[index]
            index += 1
            continue

        start = index + 1
        index = start
        while index < len(lines):
            closing = FENCE.match(lines[index])
            if closing and closing[1][0] == opening[1][0] and len(closing[1]) >= len(opening[1]) and not closing[2].strip():
                break
            index += 1
        language = opening[2].strip().lower().split()
        foreign = language and language[0] in SHELLS and FOREIGN_HOST.search('\n'.join(lines[start:index]))
        # Language-specific data/code and Markdown demonstration fences describe
        # examples, not repository references. Plain and local shell fences can
        # still contain actionable quoted paths.
        example = language and language[0] not in SHELLS
        if not foreign and not example:
            for number in range(start, index):
                yield number + 1, lines[number]
        index += 1


def local_target(raw):
    target = unquote(raw.strip())
    if target.startswith('<') and target.endswith('>'):
        target = target[1:-1]
    if not target or re.search(r'\$|<[^>]*>|%\w+%|[*{}]|\.\.\.|…|/\.[a-z]+$', target):
        return None
    if target.startswith('path/to/') or re.search(r'(?:^|[/.])Foo(?:[/.]|$)', target):
        # Conventional example placeholders are not filesystem references.
        return None
    if target.startswith(('#', '/', '~', '\\')) or re.match(r'^[\w+.-]+:', target):
        return None
    target = target.split('#', 1)[0].split('?', 1)[0]
    target = re.sub(r':\d+(?:-\d+)?$', '', target).replace('\\', '/')
    parts = Path(target).parts
    if any(part in {'.slim', 'out', 'CMakeFiles'} for part in parts):
        return None
    if any(parts[i:i + 2] == ('docs', 'diagrams') for i in range(len(parts) - 1)):
        return None
    return target or None


def path_like(target):
    return '/' in target and (Path(target).suffix in EXTENSIONS or
                              target.removeprefix('./').split('/')[0] in TOP_LEVEL)


def skill_names(line, local_skills):
    # Explicit unknown "load `name` skill" is actionable too: otherwise removed
    # local skill directories would make their own references invisible.
    pattern = r'\b(?:use|load|see)\s+(?:(?:the|existing)\s+)?(?:skill\s+)?[`"\'*]*([\w-]+(?::[\w-]+)?)[`"\'*]*'
    for match in re.finditer(pattern, line, re.I):
        name = match[1]
        bare = name.removeprefix('superpowers:')
        explicit = (re.match(r'\s+skill\b', line[match.end():], re.I) and
                    re.search(r'[`"\']' + re.escape(name) + r'[`"\']', match[0]))
        if bare in local_skills or bare in PROVIDER_SKILLS or explicit or name.startswith('superpowers:'):
            if bare not in {'this', 'the', 'a', 'its'}:
                yield bare, name.startswith('superpowers:')


def audit(root, include_historical=False, include_quoted_paths=False):
    if not root.is_dir():
        raise ValueError(f'Repository directory not found: {root}')
    files = documents(root, include_historical)
    skills_dir = root / '.opencode/skills'
    local_skills = {path.name for path in skills_dir.iterdir() if path.is_dir()} if skills_dir.is_dir() else set()
    include_roots = [root / 'include', *(root / 'lib').glob('**/include')]
    findings = set()
    for path in sorted(files):
        relative = path.relative_to(root).as_posix()
        text = path.read_text(encoding='utf-8-sig')
        aliases = dict(re.findall(r'`([A-Z]+)/`\s+(?:below\s+)?is\s+`([^`]+/)`', text))
        for number, line in prose_lines(text):
            references = [(match[1], False) for match in LINK.finditer(line)]
            # Mask links to avoid treating quoted titles as paths.
            remainder = LINK.sub('', line)
            for match in QUOTED.finditer(remainder):
                raw = next(value for value in match.groups() if value is not None)
                # Operational helper names remain actionable even when general
                # quoted-path inference is disabled. Also recognize helpers in
                # inline commands, without attempting shell/flag validation.
                references.extend((helper[0], True) for helper in HELPER.finditer(raw))
                if include_quoted_paths:
                    target = local_target(raw)
                    if target and path_like(target) and not re.search(r'\s', target):
                        references.append((raw, True))
            for raw, allow_root in references:
                target = local_target(raw)
                if target and allow_root:
                    prefix, _, suffix = target.partition('/')
                    if prefix in aliases:
                        target = aliases[prefix] + suffix
                    # Public include spellings are resolved by the compiler's
                    # include root rather than the document's directory.
                    if any((include / target).is_file() for include in include_roots):
                        continue
                if target and not (path.parent / target).exists() and not (allow_root and (root / target).exists()):
                    findings.add((relative, number, 'missing', target))
            for name, provider in skill_names(line, local_skills):
                if not (skills_dir / name / 'SKILL.md').is_file():
                    kind = 'external' if provider or name in PROVIDER_SKILLS else 'missing-skill'
                    findings.add((relative, number, kind, name))

    ordered = [dict(zip(('path', 'line', 'kind', 'target'), finding)) for finding in sorted(findings)]
    counts = Counter(finding['kind'] for finding in ordered)
    summary = {'files': len(files), **{kind: counts[kind] for kind in ('missing', 'missing-skill', 'external')}}
    return {'findings': ordered, 'summary': summary}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT, help='Repository root (default: script checkout)')
    parser.add_argument('--json', action='store_true', help='Emit findings and summary as JSON')
    parser.add_argument('--include-historical', action='store_true',
                        help='Also scan historical, plan, proposal, and diagram documentation trees')
    parser.add_argument('--include-quoted-paths', action='store_true',
                        help='Also infer general repository paths from backticks and quotes')
    args = parser.parse_args()
    try:
        result = audit(args.root.resolve(strict=True), args.include_historical, args.include_quoted_paths)
        if args.json:
            print(json.dumps(result, indent=2))
        else:
            for finding in result['findings']:
                print('{path}:{line}: {kind}: {target}'.format(**finding))
            print('Summary: ' + '; '.join(f'{key}: {value}' for key, value in result['summary'].items()))
        return int(bool(result['summary']['missing'] or result['summary']['missing-skill']))
    except (OSError, ValueError) as error:
        print(f'ERROR: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
