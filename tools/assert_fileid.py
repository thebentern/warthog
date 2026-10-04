#!/usr/bin/env python3
"""Name the source file of an AT+ASSERT? fileid.

The build defines MMOSAL_FILEID for every C source of morselib, hostap, the shims, mmpktmem and
mmutils, for components/halow's mmhalow.c, and for the main/ sources that assert (MMOSAL_ASSERT or
MMOSAL_LOG_FAILURE_INFO), as the first 8 hex digits of the SHA-256 of its path relative to the
repository root (components/halow/components/mmosal_fileid.cmake). This hashes every .c in the
repository the same way. fileid 0 is a source built without MMOSAL_FILEID.

Usage: tools/assert_fileid.py <fileid>...   (0x1a2b3c4d, 1a2b3c4d or fileid=0x1a2b3c4d)
       tools/assert_fileid.py --list        (every fileid and its file)
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SKIP = {'.git', '.pio', 'managed_components', 'build', 'node_modules'}


def fileid(rel):
    return int(hashlib.sha256(rel.encode()).hexdigest()[:8], 16)


def sources():
    for top, dirs, files in os.walk(ROOT):
        dirs[:] = sorted(d for d in dirs if d not in SKIP)
        for f in sorted(files):
            if f.endswith('.c'):
                yield os.path.relpath(os.path.join(top, f), ROOT).replace(os.sep, '/')


def main(argv):
    if len(argv) < 2 or argv[1] in ('-h', '--help'):
        print(__doc__.strip())
        return 2
    table = {}
    for rel in sources():
        table.setdefault(fileid(rel), []).append(rel)
    if argv[1] == '--list':
        for fid in sorted(table):
            for rel in table[fid]:
                print('0x%08x %s' % (fid, rel))
        return 0
    rc = 0
    for arg in argv[1:]:
        text = arg.split('=', 1)[-1]
        try:
            fid = int(text, 16)
        except ValueError:
            print('%s: not a hex fileid' % arg)
            rc = 1
            continue
        if fid == 0:
            print('0x00000000 built without MMOSAL_FILEID')
            continue
        for rel in table.get(fid, []):
            print('0x%08x %s' % (fid, rel))
        if fid not in table:
            print('0x%08x no .c in this tree hashes to it' % fid)
            rc = 1
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv))
