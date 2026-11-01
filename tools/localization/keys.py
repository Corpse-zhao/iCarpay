"""The English keys Showcase passes to L(), in source order."""
from pathlib import Path
import re

SOURCE = Path(__file__).resolve().parents[2] / 'source/Showcase.m'
LITERAL = re.compile(r'@?"((?:[^"\\]|\\.)*)"')


def unescape(text):
    return re.sub(r'\\(u[0-9a-fA-F]{4}|.)', lambda m: chr(int(m.group(1)[1:], 16))
                  if m.group(1)[0] == 'u' else {'n': '\n', 't': '\t'}.get(m.group(1), m.group(1)), text)


def keys(source=None):
    text = (source or SOURCE).read_text()
    found = []
    for match in re.finditer(r'\bL\(\s*@"', text):
        at, parts = match.end() - 2, []
        while True:
            literal = LITERAL.match(text, at)
            parts.append(unescape(literal.group(1)))
            at = literal.end()
            gap = re.match(r'\s*', text[at:]).end()
            if text[at + gap] != '"':
                break
            at += gap
        key = ''.join(parts)
        if key not in found:
            found.append(key)
    return found


if __name__ == '__main__':
    for key in keys():
        print(repr(key))
