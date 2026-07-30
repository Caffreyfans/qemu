#!/bin/zsh

set -e

ROOT_DIR=${0:A:h}
EDK2_DIR="${ROOT_DIR}/roms/edk2"
BUILD_DIR="${EDK2_DIR}/Build/MetaXPkg/DEBUG_GCC/X64"
GOP_EFI="${BUILD_DIR}/MetaXGopDxe.efi"
GOP_ROM="${BUILD_DIR}/MetaXGopDxe.rom"
ESP_DIR="${ROOT_DIR}/esp"
SERIAL_LOG="${ROOT_DIR}/debug.log"
OVMF_DEBUGCON_LOG="${ROOT_DIR}/ovmf-debugcon.log"
QEMU_LOG="${ROOT_DIR}/qemu.log"
QEMU_STDERR_LOG="${ROOT_DIR}/qemu-stderr.log"

mkdir -p "${ESP_DIR}"

pushd "${EDK2_DIR}" >/dev/null
SCRIPT_ARGS=("$@")
set --
source edksetup.sh >/tmp/metax-edksetup.log
set -- "${SCRIPT_ARGS[@]}"
build -p MetaXPkg/MetaXPkg.dsc -a X64 -b DEBUG -t GCC
"${EDK2_DIR}/BaseTools/Source/C/bin/EfiRom" \
  -f 0x9999 \
  -i 0x0001 \
  -e "${GOP_EFI}" \
  -o "${GOP_ROM}" \
  -q
popd >/dev/null

cp "${GOP_ROM}" "${ESP_DIR}/"
cp "${BUILD_DIR}"/*.efi "${ESP_DIR}/"

if [[ "${1:-}" == "--build-only" ]]; then
  exit 0
fi

rm -f "${SERIAL_LOG}" "${OVMF_DEBUGCON_LOG}" "${QEMU_LOG}" "${QEMU_STDERR_LOG}"
  
exec "${ROOT_DIR}/build/qemu-system-x86_64" \
  -machine q35 \
  -m 2G \
  -D "${QEMU_LOG}" \
  -d guest_errors,unimp \
  -vga none \
  -device metaxgpu,romfile="${ESP_DIR}/MetaXGopDxe.rom" \
  -bios "${ESP_DIR}/OVMF.fd" \
  -drive format=raw,file=fat:rw:"${ESP_DIR}" \
  -serial file:"${SERIAL_LOG}" \
  -debugcon file:"${OVMF_DEBUGCON_LOG}" \
  -global isa-debugcon.iobase=0x402 \
  2> "${QEMU_STDERR_LOG}"
