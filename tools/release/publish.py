#!/usr/bin/env python3
"""Publish a verified preview; retries preserve existing release bytes and notes."""
import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from prepare import ASSETS, verify_files
from check_version import is_stable_release, version_strings


def gh(*args, check=True):
    return subprocess.run(['gh', *map(str, args)], check=check, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def compare_assets(tag, directory, existing):
    if existing-ASSETS:
        raise ValueError('unexpected existing release assets')
    with tempfile.TemporaryDirectory(prefix='m1-release-compare-') as tmp:
        if existing:
            gh('release', 'download', tag, '--dir', tmp)
        for name in existing:
            if (Path(tmp)/name).read_bytes() != (directory/name).read_bytes():
                raise ValueError(f'existing release asset differs: {name}')


def publish(tag, directory):
    verify_files(directory)
    metadata = json.loads((directory/'release.json').read_text())
    stable = is_stable_release(tag)
    expected = version_strings(tag[1:], metadata['source_commit'])
    if (metadata['tag'] != tag or metadata['version'] != expected['version'] or
            metadata['prerelease'] is not (not stable) or
            (stable and metadata.get('hardware_validation') != 'PASS')):
        raise ValueError('release identity/policy mismatch')
    policy = ['--prerelease=false', '--latest=true'] if stable else ['--prerelease', '--latest=false']
    view = gh('release', 'view', tag, '--json', 'tagName,isDraft,isPrerelease,body,assets', check=False)
    if view.returncode:
        gh('release', 'create', tag, '--verify-tag', '--draft', *policy,
           '--title', tag if stable else f'{tag}（开发预览）', '--notes-file', directory/'notes.md',
           *[directory/name for name in sorted(ASSETS)])
    else:
        release = json.loads(view.stdout)
        if (release['tagName'] != tag or release['body'].strip() != (directory/'notes.md').read_text().strip() or
                release['isPrerelease'] is not (not stable)):
            raise ValueError('existing release notes or policy differ; refusing to overwrite')
        existing = {asset['name'] for asset in release['assets']}
        compare_assets(tag, directory, existing)
        missing = ASSETS-existing
        if missing:
            if not release['isDraft']:
                raise ValueError('published release is missing assets; refusing to change it')
            gh('release', 'upload', tag, *[directory/name for name in sorted(missing)])
        elif not release['isDraft']:
            return  # Fully verified published release: idempotent no-op.
    # Read back every upload while the release is still a draft.
    compare_assets(tag, directory, ASSETS)
    gh('release', 'edit', tag, '--draft=false', *policy)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag')
    parser.add_argument('--directory', type=Path, required=True)
    args = parser.parse_args()
    publish(args.tag, args.directory)
