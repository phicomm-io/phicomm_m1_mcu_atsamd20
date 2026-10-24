"""Exercise frozen notes, artifact identity and draft/retry publication boundaries."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tools/release'))
import notes
import prepare
import publish

TAG = 'v0.9.1'
COMMIT = 'a'*40
LOG = '# Releases\n\n## v0.9.1\n\nThis release adds verified automatic preview publishing.\n'


class NotesTests(unittest.TestCase):
    def test_extract_only_requested_section(self):
        log = LOG+'\n### Details\nIncluded.\n\n## Other documentation\nNot included.\n'
        self.assertIn('Included.', notes.extract(log, TAG))
        self.assertNotIn('Not included.', notes.extract(log, TAG))

    def test_reject_missing_duplicate_invalid_and_unfinished(self):
        for log, tag in ((LOG, 'v0.9.2'), (LOG+LOG, TAG), (LOG, 'v0.09.1'),
                         (LOG+'\nTODO finish', TAG), (LOG+'\n待补充', TAG),
                         ('## v0.9.1\nshort', TAG)):
            with self.subTest(log=log, tag=tag), self.assertRaises(ValueError):
                notes.extract(log, tag)


class ReleaseFixture(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.ota, self.debug, self.output = [self.root/p for p in ('ota', 'debug', 'output')]
        self.ota.mkdir()
        self.debug.mkdir()
        self.payload = struct.pack('<II', 0x20008000, 0x4041)+bytes(504)
        common = dict(version='0.9.1-aaaaaa', uart_version='0.9.1-aaaaaa', base_version='0.9.1',
                      git_tag=TAG, git_commit=COMMIT, dirty=False, source_commit=COMMIT,
                      source_dirty=False, hardware_validation='NOT_RUN', release=False)
        ota = dict(common, bytes=len(self.payload), sha256=prepare.sha(self.payload),
                   target='ATSAMD20G18A', format='raw-samd20-application',
                   link_address='0x4000', image_signature=False)
        (self.ota/'m1-mcu-ota.bin').write_bytes(self.payload)
        self.write(self.ota/'m1-mcu-ota.json', ota)
        self.write(self.debug/'version.json', common)
        boot = b'boot fixture'
        (self.debug/'m1-boot.bin').write_bytes(boot)
        reports = []
        for name, payload in (('m1-app', self.payload), ('m1-boot', boot)):
            self.write(self.debug/f'{name}.json', dict(common, bytes=len(payload), sha256=prepare.sha(payload)))
            elf = (name+' ELF fixture').encode()
            (self.debug/f'{name}.elf').write_bytes(elf)
            (self.debug/f'{name}.map').write_text('map fixture\n')
            reports.append(dict(elf=f'{name}.elf', sha256=prepare.sha(elf), cases=5,
                                status='PASS: register model', gclk2_hz_nominal=1024, wdt_seconds_nominal=8))
        self.write(self.debug/'platform-clock-verification.json', reports)

    @staticmethod
    def write(path, data):
        path.write_text(json.dumps(data))

    def build(self, output=None):
        return prepare.prepare(TAG, COMMIT, self.ota, self.debug, LOG, output or self.output)


class BundleTests(ReleaseFixture):
    def test_bundle_preserves_payload_and_separates_boot(self):
        record = self.build()
        prepare.verify_files(self.output)
        self.assertEqual((self.output/'m1-mcu-ota.bin').read_bytes(), self.payload)
        self.assertEqual(record['hardware_validation'], 'NOT_RUN')
        self.assertEqual(record['release_log_sha256'], prepare.sha(LOG.encode()))
        manifest = json.loads((self.output/'m1-mcu-ota.json').read_text())
        self.assertIs(manifest['release'], True)
        self.assertIs(manifest['prerelease'], True)
        with zipfile.ZipFile(self.output/'m1-mcu-debug-recovery.zip') as archive:
            self.assertEqual(set(archive.namelist()), set(prepare.DEBUG_FILES)|{'m1-app.bin', 'README.txt'})
            self.assertEqual(archive.read('m1-app.bin'), self.payload)
            self.assertEqual(archive.read('m1-boot.bin'), b'boot fixture')
        self.assertIn(COMMIT, (self.output/'notes.md').read_text())
        other = self.root/'second'
        self.build(other)
        for name in prepare.ASSETS:
            self.assertEqual((self.output/name).read_bytes(), (other/name).read_bytes(), name)

    def test_reject_altered_ota_identity_format_payload_or_proof(self):
        path = self.ota/'m1-mcu-ota.json'
        original = json.loads(path.read_text())
        changes = dict(git_commit='b'*40, git_tag='v0.9.0', source_commit='b'*40,
                       source_dirty=True, dirty=True, bytes=0, sha256='bad',
                       target='wrong', image_signature=True, release=True,
                       hardware_validation='PASS')
        for key, value in changes.items():
            with self.subTest(key=key):
                self.write(path, dict(original, **{key: value}))
                with self.assertRaises(ValueError):
                    self.build()
        self.write(path, original)
        (self.ota/'m1-mcu-ota.bin').write_bytes(bytes(512))
        with self.assertRaisesRegex(ValueError, 'vector'):
            self.build()

    def test_reject_debug_binary_and_clock_report_mismatch(self):
        for filename in ('m1-boot.bin', 'm1-app.elf', 'm1-boot.elf'):
            path = self.debug/filename
            original = path.read_bytes()
            path.write_bytes(b'altered')
            with self.subTest(file=filename), self.assertRaises(ValueError):
                self.build()
            path.write_bytes(original)
        path = self.debug/'platform-clock-verification.json'
        reports = json.loads(path.read_text())
        for key, value in (('status', 'FAIL'), ('cases', 4), ('gclk2_hz_nominal', 32768),
                           ('wdt_seconds_nominal', 0.25)):
            broken = [dict(r) for r in reports]
            broken[0][key] = value
            self.write(path, broken)
            with self.subTest(key=key), self.assertRaises(ValueError):
                self.build()

    def test_reject_asset_tampering_and_checksum_path_escape(self):
        self.build()
        path = self.output/'SHA256SUMS'
        original = path.read_text()
        for invalid in (original+original, original.replace('notes.md', '../notes.md'),
                        '\n'.join(original.splitlines()[:-1])):
            path.write_text(invalid)
            with self.assertRaises(ValueError):
                prepare.verify_files(self.output)
        path.write_text(original)
        (self.output/'notes.md').write_text('altered')
        with self.assertRaisesRegex(ValueError, 'checksum mismatch'):
            prepare.verify_files(self.output)


class PublishTests(ReleaseFixture):
    def setUp(self):
        super().setUp()
        self.build()
        self.remote = None
        self.calls = []
        self.remote_bytes = {name: (self.output/name).read_bytes() for name in prepare.ASSETS}
        self.fail_create = False
        self.mock = patch.object(publish, 'gh', side_effect=self.gh)
        self.mock.start()
        self.addCleanup(self.mock.stop)

    def existing(self, draft=False, names=None):
        self.remote = dict(tagName=TAG, isDraft=draft, isPrerelease=True,
                           body=(self.output/'notes.md').read_text(),
                           assets=[dict(name=name) for name in sorted(prepare.ASSETS if names is None else names)])

    def gh(self, *args, **kwargs):
        self.calls.append(tuple(map(str, args)))
        command = args[1]
        if command == 'view':
            return subprocess.CompletedProcess(args, 0 if self.remote else 1, json.dumps(self.remote))
        if command == 'create' and self.fail_create:
            raise subprocess.CalledProcessError(1, args)
        if command == 'download':
            target = Path(args[args.index('--dir')+1])
            for name, data in self.remote_bytes.items():
                (target/name).write_bytes(data)
        return subprocess.CompletedProcess(args, 0, '')

    def test_new_release_is_draft_until_upload_readback(self):
        publish.publish(TAG, self.output)
        self.assertEqual([c[1] for c in self.calls], ['view', 'create', 'download', 'edit'])
        create, edit = self.calls[1], self.calls[-1]
        for flag in ('--verify-tag', '--draft', '--prerelease', '--latest=false', '--notes-file'):
            self.assertIn(flag, create)
        self.assertIn('--draft=false', edit)
        self.assertTrue(all(str(self.output/name) in create for name in prepare.ASSETS))

    def test_published_retry_is_read_only(self):
        self.existing()
        publish.publish(TAG, self.output)
        self.assertEqual([c[1] for c in self.calls], ['view', 'download'])

    def test_partial_draft_uploads_only_missing_assets(self):
        self.existing(draft=True, names={'notes.md'})
        publish.publish(TAG, self.output)
        self.assertEqual([c[1] for c in self.calls], ['view', 'download', 'upload', 'download', 'edit'])
        upload = self.calls[2]
        self.assertNotIn(str(self.output/'notes.md'), upload)
        self.assertNotIn('--clobber', upload)
        self.assertTrue(all(str(self.output/name) in upload for name in prepare.ASSETS-{'notes.md'}))

    def test_published_missing_assets_is_not_repaired(self):
        self.existing(names={'notes.md'})
        with self.assertRaisesRegex(ValueError, 'published release is missing'):
            publish.publish(TAG, self.output)
        self.assertEqual([c[1] for c in self.calls], ['view', 'download'])

    def test_different_existing_notes_bytes_or_policy_are_never_overwritten(self):
        for change in ('body', 'bytes', 'isPrerelease'):
            self.existing(draft=True)
            self.calls.clear()
            if change == 'bytes':
                self.remote_bytes['m1-mcu-ota.bin'] = b'other build'
            else:
                self.remote[change] = 'other notes' if change == 'body' else False
            with self.subTest(change=change), self.assertRaises(ValueError):
                publish.publish(TAG, self.output)
            self.assertFalse({'create', 'upload', 'edit'} & {c[1] for c in self.calls})

    def test_failed_creation_or_upload_readback_cannot_publish(self):
        self.fail_create = True
        with self.assertRaises(subprocess.CalledProcessError):
            publish.publish(TAG, self.output)
        self.assertNotIn('edit', [c[1] for c in self.calls])
        self.fail_create = False
        self.calls.clear()
        self.remote_bytes['m1-mcu-ota.bin'] = b'corrupt uploaded bytes'
        with self.assertRaisesRegex(ValueError, 'differs'):
            publish.publish(TAG, self.output)
        self.assertNotIn('edit', [c[1] for c in self.calls])
