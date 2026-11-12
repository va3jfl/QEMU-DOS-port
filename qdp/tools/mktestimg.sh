#!/bin/bash
# qdp/tools/mktestimg.sh OUT.img - the QEMU DOS port's test disk: the regression's MS-DOS 7.1 disk (tools/dos95-ms71.img)
# with C:\QEMUDOS (build-qdp's kernel and loader, QEMU's BIOS files) and AUTOEXEC.BAT starting QEMUDOS, a machine that
# boots MS-DOS 7.1 from a floppy image (MS-DOS 7.1R2.img, archive.org's), and the test programs as they are.
set -e
cd "$(dirname "$0")/../.."
OUT=${1:?usage: mktestimg.sh OUT.img}
[ -f build-qdp/QEMUDOS.KRN ] || { echo "make -f Makefile.qdp first"; exit 1; }
cp tools/dos95-ms71.img "$OUT"
I="-i $OUT@@32256"
export MTOOLS_SKIP_CHECK=1
mmd $I ::/QEMUDOS ::/QEMUDOS/QEMU ::/QEMUDOS/MACHINES ::/QEMUDOS/IMAGES ::/QEMUDOS/DISKS 2>/dev/null || true
mcopy -o $I build-qdp/QEMUDOS.COM build-qdp/QEMUDOS.KRN ::/QEMUDOS/
mcopy -o $I dist/DOS95/QEMU/bios.bin dist/DOS95/QEMU/vgabios.bin dist/DOS95/QEMU/vgabios-cirrus.bin ::/QEMUDOS/QEMU/
mcopy -o $I "tools/msdos71/MS-DOS 7.1R2.img" ::/QEMUDOS/IMAGES/MSDOS71.IMG
S=$(mktemp -d)
printf '; QEMU DOS port machine\r\n[Machine]\r\nName=MS-DOS 7.1 floppy\r\nMemory=32\r\nCpus=1\r\nCpu=\r\nHda=\r\nHdb=\r\nHdd=\r\nCdrom=\r\nFloppy=C:\\QEMUDOS\\IMAGES\\MSDOS71.IMG\r\nFloppyB=\r\nBoot=a\r\nVga=cirrus\r\nSound=1\r\nPcspk=1\r\nUsbTablet=0\r\nNet=2\r\nMac=\r\nLocalTime=1\r\nAcpi=1\r\nAutoStart=0\r\nExtra=\r\n' > $S/MSDOS71.VM
mcopy -o $I $S/MSDOS71.VM ::/QEMUDOS/MACHINES/
printf '@ECHO OFF\r\nSET PATH=C:\\DOS;C:\\QEMUDOS\r\nSET TEMP=C:\\TEMP\r\nSET TMP=C:\\TEMP\r\nC:\\DOS\\SHSUCDX.COM /D:MSCD001\r\nC:\\QEMUDOS\\QEMUDOS.COM\r\n' > $S/AUTOEXEC.BAT
mcopy -o $I $S/AUTOEXEC.BAT ::/AUTOEXEC.BAT
rm -rf $S
echo "$OUT: C:\\QEMUDOS, AUTOEXEC.BAT starts QEMUDOS"
