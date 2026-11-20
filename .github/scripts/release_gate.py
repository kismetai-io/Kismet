import hashlib
import json
from pathlib import Path
import re

ENGINES = ('5.6', '5.7', '5.8', '5.9')
PLATFORMS = ('Win64', 'Mac')


def validate_version(version):
    if not isinstance(version, str) or not re.fullmatch(r'(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)', version):
        raise ValueError('Release version must be stable major.minor.patch')
    return version


def inventory(root, version, commit):
    validate_version(version)
    if not re.fullmatch('[0-9a-f]{40}', commit):
        raise ValueError('An exact source commit is required')
    expected = {f'KismetAI-{version}-UE{e}-{p}-release{s}.zip'
                for e in ENGINES for p in PLATFORMS for s in ('', '-src')}
    actual = {p.name for p in root.glob('*.zip')}
    if actual != expected:
        raise ValueError(f'Incomplete or unexpected release matrix: {sorted(actual ^ expected)}')
    assets = []
    for name in sorted(expected):
        path = root / name
        if path.is_symlink() or not path.is_file() or not path.stat().st_size:
            raise ValueError(f'Invalid package: {name}')
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        assets.append({'name': name, 'size': path.stat().st_size, 'sha256': digest})
    return {'schema': 1, 'version': version, 'commit': commit, 'assets': assets}


def verify(root, receipt):
    actual = inventory(root, receipt['version'], receipt['commit'])
    if receipt != actual:
        raise ValueError('Release receipt does not match the complete package set')
    return actual


def github(path, method='GET', data=None):
    import subprocess
    command = ['gh', 'api', '--method', method, path]
    if data is not None:
        command += ['--input', '-']
    result = subprocess.run(command, input=json.dumps(data) if data is not None else None,
                            text=True, capture_output=True)
    if result.returncode:
        if method == 'GET' and '(HTTP 404)' in result.stderr:
            return None
        raise RuntimeError(f'GitHub request failed: {method} {path}: {result.stderr}')
    return json.loads(result.stdout) if result.stdout.strip() else None


def current_release(repo, version):
    tag = 'v' + validate_version(version)
    page = 1
    while True:
        releases = github(f'repos/{repo}/releases?per_page=100&page={page}')
        for release in releases:
            if release['tag_name'] == tag:
                return release
        if len(releases) < 100:
            return None
        page += 1


def prepare():
    import os
    version = validate_version(json.loads(Path('KismetAI/KismetAI.uplugin').read_text())['VersionName'])
    release = current_release(os.environ['GITHUB_REPOSITORY'], version)
    eligible = release is None or release['draft']
    with open(os.environ['GITHUB_OUTPUT'], 'a') as out:
        out.write(f'version={version}\neligible={str(eligible).lower()}\n')
    print(f'Version {version}: ' + ('prepare candidate' if eligible else 'already published; bump the plugin version for a new release'))


def draft(root, version, commit):
    import os
    import subprocess
    repo = os.environ['GITHUB_REPOSITORY']
    # A newer merge must not be replaced by an older build finishing later.
    latest = github(f'repos/{repo}/git/ref/heads/main')['object']['sha']
    if latest != commit:
        print('A newer main commit exists; this candidate is superseded')
        return
    receipt = inventory(root, version, commit)
    receipt_path = root / 'release-receipt.json'
    receipt_path.write_text(json.dumps(receipt, indent=2) + '\n')
    release = current_release(repo, version)
    if release and not release['draft']:
        raise ValueError('Published versions are immutable')
    # A published tag must never be moved to a different candidate.
    tag = github(f'repos/{repo}/git/ref/tags/v{version}')
    if tag is not None:
        raise ValueError('Release tag already exists; use a new version')
    body = (f'Release candidate for `{commit}`.\n\n'
            'Windows and Mac, UE 5.6–5.9: all builds and automation suites passed.\n\n'
            '**Smoke testing:** download the packages below and record your findings before publishing. '
            'Publishing promotes these exact packages to the download service and release catalogue. '
            'Evals and backend deployment are not part of this gate.\n\n'
            'A later successful merge with this version can replace this unpublished draft; '
            'check the source commit and checksums again before publishing.')
    data = {'tag_name': f'v{version}', 'target_commitish': commit,
            'name': f'Kismet {version}', 'body': body, 'draft': True}
    if release:
        release_id = release['id']
        github(f'repos/{repo}/releases/{release_id}', 'PATCH', data)
    else:
        release_id = github(f'repos/{repo}/releases', 'POST', data)['id']
    for asset in [root / a['name'] for a in receipt['assets']] + [receipt_path]:
        if not github(f'repos/{repo}/releases/{release_id}')['draft']:
            raise ValueError('Draft was published during replacement; promotion must validate its receipt')
        subprocess.run(['gh', 'release', 'upload', f'v{version}', '--repo', repo,
                        '--clobber', str(asset)], check=True)
    remote = github(f'repos/{repo}/releases/{release_id}')
    expected = {a['name'] for a in receipt['assets']} | {'release-receipt.json'}
    if {a['name'] for a in remote['assets']} != expected:
        raise ValueError('Draft has unexpected assets; remove them before publishing')
    print(remote['html_url'])


def validate_published(root):
    import os
    event = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())
    release = event['release']
    if release['draft'] or release['prerelease']:
        raise ValueError('Only published stable releases may reach the stable catalogue')
    receipt = json.loads((root / 'release-receipt.json').read_text())
    verify(root, receipt)
    if release['tag_name'] != 'v' + receipt['version']:
        raise ValueError('Release tag and package version differ')
    repo = os.environ['GITHUB_REPOSITORY']
    commit = github(f'repos/{repo}/commits/{release["tag_name"]}')['sha']
    if commit != receipt['commit']:
        raise ValueError('Published tag does not identify the smoke-tested source commit')
    return receipt


def publish(root):
    import os
    import boto3
    import psycopg
    from botocore.exceptions import ClientError
    receipt = validate_published(root)
    base_url = os.environ['RELEASES_BASE_URL'].rstrip('/')
    if not base_url.startswith('https://'):
        raise ValueError('Downloads require HTTPS')
    s3 = boto3.client('s3', endpoint_url=os.environ['R2_ENDPOINT_URL'], region_name='auto')
    bucket = os.environ['R2_BUCKET']
    # Content-addressed keys prevent retry or concurrent candidates overwriting bytes.
    locations = {}
    for asset in receipt['assets']:
        key = f'{receipt["version"]}/{asset["sha256"]}/{asset["name"]}'
        try:
            s3.head_object(Bucket=bucket, Key=key)
        except ClientError as error:
            if error.response['Error']['Code'] not in ('404', 'NoSuchKey', 'NotFound'):
                raise
            s3.upload_file(str(root / asset['name']), bucket, key,
                           ExtraArgs={'ContentType': 'application/zip', 'CacheControl': 'public,max-age=31536000,immutable'})
        response = s3.get_object(Bucket=bucket, Key=key)
        digest = hashlib.sha256()
        size = 0
        with response['Body'] as stream:
            for chunk in stream.iter_chunks(chunk_size=1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
        if digest.hexdigest() != asset['sha256'] or size != asset['size']:
            raise ValueError(f'Uploaded object differs: {asset["name"]}')
        locations[asset['name']] = base_url + '/' + key
    assets = {a['name']: a for a in receipt['assets']}
    notes = json.loads(Path(os.environ['GITHUB_EVENT_PATH']).read_text())['release'].get('body') or ''
    with psycopg.connect(os.environ['RELEASE_DATABASE_URL'], sslmode='require') as connection:
        with connection.cursor() as cursor:
            cursor.execute("SELECT pg_advisory_xact_lock(hashtext('kismet-release-publish'))")
            for engine in ENGINES:
                for platform in PLATFORMS:
                    name = f'KismetAI-{receipt["version"]}-UE{engine}-{platform}-release.zip'
                    src = name[:-4] + '-src.zip'
                    values = (receipt['version'], 'stable', engine, platform, locations[name],
                              assets[name]['size'], assets[name]['sha256'], locations[src],
                              assets[src]['size'], assets[src]['sha256'], notes)
                    cursor.execute('SELECT url, sha256, source_url, source_sha256 FROM public.releases WHERE version=%s AND channel=%s AND engine=%s AND platform=%s', values[:4])
                    existing = cursor.fetchone()
                    if existing:
                        if existing != (values[4], values[6], values[7], values[9]):
                            raise ValueError('Published catalogue version already has different assets')
                        continue
                    cursor.execute('''INSERT INTO public.releases
                        (version,channel,engine,platform,url,size_bytes,sha256,source_url,source_size_bytes,source_sha256,notes)
                        VALUES (%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s)''', values)
    print(f'Published {len(assets)} verified packages and eight catalogue rows for {receipt["version"]}')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser()
    parser.add_argument('action', choices=['prepare', 'draft', 'verify-published', 'publish'])
    parser.add_argument('--root', type=Path, default=Path('release-assets'))
    parser.add_argument('--version')
    parser.add_argument('--commit')
    args = parser.parse_args()
    if args.action == 'prepare':
        prepare()
    elif args.action == 'draft':
        draft(args.root, args.version, args.commit)
    elif args.action == 'verify-published':
        validate_published(args.root)
    else:
        publish(args.root)
