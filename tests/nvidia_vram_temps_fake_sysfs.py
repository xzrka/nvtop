#!/usr/bin/env python3
"""Stand a device tree with the shape of sysfs, holding BAR0 contents with junction
and memory temperatures of our own, for the NVIDIA GPUs of this machine.

Pointing NVTOP_PCI_SYSFS_ROOT at it lets nvtop show the two fields where they cannot
be read, which is how the layout is tried out without the super user's privileges:

    NVTOP_PCI_SYSFS_ROOT=$(tests/nvidia_vram_temps_fake_sysfs.py) nvtop -s

Nothing here reaches the real device tree; the registers are ordinary files under a
directory of our choosing.
"""

import os
import struct
import sys

# GDDR6 layout, the one the RTX 30 and 40 series boards read their sensors from
JUNCTION_THERM_OFFSET = 0x2046C
VRAM_ADC_OFFSET = 0xE2A8
JUNCTION_SHIFT = 8
VRAM_ADC_PER_DEGREE = 32

# A file covering both registers is enough for the GDDR6 layout; the GDDR7 one maps
# a block far above it and would need a larger image.
IMAGE_SIZE = 0x21000

PCI_DEVICES = "/sys/bus/pci/devices"
NVIDIA_VENDOR = "0x10de"
DISPLAY_CONTROLLER_CLASS = "0x030000"


def contents(junction, vram):
    image = bytearray(IMAGE_SIZE)
    struct.pack_into("<I", image, JUNCTION_THERM_OFFSET, junction << JUNCTION_SHIFT)
    struct.pack_into("<I", image, VRAM_ADC_OFFSET, vram * VRAM_ADC_PER_DEGREE)
    return bytes(image)


def nvidia_display_functions():
    for bdf in sorted(os.listdir(PCI_DEVICES)):
        device = os.path.join(PCI_DEVICES, bdf)
        try:
            with open(os.path.join(device, "vendor")) as vendor:
                if vendor.read().strip() != NVIDIA_VENDOR:
                    continue
            with open(os.path.join(device, "class")) as klass:
                if klass.read().strip() != DISPLAY_CONTROLLER_CLASS:
                    continue
        except OSError:
            continue
        yield bdf, device


def stand_device(fake_root, bdf, device, junction, vram):
    target = os.path.join(fake_root, "bus/pci/devices", bdf)
    os.makedirs(target)

    # The device id and the BAR0 range of the real device, so the same checks and
    # the same register layout apply as they would on the hardware
    with open(os.path.join(device, "device")) as source:
        with open(os.path.join(target, "device"), "w") as out:
            out.write(source.read())

    with open(os.path.join(device, "resource")) as source:
        first_line = source.readline()
    with open(os.path.join(target, "resource"), "w") as out:
        out.write(first_line)

    with open(os.path.join(target, "resource0"), "wb") as out:
        out.write(contents(junction, vram))

    return f"{bdf}: junction {junction}C, vram {vram}C"


def main():
    fake_root = sys.argv[1] if len(sys.argv) > 1 else "/tmp/nvtop-fake-sysfs"
    junction_base = int(os.environ.get("NVTOP_FAKE_JUNCTION_TEMP", 68))
    vram_base = int(os.environ.get("NVTOP_FAKE_VRAM_TEMP", 54))

    if os.path.isdir(fake_root):
        import shutil

        shutil.rmtree(fake_root)
    os.makedirs(os.path.join(fake_root, "bus/pci/devices"))

    reported = []
    for index, (bdf, device) in enumerate(nvidia_display_functions()):
        # A few degrees apart, so the fields of neighbouring devices are not
        # mistaken for one another while looking at the layout
        reported.append(stand_device(fake_root, bdf, device, junction_base + 3 * index, vram_base + 2 * index))

    if not reported:
        print(f"no NVIDIA display controller found under {PCI_DEVICES}", file=sys.stderr)
        return 1

    for line in reported:
        print(line, file=sys.stderr)
    print(fake_root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
