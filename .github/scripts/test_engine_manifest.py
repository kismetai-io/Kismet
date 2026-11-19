import unittest
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from engine_manifest import seal, unseal


class ManifestEncryptionTests(unittest.TestCase):
    def setUp(self):
        self.key = bytes(range(32))
        self.data = b'manifest file hashes and download metadata' * 100

    def test_roundtrip_and_random_nonce(self):
        first = seal(self.data, self.key)
        second = seal(self.data, self.key)
        self.assertNotEqual(first, second)
        self.assertNotIn(self.data, first)
        self.assertEqual(unseal(first, self.key), self.data)
        self.assertEqual(unseal(second, self.key), self.data)

    def test_wrong_key_is_rejected(self):
        with self.assertRaises(ValueError):
            unseal(seal(self.data, self.key), bytes(reversed(self.key)))

    def test_corruption_and_truncation_are_rejected(self):
        encrypted = seal(self.data, self.key)
        for offset in (0, 12, 25, len(encrypted) - 1):
            broken = bytearray(encrypted)
            broken[offset] ^= 1
            with self.subTest(offset=offset), self.assertRaises(ValueError):
                unseal(bytes(broken), self.key)
        for length in (0, 5, 20, len(encrypted) - 1):
            with self.subTest(length=length), self.assertRaises(ValueError):
                unseal(encrypted[:length], self.key)

    def test_only_256_bit_keys_accepted(self):
        for key in (b'', b'x' * 16, b'x' * 24, b'x' * 33):
            with self.subTest(size=len(key)), self.assertRaises(ValueError):
                seal(self.data, key)

    def test_cli_authenticates_before_writing(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'input.enc'
            destination = Path(directory) / 'output.manifest'
            source.write_bytes(seal(self.data, self.key))
            command = [sys.executable, str(Path(__file__).with_name('engine_manifest.py')),
                       str(source), str(destination)]
            env = {**os.environ, 'UE_MANIFEST_KEY': bytes(reversed(self.key)).hex()}
            result = subprocess.run(command, env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(destination.exists())
            destination.write_bytes(b'existing file')
            result = subprocess.run(command, env=env, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(destination.read_bytes(), b'existing file')
            env['UE_MANIFEST_KEY'] = self.key.hex()
            result = subprocess.run(command, env=env, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(destination.read_bytes(), self.data)


if __name__ == '__main__':
    unittest.main()
