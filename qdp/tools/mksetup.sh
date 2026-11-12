#!/bin/bash
# qdp/tools/mksetup.sh - the QEMU DOS port's setup media, from the tree (build-qdp's kernel first: make -f Makefile.qdp):
#
#   build-qdp/setup/QEMUDOS.ISO   the setup CD: a BIOS boots its MS-DOS 7.1 disk (El Torito hard-disk emulation), whose
#                                 AUTOEXEC.BAT runs QEMUDOS /SETUP; a UEFI PC starts DOS95's BIOS layer from its EFI image
#   build-qdp/setup/SETUP.IMG     the same disk for a USB stick (or an emulator's hard disk): a BIOS boots it, a UEFI PC
#                                 starts \EFI\BOOT\BOOTX64.EFI on it (Secure Boot off)
#
# Inputs, all in the tree: build-qdp/QEMUDOS.COM, QEMUDOS.KRN; dist/DOS95/QEMU (QEMU 0.14's BIOS files);
# dist/DOS95/EFI (CSMWrap: tools/csmwrap); tools/dos95-ms71.img (MS-DOS 7.1: IO.SYS, MSDOS.SYS, COMMAND.COM, \DOS -
# put there by its own SYS, tools/mkms71img.sh); qdp/boot (the MBR's source, MS-DOS 7.1's FAT16 and FAT32 boot records).
set -e
cd "$(dirname "$0")/../.."
B=build-qdp/setup
export MTOOLS_SKIP_CHECK=1
for f in build-qdp/QEMUDOS.COM build-qdp/QEMUDOS.KRN; do [ -f $f ] || { echo "mksetup: $f missing - make -f Makefile.qdp"; exit 1; }; done
rm -rf $B && mkdir -p $B/sys $B/dos $B/FILES $B/iso
MS="-i tools/dos95-ms71.img@@32256"
mcopy $MS ::/IO.SYS ::/MSDOS.SYS ::/COMMAND.COM $B/sys/
mcopy $MS "::/DOS/*" $B/dos/
nasm -f bin -o $B/MBR.BIN qdp/boot/mbr.asm

# ---- what is installed: the new disk's root (SETUP\FILES) ----
F=$B/FILES
cp $B/sys/* $F/
mkdir -p $F/DOS $F/QEMUDOS/QEMU $F/QEMUDOS/MACHINES $F/QEMUDOS/DISKS $F/QEMUDOS/IMAGES $F/TEMP $F/EFI/BOOT
cp $B/dos/* $F/DOS/
cp build-qdp/QEMUDOS.COM build-qdp/QEMUDOS.KRN $F/QEMUDOS/
cp dist/DOS95/QEMU/*.bin $F/QEMUDOS/QEMU/
cp dist/DOS95/EFI/BOOTX64.EFI dist/DOS95/EFI/BOOTIA32.EFI $F/EFI/BOOT/
cp qdp/dist/README.TXT $F/QEMUDOS/README.TXT
printf 'DEVICE=C:\\DOS\\HIMEM.SYS\r\nDOS=HIGH\r\nFILES=40\r\nBUFFERS=30\r\nLASTDRIVE=Z\r\nREM a CD-ROM drive for DOS (an IDE or SATA one in IDE mode)\r\nDEVICE=C:\\DOS\\UDVD2.SYS /D:MSCD001\r\n' > $F/CONFIG.SYS
# (its last line: 64 characters and CR LF the setup turns into the line that starts the QEMU DOS port, or a remark)
{ printf '@ECHO OFF\r\nPROMPT $P$G\r\nPATH C:\\DOS;C:\\QEMUDOS\r\nSET TEMP=C:\\TEMP\r\nSET TMP=C:\\TEMP\r\nC:\\DOS\\SHSUCDX.COM /D:MSCD001 /Q\r\n'
  printf '%-64s\r\n' 'REM QDP-START-LINE'; } > $F/AUTOEXEC.BAT

# ---- the setup disk: MS-DOS 7.1 starting QEMUDOS /SETUP, the QEMU DOS port, SETUP\ ----
python3 qdp/tools/mkdosdisk.py $B/SETUP.IMG 64 --mbr $B/MBR.BIN --sysdir $B/sys --sig 51445053
I="-i $B/SETUP.IMG@@32256"
printf 'DEVICE=C:\\DOS\\HIMEM.SYS\r\nDOS=HIGH\r\nFILES=40\r\nBUFFERS=30\r\nLASTDRIVE=Z\r\n' > $B/CONFIG.SYS
printf '@ECHO OFF\r\nPATH C:\\DOS;C:\\QEMUDOS\r\nECHO.\r\nECHO QEMU DOS port Setup\r\nECHO.\r\nC:\\QEMUDOS\\QEMUDOS.COM /SETUP\r\nECHO.\r\nECHO Type QEMUDOS /SETUP to start the setup again.\r\n' > $B/AUTOEXEC.BAT
mcopy $I $B/CONFIG.SYS $B/AUTOEXEC.BAT ::/
mmd $I ::/DOS ::/QEMUDOS ::/QEMUDOS/QEMU ::/SETUP ::/EFI ::/EFI/BOOT
mcopy $I $B/dos/* ::/DOS/
mcopy $I build-qdp/QEMUDOS.COM build-qdp/QEMUDOS.KRN ::/QEMUDOS/
mcopy $I dist/DOS95/QEMU/*.bin ::/QEMUDOS/QEMU/
mcopy $I dist/DOS95/EFI/BOOTX64.EFI dist/DOS95/EFI/BOOTIA32.EFI ::/EFI/BOOT/
mcopy $I $B/MBR.BIN ::/SETUP/MBR.BIN
mcopy $I qdp/boot/fat32vbr.bin ::/SETUP/FAT32VBR.BIN
mcopy -s $I $F ::/SETUP/
mcopy $I qdp/dist/README.TXT ::/README.TXT
echo "SETUP.IMG: $(mdir -i $B/SETUP.IMG@@32256 :: | tail -1)"

# ---- the CD: the setup disk as its El Torito hard disk, an EFI image (DOS95's BIOS layer) beside it ----
mformat -C -f 1440 -i $B/EFIBOOT.IMG ::
mmd -i $B/EFIBOOT.IMG ::/EFI ::/EFI/BOOT
mcopy -i $B/EFIBOOT.IMG dist/DOS95/EFI/BOOTX64.EFI dist/DOS95/EFI/BOOTIA32.EFI ::/EFI/BOOT/
cp $B/SETUP.IMG $B/EFIBOOT.IMG $B/iso/
cp qdp/dist/README.TXT $B/iso/README.TXT
xorriso -as mkisofs -quiet -V QEMUDOS_SETUP -J -R -b SETUP.IMG -hard-disk-boot \
    -eltorito-alt-boot -e EFIBOOT.IMG -no-emul-boot -o $B/QEMUDOS.ISO $B/iso
rm -rf $B/sys $B/dos $B/iso $B/FILES $B/CONFIG.SYS $B/AUTOEXEC.BAT
ls -la $B/QEMUDOS.ISO $B/SETUP.IMG
