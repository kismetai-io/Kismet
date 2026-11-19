import argparse
import os
from pathlib import Path

from Cryptodome.Cipher import AES

HEADER = b'KISMET-UE-MANIFEST\x01'


def seal(data, key):
    if len(key) != 32:
        raise ValueError('UE_MANIFEST_KEY must contain 32 bytes encoded as hex')
    cipher = AES.new(key, AES.MODE_GCM, nonce=os.urandom(12))
    cipher.update(HEADER)
    encrypted, tag = cipher.encrypt_and_digest(data)
    return HEADER + cipher.nonce + tag + encrypted


def unseal(data, key):
    if len(key) != 32:
        raise ValueError('UE_MANIFEST_KEY must contain 32 bytes encoded as hex')
    offset = len(HEADER)
    if not data.startswith(HEADER) or len(data) < offset + 28:
        raise ValueError('Invalid encrypted manifest header')
    cipher = AES.new(key, AES.MODE_GCM, nonce=data[offset:offset + 12])
    cipher.update(HEADER)
    return cipher.decrypt_and_verify(data[offset + 28:], data[offset + 12:offset + 28])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--prepare', action='store_true')
    args = parser.parse_args()
    key = bytes.fromhex(os.environ['UE_MANIFEST_KEY'])
    data = args.source.read_bytes()
    if args.prepare:
        from legendary.models.manifest import Manifest
        from runner_engine import selected
        manifest = Manifest.read_all(data)
        manifest.file_manifest_list.elements = [
            f for f in manifest.file_manifest_list.elements if selected(f, with_content=True)]
        required = {part.guid for f in manifest.file_manifest_list.elements for part in f.chunk_parts}
        manifest.chunk_data_list.elements = [c for c in manifest.chunk_data_list.elements if c.guid in required]
        data = seal(manifest.write(), key)
    else:
        data = unseal(data, key)
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    args.destination.write_bytes(data)


if __name__ == '__main__':
    main()
