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

#define _FILE_OFFSET_BITS 64

#include "nvtop/nvidia_vram_temps.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>

// Register layouts, ported from ThomasBaruzier/gddr6-core-junction-vram-temps.
#define GDDR6_JUNCTION_THERM_OFFSET 0x0002046Cu
#define GDDR6_JUNCTION_SHIFT 8u
#define GDDR6_JUNCTION_MASK 0xFFu
#define GDDR6_VRAM_ADC_OFFSET 0x0000E2A8u
#define GDDR6_VRAM_ADC_MASK 0xFFFu
#define GDDR6_VRAM_ADC_DIVISOR 32u
// Both GDDR6 sensors report their own "no reading" value from 0x7F on
#define GDDR6_TEMP_LIMIT_C 0x7Fu

#define BLACKWELL_THERM_OFFSET 0x00AD0A90u
#define BLACKWELL_THERM_CHANNEL_COUNT 4u
#define BLACKWELL_THERM_STRIDE 0x4u
#define BLACKWELL_THERM_HW_MAX_OFFSET 0x10u
#define BLACKWELL_THERM_VALUE_MASK 0xFFFFu
#define BLACKWELL_THERM_SCALE 256u
#define BLACKWELL_THERM_MAX_C 150u

#define GDDR7_DQR_OFFSET 0x009024C0u
#define GDDR7_DQR_VALID_OFFSET 0x10u
#define GDDR7_DQR_STRIDE 0x4000u
#define GDDR7_DQR_SUBREGISTER_COUNT 4u
#define GDDR7_DQR_VALID_SHIFT 24u
#define GDDR7_MAX_FBPA_COUNT 16u
#define GDDR7_CLAMSHELL_FBPA_COUNT 8u
#define GDDR7_DQR_SPAN ((GDDR7_MAX_FBPA_COUNT - 1u) * GDDR7_DQR_STRIDE + GDDR7_DQR_VALID_OFFSET + sizeof(uint32_t))

#define GDDR7_STRAP_BROADCAST_OFFSET 0x009A0200u
#define GDDR7_STRAP_X16_BIT 22u

#define GDDR7_TEMP_CODE_SHIFT 16u
#define GDDR7_TEMP_CODE_MASK 0xFFu
#define GDDR7_ZERO_C_CODE 20u
#define GDDR7_CODE_MIN 21u
#define GDDR7_CODE_MAX 80u
#define GDDR7_DEGREES_PER_CODE 2u

// A register nothing drives answers with this pattern, whichever family the board
// is of: it is what the GPU returns for an offset it does not implement, and it
// decodes into a plausible looking temperature unless it is turned down first
#define REGISTER_POISON_MASK 0xFFFF0000u
#define REGISTER_POISON_VALUE 0xBADF0000u

// Blackwell controllers, which read their sensors from another set of registers
#define PCI_DEVICE_ID_RTX_5090 0x2B85u
#define PCI_DEVICE_ID_RTX_5080 0x2B82u
#define PCI_DEVICE_ID_RTX_5070_TI 0x2C05u
#define PCI_DEVICE_ID_RTX_5070 0x2C02u
#define PCI_DEVICE_ID_RTX_5060_TI 0x2D04u
#define PCI_DEVICE_ID_RTX_PRO_4000_SFF 0x2C33u

#define PCI_BAR_IO_FLAG 0x1u

#define PCI_SYSFS_ROOT_DEFAULT "/sys"

typedef enum {
  junction_method_therm_byte, // GDDR6: a byte of a thermal register
  junction_method_blackwell,  // Blackwell: four die channels and a hardware maximum
} junction_method;

typedef enum {
  vram_method_gddr6_adc, // GDDR6/GDDR6X: an ADC count
  vram_method_gddr7_dqr, // GDDR7: a temperature code per memory partition
} vram_method;

typedef enum { gddr7_topology_unknown, gddr7_topology_standard, gddr7_topology_clamshell } gddr7_topology;

typedef struct {
  junction_method junction;
  vram_method vram;
} sensor_profile;

struct nvidia_vram_temps {
  // Every open instance, so that a backend shutting down can release them all
  struct nvidia_vram_temps *next_open;
  sensor_profile profile;
  gddr7_topology topology;
  // One mapping per sensor window, both of them relative to BAR0
  void *junction_mapping;
  size_t junction_mapping_size;
  volatile uint8_t *junction_regs;
  void *vram_mapping;
  size_t vram_mapping_size;
  volatile uint8_t *vram_regs;
};

// Blackwell uses the GDDR7 registers for both of its sensors
static const sensor_profile gddr6_profile = {
    .junction = junction_method_therm_byte,
    .vram = vram_method_gddr6_adc,
};

static const sensor_profile blackwell_profile = {
    .junction = junction_method_blackwell,
    .vram = vram_method_gddr7_dqr,
};

static bool is_blackwell_device(uint16_t pci_device_id) {
  switch (pci_device_id) {
  case PCI_DEVICE_ID_RTX_5090:
  case PCI_DEVICE_ID_RTX_5080:
  case PCI_DEVICE_ID_RTX_5070_TI:
  case PCI_DEVICE_ID_RTX_5070:
  case PCI_DEVICE_ID_RTX_5060_TI:
  case PCI_DEVICE_ID_RTX_PRO_4000_SFF:
    return true;
  default:
    return false;
  }
}

// The GDDR6 registers mean nothing on the architectures that predate GDDR6, where
// reading them only returns values that look plausible.
static bool layout_is_known(uint16_t pci_device_id, bool gddr6_capable) {
  if (!pci_device_id)
    return false;
  return is_blackwell_device(pci_device_id) || gddr6_capable;
}

// Where BAR0 sits and how large it is, together with the file descriptors it may
// be mapped through. A kernel that will not expose the BAR as a file, or whose
// memory restriction refuses the mapping, is served by /dev/mem instead.
struct bar0_location {
  uint64_t start;
  uint64_t size;
  int resource_fd; // sysfs resource0, offsets are relative to the BAR
  int devmem_fd;   // /dev/mem, offsets have to be shifted by the BAR start
};

static void bar0_close(struct bar0_location *bar) {
  if (bar->resource_fd >= 0)
    close(bar->resource_fd);
  if (bar->devmem_fd >= 0)
    close(bar->devmem_fd);
  bar->resource_fd = -1;
  bar->devmem_fd = -1;
}

// Parses the first line of the sysfs "resource" file: the start, end and flags of
// BAR0 in hexadecimal. An unassigned BAR is reported with no size.
static bool parse_bar0_resource_file(const char *path, uint64_t *start, uint64_t *size) {
  FILE *file = fopen(path, "r");
  if (!file)
    return false;

  unsigned long long bar_start = 0, bar_end = 0, flags = 0;
  int matched = fscanf(file, "%llx %llx %llx", &bar_start, &bar_end, &flags);
  fclose(file);

  if (matched != 3)
    return false;

  if (flags & PCI_BAR_IO_FLAG)
    return false;

  if (bar_end <= bar_start)
    return false;

  *start = bar_start;
  *size = bar_end - bar_start + 1u;
  return true;
}

static bool bar0_locate(const char *sysfs_root, const char *bdf, struct bar0_location *bar) {
  char path[PATH_MAX];
  uint64_t start = 0, size = 0;

  bar->resource_fd = -1;
  bar->devmem_fd = -1;

  snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/resource", sysfs_root, bdf);
  if (!parse_bar0_resource_file(path, &start, &size))
    return false;

  bar->start = start;
  bar->size = size;

  snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/resource0", sysfs_root, bdf);
  bar->resource_fd = open(path, O_RDONLY | O_SYNC);

  if (bar->resource_fd < 0)
    bar->devmem_fd = open("/dev/mem", O_RDONLY | O_SYNC);

  return bar->resource_fd >= 0 || bar->devmem_fd >= 0;
}

// Maps [offset, offset + span) of BAR0 through one of the available files. The
// offset has to be page aligned, so the mapping starts on the page below and the
// registers are found by walking into it.
static bool bar0_map_one(const struct bar0_location *bar, int fd, bool physical, long page_size, uint64_t offset,
                         size_t span, volatile uint8_t **registers, void **mapping, size_t *mapping_size) {
  if (fd < 0)
    return false;

  if (offset >= bar->size || span > bar->size - offset)
    return false;

  uint64_t address = physical ? offset + bar->start : offset;
  uint64_t page = (uint64_t)page_size;
  uint64_t within_page = address % page;
  uint64_t aligned = address - within_page;

  if (aligned > (uint64_t)INT64_MAX)
    return false;

  size_t length = (size_t)within_page + span;
  void *mapped = mmap(NULL, length, PROT_READ, MAP_SHARED, fd, (off_t)aligned);
  if (mapped == MAP_FAILED)
    return false;

  *mapping = mapped;
  *mapping_size = length;
  *registers = (volatile uint8_t *)mapped + within_page;
  return true;
}

// Tries the sysfs file first: it is not subject to the kernel restriction on
// /dev/mem. A kernel that refuses it, or exposes no file at all, is served by
// /dev/mem, which is opened on the way.
static bool bar0_map(struct bar0_location *bar, long page_size, uint64_t offset, size_t span,
                     volatile uint8_t **registers, void **mapping, size_t *mapping_size) {
  *registers = NULL;
  *mapping = NULL;
  *mapping_size = 0;

  if (bar0_map_one(bar, bar->resource_fd, false, page_size, offset, span, registers, mapping, mapping_size))
    return true;

  if (bar->devmem_fd < 0)
    bar->devmem_fd = open("/dev/mem", O_RDONLY | O_SYNC);

  return bar0_map_one(bar, bar->devmem_fd, true, page_size, offset, span, registers, mapping, mapping_size);
}

static uint32_t read32(const volatile uint8_t *registers, size_t offset) {
  return *(const volatile uint32_t *)(registers + offset);
}

static bool read_file_u16(const char *path, uint16_t *value) {
  FILE *file = fopen(path, "r");
  if (!file)
    return false;

  unsigned long read_value = 0;
  int matched = fscanf(file, "%lx", &read_value);
  fclose(file);

  if (matched != 1)
    return false;

  *value = (uint16_t)read_value;
  return true;
}

static nvidia_vram_temps *open_instances;

static void track_open_instance(nvidia_vram_temps *temps) {
  temps->next_open = open_instances;
  open_instances = temps;
}

static void untrack_open_instance(nvidia_vram_temps *temps) {
  nvidia_vram_temps **instance = &open_instances;
  while (*instance && *instance != temps)
    instance = &(*instance)->next_open;
  if (*instance)
    *instance = temps->next_open;
  temps->next_open = NULL;
}

static unsigned int hottest(unsigned int first, unsigned int second) {
  if (second == NVTOP_TEMPERATURE_INVALID)
    return first;
  if (first == NVTOP_TEMPERATURE_INVALID)
    return second;
  return first > second ? first : second;
}

// A register nothing drives, which is what reading an unimplemented offset of the
// memory controller gives back
static bool is_poisoned(uint32_t raw) { return (raw & REGISTER_POISON_MASK) == REGISTER_POISON_VALUE; }

static bool decode_gddr6_junction(uint32_t raw, unsigned int *celsius) {
  if (raw == UINT32_MAX || is_poisoned(raw))
    return false;

  unsigned int value = (raw >> GDDR6_JUNCTION_SHIFT) & GDDR6_JUNCTION_MASK;
  if (value >= GDDR6_TEMP_LIMIT_C)
    return false;

  *celsius = value;
  return true;
}

static bool decode_gddr6_vram(uint32_t raw, unsigned int *celsius) {
  if (raw == UINT32_MAX || is_poisoned(raw))
    return false;

  unsigned int counts = raw & GDDR6_VRAM_ADC_MASK;
  // A converter reading nothing. It is not the cold of a machine that has just
  // been switched on either: the memory of a board whose die reports 69C cannot sit
  // at the freezing point of water, and the boards that wire no thermistor to this
  // converter answer 0 forever, beside a junction register that reads perfectly.
  if (!counts)
    return false;

  unsigned int value = counts / GDDR6_VRAM_ADC_DIVISOR;
  if (value >= GDDR6_TEMP_LIMIT_C)
    return false;

  *celsius = value;
  return true;
}

// Blackwell reports its temperatures as a fixed point value in 1/256 °C
static bool decode_blackwell_temperature(uint32_t raw, unsigned int *celsius) {
  if (raw == UINT32_MAX || is_poisoned(raw))
    return false;

  uint32_t fixed = raw & BLACKWELL_THERM_VALUE_MASK;
  if (!fixed || fixed > BLACKWELL_THERM_MAX_C * BLACKWELL_THERM_SCALE)
    return false;

  *celsius = (fixed + BLACKWELL_THERM_SCALE / 2u) / BLACKWELL_THERM_SCALE;
  return true;
}

static bool decode_gddr7_vram(uint32_t raw, unsigned int *celsius) {
  if (raw == UINT32_MAX || is_poisoned(raw))
    return false;

  unsigned int code = (raw >> GDDR7_TEMP_CODE_SHIFT) & GDDR7_TEMP_CODE_MASK;
  if (code < GDDR7_CODE_MIN || code > GDDR7_CODE_MAX)
    return false;

  // The code counts half degrees above its 0°C value
  *celsius = (code - GDDR7_ZERO_C_CODE) * GDDR7_DEGREES_PER_CODE;
  return true;
}

// How many memory partitions the board wires up decides how far the GDDR7
// temperature registers may be walked
static gddr7_topology detect_gddr7_topology(struct bar0_location *bar, long page_size) {
  void *mapping = NULL;
  size_t mapping_size = 0;
  volatile uint8_t *registers = NULL;

  if (!bar0_map(bar, page_size, GDDR7_STRAP_BROADCAST_OFFSET, sizeof(uint32_t), &registers, &mapping, &mapping_size))
    return gddr7_topology_unknown;

  uint32_t strap = read32(registers, 0);
  munmap(mapping, mapping_size);

  if (!strap || strap == UINT32_MAX || is_poisoned(strap))
    return gddr7_topology_unknown;

  return (strap & (1u << GDDR7_STRAP_X16_BIT)) ? gddr7_topology_clamshell : gddr7_topology_standard;
}

nvidia_vram_temps *nvidia_vram_temps_open(unsigned int domain, unsigned int bus, unsigned int device,
                                          unsigned int function, uint16_t pci_device_id, bool gddr6_capable,
                                          enum nvidia_vram_temps_status *status) {
  const char *sysfs_root = getenv(NVTOP_PCI_SYSFS_ROOT_ENV);
  if (!sysfs_root || !*sysfs_root)
    sysfs_root = PCI_SYSFS_ROOT_DEFAULT;

  char bdf[32];
  snprintf(bdf, sizeof(bdf), "%04x:%02x:%02x.%u", domain, bus, device, function);

  // NVML does not report the device id on every driver, sysfs always does
  if (!pci_device_id) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/device", sysfs_root, bdf);
    if (!read_file_u16(path, &pci_device_id))
      pci_device_id = 0;
  }

  if (!layout_is_known(pci_device_id, gddr6_capable)) {
    if (status)
      *status = nvidia_vram_temps_unsupported;
    return NULL;
  }

  struct bar0_location bar;
  if (!bar0_locate(sysfs_root, bdf, &bar)) {
    // The usual reason on a machine that is not the super user's: the BAR is there,
    // it simply may not be opened
    if (status)
      *status = (errno == EACCES || errno == EPERM) ? nvidia_vram_temps_no_access : nvidia_vram_temps_no_bar0;
    return NULL;
  }

  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    page_size = 4096;

  struct nvidia_vram_temps *temps = calloc(1, sizeof(*temps));
  if (!temps) {
    bar0_close(&bar);
    if (status)
      *status = nvidia_vram_temps_no_bar0;
    return NULL;
  }

  temps->profile = is_blackwell_device(pci_device_id) ? blackwell_profile : gddr6_profile;

  uint64_t junction_offset;
  size_t junction_span;
  uint64_t vram_offset;
  size_t vram_span;

  if (temps->profile.junction == junction_method_blackwell) {
    junction_offset = BLACKWELL_THERM_OFFSET;
    junction_span = BLACKWELL_THERM_HW_MAX_OFFSET + sizeof(uint32_t);
  } else {
    junction_offset = GDDR6_JUNCTION_THERM_OFFSET;
    junction_span = sizeof(uint32_t);
  }

  if (temps->profile.vram == vram_method_gddr7_dqr) {
    vram_offset = GDDR7_DQR_OFFSET;
    vram_span = GDDR7_DQR_SPAN;
  } else {
    vram_offset = GDDR6_VRAM_ADC_OFFSET;
    vram_span = sizeof(uint32_t);
  }

  bar0_map(&bar, page_size, junction_offset, junction_span, &temps->junction_regs, &temps->junction_mapping,
           &temps->junction_mapping_size);
  bar0_map(&bar, page_size, vram_offset, vram_span, &temps->vram_regs, &temps->vram_mapping, &temps->vram_mapping_size);
  // Why the mapping failed has to be read here: everything below is allowed to
  // overwrite errno on its way to reporting nothing
  int mapping_errno = errno;

  if (temps->profile.vram == vram_method_gddr7_dqr)
    temps->topology = detect_gddr7_topology(&bar, page_size);

  bool mapped = temps->junction_regs || temps->vram_regs;
  bar0_close(&bar);

  if (!mapped) {
    nvidia_vram_temps_close(temps);
    if (status)
      *status =
          (mapping_errno == EACCES || mapping_errno == EPERM) ? nvidia_vram_temps_no_access : nvidia_vram_temps_no_bar0;
    return NULL;
  }

  if (status)
    *status = nvidia_vram_temps_ok;
  track_open_instance(temps);
  return temps;
}

bool nvidia_vram_temps_available(const nvidia_vram_temps *temps) {
  return temps && (temps->junction_regs || temps->vram_regs);
}

bool nvidia_vram_temps_read(const nvidia_vram_temps *temps, unsigned int *junction_temp, unsigned int *vram_temp) {
  if (!temps)
    return false;

  unsigned int junction = NVTOP_TEMPERATURE_INVALID;
  unsigned int vram = NVTOP_TEMPERATURE_INVALID;

  if (temps->junction_regs) {
    if (temps->profile.junction == junction_method_blackwell) {
      // The junction is the hottest of the die channels and of the maximum the
      // hardware tracked on its own
      for (unsigned int channel = 0; channel < BLACKWELL_THERM_CHANNEL_COUNT; ++channel) {
        unsigned int value;
        if (decode_blackwell_temperature(read32(temps->junction_regs, channel * BLACKWELL_THERM_STRIDE), &value))
          junction = hottest(junction, value);
      }
      unsigned int value;
      if (decode_blackwell_temperature(read32(temps->junction_regs, BLACKWELL_THERM_HW_MAX_OFFSET), &value))
        junction = hottest(junction, value);
    } else {
      unsigned int value;
      if (decode_gddr6_junction(read32(temps->junction_regs, 0), &value))
        junction = value;
    }
  }

  if (temps->vram_regs) {
    if (temps->profile.vram == vram_method_gddr7_dqr) {
      // One code per memory partition, and the memory runs as hot as its hottest
      unsigned int fbpa_count =
          temps->topology == gddr7_topology_clamshell ? GDDR7_CLAMSHELL_FBPA_COUNT : GDDR7_MAX_FBPA_COUNT;
      for (unsigned int fbpa = 0; fbpa < fbpa_count; ++fbpa) {
        size_t fbpa_offset = (size_t)fbpa * GDDR7_DQR_STRIDE;
        uint32_t valid = read32(temps->vram_regs, fbpa_offset + GDDR7_DQR_VALID_OFFSET);
        if (valid == UINT32_MAX || is_poisoned(valid))
          continue;

        for (unsigned int sub = 0; sub < GDDR7_DQR_SUBREGISTER_COUNT; ++sub) {
          if (!(valid & (1u << (GDDR7_DQR_VALID_SHIFT + sub))))
            continue;
          unsigned int value;
          if (decode_gddr7_vram(read32(temps->vram_regs, fbpa_offset + sub * sizeof(uint32_t)), &value))
            vram = hottest(vram, value);
        }
      }
    } else {
      unsigned int value;
      if (decode_gddr6_vram(read32(temps->vram_regs, 0), &value))
        vram = value;
    }
  }

  if (junction_temp)
    *junction_temp = junction;
  if (vram_temp)
    *vram_temp = vram;

  return junction != NVTOP_TEMPERATURE_INVALID || vram != NVTOP_TEMPERATURE_INVALID;
}

void nvidia_vram_temps_close(nvidia_vram_temps *temps) {
  if (!temps)
    return;

  untrack_open_instance(temps);

  if (temps->junction_mapping)
    munmap(temps->junction_mapping, temps->junction_mapping_size);
  if (temps->vram_mapping)
    munmap(temps->vram_mapping, temps->vram_mapping_size);

  free(temps);
}

void nvidia_vram_temps_close_all(void) {
  while (open_instances)
    nvidia_vram_temps_close(open_instances);
}
