import ctypes
import importlib.util
import json
from pathlib import Path
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
        for tag in ('0.9.0', 'v0.09.0', 'v0.9.0-rc1', 'v1.2.3\n'):
            with self.subTest(tag=tag), self.assertRaises(ValueError):
                version.resolve_version(self.root, tag)
        self.git('tag', 'v123456789.1.0')
        with self.assertRaisesRegex(ValueError, '16-byte'):
            version.resolve_version(self.root, 'v123456789.1.0')

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
