// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>

typedef struct {
  char *device;           // e.g. /dev/vda
  char *type;             // e.g. disk, rom, ...
  char *bus;              // e.g. usb, sata, virtio, nvme, ...
  char *model;
  uint64_t size;          // size in bytes
  double size_gb;         // size in GB
  bool is_default_device; // device UEFI will try to boot from first
  bool is_boot_device;    // device insaller got loaded from
  int weight;
} device_t;

extern device_t *device_free(device_t *var);
extern device_t *devices_freep(device_t **var);

extern int get_devices(device_t **ret, int *count);
