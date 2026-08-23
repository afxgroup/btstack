#!/usr/bin/env python3
"""
Render a Markdown file as plain text.

AmigaOS has no Markdown reader, so the documentation has to ship as text. It is
converted rather than kept as a second copy: two hand-written versions of the
same document drift, and the one nobody edits is the one people read.

The output is deliberately plain - no ANSI, no box drawing beyond the underlines
that mark headings - so it reads the same in a shell, in MultiView and in an
editor.
"""

import re
import sys

WIDTH = 78


def strip_inline(text):
    """Remove the markup that only makes sense when rendered."""
    text = re.sub(r'!\[([^\]]*)\]\([^)]*\)', r'\1', text)      # images
    text = re.sub(r'\[([^\]]+)\]\(([^)]+)\)', r'\1 (\2)', text)  # links
    text = re.sub(r'`([^`]+)`', r'\1', text)                     # code spans
    text = re.sub(r'\*\*([^*]+)\*\*', r'\1', text)               # bold
    text = re.sub(r'(?<!\*)\*([^*]+)\*(?!\*)', r'\1', text)      # italic
    return text


def wrap(text, indent):
    """Wrap to WIDTH, keeping the given indent on every line."""
    words = text.split()
    if not words:
        return ['']
    lines = []
    line = indent
    for word in words:
        if len(line) + len(word) + 1 > WIDTH and line.strip():
            lines.append(line.rstrip())
            line = indent + word
        else:
            line = line + word if line == indent else line + ' ' + word
    lines.append(line.rstrip())
    return lines


def convert(src):
    out = []
    in_code = False

    for raw in src.splitlines():
        line = raw.rstrip()

        # Fenced code: pass through verbatim, indented, so commands stay exact
        if line.lstrip().startswith('```'):
            in_code = not in_code
            if not in_code:
                out.append('')
            continue
        if in_code:
            out.append('    ' + line)
            continue

        if not line.strip():
            out.append('')
            continue

        heading = re.match(r'^(#{1,6})\s+(.*)$', line)
        if heading:
            level, title = len(heading.group(1)), strip_inline(heading.group(2))
            if out and out[-1] != '':
                out.append('')
            if level == 1:
                out.append('=' * WIDTH)
                out.append(title.center(WIDTH).rstrip())
                out.append('=' * WIDTH)
            elif level == 2:
                out.append(title.upper())
                out.append('-' * WIDTH)
            else:
                out.append(title)
                out.append('')
            continue

        # Horizontal rules become a plain separator
        if re.match(r'^\s*([-*_])\1{2,}\s*$', line):
            out.append('-' * WIDTH)
            continue

        bullet = re.match(r'^(\s*)[-*+]\s+(.*)$', line)
        if bullet:
            lead = len(bullet.group(1))
            body = strip_inline(bullet.group(2))
            first = ' ' * (2 + lead) + '* '
            wrapped = wrap(body, ' ' * len(first))
            out.append(first + wrapped[0].lstrip())
            out.extend(wrapped[1:])
            continue

        numbered = re.match(r'^(\s*)(\d+)\.\s+(.*)$', line)
        if numbered:
            lead = len(numbered.group(1))
            first = ' ' * (2 + lead) + numbered.group(2) + '. '
            body = strip_inline(numbered.group(3))
            wrapped = wrap(body, ' ' * len(first))
            out.append(first + wrapped[0].lstrip())
            out.extend(wrapped[1:])
            continue

        if line.startswith('>'):
            body = strip_inline(line.lstrip('> ').rstrip())
            out.extend(wrap(body, '      '))
            continue

        out.extend(wrap(strip_inline(line), '  '))

    # collapse the runs of blank lines the rules above can leave behind
    text = '\n'.join(out)
    text = re.sub(r'\n{3,}', '\n\n', text)
    return text.strip() + '\n'


def main():
    if len(sys.argv) != 3:
        sys.exit('usage: md2txt.py <input.md> <output.txt>')
    with open(sys.argv[1], encoding='utf-8') as f:
        src = f.read()
    with open(sys.argv[2], 'w', encoding='utf-8') as f:
        f.write(convert(src))


if __name__ == '__main__':
    main()
