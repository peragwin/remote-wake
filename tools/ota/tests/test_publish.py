import base64
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import publish
import test_manifest as fixtures


class PublicationTests(unittest.TestCase):
    def setUp(self):
        self.key = fixtures.Ed25519PrivateKey.from_private_bytes(fixtures.TEST_SEED)
        self.payload = fixtures.payload()
        self.envelope = publish.m.sign(self.payload, self.key, 'test-rfc8032')
        self.plan = publish.publication_plan(self.payload, 'example/remote-wake', 'v1.2.3', 'app.bin', 'manifest.json')

    def fake_github(self, calls, fail_download=False, prior=None):
        def run(*args, **kwargs):
            calls.append((args, kwargs))
            if args[0] == 'api' and args[1].startswith('repos/'):
                return prior
            if args[:2] == ('release', 'download'):
                if fail_download:
                    raise ValueError('failed download')
                directory = Path(args[args.index('--dir') + 1])
                (directory / 'app.bin').write_bytes(fixtures.make_image())
                (directory / 'manifest.json').write_bytes(publish.m.canonical(self.envelope))
            return b'{}'
        return run

    def test_versioned_assets_verified_before_atomic_channel_promotion(self):
        calls = []
        with mock.patch.object(publish, 'gh', side_effect=self.fake_github(calls)):
            publish.publish(Path('app.bin'), Path('manifest.json'), self.envelope, self.payload,
                            self.key.public_key(), 'test-rfc8032', self.plan, 'ota')
        self.assertEqual([call[0][:2] for call in calls], [
            ('api', 'repos/example/remote-wake/contents/channels/remote-wake/esp32-s3-devkitc/hid-power/stable/rw-4m-ab-v1.json?ref=ota'),
            ('api', 'repos/example/remote-wake/contents/catalog.json?ref=ota'),
            ('release', 'create'), ('release', 'download'), ('release', 'edit'), ('api', '--method'),
            ('api', 'repos/example/remote-wake/contents/catalog.json?ref=ota'), ('api', '--method')])
        body = json.loads(calls[-3][1]['body'])
        self.assertEqual(json.loads(base64.b64decode(body['content'])), self.envelope)

    def test_upload_verification_failure_never_promotes_channel(self):
        calls = []
        with mock.patch.object(publish, 'gh', side_effect=self.fake_github(calls, fail_download=True)), self.assertRaises(ValueError):
            publish.publish(Path('app.bin'), Path('manifest.json'), self.envelope, self.payload,
                            self.key.public_key(), 'test-rfc8032', self.plan, 'ota')
        self.assertFalse(any(args[:2] == ('api', '--method') for args, kwargs in calls))
        self.assertFalse(any(args[:2] == ('release', 'edit') for args, kwargs in calls))

    def test_stale_channel_aborts_before_publication(self):
        previous = json.dumps({'sha': 'previoussha', 'content': base64.b64encode(publish.m.canonical(self.envelope)).decode()}).encode()
        calls = []
        with mock.patch.object(publish, 'gh', side_effect=self.fake_github(calls, prior=previous)), self.assertRaisesRegex(ValueError, 'newer'):
            publish.publish(Path('app.bin'), Path('manifest.json'), self.envelope, self.payload,
                            self.key.public_key(), 'test-rfc8032', self.plan, 'ota')
        self.assertEqual(len(calls), 1)

    def test_catalog_merge_preserves_other_tuple_and_rejects_stale(self):
        first = publish.merge_catalog({'v': 1, 'releases': []}, self.payload, self.plan['manifest_url'])
        second_payload = {**self.payload, 'product': 'voidstats'}
        merged = publish.merge_catalog(first, second_payload, self.plan['manifest_url'])
        self.assertEqual(len(merged['releases']), 2)
        self.assertEqual(merged['releases'][0], first['releases'][0])
        with self.assertRaises(ValueError):
            publish.merge_catalog(first, self.payload, self.plan['manifest_url'])

    def test_catalog_cas_conflict_rereads_and_merges(self):
        other = publish.merge_catalog({'v': 1, 'releases': []}, {**self.payload, 'product': 'another'}, self.plan['manifest_url'])
        responses = [None, publish.Conflict('conflict'), (other, 'latestsha'), b'{}']
        requests = []
        def read(api, branch, limit):
            item = responses.pop(0)
            return (None, None) if item is None else item
        def write(*args, **kwargs):
            requests.append(json.loads(kwargs['body']))
            item = responses.pop(0)
            if isinstance(item, Exception):
                raise item
            return item
        with mock.patch.object(publish, 'read_content', side_effect=read), mock.patch.object(publish, 'gh', side_effect=write):
            publish.promote_catalog('example/remote-wake', 'ota', self.payload, self.plan['manifest_url'])
        self.assertEqual(requests[-1]['sha'], 'latestsha')
        merged = json.loads(base64.b64decode(requests[-1]['content']))
        self.assertEqual(len(merged['releases']), 2)

    def test_plan_rejects_latest_or_other_repository(self):
        for url in ['https://github.com/example/remote-wake/releases/latest/download/app.bin',
                    'https://github.com/attacker/repo/releases/download/v1.2.3/app.bin']:
            with self.assertRaises(ValueError):
                publish.publication_plan({**self.payload, 'url': url}, 'example/remote-wake', 'v1.2.3', 'app.bin', 'manifest.json')
