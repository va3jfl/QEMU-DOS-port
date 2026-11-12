#!/usr/bin/env python3
"""qdp/tools/mkdosdisk.py OUT.img SIZE_MB [--sig HEX] - an MS-DOS 7.1 hard disk image that boots: the QEMU DOS port's
MBR (qdp/boot/mbr.asm, assembled by the caller into MBR.bin), one active FAT16 partition from sector 63 (16 heads,
63 sectors a track: the geometry an El Torito hard-disk emulation and every BIOS agree on), the filesystem made by
mtools' mformat, MS-DOS 7.1's own boot code on it (qdp/boot/fat16vbr.bin: its boot sector, as its SYS wrote it on
tools/dos95-ms71.img - the BPB mformat's), IO.SYS copied first.  The caller copies the rest with mtools.
   --mbr FILE   the assembled MBR (512 bytes)    --sysdir DIR   where IO.SYS, MSDOS.SYS, COMMAND.COM are
   --sig HEX    the disk's signature (MBR 1B8h) - the setup disk's is 51445053 ('QDPS'): the setup leaves it alone"""
import os, struct, subprocess, sys, argparse
ap = argparse.ArgumentParser()
ap.add_argument('out'); ap.add_argument('size_mb', type=int)
ap.add_argument('--mbr', required=True); ap.add_argument('--sysdir', required=True)
ap.add_argument('--vbr', default=os.path.join(os.path.dirname(__file__), '..', 'boot', 'fat16vbr.bin'))
ap.add_argument('--sig', default='0')
a = ap.parse_args()
H, S = 16, 63
total = a.size_mb * 2048
cyls = total // (H * S)
total = cyls * H * S
start = 63
psize = total - start
if psize > 4194304 - start or psize < 8400:
    sys.exit('mkdosdisk: FAT16 here is 4.2 MB to 2 GB')
def chs(lba):
    c, r = divmod(lba, H * S); h, s = divmod(r, S)
    if c > 1023: c, h, s = 1023, H - 1, S - 1      # (past what CHS can say: its largest, the LBA values count)
    return bytes([h, ((c >> 2) & 0xC0) | (s + 1), c & 0xFF])
with open(a.out, 'wb') as f:
    f.truncate(total * 512)
mbr = bytearray(open(a.mbr, 'rb').read())
assert len(mbr) == 512 and mbr[510:512] == b'\x55\xaa'
struct.pack_into('<I', mbr, 0x1B8, int(a.sig, 16))
# type 06h (FAT16 by cylinder, head and sector): MS-DOS 7.1's boot code takes its partition's type from the MBR, and with
# 0Eh it reads by INT 13h's extensions - which an El Torito hard-disk emulation may not have (SeaBIOS's has not)
ent = bytes([0x80]) + chs(start) + bytes([0x06]) + chs(start + psize - 1) + struct.pack('<II', start, psize)
mbr[0x1BE:0x1CE] = ent
with open(a.out, 'r+b') as f:
    f.write(mbr)
env = dict(os.environ, MTOOLS_SKIP_CHECK='1')
img = '%s@@%d' % (a.out, start * 512)
# the filesystem one cylinder short of its partition: an El Torito emulation takes the disk's cylinders from the
# partition's last one and may count one fewer (SeaBIOS's does) - nothing of DOS's lies in that cylinder
fs_sectors = psize - H * S
subprocess.run(['mformat', '-i', img, '-T', str(fs_sectors), '-h', str(H), '-s', str(S), '-H', str(start), '-c', '8', '::'], check=True, env=env)
vbr = open(a.vbr, 'rb').read()
assert len(vbr) == 512 and vbr[3:11] == b'MSWIN4.1' and vbr[510:512] == b'\x55\xaa'
with open(a.out, 'r+b') as f:
    f.seek(start * 512); cur = bytearray(f.read(512))
    cur[0:3] = vbr[0:3]; cur[3:11] = b'MSWIN4.1'; cur[0x3E:0x200] = vbr[0x3E:0x200]
    cur[0x24] = 0x80                                  # (BS_DrvNum: the first hard disk)
    f.seek(start * 512); f.write(cur)
for n in ('IO.SYS', 'MSDOS.SYS', 'COMMAND.COM'):
    p = os.path.join(a.sysdir, n)
    subprocess.run(['mcopy', '-i', img, p, '::/' + n], check=True, env=env)
subprocess.run(['mattrib', '-i', img, '+s', '+h', '+r', '::/IO.SYS', '::/MSDOS.SYS'], check=True, env=env)
print('%s: %d MB, FAT16 partition at %d, %d sectors' % (a.out, total // 2048, start, psize))
