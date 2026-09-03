#!/usr/bin/env python3
"""download_vac.py — fetch the published SDSS DR17 Cosmic Slime VAC cube.

Downloads, into bin/data/reference/ (or --dest):

    mcpm_v1_0_0_datacube_SDSS_z_44-476mpc.sha1sum   the archive's checksums
    export_metadata.txt                              544 B
    trace.bin.bz2                                    345 MB

from the SDSS Science Archive Server and verifies each file's sha1 against
the archive's own .sha1sum listing. Files already present with a matching
hash are skipped, so the script is safe to re-run. The downloaded
export_metadata.txt must also be identical to the committed copy in
bin/data/reference/ — a difference would mean the VAC was re-released and
the vendored reference cube needs re-deriving.

Standard library only; no third-party packages. Next step after this:
tools/validate/extract_reference.py (produces mcpm_sdss_d8.npy).

Usage:
    .venv/bin/python tools/validate/download_vac.py [--dest DIR]
"""
import argparse
import hashlib
import sys
import urllib.request
from pathlib import Path

BASE_URL = ('https://data.sdss.org/sas/dr17/env/EBOSS_LSS/mcpm/v1_0_1/'
            'datacube/SDSS_z_44-476mpc/')
SHA1SUM_FILE = 'mcpm_v1_0_0_datacube_SDSS_z_44-476mpc.sha1sum'
FILES = ('export_metadata.txt', 'trace.bin.bz2')
REPO_ROOT = Path(__file__).resolve().parents[2]
DEST_DEFAULT = REPO_ROOT / 'bin' / 'data' / 'reference'
CHUNK = 1 << 20


def sha1_of(path):
    h = hashlib.sha1()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(CHUNK), b''):
            h.update(block)
    return h.hexdigest()


def fetch(name, dest):
    """Stream BASE_URL/name to dest/name via a .part file; print progress."""
    url = BASE_URL + name
    part = dest / (name + '.part')
    final = dest / name
    print(f'downloading {url}')
    with urllib.request.urlopen(url) as resp, open(part, 'wb') as out:
        total = int(resp.headers.get('Content-Length') or 0)
        show = total > CHUNK
        done = 0
        for block in iter(lambda: resp.read(CHUNK), b''):
            out.write(block)
            done += len(block)
            if show:
                print(f'\r  {done / 1e6:8.1f} / {total / 1e6:.1f} MB', end='',
                      flush=True)
        if show:
            print()
    part.replace(final)
    return final


def parse_sha1sums(path):
    """{filename: sha1} from a `sha1  name` listing."""
    out = {}
    for line in path.read_text().splitlines():
        parts = line.split()
        if len(parts) == 2:
            out[parts[1]] = parts[0].lower()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--dest', type=Path, default=DEST_DEFAULT)
    args = ap.parse_args()
    dest = args.dest
    dest.mkdir(parents=True, exist_ok=True)

    # The checksum listing is small and authoritative: always refetch it.
    sums = parse_sha1sums(fetch(SHA1SUM_FILE, dest))
    missing = [f for f in FILES if f not in sums]
    if missing:
        sys.exit(f'FATAL: {SHA1SUM_FILE} lists no hash for {missing}')

    failed = False
    for name in FILES:
        path = dest / name
        if path.exists():
            if sha1_of(path) == sums[name]:
                print(f'{name}: present, sha1 OK — skipped')
                continue
            if name == 'export_metadata.txt':
                # Never overwrite the (possibly committed) metadata in place.
                print(f'{name}: present but its sha1 differs from the archive '
                      f'listing — the VAC may have been re-released; not '
                      f'overwriting. Fetch with --dest elsewhere to inspect.')
                failed = True
                continue
        fetch(name, dest)
        got = sha1_of(path)
        if got == sums[name]:
            print(f'{name}: sha1 OK ({got})')
        else:
            print(f'{name}: SHA1 MISMATCH — got {got}, archive says {sums[name]}')
            failed = True

    # Cross-check against the committed metadata: identical, or the VAC changed.
    committed = DEST_DEFAULT / 'export_metadata.txt'
    fetched = dest / 'export_metadata.txt'
    if committed.exists() and fetched.exists() and committed.resolve() != fetched.resolve():
        if committed.read_bytes() == fetched.read_bytes():
            print('export_metadata.txt: identical to committed copy')
        else:
            print('export_metadata.txt: DIFFERS from committed copy — the VAC '
                  'may have been re-released; re-derive the reference cube')
            failed = True

    if failed:
        sys.exit(1)
    print(f'done — files in {dest}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
