#!/bin/zsh

cp roms/edk2/Build/MyGpuPkg/DEBUG_GCC/X64/MyGpuGopDxe.efi esp
cp roms/edk2/Build/MyGpuPkg/DEBUG_GCC/X64/GopTest.efi esp

build/qemu-system-x86_64 -machine q35 \
  -m 2G \
  -vga none \
  -device fakegpu \
  -bios esp/OVMF.fd \
  -drive format=raw,file=fat:rw:./esp \
  -display gtk \
  -serial stdio