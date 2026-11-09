#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (c) 2026 FilePan contributors

"""Generates mime_table.inc, FilePan's file name -> MIME type and kind tables.

The data comes from freedesktop.org shared-mime-info (third_party/shared-mime-info), with
FilePan's decisions in mime_policy.txt. Run at build time, with the standard library only:

    python3 -I generate_mime_table.py --smi-dir <shared-mime-info> --policy mime_policy.txt \\
        --output mime_table.inc

Only simple globs are used: "*.ext" and the compound "*.a.b". A compound extension is kept only
when its type is an archive FilePan opens (tar.gz, src.rpm): any other one (pdf.gz, img.xz) names
a compressed file that the last extension describes better, so it goes by that. Split volumes
(7z.001) are dropped too: libarchive can't open them. Literal names (Makefile) and wildcard
patterns (*.[1-9]) are ignored, and so is content sniffing (magic), so the generator
fails on any extension shared-mime-info maps to several types at the same weight unless the policy
picks one. It also fails on policy lines that name unknown types or that change nothing, so a
submodule bump that adds an ambiguity or makes an override stale needs an explicit decision.

The output is deterministic and is only rewritten when it changes.
"""

import argparse
import re
import sys
import xml.etree.ElementTree as ET

NS = '{http://www.freedesktop.org/standards/shared-mime-info}'
XML_LANG = '{http://www.w3.org/XML/1998/namespace}lang'
SIMPLE_GLOB = re.compile(r'^\*\.([^*?\[\]]+)$')
# Characters that can go into a C++ string literal unescaped, and that MIME types and the
# extensions FilePan matches use.
SAFE = re.compile(r'^[A-Za-z0-9!#$&^_.+\-/]+$')

KINDS = ('Directory', 'Image', 'Video', 'Audio', 'Archive', 'Apk', 'Pdf', 'Document',
         'Spreadsheet', 'Presentation', 'Text', 'Code', 'Font', 'Other')

# generic-icon names (Icon Naming Specification) that say what kind a type is.
ICON_KINDS = {
    'x-office-document': 'Document',
    'x-office-spreadsheet': 'Spreadsheet',
    'x-office-presentation': 'Presentation',
    'font-x-generic': 'Font',
    'text-x-script': 'Code',
    'text-html': 'Code',
}
# A text type that inherits from one of these is source code or structured data.
CODE_ANCESTORS = ('text/x-csrc', 'text/x-c++src', 'application/javascript', 'application/xml',
                  'application/json', 'application/x-executable')
# Lookup in mime.cpp splits at most this many dot-separated parts off a name.
MAX_EXTENSION_PARTS = 2
# Matches kMaxExtensionLength in mime.cpp.
MAX_EXTENSION_LENGTH = 48


class Error(Exception):
    pass


class Type:
    def __init__(self, name):
        self.name = name
        self.comment = ''
        self.generic_icon = None
        self.parents = []
        self.aliases = []
        # (pattern, weight, case_sensitive)
        self.globs = []


def load_smi(path):
    root = ET.parse(path).getroot()
    types = {}
    for element in root.iter(NS + 'mime-type'):
        name = element.get('type').lower()
        if name in types:
            raise Error(f'{path}: duplicate type {name}')
        t = Type(name)
        for comment in element.findall(NS + 'comment'):
            if comment.get(XML_LANG) is None:
                t.comment = (comment.text or '').strip()
        icon = element.find(NS + 'generic-icon')
        if icon is not None:
            t.generic_icon = icon.get('name')
        t.parents = [e.get('type').lower() for e in element.findall(NS + 'sub-class-of')]
        t.aliases = [e.get('type').lower() for e in element.findall(NS + 'alias')]
        for glob in element.findall(NS + 'glob'):
            t.globs.append((glob.get('pattern'), int(glob.get('weight', '50')),
                            glob.get('case-sensitive') == 'true'))
        types[name] = t
    aliases = {}
    for t in types.values():
        for alias in t.aliases:
            if alias in types or aliases.get(alias, t.name) != t.name:
                raise Error(f'{path}: alias {alias} of {t.name} names another type')
            aliases[alias] = t.name
    return types, aliases


def smi_version(meson_build):
    with open(meson_build, encoding='utf-8') as f:
        match = re.search(r"version\s*:\s*'([^']+)'", f.read())
    if not match:
        raise Error(f'{meson_build}: no project version')
    return match.group(1)


class Policy:
    def __init__(self):
        # extension (as written) -> (type, kind or None, line number)
        self.extensions = {}
        # type -> (kind, line number)
        self.kinds = {}
        # type -> line number
        self.archives = {}


def load_policy(path):
    policy = Policy()
    with open(path, encoding='utf-8') as f:
        lines = f.read().splitlines()
    for number, line in enumerate(lines, 1):
        words = line.split('#', 1)[0].split()
        if not words:
            continue
        where = f'{path}:{number}'
        verb, args = words[0], words[1:]
        if verb == 'ext' and len(args) in (2, 3):
            extension, type_ = args[0], args[1].lower()
            kind = args[2] if len(args) == 3 else None
            if kind is not None and kind not in KINDS:
                raise Error(f'{where}: unknown kind {kind}')
            if extension.lower() in (e.lower() for e in policy.extensions):
                raise Error(f'{where}: duplicate extension {extension}')
            policy.extensions[extension] = (type_, kind, number)
        elif verb == 'kind' and len(args) == 2:
            type_, kind = args[0].lower(), args[1]
            if kind not in KINDS:
                raise Error(f'{where}: unknown kind {kind}')
            if type_ in policy.kinds:
                raise Error(f'{where}: duplicate kind line for {type_}')
            policy.kinds[type_] = (kind, number)
        elif verb == 'archive' and len(args) == 1:
            type_ = args[0].lower()
            if type_ in policy.archives:
                raise Error(f'{where}: duplicate archive line for {type_}')
            policy.archives[type_] = number
        else:
            raise Error(f'{where}: expected "ext <extension> <type> [<Kind>]", '
                        f'"kind <type> <Kind>" or "archive <type>"')
    return policy


class Generator:
    def __init__(self, types, aliases, policy, policy_path):
        self.types = types
        self.aliases = aliases
        self.policy = policy
        self.policy_path = policy_path
        self.kind_lines = {}
        self.archive_lines = set()

    def where(self, number):
        return f'{self.policy_path}:{number}'

    def canonical(self, name):
        return self.aliases.get(name, name)

    def known(self, name):
        return name in self.types or name in self.aliases

    def ancestors(self, name):
        """The sub-class-of ancestors of a canonical type, nearest first."""
        seen, queue = [], [name]
        while queue:
            parents = []
            for t in queue:
                for parent in self.types[t].parents:
                    parent = self.canonical(parent)
                    if parent in self.types and parent != name and parent not in seen:
                        seen.append(parent)
                        parents.append(parent)
            queue = parents
        return seen

    def derived_kind(self, name):
        """The kind of a canonical type, from the policy's kind and archive lines and the data."""
        if name == 'inode/directory':
            return 'Directory'
        if name in self.kind_lines:
            return self.kind_lines[name]
        if name in self.archive_lines:
            return 'Archive'
        t = self.types[name]
        if t.generic_icon in ICON_KINDS:
            return ICON_KINDS[t.generic_icon]
        # Media kinds come only from the top-level type: they start the platform decoders, so an
        # inherited icon (application/ogg, model/*) must not make a type Image, Video or Audio.
        top = name.split('/', 1)[0]
        if top in ('image', 'video', 'audio'):
            return top.capitalize()
        if top == 'font':
            return 'Font'
        chain = self.ancestors(name)
        for ancestor in chain:
            icon = self.types[ancestor].generic_icon
            if icon is not None:
                if icon in ICON_KINDS:
                    return ICON_KINDS[icon]
                break
        if 'text/plain' in chain and any(a in CODE_ANCESTORS for a in chain):
            return 'Code'
        if top == 'text' or 'text/plain' in chain:
            comment = t.comment.lower()
            if 'source code' in comment or comment.endswith(' script'):
                return 'Code'
            return 'Text'
        return 'Other'

    def resolve_kinds(self):
        for name, (kind, number) in self.policy.kinds.items():
            if not self.known(name):
                raise Error(f'{self.where(number)}: unknown type {name}')
            if name in self.aliases:
                raise Error(f'{self.where(number)}: {name} is an alias of '
                            f'{self.aliases[name]}; name the type')
        for name, number in self.policy.archives.items():
            if not self.known(name):
                raise Error(f'{self.where(number)}: unknown type {name}')
            if name in self.aliases:
                raise Error(f'{self.where(number)}: {name} is an alias of '
                            f'{self.aliases[name]}; name the type')
            if name in self.policy.kinds:
                raise Error(f'{self.where(number)}: {name} also has a kind line')
        # Each line must change the kind the type would otherwise get.
        redundant = [f'{self.where(number)}: redundant, {name} is {kind} already'
                     for name, (kind, number) in self.policy.kinds.items()
                     if self.derived_kind(name) == kind]
        redundant += [f'{self.where(number)}: redundant, {name} is Archive already'
                      for name, number in self.policy.archives.items()
                      if self.derived_kind(name) == 'Archive']
        if redundant:
            raise Error('\n'.join(redundant))
        self.kind_lines = {name: kind for name, (kind, _) in self.policy.kinds.items()}
        self.archive_lines = set(self.policy.archives)

    def candidates(self):
        """Simple glob candidates: (single, compound, exact_case), each key -> [(weight, len, type)]."""
        single, compound, exact_case = {}, {}, {}
        for t in self.types.values():
            for pattern, weight, case_sensitive in t.globs:
                match = SIMPLE_GLOB.match(pattern)
                if not match:
                    continue
                extension = match.group(1)
                parts = extension.split('.')
                if len(parts) > MAX_EXTENSION_PARTS or '' in parts:
                    raise Error(f'{t.name}: glob {pattern} has more than '
                                f'{MAX_EXTENSION_PARTS} parts')
                # A split volume (7z.001): libarchive opens only whole archives.
                if len(parts) > 1 and parts[-1].isdigit():
                    continue
                if case_sensitive and extension != extension.lower():
                    if len(parts) > 1:
                        raise Error(f'{t.name}: case-sensitive compound glob {pattern}')
                    table, key = exact_case, extension
                else:
                    table = compound if len(parts) > 1 else single
                    key = extension.lower()
                table.setdefault(key, []).append((weight, len(pattern), t.name))
        return single, compound, exact_case

    def pick(self, key, entries):
        """The type shared-mime-info picks for a key by name alone, or None on a tie."""
        best = max((w, n) for w, n, _ in entries)
        tied = sorted({t for w, n, t in entries if (w, n) == best})
        if len(tied) == 1:
            return tied[0], tied
        # One tied type that all the others inherit from is the safe, generic choice.
        roots = [t for t in tied if all(u == t or t in self.ancestors(u) for u in tied)]
        if len(roots) == 1:
            return roots[0], tied
        return None, tied

    def kind_of(self, type_, additions):
        """The kind an extension's type gets, as generate() emits it."""
        if type_ in self.aliases:
            kind = self.derived_kind(self.aliases[type_])
            top = type_.split('/', 1)[0].capitalize()
            media = ('Image', 'Video', 'Audio')
            return top if kind in media and top in media else kind
        if type_ in self.types:
            return self.derived_kind(type_)
        return additions[type_]

    def resolve_extensions(self):
        single, compound, exact_case = self.candidates()
        # Compound extensions only for archives FilePan opens; the rest go by the last extension.
        compound = {key: entries for key, entries in compound.items()
                    if any(self.derived_kind(t) == 'Archive' for _, _, t in entries)}
        tables = {'single': {}, 'compound': {}, 'exact': {}}
        unresolved = []
        for name, candidates in (('single', single), ('compound', compound),
                                 ('exact', exact_case)):
            for key, entries in candidates.items():
                picked, tied = self.pick(key, entries)
                if picked is None:
                    unresolved.append((key, tied))
                tables[name][key] = picked
        # The policy's ext lines: overrides, tie-breaks and additions.
        additions = {}
        redundant = []
        for extension, (type_, kind, number) in self.policy.extensions.items():
            where = self.where(number)
            if not SAFE.match(extension) or len(extension) > MAX_EXTENSION_LENGTH:
                raise Error(f'{where}: bad extension {extension}')
            parts = extension.split('.')
            if len(parts) > MAX_EXTENSION_PARTS or '' in parts:
                raise Error(f'{where}: bad extension {extension}')
            if extension in tables['exact']:
                table, key = tables['exact'], extension
            else:
                if extension != extension.lower():
                    raise Error(f'{where}: write {extension} in lower case')
                table = tables['compound' if len(parts) > 1 else 'single']
                key = extension
            if self.known(type_):
                if kind is not None:
                    raise Error(f'{where}: {type_} is a shared-mime-info type; '
                                f'give its kind with a kind line')
            elif kind is None:
                raise Error(f'{where}: unknown type {type_}; an addition needs a kind')
            elif type_ in additions and additions[type_] != kind:
                raise Error(f'{where}: {type_} already added as {additions[type_]}')
            else:
                additions[type_] = kind
            if key in table and table[key] == type_:
                redundant.append(f'{where}: redundant, shared-mime-info maps {extension} to '
                                 f'{type_} already')
            table[key] = type_
            if len(parts) > 1 and self.kind_of(type_, additions) != 'Archive':
                raise Error(f'{where}: {extension} is not an archive; compound extensions are '
                            f'only for archives, the rest go by the last extension')
        tables['compound'] = {key: type_ for key, type_ in tables['compound'].items()
                              if type_ is None or self.kind_of(type_, additions) == 'Archive'}
        if redundant:
            raise Error('\n'.join(redundant))
        unresolved = [(key, tied) for key, tied in unresolved
                      if key not in self.policy.extensions]
        if unresolved:
            lines = '\n'.join(f'  {key}: {" ".join(tied)}' for key, tied in sorted(unresolved))
            raise Error(f'{self.policy_path}: these extensions map to several types at the same '
                        f'weight; pick one with an ext line:\n{lines}')
        return tables, additions

    def generate(self, version):
        self.resolve_kinds()
        tables, additions = self.resolve_extensions()

        kinds = {name: self.derived_kind(name) for name in self.types}
        # Aliases named by ext lines are emitted as types too, so lookups return that spelling.
        # They take the kind of their type, but a media kind follows the alias's own top-level
        # type: audio/3gpp is Audio though video/3gpp is Video.
        for table in tables.values():
            for type_ in table.values():
                if type_ not in kinds:
                    kinds[type_] = self.kind_of(type_, additions)
        type_names = sorted(kinds)
        ids = {name: i for i, name in enumerate(type_names)}
        if len(type_names) > 0xffff:
            raise Error('too many types for a 16-bit id')
        alias_ids = sorted((alias, ids[canonical]) for alias, canonical in self.aliases.items()
                           if alias not in ids)

        for name in type_names + [alias for alias, _ in alias_ids]:
            if not SAFE.match(name) or name != name.lower():
                raise Error(f'bad type name {name}')
        for table in tables.values():
            for key in table:
                if not SAFE.match(key) or len(key) > MAX_EXTENSION_LENGTH:
                    raise Error(f'bad extension {key}')

        out = [
            '// Generated by generate_mime_table.py from shared-mime-info '
            f'{version} (GPL-2.0-or-later)',
            '// and mime_policy.txt. Do not edit.',
            '// clang-format off',
            '',
            'struct TypeEntry {',
            '    std::string_view type;',
            '    FileKind kind;',
            '};',
            '',
            'struct ExtEntry {',
            '    std::string_view extension;',
            '    std::uint16_t type;  // Index into kTypes.',
            '};',
            '',
            'struct AliasEntry {',
            '    std::string_view alias;',
            '    std::uint16_t type;  // Index into kTypes.',
            '};',
            '',
            '// Every type, lower case, sorted bytewise.',
            f'inline constexpr std::array<TypeEntry, {len(type_names)}> kTypes = {{{{',
        ]
        out += [f'    {{"{name}", FileKind::k{kinds[name]}}},' for name in type_names]
        out.append('}};')

        def emit(comment, name, table):
            out.extend(['', comment,
                        f'inline constexpr std::array<ExtEntry, {len(table)}> {name} = {{{{'])
            out.extend(f'    {{"{key}", {ids[table[key]]}}},  // {table[key]}'
                       for key in sorted(table))
            out.append('}};')

        emit('// Single extensions, lower case, sorted bytewise.', 'kExtensions',
             tables['single'])
        emit('// Two-part extensions of archives, such as "tar.gz", lower case, sorted bytewise.',
             'kCompoundExtensions', tables['compound'])
        emit('// Extensions that only match in this exact case, sorted bytewise.',
             'kCaseSensitiveExtensions', tables['exact'])
        out.extend(['', '// Other names for types in kTypes, lower case, sorted bytewise.',
                    f'inline constexpr std::array<AliasEntry, {len(alias_ids)}> kAliases = {{{{'])
        out.extend(f'    {{"{alias}", {i}}},  // {type_names[i]}' for alias, i in alias_ids)
        out.append('}};')
        return '\n'.join(out) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    parser.add_argument('--smi-dir', required=True, help='the shared-mime-info checkout')
    parser.add_argument('--policy', required=True, help='mime_policy.txt')
    parser.add_argument('--output', required=True, help='the generated mime_table.inc')
    args = parser.parse_args()
    try:
        types, aliases = load_smi(f'{args.smi_dir}/data/freedesktop.org.xml.in')
        version = smi_version(f'{args.smi_dir}/meson.build')
        policy = load_policy(args.policy)
        content = Generator(types, aliases, policy, args.policy).generate(version)
    except (Error, OSError, ET.ParseError) as e:
        print(f'generate_mime_table.py: error: {e}', file=sys.stderr)
        return 1
    try:
        with open(args.output, encoding='utf-8', newline='') as f:
            if f.read() == content:
                return 0
    except FileNotFoundError:
        pass
    with open(args.output, 'w', encoding='utf-8', newline='\n') as f:
        f.write(content)
    return 0


if __name__ == '__main__':
    sys.exit(main())
