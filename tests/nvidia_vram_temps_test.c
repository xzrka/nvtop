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
 * Stands a device tree with the shape of sysfs, points the BAR0 temperature reader
 * at it and checks what comes back, so the register decoding is covered without a
 * GPU and without the permission to map one. Build and run it with
 * nvidia_vram_temps_test.sh.
 */

#define _GNU_SOURCE

#include "nvtop/nvidia_vram_temps.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

// Big enough to hold every register the reader walks
#define BAR0_SIZE (16u * 1024u * 1024u)
#define BAR0_START 0xf0000000ull

#define GDDR6_JUNCTION_THERM_OFFSET 0x0002046Cu
#define GDDR6_VRAM_ADC_OFFSET 0x0000E2A8u
#define BLACKWELL_THERM_OFFSET 0x00AD0A90u
#define BLACKWELL_THERM_HW_MAX_OFFSET 0x10u
#define GDDR7_DQR_OFFSET 0x009024C0u
#define GDDR7_DQR_VALID_OFFSET 0x10u
#define GDDR7_DQR_STRIDE 0x4000u
#define GDDR7_STRAP_BROADCAST_OFFSET 0x009A0200u

static char fake_root[PATH_MAX];
static unsigned failures;

static void check(const char *what, unsigned expected, unsigned got) {
  if (expected == got) {
    printf("  ok   %-44s %u\n", what, got);
    return;
  }
  printf("  FAIL %-44s expected %u got %u\n", what, expected, got);
  failures++;
}

static void check_status(const char *what, enum nvidia_vram_temps_status expected, enum nvidia_vram_temps_status got) {
  if (expected == got) {
    printf("  ok   %-44s status %d\n", what, (int)got);
    return;
  }
  printf("  FAIL %-44s expected status %d got %d\n", what, (int)expected, (int)got);
  failures++;
}

static void make_directories(const char *path) {
  char partial[PATH_MAX + 64];
  snprintf(partial, sizeof(partial), "%s", path);
  for (char *slash = partial + 1; *slash; ++slash) {
    if (*slash != '/')
      continue;
    *slash = '\0';
    if (mkdir(partial, 0700) && errno != EEXIST) {
      perror(partial);
      exit(EXIT_FAILURE);
    }
    *slash = '/';
  }
  if (mkdir(partial, 0700) && errno != EEXIST) {
    perror(partial);
    exit(EXIT_FAILURE);
  }
}

static void write_file(const char *path, const void *data, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    perror(path);
    exit(EXIT_FAILURE);
  }
  if (write(fd, data, size) != (ssize_t)size) {
    perror(path);
    exit(EXIT_FAILURE);
  }
  close(fd);
}

static void write_le32(uint8_t *image, uint64_t offset, uint32_t value) {
  image[offset + 0] = value & 0xFFu;
  image[offset + 1] = (value >> 8) & 0xFFu;
  image[offset + 2] = (value >> 16) & 0xFFu;
  image[offset + 3] = (value >> 24) & 0xFFu;
}

// Stands a PCI device whose BAR0 holds the given register contents
static void fake_device(const char *bdf, uint16_t device_id, const uint8_t *registers) {
  // Room for the root plus the shortest path it is joined with
  char path[PATH_MAX + 64];
  char content[128];
  snprintf(path, sizeof(path), "%s/bus/pci/devices/%s", fake_root, bdf);
  make_directories(path);

  snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/device", fake_root, bdf);
  snprintf(content, sizeof(content), "0x%04x\n", device_id);
  write_file(path, content, strlen(content));

  snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/resource", fake_root, bdf);
  snprintf(content, sizeof(content), "0x%016llx 0x%016llx 0x%016llx\n", (unsigned long long)BAR0_START,
           (unsigned long long)(BAR0_START + BAR0_SIZE - 1u), 0x0000000000040200ull);
  write_file(path, content, strlen(content));

  snprintf(path, sizeof(path), "%s/bus/pci/devices/%s/resource0", fake_root, bdf);
  uint8_t *image = calloc(BAR0_SIZE, 1);
  if (!image) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }
  memcpy(image, registers, BAR0_SIZE);
  write_file(path, image, BAR0_SIZE);
  free(image);
}

static uint8_t *register_image(void) {
  uint8_t *registers = calloc(BAR0_SIZE, 1);
  if (!registers) {
    perror("calloc");
    exit(EXIT_FAILURE);
  }
  return registers;
}

static void test_gddr6_sensors(void) {
  printf("GDDR6 board (RTX 3090, GA102)\n");
  uint8_t *registers = register_image();

  // The junction sensor holds its value as a byte, the VRAM one as an ADC count
  write_le32(registers, GDDR6_JUNCTION_THERM_OFFSET, 68u << 8);
  write_le32(registers, GDDR6_VRAM_ADC_OFFSET, 54u * 32u);
  fake_device("0000:41:00.0", 0x2204, registers);

  enum nvidia_vram_temps_status status = nvidia_vram_temps_unsupported;
  nvidia_vram_temps *temps = nvidia_vram_temps_open(0, 0x41, 0, 0, 0x2204, true, &status);
  check_status("open", nvidia_vram_temps_ok, status);
  if (!temps) {
    failures++;
    free(registers);
    return;
  }

  check("mapped", 1, nvidia_vram_temps_available(temps));

  unsigned junction = NVTOP_TEMPERATURE_INVALID, vram = NVTOP_TEMPERATURE_INVALID;
  check("read", 1, nvidia_vram_temps_read(temps, &junction, &vram));
  check("junction temperature", 68, junction);
  check("vram temperature", 54, vram);
  nvidia_vram_temps_close(temps);

  // Registers answering with all ones carry no temperature, but stay readable
  write_le32(registers, GDDR6_JUNCTION_THERM_OFFSET, UINT32_MAX);
  write_le32(registers, GDDR6_VRAM_ADC_OFFSET, UINT32_MAX);
  fake_device("0000:41:00.0", 0x2204, registers);
  temps = nvidia_vram_temps_open(0, 0x41, 0, 0, 0x2204, true, &status);
  if (!temps) {
    printf("  FAIL %-44s open with dead sensors\n", "open");
    failures++;
    free(registers);
    return;
  }
  check("read of dead sensors reports nothing", 0, nvidia_vram_temps_read(temps, &junction, &vram));
  check("junction stays unreadable", NVTOP_TEMPERATURE_INVALID, junction);
  check("vram stays unreadable", NVTOP_TEMPERATURE_INVALID, vram);
  nvidia_vram_temps_close(temps);

  free(registers);
}

static void test_blackwell_sensors(void) {
  printf("Blackwell board (RTX 5090)\n");
  uint8_t *registers = register_image();

  // Four die channels in 1/256 °C and the maximum the hardware tracked itself
  write_le32(registers, BLACKWELL_THERM_OFFSET + 0x00, 70u * 256u);
  write_le32(registers, BLACKWELL_THERM_OFFSET + 0x04, 72u * 256u);
  write_le32(registers, BLACKWELL_THERM_OFFSET + 0x08, 71u * 256u);
  write_le32(registers, BLACKWELL_THERM_OFFSET + 0x0C, 0u); // a channel reporting nothing
  write_le32(registers, BLACKWELL_THERM_OFFSET + BLACKWELL_THERM_HW_MAX_OFFSET, 75u * 256u);

  // A strap saying the memory partitions are not wired in clamshell
  write_le32(registers, GDDR7_STRAP_BROADCAST_OFFSET, 0x00000001u);

  // GDDR7 temperature codes, half degrees above the 0°C code: two valid partitions
  // in the first bank, one in the second, and a channel that answers with poison
  write_le32(registers, GDDR7_DQR_OFFSET + GDDR7_DQR_VALID_OFFSET, 0x03000000u);
  write_le32(registers, GDDR7_DQR_OFFSET + 0 * sizeof(uint32_t), (20u + 28u) << 16); // 56°C
  write_le32(registers, GDDR7_DQR_OFFSET + 1 * sizeof(uint32_t), (20u + 30u) << 16); // 60°C
  write_le32(registers, GDDR7_DQR_OFFSET + 2 * sizeof(uint32_t), 0xBADF0000u);
  write_le32(registers, GDDR7_DQR_OFFSET + GDDR7_DQR_STRIDE + GDDR7_DQR_VALID_OFFSET, 0x01000000u);
  write_le32(registers, GDDR7_DQR_OFFSET + GDDR7_DQR_STRIDE, (20u + 26u) << 16); // 52°C
  fake_device("0000:01:00.0", 0x2B85, registers);

  enum nvidia_vram_temps_status status = nvidia_vram_temps_unsupported;
  nvidia_vram_temps *temps = nvidia_vram_temps_open(0, 0x01, 0, 0, 0x2B85, true, &status);
  check_status("open", nvidia_vram_temps_ok, status);
  if (!temps) {
    failures++;
    free(registers);
    return;
  }

  unsigned junction = NVTOP_TEMPERATURE_INVALID, vram = NVTOP_TEMPERATURE_INVALID;
  check("read", 1, nvidia_vram_temps_read(temps, &junction, &vram));
  check("junction is the hottest channel", 75, junction);
  check("vram is the hottest partition", 60, vram);
  nvidia_vram_temps_close(temps);

  free(registers);
}

// The answers that are no temperature, which is what decides whether a field is
// shown at all rather than a number that only looks like one
static void test_answers_that_are_no_temperature(void) {
  printf("Answers that are no temperature\n");
  uint8_t *registers = register_image();

  // An offset nothing drives answers with the pattern of an unimplemented register,
  // and that pattern happens to hold a plausible 80C in the byte the junction is
  // read from: the difference between a board without the sensor and a hot one.
  write_le32(registers, GDDR6_JUNCTION_THERM_OFFSET, 0xBADF5040u);
  write_le32(registers, GDDR6_VRAM_ADC_OFFSET, 0xBADF1002u);
  fake_device("0000:01:00.0", 0x2504, registers);

  enum nvidia_vram_temps_status status = nvidia_vram_temps_unsupported;
  nvidia_vram_temps *temps = nvidia_vram_temps_open(0, 0x01, 0, 0, 0x2504, true, &status);
  check_status("open", nvidia_vram_temps_ok, status);
  if (!temps) {
    failures++;
    free(registers);
    return;
  }

  unsigned junction = NVTOP_TEMPERATURE_INVALID, vram = NVTOP_TEMPERATURE_INVALID;
  check("the pattern of an unimplemented offset", 0, nvidia_vram_temps_read(temps, &junction, &vram));
  check("junction", NVTOP_TEMPERATURE_INVALID, junction);
  check("vram", NVTOP_TEMPERATURE_INVALID, vram);
  nvidia_vram_temps_close(temps);

  // A board with no thermistor wired to the memory converter: the converter counts
  // nothing, forever, next to a junction register that reads like any other. This is
  // how the RTX 3050 of the board this was found on answers.
  write_le32(registers, GDDR6_JUNCTION_THERM_OFFSET, 69u << 8);
  write_le32(registers, GDDR6_VRAM_ADC_OFFSET, 0u);
  fake_device("0000:01:00.0", 0x2504, registers);
  temps = nvidia_vram_temps_open(0, 0x01, 0, 0, 0x2504, true, &status);
  if (!temps) {
    printf("  FAIL %-44s open with a converter counting nothing\n", "open");
    failures++;
    free(registers);
    return;
  }
  check("the junction is still read", 1, nvidia_vram_temps_read(temps, &junction, &vram));
  check("junction temperature", 69, junction);
  check("a converter counting nothing", NVTOP_TEMPERATURE_INVALID, vram);
  nvidia_vram_temps_close(temps);

  free(registers);
}

static void test_uncovered_devices(void) {
  printf("Boards the register layouts do not cover\n");
  uint8_t *registers = register_image();
  write_le32(registers, GDDR6_JUNCTION_THERM_OFFSET, 68u << 8);

  enum nvidia_vram_temps_status status;
  nvidia_vram_temps *temps;

  // An architecture that predates GDDR6 has nothing at those offsets
  fake_device("0000:02:00.0", 0x1C03, registers);
  status = nvidia_vram_temps_ok;
  temps = nvidia_vram_temps_open(0, 0x02, 0, 0, 0x1C03, false, &status);
  check_status("a pre-GDDR6 board is left alone", nvidia_vram_temps_unsupported, status);
  check("nothing is mapped", 0, temps != NULL);
  nvidia_vram_temps_close(temps);

  // A device id NVML does not report is taken from the device tree instead
  fake_device("0000:03:00.0", 0x2204, registers);
  status = nvidia_vram_temps_unsupported;
  temps = nvidia_vram_temps_open(0, 0x03, 0, 0, 0, true, &status);
  check_status("the device id falls back to the device tree", nvidia_vram_temps_ok, status);
  unsigned junction = NVTOP_TEMPERATURE_INVALID, vram = NVTOP_TEMPERATURE_INVALID;
  if (temps) {
    check("read through the sysfs device id", 1, nvidia_vram_temps_read(temps, &junction, &vram));
    check("junction temperature", 68, junction);
    // Nothing is wired to the converter of a board whose register image was made
    // without one, and a converter counting nothing is no temperature.
    check("a converter counting nothing", NVTOP_TEMPERATURE_INVALID, vram);
  } else {
    failures++;
  }
  nvidia_vram_temps_close(temps);

  // A device that is not there at all
  status = nvidia_vram_temps_ok;
  temps = nvidia_vram_temps_open(0, 0x99, 0, 0, 0x2204, true, &status);
  check_status("an absent device", nvidia_vram_temps_no_bar0, status);
  check("nothing is mapped", 0, temps != NULL);
  nvidia_vram_temps_close(temps);

  free(registers);
}

int main(void) {
  snprintf(fake_root, sizeof(fake_root), "/tmp/nvtop-fake-sysfs-XXXXXX");
  if (!mkdtemp(fake_root)) {
    perror(fake_root);
    return EXIT_FAILURE;
  }
  setenv(NVTOP_PCI_SYSFS_ROOT_ENV, fake_root, 1);
  printf("fake device tree: %s\n", fake_root);

  test_gddr6_sensors();
  test_answers_that_are_no_temperature();
  test_blackwell_sensors();
  test_uncovered_devices();

  char command[PATH_MAX + 16];
  snprintf(command, sizeof(command), "rm -rf '%s'", fake_root);
  int removed = system(command);
  (void)removed;

  if (failures) {
    printf("%u check(s) failed\n", failures);
    return EXIT_FAILURE;
  }
  printf("all checks passed\n");
  return EXIT_SUCCESS;
}
