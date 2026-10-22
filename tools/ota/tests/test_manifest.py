import copy
import hashlib
import json
from pathlib import Path
import struct
import tempfile
from unittest import mock
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import manifest as m
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

# RFC 8032 public test seed: never a publisher key and never written to a key file.
TEST_SEED = bytes.fromhex('9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60')
IDENTITY = {'product': 'remote-wake', 'board': 'esp32-s3-devkitc', 'profile': 'hid-power',
            'channel': 'stable', 'layout': 'rw-4m-ab-v1', 'chip': 'esp32s3'}


def make_image(identity=None, release=7, duplicate=False):
    identity = identity or IDENTITY
    record = bytearray(236)
    record[:16] = m.MAGIC
    struct.pack_into('<III', record, 16, 1, 1, release)
    fields = {**identity, 'version': '1.2.3'}
    for name, offset, length in m.RECORD_STRINGS:
        value = fields[name].encode()
        record[offset:offset+len(value)] = value
    data = bytearray(256)
    struct.pack_into('<I', data, 0, 0xabcd5432)
    data[16:21] = b'1.2.3'
    data += record * (2 if duplicate else 1)
    header = bytearray(24)
    header[0], header[1], header[23] = 0xe9, 1, 1
    struct.pack_into('<H', header, 12, m.CHIP_IDS[identity['chip']])
    image = header + struct.pack('<II', 0x3c000020, len(data)) + data
    image += bytes((-len(image) - 1) % 16)
    checksum = 0xef
    for byte in data:
        checksum ^= byte
    image.append(checksum)
    image += hashlib.sha256(image).digest()
    return bytes(image)


def payload(image=None):
    image = image or make_image()
    return {'v': 1, 'min_updater': 1, **IDENTITY, 'min_flash': 4194304, 'min_psram': 0,
            'version': '1.2.3', 'release': 7, 'url': 'https://github.com/example/remote-wake/releases/download/v1.2.3/app.bin',
            'size': len(image), 'sha256': hashlib.sha256(image).hexdigest(),
            'published': 1700000000, 'expires': 1800000000, 'key_generation': 1}


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.key = Ed25519PrivateKey.from_private_bytes(TEST_SEED)
        self.image = make_image()
        self.payload = payload(self.image)
        self.checks = {'now': 1750000000, 'expected': IDENTITY, 'current_release': 6,
                       'key_generation': 1, 'source_prefix': 'https://github.com/example/remote-wake/releases/download/'}

    def test_real_signature_and_image_match(self):
        envelope = m.sign(self.payload, self.key, 'test-rfc8032', **self.checks)
        verified = m.verify(envelope, self.key.public_key(), 'test-rfc8032', **self.checks)
        self.assertEqual(m.check_image(self.image, verified)['release'], 7)

    def test_deterministic_fixture(self):
        fixture = json.loads((Path(__file__).parents[1] / 'fixtures' / 'manifest-v1.json').read_text())
        self.assertEqual(m.sign(self.payload, self.key, 'test-rfc8032', **self.checks), fixture['envelope'])
        self.assertEqual(self.key.public_key().public_bytes_raw().hex(), fixture['public_key_hex'])
        self.assertEqual(self.image.hex(), fixture['image_hex'])

    def test_cli_prepare_sign_verify_offline(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'app.bin').write_bytes(self.image)
            (root / 'private.pem').write_bytes(self.key.private_bytes(m.serialization.Encoding.PEM,
                m.serialization.PrivateFormat.PKCS8, m.serialization.NoEncryption()))
            (root / 'private.pem').chmod(0o600)
            (root / 'public.pem').write_bytes(self.key.public_key().public_bytes(
                m.serialization.Encoding.PEM, m.serialization.PublicFormat.SubjectPublicKeyInfo))
            self.assertEqual(m.main(['prepare', '--image', str(root/'app.bin'), '--url', self.payload['url'],
                '--min-flash', '4194304', '--min-psram', '0', '--published', '1700000000',
                '--expires', '1800000000', '--key-generation', '1', '--output', str(root/'payload.json'),
                '--identity-output', str(root/'identity.json')]), 0)
            shared = ['--kid', 'test-rfc8032', '--image', str(root/'app.bin'), '--now', '1750000000',
                '--current-release', '6', '--key-generation', '1', '--identity', str(root/'identity.json'),
                '--source-prefix', self.checks['source_prefix']]
            self.assertEqual(m.main(['sign', '--key', str(root/'private.pem'), '--payload', str(root/'payload.json'),
                '--output', str(root/'manifest.json'), *shared]), 0)
            with mock.patch('sys.stdout'):
                self.assertEqual(m.main(['verify', '--key', str(root/'public.pem'), '--manifest', str(root/'manifest.json'), *shared]), 0)
            (root / 'private.pem').chmod(0o644)
            with self.assertRaisesRegex(ValueError, 'owner-only'):
                m.load_key(root/'private.pem', True)

    def test_tampering_and_wrong_domain_fail(self):
        envelope = m.sign(self.payload, self.key, 'test-rfc8032')
        envelope['sig'] = m.b64(self.key.sign(m.canonical(self.payload)))
        with self.assertRaisesRegex(ValueError, 'signature'):
            m.verify(envelope, self.key.public_key(), 'test-rfc8032')
        envelope = m.sign(self.payload, self.key, 'test-rfc8032')
        envelope['payload'] = m.b64(m.canonical({**self.payload, 'product': 'voidstats'}))
        with self.assertRaisesRegex(ValueError, 'signature'):
            m.verify(envelope, self.key.public_key(), 'test-rfc8032')

    def test_strict_schema_and_bounds(self):
        for field, value in [('v', True), ('release', 0), ('release', 4294967296), ('min_updater', 2),
                             ('sha256', 'F'*64), ('product', 'x'*32), ('size', '100'), ('url', 'http://example.com/a'),
                             ('unexpected', 1), ('profile', {'nested': 1}), ('key_generation', False)]:
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                m.validate({**self.payload, field: value})
        with self.assertRaises(ValueError):
            m.loads(b'{"v":1,"v":1}')
        with self.assertRaises(ValueError):
            m.loads(b'{"x":NaN}')
        with self.assertRaises(ValueError):
            m.unb64('YQ==', 10)

    def test_wrong_product_channel_layout_stale_expired_key_source(self):
        for checks in [{'expected': {**IDENTITY, 'product': 'voidstats'}},
                       {'expected': {**IDENTITY, 'channel': 'canary'}},
                       {'expected': {**IDENTITY, 'layout': 'other'}}, {'current_release': 7},
                       {'now': 1800000000}, {'now': 1600000000}, {'key_generation': 2},
                       {'source_prefix': 'https://github.com/another/repo/'}]:
            with self.subTest(checks=checks), self.assertRaises(ValueError):
                m.validate(self.payload, **checks)

    def test_hash_truncation_and_wrong_embedded_product(self):
        for image in [self.image[:-1], self.image[:-1] + b'X', make_image({**IDENTITY, 'product': 'voidstats'})]:
            with self.subTest(image=image[-10:]), self.assertRaises(ValueError):
                m.check_image(image, self.payload)
        wrong = make_image({**IDENTITY, 'product': 'voidstats'})
        matched_hash = {**self.payload, 'size': len(wrong), 'sha256': hashlib.sha256(wrong).hexdigest()}
        with self.assertRaisesRegex(ValueError, 'product'):
            m.check_image(wrong, matched_hash)

    def test_merged_bootloader_duplicate_record_bad_esp_header_rejected(self):
        for image in [bytes(4096) + self.image, self.image + b'extra', make_image(duplicate=True), b'not-firmware']:
            with self.subTest(image=image[-10:]), self.assertRaises(ValueError):
                m.image_identity(image)
        bad = bytearray(self.image)
        bad[12] = 5
        with self.assertRaises(ValueError):
            m.image_identity(bytes(bad))


if __name__ == '__main__':
    unittest.main()
