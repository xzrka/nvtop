/*
 *
 * Copyright (C) 2025 Nvtop contributors
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

/*
 * VRAM and junction ("hot spot") temperatures for NVIDIA GeForce GPUs.
 *
 * The core temperature NVML reports says little about how hot the memory is
 * running on GDDR6/GDDR6X/GDDR7 boards, and the vendors do not expose those two
 * sensors through any supported API. They are readable, however, through
 * undocumented BAR0 registers. The register offsets and their decodings are
 * ported from ThomasBaruzier/gddr6-core-junction-vram-temps (gputemps), itself
 * based on olealgoritme/gddr6, jjziets/gddr6_temps, igor'sLAB and
 * sunnyyangyangyang/gddr7-temp.
 *
 * Reading BAR0 requires the permission to map it, that is root on a stock
 * Linux, so every entry point has to keep working when nothing could be mapped.
 */

#ifndef NVTOP_NVIDIA_VRAM_TEMPS_H__
#define NVTOP_NVIDIA_VRAM_TEMPS_H__

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

// A temperature that was not read, so that "no sensor" and 0°C stay distinct.
#define NVTOP_TEMPERATURE_INVALID UINT_MAX

// Testing seam: pointing this environment variable at a directory holding a device
// tree of the shape of /sys lets the register decoding be exercised without a GPU
// and without the permission to map one.
#define NVTOP_PCI_SYSFS_ROOT_ENV "NVTOP_PCI_SYSFS_ROOT"

// Why the sensor windows could not be mapped, so that the caller can tell the
// user what is missing rather than show an empty field.
enum nvidia_vram_temps_status {
  nvidia_vram_temps_ok = 0,      // at least one sensor window was mapped
  nvidia_vram_temps_unsupported, // no known register layout for this device
  nvidia_vram_temps_no_access,   // registers are known but may not be read (needs root)
  nvidia_vram_temps_no_bar0,     // the PCI device exposes no usable BAR0
};

typedef struct nvidia_vram_temps nvidia_vram_temps;

/*
 * Maps the BAR0 windows holding the VRAM and junction sensors of the PCI device
 * at domain:bus:device.function. `pci_device_id` is the device id NVML reports,
 * used to select the register layout; when it is unknown the sysfs entry of the
 * device is asked instead. `gddr6_capable` says whether the architecture is new
 * enough for the GDDR6 registers to mean anything, which the caller knows from
 * the reported architecture.
 *
 * Returns NULL and, when `status` is not NULL, the reason nothing was mapped.
 */
nvidia_vram_temps *nvidia_vram_temps_open(unsigned int domain, unsigned int bus, unsigned int device,
                                          unsigned int function, uint16_t pci_device_id, bool gddr6_capable,
                                          enum nvidia_vram_temps_status *status);

// True when at least one sensor window was mapped.
bool nvidia_vram_temps_available(const nvidia_vram_temps *temps);

// Reads both sensors. An output the device does not report is set to
// NVTOP_TEMPERATURE_INVALID. True if at least one temperature was read.
bool nvidia_vram_temps_read(const nvidia_vram_temps *temps, unsigned int *junction_temp, unsigned int *vram_temp);

void nvidia_vram_temps_close(nvidia_vram_temps *temps);

// Releases every still open instance. A GPU backend has no per device teardown to
// call nvidia_vram_temps_close from, so it calls this once when it shuts down.
void nvidia_vram_temps_close_all(void);

#endif // NVTOP_NVIDIA_VRAM_TEMPS_H__
