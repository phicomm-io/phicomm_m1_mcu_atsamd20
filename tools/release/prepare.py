#!/usr/bin/env python3
"""Package verified Actions artifacts for an immutable GitHub preview release."""
import argparse
import hashlib
import json
from pathlib import Path
import sys
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from check_version import resolve_version
from pack_ota import validate
from notes import extract

DEBUG_FILES = ('m1-app.elf', 'm1-app.map', 'm1-app.json', 'm1-boot.bin',
               'm1-boot.elf', 'm1-boot.map', 'm1-boot.json', 'version.json',
               'platform-clock-verification.json')
ASSETS = {'m1-mcu-ota.bin', 'm1-mcu-ota.json', 'm1-mcu-debug-recovery.zip',
          'notes.md', 'release.json', 'SHA256SUMS'}


def sha(data):
    return hashlib.sha256(data).hexdigest()


def identity(record, tag, commit):
    expected = dict(version=f'{tag[1:]}-{commit[:6]}', uart_version=f'{tag[1:]}-{commit[:6]}',
                    base_version=tag[1:], git_tag=tag, git_commit=commit, dirty=False)
    if any(record.get(key) != value for key, value in expected.items()) or record.get('dirty') is not False:
        raise ValueError('artifact version/tag/commit/dirty mismatch')


def verify_files(directory):
    if {p.name for p in directory.iterdir()} != ASSETS:
        raise ValueError('unexpected or missing release assets')
    lines = (directory/'SHA256SUMS').read_text().splitlines()
    checked = set()
    for line in lines:
        digest, name = line.split()
        if name not in ASSETS-{'SHA256SUMS'} or name in checked:
            raise ValueError('invalid checksum asset name')
        if sha((directory/name).read_bytes()) != digest:
            raise ValueError(f'release asset checksum mismatch: {name}')
        checked.add(name)
    if checked != ASSETS-{'SHA256SUMS'}:
        raise ValueError('incomplete release checksums')


def prepare(tag, commit, ota, debug, log, output, acceptance=None):
    body = extract(log, tag)
    if len(commit) != 40 or any(c not in '0123456789abcdef' for c in commit):
        raise ValueError('expected full source commit')
    record = json.loads((ota/'m1-mcu-ota.json').read_text())
    identity(record, tag, commit)
    data = (ota/'m1-mcu-ota.bin').read_bytes()
    validate(data)
    if record.get('source_commit') != commit or record.get('source_dirty') is not False:
        raise ValueError('OTA source identity mismatch')
    if record.get('bytes') != len(data) or record.get('sha256') != sha(data):
        raise ValueError('OTA payload mismatch')
    if (record.get('target'), record.get('format'), record.get('link_address'), record.get('image_signature')) != (
            'ATSAMD20G18A', 'raw-samd20-application', '0x4000', False):
        raise ValueError('wrong MCU OTA format or target')
    if record.get('hardware_validation') != 'NOT_RUN' or record.get('release') is not False:
        raise ValueError('expected an unvalidated Actions candidate for this preview policy')
    snapshot = json.loads((debug/'version.json').read_text())
    identity(snapshot, tag, commit)
    for name, payload in (('m1-app', data), ('m1-boot', (debug/'m1-boot.bin').read_bytes())):
        meta = json.loads((debug/f'{name}.json').read_text())
        identity(meta, tag, commit)
        if (meta.get('source_commit') != commit or meta.get('source_dirty') is not False or
                meta.get('bytes') != len(payload) or meta.get('sha256') != sha(payload)):
            raise ValueError(f'{name}: debug binary/metadata mismatch')
    reports = json.loads((debug/'platform-clock-verification.json').read_text())
    if len(reports) != 2 or {r['elf'] for r in reports} != {'m1-app.elf', 'm1-boot.elf'}:
        raise ValueError('missing ARM clock reports')
    for report in reports:
        if (report['sha256'] != sha((debug/report['elf']).read_bytes()) or report['cases'] != 5 or
                not report['status'].startswith('PASS') or report['gclk2_hz_nominal'] != 1024 or
                report['wdt_seconds_nominal'] != 8):
            raise ValueError('ARM clock report mismatch')
    stable = int(tag[1:].split('.')[0]) >= 1
    if stable:
        if not acceptance:
            raise ValueError('stable MCU release requires matching hardware acceptance')
        evidence = json.loads(Path(acceptance).read_text())
        if (evidence.get('schema') != 1 or evidence.get('tag') != tag or
                evidence.get('source_commit') != commit or evidence.get('hardware_validation') != 'PASS' or
                evidence.get('app_sha256') != sha(data) or
                evidence.get('boot_sha256') != sha((debug/'m1-boot.bin').read_bytes())):
            raise ValueError('hardware acceptance does not match this release')
        record['hardware_validation'] = 'PASS'
    if output.exists() and any(output.iterdir()):
        raise ValueError('release output must be empty')
    output.mkdir(parents=True, exist_ok=True)
    (output/'m1-mcu-ota.bin').write_bytes(data)
    # Published assets carry a release label; this never upgrades hardware proof.
    record.update(release=True, prerelease=not stable)
    (output/'m1-mcu-ota.json').write_text(json.dumps(record, indent=2)+'\n')
    with zipfile.ZipFile(output/'m1-mcu-debug-recovery.zip', 'w') as archive:
        entries = {name: (debug/name).read_bytes() for name in DEBUG_FILES}
        entries['m1-app.bin'] = data
        entries['README.txt'] = ('ATSAMD20G18A SWD recovery/development files.\n'
                                'm1-app.bin: application at 0x4000.\n'
                                'm1-boot.bin: separate bootloader at 0x0; NOT an OTA payload.\n'
                                f'Hardware validation: {record["hardware_validation"]}.\n').encode()
        if stable:
            entries['hardware-acceptance.json'] = Path(acceptance).read_bytes()
        for name, contents in sorted(entries.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.external_attr = 0o100644 << 16
            archive.writestr(info, contents)
    notes = (f'# {tag}\n\n{body}\n'
             f'### 构建信息\n\n版本：`{record["version"]}`；源码：`{commit}`。\n\n' +
             ('原厂 11.x 用户可优先使用；17.1.2.3 / 17.x 待镜像分析后再确认兼容性。\n\n' if stable else
              '本发布为开发预览。\n\n') +
             '### 下载文件\n\n'
             '- `m1-mcu-ota.bin`：ATSAMD20G18A 主控应用，链接于 `0x4000`；不用于 Wi-Fi MCU 更新。\n'
             '- `m1-mcu-ota.json`：版本、来源、长度、SHA-256 及验证状态。\n'
             '- `m1-mcu-debug-recovery.zip`：独立 Boot、应用与 ELF/map，仅供开发和 SWD 恢复。\n'
             '- `SHA256SUMS`：下载后可运行 `sha256sum -c SHA256SUMS` 校验。\n\n'
             '镜像无签名，SHA/CRC 不提供来源认证。请保留原厂备份和 SWD 恢复能力。\n')
    (output/'notes.md').write_text(notes)
    files = {p.name: dict(bytes=p.stat().st_size, sha256=sha(p.read_bytes()))
             for p in sorted(output.iterdir())}
    catalog = dict(schema=1, tag=tag, version=record['version'], source_commit=commit,
                   target='ATSAMD20G18A', prerelease=not stable, hardware_validation=record['hardware_validation'],
                   release_log_sha256=sha(log.encode()), assets=files)
    (output/'release.json').write_text(json.dumps(catalog, indent=2)+'\n')
    sums = ''.join(f'{sha(p.read_bytes())}  {p.name}\n' for p in sorted(output.iterdir()))
    (output/'SHA256SUMS').write_text(sums)
    verify_files(output)
    return catalog


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tag')
    parser.add_argument('commit')
    parser.add_argument('--ota', type=Path, required=True)
    parser.add_argument('--debug', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--acceptance', type=Path)
    args = parser.parse_args()
    source = resolve_version(tag=args.tag)
    if source['git_commit'] != args.commit:
        parser.error('tag checkout does not match workflow commit')
    result = prepare(args.tag, args.commit, args.ota, args.debug,
                     (ROOT/'docs/release/Release-Log.md').read_text(), args.output, args.acceptance)
    print(json.dumps(result, indent=2))
