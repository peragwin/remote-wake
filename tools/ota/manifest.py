#!/usr/bin/env python3
"""Offline, domain-separated OTA release signing and verification."""
import argparse
import base64
import binascii
import hashlib
import json
from pathlib import Path
import re
import sys
import time
from urllib.parse import urlsplit

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey, Ed25519PublicKey

DOMAIN = b'remote-wake-firmware-v1\n'
MAX_ENVELOPE = 6144
MAX_PAYLOAD = 3072
MAX_IMAGE = 16 * 1024 * 1024
IDENTITY = ('product', 'board', 'profile', 'channel', 'layout')


def duplicate_free(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate JSON key: ' + key)
        result[key] = value
    return result


def loads(data, limit=MAX_PAYLOAD):
    if len(data) > limit:
        raise ValueError('JSON exceeds size bound')
    try:
        result = json.loads(data, object_pairs_hook=duplicate_free,
                            parse_constant=lambda value: (_ for _ in ()).throw(ValueError('non-finite JSON')))
    except (UnicodeError, RecursionError) as error:
        raise ValueError('invalid UTF-8 JSON') from error
    if not isinstance(result, dict):
        raise ValueError('JSON root must be an object')
    return result


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=True, allow_nan=False).encode('ascii')


def b64(data):
    return base64.urlsafe_b64encode(data).decode('ascii').rstrip('=')


def unb64(value, limit):
    if not isinstance(value, str) or len(value) > ((limit + 2) // 3) * 4 or not re.fullmatch(r'[A-Za-z0-9_-]+', value):
        raise ValueError('invalid base64url')
    try:
        decoded = base64.urlsafe_b64decode(value + '=' * (-len(value) % 4))
    except (binascii.Error, ValueError) as error:
        raise ValueError('invalid base64url') from error
    if len(decoded) > limit or b64(decoded) != value:
        raise ValueError('noncanonical base64url')
    return decoded


def integer(value, low, high, field):
    if type(value) is not int or not low <= value <= high:
        raise ValueError(field + ' must be a bounded integer')


def identifier(value, field, maximum=64):
    if not isinstance(value, str) or not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,' + str(maximum - 1) + '}', value):
        raise ValueError('invalid ' + field)


def read_bounded(path, limit):
    with Path(path).open('rb') as source:
        data = source.read(limit + 1)
    if len(data) > limit:
        raise ValueError(str(path) + ' exceeds size bound')
    return data


def load_key(path, private=False):
    key_path = Path(path).resolve(strict=True)
    if private:
        repository = Path(__file__).resolve().parents[2]
        if key_path == repository or repository in key_path.parents:
            raise ValueError('publisher private key must remain outside the repository')
        if key_path.stat().st_mode & 0o077:
            raise ValueError('publisher private key must have owner-only file permissions')
    raw = read_bounded(path, 8192)
    try:
        if private:
            key = serialization.load_pem_private_key(raw, password=None)
        else:
            key = serialization.load_pem_public_key(raw)
    except (ValueError, TypeError) as error:
        raise ValueError('key must be an Ed25519 PEM file') from error
    expected = Ed25519PrivateKey if private else Ed25519PublicKey
    if not isinstance(key, expected):
        raise ValueError('key must be Ed25519')
    return key

FIELDS = {'v', 'min_updater', *IDENTITY, 'chip', 'min_flash', 'min_psram',
          'version', 'release', 'url', 'size', 'sha256', 'published', 'expires', 'key_generation'}


def validate(payload, *, now=None, expected=None, current_release=None, key_generation=None, source_prefix=None):
    if set(payload) != FIELDS:
        raise ValueError('manifest requires exactly the documented fields')
    for field, maximum in [('product', 31), ('board', 31), ('profile', 47), ('channel', 15),
                           ('layout', 31), ('chip', 15), ('version', 31)]:
        identifier(payload[field], field, maximum)
    integer(payload['v'], 1, 1, 'v')
    integer(payload['min_updater'], 1, 1, 'min_updater')
    for field in ('min_flash', 'min_psram', 'published', 'expires'):
        integer(payload[field], 0, 4294967295, field)
    for field in ('release', 'size', 'key_generation'):
        integer(payload[field], 1, 4294967295, field)
    slot_sizes = {'rw-4m-ab-v1': 0x1e0000, 'vs-16m-ab-v1': 0x600000}
    if payload['size'] > slot_sizes.get(payload['layout'], MAX_IMAGE):
        raise ValueError('image exceeds known inactive application slot capacity')
    if payload['size'] > MAX_IMAGE or payload['min_flash'] < payload['size']:
        raise ValueError('image exceeds publisher size/flash bounds')
    if not isinstance(payload['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', payload['sha256']):
        raise ValueError('sha256 must be lowercase hex')
    url = payload['url']
    if not isinstance(url, str) or not 1 <= len(url) <= 511 or any(ord(c) < 33 or ord(c) > 126 for c in url):
        raise ValueError('invalid image URL')
    parsed = urlsplit(url)
    if (parsed.scheme != 'https' or not re.fullmatch(r'[A-Za-z0-9.-]{1,127}', parsed.netloc)
            or not parsed.path.startswith('/') or parsed.fragment or parsed.query
            or any(char in url for char in ('%', '?', '\\', '#'))
            or any(segment in ('.', '..') for segment in parsed.path.split('/'))):
        raise ValueError('image URL must be simple HTTPS without credentials, ports, query or traversal')
    if source_prefix is not None and (not source_prefix.startswith('https://')
            or not source_prefix.endswith('/') or len(source_prefix) > 383):
        raise ValueError('source prefix must be bounded HTTPS ending /')
    if source_prefix is not None and not url.startswith(source_prefix):
        raise ValueError('image URL is outside release source')
    if payload['expires'] <= payload['published']:
        raise ValueError('expiry must follow publication')
    if now is not None and (payload['published'] > now + 300 or payload['expires'] <= now):
        raise ValueError('manifest is expired or not yet published')
    if current_release is not None:
        integer(current_release, 0, 4294967295, 'current_release')
        if payload['release'] <= current_release:
            raise ValueError('stale release counter')
    if key_generation is not None and payload['key_generation'] != key_generation:
        raise ValueError('wrong signing key generation')
    if expected:
        for field, value in expected.items():
            if field not in (*IDENTITY, 'chip') or payload[field] != value:
                raise ValueError('wrong release identity: ' + field)
    return payload


def sign(payload, key, kid, **checks):
    identifier(kid, 'kid', 15)
    validate(payload, **checks)
    raw = canonical(payload)
    if len(raw) > MAX_PAYLOAD:
        raise ValueError('manifest payload exceeds size bound')
    return {'v': 1, 'kid': kid, 'payload': b64(raw), 'sig': b64(key.sign(DOMAIN + raw))}


def verify(envelope, key, kid, **checks):
    if set(envelope) != {'v', 'kid', 'payload', 'sig'} or type(envelope.get('v')) is not int or envelope['v'] != 1:
        raise ValueError('invalid signature envelope')
    identifier(envelope['kid'], 'kid', 15)
    if envelope['kid'] != kid:
        raise ValueError('untrusted signing key ID')
    raw = unb64(envelope['payload'], MAX_PAYLOAD)
    signature = unb64(envelope['sig'], 64)
    if len(signature) != 64:
        raise ValueError('invalid signature length')
    try:
        key.verify(signature, DOMAIN + raw)
    except InvalidSignature as error:
        raise ValueError('publisher signature does not verify') from error
    if any(byte > 126 for byte in raw) or b'\\u' in raw:
        raise ValueError('manifest wire payload must be ASCII without Unicode escapes')
    return validate(loads(raw), **checks)

MAGIC = b'RWOTA-IDENTITY1!'
IMAGE_RECORD_SIZE = 236
CHIP_IDS = {'esp32': 0, 'esp32c3': 5, 'esp32s3': 9, 'esp32c2': 12, 'esp32c6': 13, 'esp32h2': 16}
RECORD_STRINGS = [('product', 28, 32), ('board', 60, 32), ('profile', 92, 48),
                  ('channel', 140, 16), ('layout', 156, 32), ('version', 188, 32), ('chip', 220, 16)]


def image_identity(image):
    import struct
    if not 32 <= len(image) <= MAX_IMAGE or image[0] != 0xe9 or not 1 <= image[1] <= 16:
        raise ValueError('not an ESP app-only image')
    # esp_app_desc_t starts the first application segment. Bootloaders and merged
    # flash packages do not meet this check, even if they contain an identity.
    if image[32:36] != struct.pack('<I', 0xabcd5432):
        raise ValueError('missing ESP application descriptor; app-only binary required')
    segment_regions = []
    cursor, checksum = 24, 0xef
    for _ in range(image[1]):
        if cursor + 8 > len(image):
            raise ValueError('truncated ESP segment header')
        size = struct.unpack_from('<I', image, cursor + 4)[0]
        if not segment_regions and size < 256:
            raise ValueError('truncated ESP application descriptor')
        start = cursor + 8
        cursor = start + size
        if cursor > len(image):
            raise ValueError('truncated ESP segment data')
        segment_regions.append((start, cursor))
        for byte in image[start:cursor]:
            checksum ^= byte
    checksum_offset = ((cursor + 16) // 16) * 16 - 1
    if checksum_offset >= len(image) or image[checksum_offset] != checksum:
        raise ValueError('invalid ESP image checksum')
    image_end = checksum_offset + 1
    if image[23] not in (0, 1):
        raise ValueError('invalid ESP hash flag')
    if image[23]:
        if image[image_end:image_end + 32] != hashlib.sha256(image[:image_end]).digest():
            raise ValueError('invalid ESP appended digest')
        image_end += 32
    if image_end != len(image):
        raise ValueError('trailing data or merged flash package is not an app-only image')
    records = []
    for start, end in segment_regions:
        offset = start
        while True:
            offset = image.find(MAGIC, offset, end)
            if offset < 0:
                break
            if offset + IMAGE_RECORD_SIZE <= end and image[offset + 16:offset + 24] == struct.pack('<II', 1, 1):
                record = {'release': struct.unpack_from('<I', image, offset + 24)[0], 'min_updater': 1}
                for field, relative, length in RECORD_STRINGS:
                    raw = image[offset + relative:offset + relative + length]
                    head, separator, tail = raw.partition(b'\0')
                    if not separator or any(tail):
                        raise ValueError('malformed embedded identity padding')
                    try:
                        record[field] = head.decode('ascii')
                    except UnicodeError as error:
                        raise ValueError('non-ASCII embedded identity') from error
                records.append(record)
            offset += len(MAGIC)
    if len(records) != 1:
        raise ValueError('application must contain exactly one embedded OTA identity')
    record = records[0]
    app_version = image[48:80].split(b'\0', 1)[0]
    if app_version != record['version'].encode('ascii'):
        raise ValueError('ESP application version disagrees with embedded identity')
    chip = record['chip']
    if chip not in CHIP_IDS or int.from_bytes(image[12:14], 'little') != CHIP_IDS[chip]:
        raise ValueError('ESP chip header disagrees with embedded identity')
    return record


def check_image(image, payload):
    if len(image) != payload['size'] or hashlib.sha256(image).hexdigest() != payload['sha256']:
        raise ValueError('image size or SHA-256 disagrees with manifest')
    record = image_identity(image)
    for field, value in record.items():
        if payload[field] != value:
            raise ValueError('embedded image disagrees with manifest: ' + field)
    return record


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    inspect = commands.add_parser('inspect', help='inspect and validate app-only embedded identity')
    inspect.add_argument('--image', required=True)
    prepare = commands.add_parser('prepare', help='derive unsigned payload and identity from an app-only image')
    prepare.add_argument('--image', required=True)
    prepare.add_argument('--url', required=True)
    prepare.add_argument('--min-flash', required=True, type=int)
    prepare.add_argument('--min-psram', required=True, type=int)
    prepare.add_argument('--published', required=True, type=int)
    prepare.add_argument('--expires', required=True, type=int)
    prepare.add_argument('--key-generation', required=True, type=int)
    prepare.add_argument('--output', required=True)
    prepare.add_argument('--identity-output', required=True)
    for command in ('sign', 'verify'):
        sub = commands.add_parser(command)
        sub.add_argument('--key', required=True, help='Ed25519 PEM file; private for sign, public for verify')
        sub.add_argument('--kid', required=True)
        sub.add_argument('--image', required=True, help='app-only ESP-IDF .bin')
        sub.add_argument('--now', type=int, default=None, help='verification time, default current UTC epoch')
        sub.add_argument('--current-release', required=True, type=int, help='confirmed/promoted high-water mark')
        sub.add_argument('--key-generation', required=True, type=int)
        sub.add_argument('--identity', required=True, help='JSON file with exact product/board/profile/channel/layout and chip')
        sub.add_argument('--source-prefix', required=True)
        if command == 'sign':
            sub.add_argument('--payload', required=True, help='unsigned payload JSON; size/hash checked against image')
            sub.add_argument('--output', required=True)
        else:
            sub.add_argument('--manifest', required=True)
    args = parser.parse_args(argv)
    try:
        image = read_bounded(args.image, MAX_IMAGE)
        if args.command == 'inspect':
            print(json.dumps(image_identity(image), sort_keys=True))
            return 0
        if args.command == 'prepare':
            record = image_identity(image)
            payload = {'v': 1, **record, 'url': args.url, 'min_flash': args.min_flash,
                       'min_psram': args.min_psram, 'published': args.published, 'expires': args.expires,
                       'key_generation': args.key_generation, 'size': len(image),
                       'sha256': hashlib.sha256(image).hexdigest()}
            validate(payload)
            Path(args.output).write_bytes(canonical(payload) + b'\n')
            Path(args.identity_output).write_bytes(canonical({field: payload[field] for field in (*IDENTITY, 'chip')}) + b'\n')
            return 0
        expected = loads(read_bounded(args.identity, 1024), 1024)
        if set(expected) != {*IDENTITY, 'chip'}:
            raise ValueError('identity file must contain exactly product/board/profile/channel/layout/chip')
        checks = {'now': int(time.time()) if args.now is None else args.now,
                  'current_release': args.current_release, 'key_generation': args.key_generation,
                  'expected': expected, 'source_prefix': args.source_prefix}
        key = load_key(args.key, args.command == 'sign')
        if args.command == 'sign':
            payload = loads(read_bounded(args.payload, MAX_PAYLOAD))
            validate(payload, **checks)
            check_image(image, payload)
            envelope = sign(payload, key, args.kid, **checks)
            Path(args.output).write_bytes(canonical(envelope) + b'\n')
        else:
            envelope = loads(read_bounded(args.manifest, MAX_ENVELOPE), MAX_ENVELOPE)
            payload = verify(envelope, key, args.kid, **checks)
            check_image(image, payload)
            print(json.dumps({'verified': True, **{field: payload[field] for field in (*IDENTITY, 'release', 'version')}}))
        return 0
    except (OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
