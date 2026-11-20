import tempfile
from pathlib import Path
import unittest
from release_gate import ENGINES, PLATFORMS, inventory, verify, validate_version


class ReleaseGateTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for engine in ENGINES:
            for platform in PLATFORMS:
                for suffix in ('', '-src'):
                    name = f'KismetAI-1.2.3-UE{engine}-{platform}-release{suffix}.zip'
                    (self.root / name).write_bytes(name.encode())

    def test_complete_matrix(self):
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        self.assertEqual(len(receipt['assets']), 16)
        verify(self.root, receipt)

    def test_missing_platform_or_source_archive(self):
        next(self.root.glob('*-src.zip')).unlink()
        with self.assertRaises(ValueError):
            inventory(self.root, '1.2.3', 'a' * 40)

    def test_wrong_version_or_extra_archive(self):
        (self.root / 'KismetAI-1.2.2-UE5.8-Mac-release.zip').write_bytes(b'old')
        with self.assertRaises(ValueError):
            inventory(self.root, '1.2.3', 'a' * 40)

    def test_changed_package_is_rejected(self):
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        next(self.root.glob('*.zip')).write_bytes(b'changed')
        with self.assertRaises(ValueError):
            verify(self.root, receipt)

    def test_receipt_cannot_omit_assets_or_escape_directory(self):
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        receipt['assets'].pop()
        with self.assertRaises(ValueError):
            verify(self.root, receipt)
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        receipt['assets'][0]['name'] = '../outside.zip'
        with self.assertRaises(ValueError):
            verify(self.root, receipt)

    def test_versions_are_stable_semver(self):
        self.assertEqual(validate_version('1.2.3'), '1.2.3')
        for value in ('v1.2.3', '1.2', '1.2.3-rc1', '../1.2.3', "1.2.3'", '01.2.3'):
            with self.subTest(value=value), self.assertRaises(ValueError):
                validate_version(value)


class DraftSafetyTests(unittest.TestCase):
    def test_finds_unpublished_draft(self):
        from unittest.mock import patch
        from release_gate import current_release
        draft = {'tag_name': 'v1.2.3', 'draft': True, 'id': 42}
        with patch('release_gate.github', return_value=[draft]):
            self.assertEqual(current_release('owner/repo', '1.2.3'), draft)

    def test_stale_build_never_mutates_draft(self):
        from unittest.mock import patch
        from release_gate import draft
        with patch.dict('os.environ', {'GITHUB_REPOSITORY': 'owner/repo'}), \
             patch('release_gate.github', return_value={'object': {'sha': 'b' * 40}}) as api:
            draft(Path('/nonexistent'), '1.2.3', 'a' * 40)
            self.assertEqual(api.call_count, 1)

class PromotionSafetyTests(unittest.TestCase):
    setUp = ReleaseGateTests.setUp

    def test_catalogue_is_untouched_when_uploaded_bytes_differ(self):
        import io
        import types
        from unittest.mock import MagicMock, patch
        from release_gate import publish
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        class Body(io.BytesIO):
            def iter_chunks(self, chunk_size):
                yield self.read()
        s3 = MagicMock()
        s3.get_object.return_value = {'Body': Body(b'corrupt upload')}
        boto = types.ModuleType('boto3')
        boto.client = MagicMock(return_value=s3)
        database = types.ModuleType('psycopg')
        database.connect = MagicMock()
        exceptions = types.ModuleType('botocore.exceptions')
        exceptions.ClientError = type('ClientError', (Exception,), {})
        with patch.dict('sys.modules', {'boto3': boto, 'psycopg': database,
                                       'botocore.exceptions': exceptions}), \
             patch.dict('os.environ', {'RELEASES_BASE_URL': 'https://example.invalid',
                                       'R2_ENDPOINT_URL': 'https://example.invalid', 'R2_BUCKET': 'test'}), \
             patch('release_gate.validate_published', return_value=receipt):
            with self.assertRaisesRegex(ValueError, 'Uploaded object differs'):
                publish(self.root)
            database.connect.assert_not_called()


if __name__ == '__main__':
    unittest.main()
