import ctypes
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('check_version', ROOT/'tools/check_version.py')
version = importlib.util.module_from_spec(spec)
spec.loader.exec_module(version)


class VersionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git('init', '-q', '-b', 'main')
        self.git('config', 'user.name', 'Version test')
        self.git('config', 'user.email', 'version@example.invalid')
        (self.root/'.gitignore').write_text('/build/\n/include/m1/generated/\n')
        (self.root/'source').write_text('initial\n')
        self.git('add', '.')
        self.git('commit', '-qm', 'initial')

    def git(self, *args):
        return version.git(self.root, *args)

    def target(self, value):
        (self.root/'VERSION').write_text(value+'\n')
        self.git('add', 'VERSION')
        self.git('commit', '-qm', 'target version')

    def test_initial_base_hash_and_dirty(self):
        commit = self.git('rev-parse', 'HEAD')
        record = version.resolve_version(self.root)
        self.assertEqual(record['version'], '0.9.0-'+commit[:6])
        self.assertIsNone(record['git_tag'])
        (self.root/'untracked').write_text('changed')
        record = version.resolve_version(self.root)
        self.assertEqual(record['version'], '0.9.0-'+commit[:6]+'-dirty')
        self.assertEqual(record['uart_version'], '0.9.0-'+commit[:6]+'-d')
        self.assertLessEqual(len(record['uart_version']), 16)

    def test_nearest_reachable_semver_not_unrelated_or_invalid_tag(self):
        self.git('tag', 'v0.9.2')
        self.git('checkout', '-q', '--orphan', 'unrelated')
        self.git('commit', '-qm', 'unrelated')
        self.git('tag', 'v9.0.0')
        self.git('checkout', '-q', 'main')
        self.git('commit', '-qm', 'next', '--allow-empty')
        self.git('tag', 'v0.9.03')
        self.git('tag', 'oss-0.3.1')
        record = version.resolve_version(self.root)
        self.assertEqual(record['git_tag'], 'v0.9.2')
        self.assertTrue(record['version'].startswith('0.9.2-'))
        self.assertTrue(record['version'].endswith(self.git('rev-parse', 'HEAD')[:6]))

    def test_exact_tag_requires_clean_head(self):
        self.git('tag', '-a', 'v0.9.0', '-m', 'initial version')
        record = version.resolve_version(self.root, 'v0.9.0')
        self.assertFalse(record['dirty'])
        self.assertEqual(record['uart_version'], record['version'])
        (self.root/'source').write_text('dirty')
        with self.assertRaisesRegex(ValueError, 'clean source'):
            version.resolve_version(self.root, 'v0.9.0')
        self.git('add', 'source')
        self.git('commit', '-qm', 'next')
        with self.assertRaisesRegex(ValueError, 'does not point to HEAD'):
            version.resolve_version(self.root, 'v0.9.0')

    def test_invalid_or_oversized_tag_is_rejected(self):
        for tag in ('0.9.0', 'v0.09.0', 'v0.9.0-rc1', 'v1.2.3\n',
                    'v1.0.0-rc.01', 'v1.0.0-rc.-1', 'v1.0.0-beta',
                    'v1.0.0-rc+build', 'v1.0.0-rc.1\n', 'v1.0.٠-rc'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                version.resolve_version(self.root, tag)
        self.git('tag', 'v123456789.1.0')
        with self.assertRaisesRegex(ValueError, '16-byte'):
            version.resolve_version(self.root, 'v123456789.1.0')

    def test_rc_tag_grammar_and_semantic_order(self):
        tags = ['v1.0.0-rc', 'v1.0.0-rc.0', 'v1.0.0-rc.2',
                'v1.0.0-rc.10', 'v1.0.0', 'v1.0.1-rc']
        self.assertEqual(sorted(reversed(tags), key=version.version_key), tags)
        for tag in tags:
            self.assertEqual(version.parse_tag(tag)['core'], tag[1:].split('-')[0])
        self.assertFalse(version.is_stable_release('v1.0.0-rc'))
        self.assertFalse(version.is_stable_release('v1.0.0-rc.0'))
        self.assertFalse(version.is_stable_release('v0.9.4'))
        self.assertTrue(version.is_stable_release('v1.0.0'))

    def test_exact_rc_tag_matches_core_version_and_keeps_hash(self):
        self.target('1.0.0')
        self.git('tag', '-a', 'v1.0.0-rc', '-m', 'RC fixture')
        commit = self.git('rev-parse', 'HEAD')
        record = version.resolve_version(self.root, 'v1.0.0-rc')
        self.assertEqual(record['base_version'], '1.0.0-rc')
        self.assertEqual(record['version'], '1.0.0-rc-'+commit[:6])
        self.assertEqual(record['uart_version'], record['version'])
        self.assertEqual(len(record['uart_version']), 15)
        self.assertEqual(record['git_tag'], 'v1.0.0-rc')
        self.assertFalse(record['dirty'])
        (self.root/'source').write_text('dirty RC')
        with self.assertRaisesRegex(ValueError, 'clean source'):
            version.resolve_version(self.root, 'v1.0.0-rc')

    def test_numbered_and_dirty_rc_never_truncate_uart_identity(self):
        self.target('1.0.0')
        for tag in ('v1.0.0-rc.0', 'v1.0.0-rc.1'):
            self.git('tag', tag)
            self.assertIsNotNone(version.parse_tag(tag))
            with self.subTest(tag=tag), self.assertRaisesRegex(ValueError, 'RC version.*16-byte.*not truncated'):
                version.resolve_version(self.root, tag)
        with self.assertRaisesRegex(ValueError, 'RC version.*16-byte.*not truncated'):
            version.version_strings('1.0.0-rc', 'a'*40, dirty=True)

    def test_rc_core_mismatch_and_noncore_version_file_are_rejected(self):
        self.target('1.0.0')
        self.git('tag', 'v1.1.0-rc')
        with self.assertRaisesRegex(ValueError, 'differs from VERSION'):
            version.resolve_version(self.root, 'v1.1.0-rc')
        self.target('1.0.0-rc')
        self.git('tag', 'v1.0.0-rc')
        with self.assertRaisesRegex(ValueError, 'VERSION must contain'):
            version.resolve_version(self.root, 'v1.0.0-rc')

    def test_core_target_preserves_nearest_rc_and_prefers_stable_at_same_commit(self):
        self.target('1.0.0')
        self.git('tag', 'v1.0.0-rc')
        self.git('commit', '-qm', 'after RC', '--allow-empty')
        self.assertEqual(version.resolve_version(self.root)['base_version'], '1.0.0-rc')
        for tag in ('v1.0.0-rc.2', 'v1.0.0-rc.10', 'v1.0.0'):
            self.git('tag', tag)
        self.assertEqual(version.resolve_version(self.root)['git_tag'], 'v1.0.0')

    def test_clean_rc_uart_reply_carries_complete_version_and_source_hash(self):
        self.target('1.0.0')
        self.git('tag', 'v1.0.0-rc')
        record = version.resolve_version(self.root, 'v1.0.0-rc')
        shutil.copytree(ROOT/'include', self.root/'include')
        version.generate(record, self.root)
        libpath = self.root/'rc-core.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-I', str(self.root/'include'),
                        str(ROOT/'src/core.c'), '-o', str(libpath)], check=True)
        lib = ctypes.CDLL(str(libpath))
        lib.m1_init.argtypes = [ctypes.c_void_p]
        lib.m1_command.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        state = ctypes.create_string_buffer(256)
        command = ctypes.create_string_buffer(b'\x23\x11'+bytes(9)+b'\x21', 12)
        reply = ctypes.create_string_buffer(20)
        lib.m1_init(state)
        lib.m1_command(state, command, reply)
        self.assertEqual(reply.raw[2], 15)
        self.assertEqual(reply.raw[3:18].decode(), record['uart_version'])
        self.assertEqual(reply.raw[19], 0x21)

    def test_parent_repository_is_not_a_version_source(self):
        nested = self.root/'nested'
        nested.mkdir()
        with self.assertRaisesRegex(ValueError, 'project Git root'):
            version.resolve_version(nested)

    def test_generated_files_do_not_make_checkout_dirty(self):
        self.git('tag', 'v0.9.0')
        record = version.resolve_version(self.root)
        version.generate(record, self.root)
        header = self.root/'include/m1/generated/build_version.h'
        original_mtime = header.stat().st_mtime_ns
        version.generate(record, self.root)
        self.assertEqual(header.stat().st_mtime_ns, original_mtime)
        self.assertEqual(json.loads((self.root/'build/version.json').read_text()), record)
        self.assertEqual(version.resolve_version(self.root), record)

    def test_built_uart_reply_matches_version_snapshot(self):
        # Exercise the C query encoder with the real generated build header.
        record = json.loads((ROOT/'build/version.json').read_text())
        libpath = self.root/'core.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-I', str(ROOT/'include'),
                        str(ROOT/'src/core.c'), '-o', str(libpath)], check=True)
        lib = ctypes.CDLL(str(libpath))
        lib.m1_init.argtypes = [ctypes.c_void_p]
        lib.m1_command.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
        state = ctypes.create_string_buffer(256)
        command = ctypes.create_string_buffer(b'\x23\x11'+bytes(9)+b'\x21', 12)
        reply = ctypes.create_string_buffer(20)
        lib.m1_init(state)
        lib.m1_command(state, command, reply)
        raw = reply.raw
        self.assertEqual(raw[:2], b'\x23\x11')
        self.assertEqual(raw[3:3+raw[2]].decode(), record['uart_version'])
        self.assertIn(record['git_commit'][:6], raw[3:3+raw[2]].decode())
        self.assertEqual(raw[19], 0x21)
