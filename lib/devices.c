// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#include <libudev.h>

#include "basics.h"
#include "efivars.h"

#include "devices.h"

static int
compare_devices(const void *a, const void *b)
{
  device_t *dev_a = (device_t *)a;
  device_t *dev_b = (device_t *)b;

  // Boot device of installer should be listed last
  if (dev_a->is_boot_device && !dev_b->is_boot_device) return 1;
  if (!dev_a->is_boot_device && dev_b->is_boot_device) return -1;

  // Default UEFI boot device should be listed first
  if (dev_a->is_default_device && !dev_b->is_default_device) return -1;
  if (!dev_a->is_default_device && dev_b->is_default_device) return 1;

  // same bus, use device name for ordering (sda before sdb)
  if (dev_a->weight == dev_b->weight)
    return strcmp(dev_a->device, dev_b->device);

  // Better devices listed first
  return (dev_b->weight - dev_a->weight);
}

static char *
oom_strdup(const char *s)
{
  char *cp;

  if (!s)
    return NULL;

  cp = strdup(s);
  if (s == NULL)
    exit(ENOMEM);
  return cp;
}

static inline void
udev_unrefp(struct udev **p)
{
  if (*p)
    *p = udev_unref(*p);
}

static inline void
udev_enumerate_unrefp(struct udev_enumerate **p)
{
  if (*p)
    *p = udev_enumerate_unref(*p);
}

static inline void
udev_device_unrefp(struct udev_device **p)
{
  if (*p)
    *p = udev_device_unref(*p);
}

device_t *
device_free(device_t *var)
{
  var->device = mfree(var->device);
  var->type = mfree(var->type);
  var->bus = mfree(var->bus);
  var->model = mfree(var->model);
  return var;
}

device_t *
devices_freep(device_t **var)
{
  device_t *disk = *var;

  if (var == NULL || *var == NULL)
    return NULL;

  for (int i = 0; disk[i].device; i++)
    device_free(&disk[i]);

  *var = mfree(disk);
  return NULL;
}

int
get_devices(device_t **ret, int *ret_count)
{
  _cleanup_efivars_ efivars_t *efi = NULL;
  _cleanup_free_ char *def_efi_part = NULL;
  _cleanup_free_ char *installer_part = NULL;
  int r;

  r = efi_get_boot_source(&efi);
  if (r < 0 && r != -ENODEV && r != -EOPNOTSUPP && r != -ENOENT)
    {
      fprintf(stderr, "Getting default EFI boot partition failed: %s\n",
	      strerror(-r));
      return -r;
    }
  else if (r == 0)
    {
      if (!isempty(efi->partition))
	installer_part = realpath(efi->partition, NULL);
      if (!isempty(efi->def_efi_partition))
	def_efi_part = realpath(efi->def_efi_partition, NULL);
    }

  _cleanup_(udev_unrefp) struct udev *udev = udev_new();
  if (!udev)
    {
      fprintf(stderr, "Cannot create udev context.\n");
      return ENOMEM;
    }

  _cleanup_(udev_enumerate_unrefp) struct udev_enumerate *enumerate = udev_enumerate_new(udev);
  if (!udev)
    {
      fprintf(stderr, "Cannot create udev enumeration context.\n");
      return 1;
    }

  r = udev_enumerate_add_match_subsystem(enumerate, "block");
  if (r < 0)
    return 1;
  r = udev_enumerate_add_match_property(enumerate, "DEVTYPE", "disk");
  if (r < 0)
    return 1;
  udev_enumerate_scan_devices(enumerate);
  if (r < 0)
    return 0;

  struct udev_list_entry *devices, *dev_list_entry;
  devices = udev_enumerate_get_list_entry(enumerate);

  _cleanup_(devices_freep) device_t *disk;
  disk = calloc(128, sizeof(device_t));
  int count = 0;

  udev_list_entry_foreach(dev_list_entry, devices)
    {
      const char *path = udev_list_entry_get_name(dev_list_entry);
      _cleanup_(udev_device_unrefp) struct udev_device *dev = udev_device_new_from_syspath(udev, path);

      if (!dev)
	continue;

      const char *device = udev_device_get_devnode(dev);
      const char *type = udev_device_get_property_value(dev, "ID_TYPE");
      const char *is_cdrom = udev_device_get_property_value(dev, "ID_CDROM");
      if (!isempty(is_cdrom) && streq(is_cdrom, "1"))
	type = "rom";
      const char *bus = udev_device_get_property_value(dev, "ID_BUS");
      if (isempty(bus))
	{
	  if (startswith(device, "/dev/vd"))
	    {
	      bus = "virtio";
	      if (isempty(type))
		type = "disk";
	    }
	  else if (startswith(device, "/dev/nvme"))
	    {
	      bus = "nvme";
	      if (isempty(type))
		type = "disk";
	    }
	}
      else if (streq(bus, "ata"))
	{
	  // check if old ata or sata
	  const char *is_sata = udev_device_get_property_value(dev, "ID_ATA_SATA");
	  if (!isempty(is_sata) && streq(is_sata, "1"))
	    bus = "sata";
	}
      const char *model = udev_device_get_property_value(dev, "ID_MODEL");
      const char *size_str = udev_device_get_sysattr_value(dev, "size");
      uint64_t size = 0;
      if (size_str)
	{
	  unsigned long long sectors = strtoull(size_str, NULL, 10); // XXX error checking
	  size = sectors * 512;
	}
      double size_gb = size / 1024.0 / 1024.0 / 1024.0;

      disk[count].device = oom_strdup(device);
      disk[count].type = oom_strdup(type);
      disk[count].bus = oom_strdup(bus);
      disk[count].model = oom_strdup(model);
      disk[count].size = size;
      disk[count].size_gb = size_gb;
      if (def_efi_part)
	disk[count].is_default_device =
	  (startswith(def_efi_part, device) != NULL);
      if (installer_part)
	disk[count].is_boot_device = startswith(installer_part, device);

      if (!isempty(bus))
	{
	  if (streq(bus, "nvme"))
	    disk[count].weight = 100;
	  else if (streq(bus, "virtio"))
	    disk[count].weight = 90;
	  else if (streq(bus, "sata"))
	    disk[count].weight = 80;
	  else if (streq(bus, "scsi"))
	    disk[count].weight = 70;
	  else if (streq(bus, "ata"))
	    disk[count].weight = 40;
	  else if (streq(bus, "usb"))
	    disk[count].weight = 10;
	  else disk[count].weight = 50;
	}

      count++;

      if (count == 128)
	{
	  fprintf(stderr, "Error: you have too many disks!\n");
	  return -E2BIG;
	}
    }

  qsort(disk, count, sizeof(device_t), compare_devices);

  if (ret)
    *ret = TAKE_PTR(disk);
  if (ret_count)
    *ret_count = count;

  return 0;
}
