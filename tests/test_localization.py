"""Every string Showcase shows has a translation in every language, with the
same format placeholders, and the generated .strings files are current."""
from pathlib import Path
import re
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / 'tools/localization'))
from keys import keys  # noqa: E402
from translations import LANGUAGES, T  # noqa: E402

used = keys()
assert not [k for k in used if k not in T], [k for k in used if k not in T]
assert not [k for k in T if k not in used], [k for k in T if k not in used]
placeholder = re.compile(r'%(?:\d+\$)?[@sdu]')
for key, values in T.items():
    assert len(values) == len(LANGUAGES), key
    for language, value in zip(LANGUAGES, values):
        assert sorted(placeholder.findall(value)) == sorted(placeholder.findall(key)), (language, key, value)
        assert value.strip() and value != '', (language, key)

for language in LANGUAGES:
    path = root / f'source/Localizations/{language}.lproj/Localizable.strings'
    # plutil parses .strings the way NSBundle does.
    parsed = subprocess.run(['plutil', '-convert', 'json', '-o', '-', str(path)],
                            capture_output=True, text=True, check=True).stdout
    import json
    table = json.loads(parsed)
    index = LANGUAGES.index(language)
    assert table == {k: T[k][index] for k in used}, language
print(f'PASS: {len(used)} strings in {len(LANGUAGES)} languages; placeholders match; .strings files current')
