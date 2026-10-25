#!/bin/sh
# Builds the USB Gate application for the Cybiko Xtreme: xtreme/USBGate.app
# (kept in the repository). Needs the GNU H8 tools (Debian/Ubuntu:
# binutils-h8300-hms gcc-h8300-hms), Python 3 and the cybiko-browser
# submodule (git submodule update --init).
set -e
cd "$(dirname "$0")/.."
out=build/xtreme
mkdir -p $out
CFLAGS="-ms -Os -fomit-frame-pointer -Wall -Wextra -ffreestanding -fno-builtin"
h8300-hitachi-coff-gcc $CFLAGS -c xtreme/main.c -o $out/main.o
for base in 0 0x10000; do
    h8300-hitachi-coff-ld -mh8300s -T xtreme/link.ld -Ttext $base -o $out/at$base.coff $out/main.o
done
python3 cybiko-browser/tools/cybiko_mkapp.py --at0 $out/at0.coff --at1 $out/at0x10000.coff \
    --name USBGate --icon xtreme/icon.txt --help-file xtreme/help.txt xtreme/USBGate.app
