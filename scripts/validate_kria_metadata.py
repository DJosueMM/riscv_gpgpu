#!/usr/bin/env python3
"""Validate the HWH contract consumed by the first-kernel Kria runner."""

import argparse
import sys
import xml.etree.ElementTree as ET
from pathlib import Path


EXPECTED_BASES = {
    ("gpgpu_scheduler_0", "C_S_AXI_CONTROL_BASEADDR"): 0xA0000000,
    ("gpgpu_scheduler_0", "C_S_AXI_CONTROL_R_BASEADDR"): 0xA0010000,
    ("memory_pipeline_0", "C_S_AXI_CONTROL_BASEADDR"): 0xA0020000,
}

EXPECTED_REGISTERS = {
    ("gpgpu_scheduler_0", "s_axi_control"): {
        "program_len": 0x10,
        "total_warps": 0x18,
        "warp_id_offset": 0x20,
        "start_r": 0x28,
        "busy": 0x30,
        "done": 0x40,
        "fault": 0x50,
    },
    ("gpgpu_scheduler_0", "s_axi_control_r"): {
        "program_ptr_1": 0x10,
        "program_ptr_2": 0x14,
        "initial_regs_ptr0_1": 0x1C,
        "initial_regs_ptr0_2": 0x20,
        "initial_regs_ptr1_1": 0x28,
        "initial_regs_ptr1_2": 0x2C,
    },
    ("memory_pipeline_0", "s_axi_control"): {
        "CTRL": 0x00,
        "global_mem_1": 0x10,
        "global_mem_2": 0x14,
    },
}

MEMORY_REGIONS = {
    "ddr": (0x60000000, 64 * 1024 * 1024),
    "ocm": (0xFFFC0000, 128 * 1024),
}


def parse_number(value: str) -> int:
    return int(value, 0)


def property_value(element: ET.Element, name: str) -> str | None:
    prop = element.find(f"./PROPERTY[@NAME='{name}']")
    return None if prop is None else prop.get("VALUE")


def module_by_instance(root: ET.Element, instance: str) -> ET.Element | None:
    return root.find(f".//MODULE[@INSTANCE='{instance}']")


def validate_metadata(hwh: Path, memory: str) -> list[str]:
    try:
        root = ET.parse(hwh).getroot()
    except (ET.ParseError, OSError) as error:
        return [f"cannot parse {hwh}: {error}"]

    errors: list[str] = []
    modules = {
        instance: module_by_instance(root, instance)
        for instance in ("gpgpu_scheduler_0", "memory_pipeline_0")
    }
    for instance, module in modules.items():
        if module is None:
            errors.append(f"missing module {instance}")

    if errors:
        return errors

    for (instance, parameter_name), expected in EXPECTED_BASES.items():
        parameter = modules[instance].find(
            f"./PARAMETERS/PARAMETER[@NAME='{parameter_name}']"
        )
        actual = None if parameter is None else parameter.get("VALUE")
        if actual is None or parse_number(actual) != expected:
            errors.append(
                f"{instance}.{parameter_name}: expected 0x{expected:08x}, got {actual}"
            )

    for (instance, interface), expected_registers in EXPECTED_REGISTERS.items():
        block = modules[instance].find(
            f"./ADDRESSBLOCKS/ADDRESSBLOCK[@INTERFACE='{interface}']"
        )
        if block is None:
            errors.append(f"{instance}: missing register interface {interface}")
            continue
        registers = {
            register.get("NAME"): register
            for register in block.findall("./REGISTERS/REGISTER")
        }
        for register_name, expected in expected_registers.items():
            register = registers.get(register_name)
            actual = None if register is None else property_value(register, "ADDRESS_OFFSET")
            width = None if register is None else property_value(register, "SIZE")
            if actual is None or parse_number(actual) != expected:
                errors.append(
                    f"{instance}.{interface}.{register_name}: "
                    f"expected 0x{expected:02x}, got {actual}"
                )
            if width is None or parse_number(width) != 32:
                errors.append(
                    f"{instance}.{interface}.{register_name}: "
                    f"expected width 32, got {width}"
                )

    region_base, region_size = MEMORY_REGIONS[memory]
    region_high = region_base + region_size - 1
    expected_masters = {
        "gpgpu_scheduler_0": ("m_axi_gmem0", "m_axi_gmem1", "m_axi_gmem2"),
        "memory_pipeline_0": ("m_axi_gmem",),
    }
    for instance, masters in expected_masters.items():
        ranges = modules[instance].findall("./MEMORYMAP/MEMRANGE")
        for master in masters:
            covers_region = any(
                entry.get("MASTERBUSINTERFACE") == master
                and parse_number(entry.get("BASEVALUE", "-1")) <= region_base
                and parse_number(entry.get("HIGHVALUE", "-1")) >= region_high
                for entry in ranges
            )
            if not covers_region:
                errors.append(
                    f"{instance}.{master}: HWH does not map {memory} "
                    f"[0x{region_base:08x},0x{region_high:08x}]"
                )

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("hwh", type=Path)
    parser.add_argument("--memory", choices=sorted(MEMORY_REGIONS), required=True)
    args = parser.parse_args()

    errors = validate_metadata(args.hwh, args.memory)
    if errors:
        for error in errors:
            print(f"ERROR: {error}", file=sys.stderr)
        return 1

    print(f"PASS: KRIA_HWH_CONTRACT memory={args.memory} hwh={args.hwh}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())