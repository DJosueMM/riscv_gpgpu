#!/usr/bin/env bash
# setup_kria_fpga_mem.sh - ONE-TIME board setup: reserve 64 MiB at 0x60000000
# for the RISC-V GPGPU FPGA demo (makes the region accessible via /dev/mem).
#
# Run this ONCE on the Kria board then reboot:
#   scp scripts/setup_kria_fpga_mem.sh ubuntu@kria:~
#   ssh -t ubuntu@kria 'bash ~/setup_kria_fpga_mem.sh'
#   ssh -t ubuntu@kria 'sudo reboot'

set -euo pipefail

# Use interactive sudo by default. SUDO_PASS remains available for controlled
# automation, but must not be placed on a command line or committed to Git.
_sudo() {
    if [[ "$(id -u)" -eq 0 ]]; then
        "$@"
    elif [[ -n "${SUDO_PASS:-}" ]]; then
        printf '%s\n' "$SUDO_PASS" | sudo -S "$@"
    else
        sudo "$@"
    fi
}

MEMMAP_ARG='memmap=64M$0x60000000'
RESERVED_NODE='/reserved-memory/riscv_gpgpu@60000000'
USER_DTB='/boot/firmware/user-override.dtb'

configure_flash_kernel_dtb() {
    for tool in dtc fdtget fdtput flash-kernel; do
        command -v "$tool" >/dev/null 2>&1 || {
            echo "ERROR: required tool not found: $tool" >&2
            return 1
        }
    done
    [[ -e /sys/firmware/fdt ]] || {
        echo "ERROR: active DTB not found at /sys/firmware/fdt" >&2
        return 1
    }
    [[ -d /boot/firmware ]] || {
        echo "ERROR: firmware partition is not mounted at /boot/firmware" >&2
        return 1
    }

    local staged_dtb
    staged_dtb="$(mktemp /tmp/riscv-gpgpu-reserved.XXXXXX.dtb)"
    trap 'rm -f "$staged_dtb"' RETURN
    _sudo cat /sys/firmware/fdt >"$staged_dtb"

    fdtput -p -c "$staged_dtb" "$RESERVED_NODE"
    fdtput -t x "$staged_dtb" "$RESERVED_NODE" reg 0 60000000 0 4000000
    fdtput "$staged_dtb" "$RESERVED_NODE" no-map
    dtc -q -I dtb -O dtb -o "${staged_dtb}.validated" "$staged_dtb"
    mv "${staged_dtb}.validated" "$staged_dtb"

    [[ "$(fdtget -t x "$staged_dtb" "$RESERVED_NODE" reg)" == \
        "0 60000000 0 4000000" ]] || {
        echo "ERROR: generated reserved-memory range is invalid" >&2
        return 1
    }
    fdtget "$staged_dtb" "$RESERVED_NODE" no-map >/dev/null || {
        echo "ERROR: generated reserved-memory node lacks no-map" >&2
        return 1
    }

    local config_backup='/etc/default/flash-kernel.riscv-gpgpu.bak'
    if [[ ! -f "$config_backup" ]]; then
        _sudo cp -a /etc/default/flash-kernel "$config_backup"
    fi

    local current_cmdline
    current_cmdline="$(grep '^LINUX_KERNEL_CMDLINE=' /etc/default/flash-kernel || true)"
    if [[ "$current_cmdline" == "LINUX_KERNEL_CMDLINE='${MEMMAP_ARG}'" ||
          "$current_cmdline" == "LINUX_KERNEL_CMDLINE=\"${MEMMAP_ARG}\"" ]]; then
        _sudo sed -i 's|^LINUX_KERNEL_CMDLINE=.*|LINUX_KERNEL_CMDLINE=""|' \
            /etc/default/flash-kernel
        _sudo flash-kernel
    fi

    if [[ -f "$USER_DTB" && ! -f "${USER_DTB}.riscv-gpgpu.bak" ]]; then
        _sudo cp -a "$USER_DTB" "${USER_DTB}.riscv-gpgpu.bak"
    fi
    _sudo install -m 0644 "$staged_dtb" "$USER_DTB"

    echo "[DONE] Installed reserved-memory DTB override. Reboot required."
    echo "[DTB] ${USER_DTB}"
    sha256sum "$staged_dtb"
}

if [[ -d /sys/firmware/devicetree/base/reserved-memory/riscv_gpgpu@60000000 ]]; then
    echo "[OK] Device tree reserves 0x60000000+64MiB for the GPGPU"
    exit 0
fi

echo "Reserving 64 MiB at 0x60000000 for FPGA RISC-V kernel memory..."

if [[ -f /etc/default/flash-kernel && -f /sys/firmware/fdt ]]; then
    configure_flash_kernel_dtb
else
    echo "ERROR: this image does not expose the supported flash-kernel DTB override path" >&2
    echo "Add an equivalent no-map reserved-memory node before using /dev/mem." >&2
    exit 1
fi

echo ""
echo "After reboot, verify with:"
echo "  test -d /sys/firmware/devicetree/base/reserved-memory/riscv_gpgpu@60000000"
echo "  sudo grep -i '^60000000-63ffffff : reserved$' /proc/iomem"
