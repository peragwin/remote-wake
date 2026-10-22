#!/usr/bin/env python3
"""Publish a verified release, then atomically promote its signed channel manifest.

Dry run by default. Only --publish invokes GitHub writes. Requires authenticated
GitHub CLI; no signing key or device connection is used here.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time

import manifest as m


class Conflict(ValueError):
    pass


def gh(*args, body=None, missing_ok=False):
    result = subprocess.run(['gh', *args], input=body, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, check=False)
    if result.returncode:
        if missing_ok and b'HTTP 404' in result.stderr:
            return None
        if b'HTTP 409' in result.stderr or b'HTTP 422' in result.stderr:
            raise Conflict('GitHub metadata changed during promotion')
        raise ValueError('GitHub operation failed: ' + ' '.join(args[:2]))
    return result.stdout


def channel_path(payload):
    return 'channels/' + '/'.join(payload[field] for field in m.IDENTITY) + '.json'


def publication_plan(payload, repository, tag, image_name, manifest_name):
    if not re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository):
        raise ValueError('repository must be OWNER/REPO')
    for value in (tag, image_name, manifest_name):
        if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', value) or value in ('.', '..'):
            raise ValueError('unsafe tag or asset filename')
    expected_url = f'https://github.com/{repository}/releases/download/{tag}/{image_name}'
    if payload['url'] != expected_url:
        raise ValueError('signed image URL must exactly match the versioned release asset')
    if image_name == manifest_name:
        raise ValueError('binary and manifest asset names must differ')
    return {'repository': repository, 'tag': tag, 'image_url': expected_url,
            'manifest_url': f'https://github.com/{repository}/releases/download/{tag}/{manifest_name}',
            'channel_path': channel_path(payload), 'release': payload['release']}


CATALOG_FIELDS = {'product', 'board', 'profile', 'channel', 'partition_layout',
                  'version', 'release_counter', 'manifest_url'}
CATALOG_MAX = 65536


def validate_catalog(catalog):
    if set(catalog) != {'v', 'releases'} or type(catalog['v']) is not int or catalog['v'] != 1:
        raise ValueError('invalid relay catalog schema')
    releases = catalog['releases']
    if not isinstance(releases, list) or len(releases) > 64:
        raise ValueError('relay catalog must contain at most 64 releases')
    seen = set()
    for entry in releases:
        if not isinstance(entry, dict) or set(entry) != CATALOG_FIELDS:
            raise ValueError('invalid relay catalog entry')
        for field, maximum in [('product', 31), ('board', 31), ('profile', 47), ('channel', 15),
                               ('partition_layout', 31), ('version', 31)]:
            m.identifier(entry[field], field, maximum)
        m.integer(entry['release_counter'], 1, 4294967295, 'release_counter')
        url = entry['manifest_url']
        if not isinstance(url, str) or len(url) > 511 or not re.fullmatch(
                r'https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+/releases/download/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', url):
            raise ValueError('catalog manifest URL must be an exact versioned GitHub release asset')
        identity = tuple(entry[field] for field in ('product', 'board', 'profile', 'channel', 'partition_layout'))
        if identity in seen:
            raise ValueError('duplicate release tuple in catalog')
        seen.add(identity)
    if len(m.canonical(catalog)) > CATALOG_MAX:
        raise ValueError('relay catalog exceeds 64 KiB')
    return catalog


def merge_catalog(catalog, payload, manifest_url):
    validate_catalog(catalog)
    entry = {field: payload[field] for field in ('product', 'board', 'profile', 'channel', 'version')}
    entry.update(partition_layout=payload['layout'], release_counter=payload['release'], manifest_url=manifest_url)
    identity_fields = ('product', 'board', 'profile', 'channel', 'partition_layout')
    releases = list(catalog['releases'])
    for index, previous in enumerate(releases):
        if all(previous[field] == entry[field] for field in identity_fields):
            if previous['release_counter'] >= entry['release_counter']:
                raise ValueError('catalog already contains this or a newer tuple release')
            releases[index] = entry
            break
    else:
        releases.append(entry)
    releases.sort(key=lambda item: tuple(item[field] for field in identity_fields))
    return validate_catalog({'v': 1, 'releases': releases})


def read_content(api, branch, limit):
    data = gh('api', api + '?ref=' + branch, missing_ok=True)
    if data is None:
        return None, None
    if len(data) > limit * 2 + 8192:
        raise ValueError('GitHub metadata response exceeds bound')
    response = json.loads(data)
    encoded = response.get('content')
    if not isinstance(encoded, str) or len(encoded) > limit * 2:
        raise ValueError('GitHub metadata content exceeds bound')
    decoded = base64.b64decode(encoded)
    if len(decoded) > limit:
        raise ValueError('GitHub metadata content exceeds bound')
    return m.loads(decoded, limit), response['sha']


def promote_catalog(repository, branch, payload, manifest_url, attempts=3):
    api = f'repos/{repository}/contents/catalog.json'
    for attempt in range(attempts):
        prior, sha = read_content(api, branch, CATALOG_MAX)
        updated = merge_catalog(prior or {'v': 1, 'releases': []}, payload, manifest_url)
        request = {'message': f'Announce {payload["product"]} release {payload["release"]}',
                   'branch': branch, 'content': base64.b64encode(m.canonical(updated) + b'\n').decode()}
        if sha:
            request['sha'] = sha
        try:
            gh('api', '--method', 'PUT', api, '--input', '-', body=m.canonical(request))
            return
        except Conflict:
            if attempt + 1 == attempts:
                raise


def publish(image_path, manifest_path, envelope, payload, public_key, kid, plan, branch):
    m.identifier(branch, 'channel branch', 64)
    api = f'repos/{plan["repository"]}/contents/{plan["channel_path"]}'
    previous_envelope, previous_sha = read_content(api, branch, m.MAX_ENVELOPE)
    if previous_envelope is not None:
        previous_payload = m.verify(previous_envelope, public_key, kid,
                                    expected={field: payload[field] for field in (*m.IDENTITY, 'chip')})
        if payload['release'] <= previous_payload['release']:
            raise ValueError('channel already contains this or a newer release')
    existing_catalog, _ = read_content(f'repos/{plan["repository"]}/contents/catalog.json', branch, CATALOG_MAX)
    merge_catalog(existing_catalog or {'v': 1, 'releases': []}, payload, plan['manifest_url'])
    repo, tag = plan['repository'], plan['tag']
    # A versioned tag must already exist; never create or move a source tag here.
    gh('release', 'create', tag, str(image_path), str(manifest_path), '--repo', repo,
       '--verify-tag', '--draft', '--title', payload['version'],
       '--notes', f'Signed {payload["product"]} release {payload["release"]}; see manifest identity.')
    # Download the uploaded assets before publishing or promoting anything.
    with tempfile.TemporaryDirectory(prefix='rw-ota-verify-') as directory:
        gh('release', 'download', tag, '--repo', repo, '--dir', directory,
           '--pattern', Path(image_path).name, '--pattern', Path(manifest_path).name)
        uploaded_image = m.read_bounded(Path(directory) / Path(image_path).name, m.MAX_IMAGE)
        uploaded_manifest = m.read_bounded(Path(directory) / Path(manifest_path).name, m.MAX_ENVELOPE)
        m.check_image(uploaded_image, payload)
        if m.loads(uploaded_manifest, m.MAX_ENVELOPE) != envelope:
            raise ValueError('uploaded manifest differs from verified signed manifest')
    gh('release', 'edit', tag, '--repo', repo, '--draft=false')
    request = {'message': f'Promote {payload["product"]} {payload["channel"]} release {payload["release"]}',
               'branch': branch, 'content': base64.b64encode(m.canonical(envelope) + b'\n').decode()}
    if previous_sha:
        request['sha'] = previous_sha
    # GitHub Contents API is a single compare-and-swap commit. Failure preserves
    # the old pointer; an already-published versioned release can remain orphaned.
    gh('api', '--method', 'PUT', api, '--input', '-', body=m.canonical(request))
    promote_catalog(repo, branch, payload, plan['manifest_url'])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest', required=True)
    parser.add_argument('--image', required=True)
    parser.add_argument('--public-key', required=True)
    parser.add_argument('--kid', required=True)
    parser.add_argument('--key-generation', required=True, type=int)
    parser.add_argument('--current-release', required=True, type=int)
    parser.add_argument('--identity', required=True)
    parser.add_argument('--source-prefix', required=True)
    parser.add_argument('--repository', required=True)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--channel-branch', default='ota')
    parser.add_argument('--publish', action='store_true')
    args = parser.parse_args(argv)
    try:
        identity = m.loads(m.read_bounded(args.identity, 1024), 1024)
        if set(identity) != {*m.IDENTITY, 'chip'}:
            raise ValueError('identity file requires the full exact identity tuple and chip')
        envelope = m.loads(m.read_bounded(args.manifest, m.MAX_ENVELOPE), m.MAX_ENVELOPE)
        public_key = m.load_key(args.public_key)
        payload = m.verify(envelope, public_key, args.kid, now=int(time.time()), expected=identity,
                           current_release=args.current_release, key_generation=args.key_generation,
                           source_prefix=args.source_prefix)
        m.check_image(m.read_bounded(args.image, m.MAX_IMAGE), payload)
        plan = publication_plan(payload, args.repository, args.tag, Path(args.image).name, Path(args.manifest).name)
        if args.publish:
            publish(Path(args.image), Path(args.manifest), envelope, payload, public_key,
                    args.kid, plan, args.channel_branch)
        print(json.dumps({'published': args.publish, **plan}, sort_keys=True))
        return 0
    except (OSError, ValueError, KeyError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    sys.exit(main())
