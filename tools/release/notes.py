#!/usr/bin/env python3
"""Extract a committed release-log section, following the Wi-Fi repo design."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]
TAG = re.compile(r'v(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\Z')


def extract(text, tag):
    if not TAG.fullmatch(tag):
        raise ValueError('expected vMAJOR.MINOR.PATCH')
    sections = list(re.finditer(r'^##[ \t]+([^\n]+?)[ \t]*$', text, re.M))
    matches = [i for i, section in enumerate(sections) if section[1] == tag]
    if len(matches) != 1:
        raise ValueError(f'{tag}: expected exactly one release section')
    index = matches[0]
    end = sections[index+1].start() if index+1 < len(sections) else len(text)
    body = text[sections[index].end():end].strip()
    if len(body) < 20 or re.search(r'(?i)\b(TODO|TBD|placeholder)\b|待补充|待填写', body):
        raise ValueError(f'{tag}: release notes are empty or unfinished')
    return body+'\n'


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(extract((ROOT/'docs/release/Release-Log.md').read_text(), args.tag))
