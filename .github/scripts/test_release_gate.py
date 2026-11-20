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

class DraftLifecycleTests(unittest.TestCase):
    setUp = ReleaseGateTests.setUp

    def run_draft(self, existing=None, publish_during_upload=False, tag=None):
        from unittest.mock import patch
        from release_gate import draft
        calls, uploads = [], []
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        def api(path, method='GET', data=None):
            calls.append((path, method, data))
            if path.endswith('/git/ref/heads/main'):
                return {'object': {'sha': 'a' * 40}}
            if '/releases?' in path:
                return [existing] if existing else []
            if '/git/ref/tags/' in path:
                return tag
            if method in ('POST', 'PATCH'):
                return {'id': 42}
            return {'draft': not (publish_during_upload and uploads),
                    'html_url': 'https://example.invalid/draft',
                    'assets': [{'name': p.name} for p in uploads]}
        def upload(command, check):
            self.assertTrue(check)
            uploads.append(Path(command[-1]))
        with patch.dict('os.environ', {'GITHUB_REPOSITORY': 'owner/repo'}), \
             patch('release_gate.github', side_effect=api), \
             patch('subprocess.run', side_effect=upload):
            draft(self.root, '1.2.3', 'a' * 40)
        self.assertEqual(len(uploads), 17)
        self.assertEqual(uploads[-1].name, 'release-receipt.json')
        import json
        self.assertEqual(json.loads(uploads[-1].read_text()), receipt)
        return calls

    def test_create_complete_draft_with_receipt_last(self):
        calls = self.run_draft()
        self.assertEqual([c[1] for c in calls if c[1] != 'GET'], ['POST'])

    def test_replace_existing_draft_for_same_version(self):
        calls = self.run_draft({'tag_name': 'v1.2.3', 'draft': True, 'id': 42})
        self.assertEqual([c[1] for c in calls if c[1] != 'GET'], ['PATCH'])

    def test_published_version_cannot_be_replaced(self):
        with self.assertRaisesRegex(ValueError, 'Published versions are immutable'):
            self.run_draft({'tag_name': 'v1.2.3', 'draft': False, 'id': 42})

    def test_existing_tag_cannot_be_moved(self):
        with self.assertRaisesRegex(ValueError, 'Release tag already exists'):
            self.run_draft(tag={'object': {'sha': 'b' * 40}})

    def test_publication_during_replacement_stops_uploads(self):
        with self.assertRaisesRegex(ValueError, 'Draft was published during replacement'):
            self.run_draft(publish_during_upload=True)


class PublishedReceiptTests(unittest.TestCase):
    setUp = ReleaseGateTests.setUp

    def validate(self, draft=False, prerelease=False, tag='v1.2.3', commit='a' * 40):
        import json
        from unittest.mock import patch
        from release_gate import validate_published
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        (self.root / 'release-receipt.json').write_text(json.dumps(receipt))
        event = self.root / 'event.json'
        event.write_text(json.dumps({'release': {'draft': draft, 'prerelease': prerelease, 'tag_name': tag}}))
        with patch.dict('os.environ', {'GITHUB_EVENT_PATH': str(event), 'GITHUB_REPOSITORY': 'owner/repo'}), \
             patch('release_gate.github', return_value={'sha': commit}):
            return validate_published(self.root)

    def test_published_tag_matches_smoke_tested_source(self):
        self.assertEqual(self.validate()['commit'], 'a' * 40)

    def test_wrong_tag_or_source_is_rejected(self):
        for args in ({'tag': 'v1.2.4'}, {'commit': 'b' * 40}, {'draft': True}, {'prerelease': True}):
            with self.subTest(args=args), self.assertRaises(ValueError):
                self.validate(**args)


class PromotionSafetyTests(unittest.TestCase):
    setUp = ReleaseGateTests.setUp

    def test_all_catalogue_rows_include_published_release_notes(self):
        import io
        import json
        import types
        from unittest.mock import MagicMock, patch
        from release_gate import publish
        receipt = inventory(self.root, '1.2.3', 'a' * 40)
        event = self.root / 'event.json'
        event.write_text(json.dumps({'release': {'body': 'Smoke-tested release notes'}}))
        class Body(io.BytesIO):
            def iter_chunks(self, chunk_size):
                yield self.read()
        s3 = MagicMock()
        s3.get_object.side_effect = lambda **kw: {'Body': Body((self.root / kw['Key'].split('/')[-1]).read_bytes())}
        boto = types.ModuleType('boto3')
        boto.client = MagicMock(return_value=s3)
        database = types.ModuleType('psycopg')
        database.connect = MagicMock()
        cursor = database.connect.return_value.__enter__.return_value.cursor.return_value.__enter__.return_value
        cursor.fetchone.return_value = None
        exceptions = types.ModuleType('botocore.exceptions')
        exceptions.ClientError = type('ClientError', (Exception,), {})
        with patch.dict('sys.modules', {'boto3': boto, 'psycopg': database,
                                       'botocore.exceptions': exceptions}), \
             patch.dict('os.environ', {'RELEASES_BASE_URL': 'https://example.invalid',
                                       'R2_ENDPOINT_URL': 'https://example.invalid', 'R2_BUCKET': 'test',
                                       'RELEASE_DATABASE_URL': 'test', 'GITHUB_EVENT_PATH': str(event)}), \
             patch('release_gate.validate_published', return_value=receipt):
            publish(self.root)
        inserts = [call.args for call in cursor.execute.call_args_list if call.args[0].startswith('INSERT')]
        self.assertEqual(len(inserts), 8)
        self.assertEqual({args[1][2:4] for args in inserts}, {(e, p) for e in ENGINES for p in PLATFORMS})
        for sql, values in inserts:
            self.assertEqual(values[-1], 'Smoke-tested release notes')
            self.assertIn('notes', sql)

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
