#!/bin/zsh

cp roms/edk2/Build/MetaXPkg/DEBUG_GCC/X64/MetaXGopDxe.rom esp
cp roms/edk2/Build/MetaXPkg/DEBUG_GCC/X64/*.efi esp
  
build/qemu-system-x86_64 \
  -machine q35 \
  -m 2G \
  -vga none \
  -device metaxgpu,romfile=esp/MetaXGopDxe.rom \
  -bios esp/OVMF.fd \
  -drive format=raw,file=fat:rw:./esp \
  -serial file:debug.log \
  -debugcon file:ovmf-debugcon.log \
  -global isa-debugcon.iobase=0x402 \
  -S -s