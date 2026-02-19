// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <getopt.h>
#include <stdint.h>
#include <limits.h>
#include <ctype.h>
#include <libudev.h>

#include "basics.h"
#include "efivars.h"


static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: rdii-helper [command]|[--help]|[--version]\n");
}

static void
print_help(void)
{
  fputs("rdii-helper - Helper functions for rdi-installer\n\n", stdout);
  print_usage(stdout);

  fputs("Commands: boot, disk\n\n", stdout);

  fputs ("Options for boot:\n", stdout);
  fputs("  -d, --debug       Print debug information\n", stdout);
  fputs ("\n", stdout);

  fputs ("Options for disk:\n", stdout);
  fputs("  -a, -all          Print all devices, even if not suitable\n", stdout);
  fputs("  -d, --debug       Print debug information\n", stdout);
  fputs ("\n", stdout);

  fputs ("Generic options:\n", stdout);
  fputs("  -h, --help        Give this help list\n", stdout);
  fputs("  -v, --version     Print program version\n", stdout);
}

static void
print_error(void)
{
  fputs("Try `rdii-helper --help' for more information.\n", stderr);
}

static int
main_boot(int argc, char **argv)
{
  _cleanup_efivars_ efivars_t *efi = NULL;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"debug",      no_argument,       NULL, 'd' },
          {"help",       no_argument,       NULL, 'h' },
          {"version",    no_argument,       NULL, 'v' },
          {NULL,         0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "dhv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
	case 'd':
	  _efivars_debug = true;
          break;
	case 'h':
          print_help();
          return 0;
        case 'v':
          printf("rdii-helper (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return EINVAL;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 0)
    {
      fprintf(stderr, "rdii-helper boot: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  r = efi_get_boot_source(&efi);
  if (r < 0)
    {
      fprintf(stderr, "Couldn't get boot source: %s\n", strerror(-r));
      return -r;
    }

  printf("Boot Entry:    %s\n", strna(efi->entry));
  printf("PXE Boot:      %s\n", efi->is_pxe_boot?"yes":"no");
  printf("Loader Device: %s\n", strna(efi->device));
  printf("Loader URL:    %s\n", strna(efi->url));
  printf("Loader Image:  %s\n", strna(efi->image));

  return 0;
}

typedef struct {
  const char *device;     // e.g. /dev/vda
  const char *type;       // e.g. disk, rom, ...
  const char *bus;        // e.g. usb, sata, virtio, nvme, ...
  const char *model;
  uint64_t size;          // size in bytes
  double size_gb;         // size in GB
  bool is_default_device; // device UEFI will try to boot from first
  bool is_boot_device;    // device insaller got loaded from
  int weight;
} device_t;

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


/* simple helper function, not very robust */
static int
parse_size(const char *str, uint64_t *res)
{
  char *ep;
  uint64_t size;
  unsigned long long ull_size = strtoull(str, &ep, 10);
  if (ull_size == ULLONG_MAX && errno == ERANGE)
    return -ERANGE;

  if (ull_size > UINT64_MAX)
    return -ERANGE;

  size = ull_size;

  if (!isempty(ep))
    {
      uint64_t old_size = size;
      if (toupper(*ep) == 'G')
	size *= 1024ULL * 1024 * 1024;
      else if (toupper(*ep) == 'M')
	size *= 1024ULL * 1024;
      else if (toupper(*ep) == 'T')
	size *= 1024ULL * 1024 * 1024 * 1024;

      /* XXX that's not good enough for Terrabyte... */
      if (size < old_size) // overflow
	return -ERANGE;
    }

  *res = size;
  return 0;
}

static inline const char *strunknown(const char *s) {
        return s ?: "Unknown";
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

static int
main_disk(int argc, char **argv)
{
  uint64_t minsize = 10 * 1000ULL * 1000 * 1000; // 10G min disk size
  bool all_devices = false;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
	  {"all",        no_argument,       NULL, 'a' },
	  {"debug",      no_argument,       NULL, 'd' },
	  {"minsize",    required_argument, NULL, 's' },
          {"help",       no_argument,       NULL, 'h' },
          {"version",    no_argument,       NULL, 'v' },
          {NULL,         0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "ads:hv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
	case 'a':
	  all_devices = true;
	  break;
	case 'd':
	  _efivars_debug = true;
          break;
	case 's':
	  r = parse_size(optarg, &minsize);
	  if (r < 0)
	    {
	      fprintf(stderr, "Error parsing '%s': %s\n",
		      optarg, strerror(-r));
	      return -r;
	    }
	  break;
	case 'h':
          print_help();
          return 0;
        case 'v':
          printf("rdii-helper (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return EINVAL;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 0)
    {
      fprintf(stderr, "rdii-helper disk: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  struct udev *udev = udev_new();  // XXX cleanup udev_unref
  if (!udev)
    {
      fprintf(stderr, "Cannot create udev context.\n");
      return ENOMEM;
    }

  struct udev_enumerate *enumerate = udev_enumerate_new(udev);
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

  device_t disk[128] = {0};
  int count = 0;

  udev_list_entry_foreach(dev_list_entry, devices)
    {
      const char *path = udev_list_entry_get_name(dev_list_entry);
      struct udev_device *dev = udev_device_new_from_syspath(udev, path);

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

      udev_device_unref(dev);

      if (count == 128)
	{
	  fprintf(stderr, "Error: you have too many disks!\n");
	  break;
	}
    }

  udev_enumerate_unref(enumerate);
  udev_unref(udev);

  // XXX set is_boot_device

  qsort(disk, count, sizeof(device_t), compare_devices);

  for (int i = 0; i < count; i++)
    {
      if (!all_devices)
	{
	  if (disk[i].is_boot_device)
	    continue;
	  if (!streq(disk[i].type, "disk"))
	    continue;
	  if (disk[i].size < minsize)
	    continue;
	}
      printf("%s - %s (%s, %.1f GB)\n", disk[i].device,
	     strunknown(disk[i].model), disk[i].bus, disk[i].size_gb);
    }

  return 0;
}

int
main(int argc, char **argv)
{
  struct option const longopts[] = {
    {"help",     no_argument,       NULL, 'h'},
    {"version",  no_argument,       NULL, 'v'},
    {NULL, 0, NULL, '\0'}
  };
  int c;

  if (argc == 1)
    {
      fprintf(stderr, "rdii-helper: no commands or options provided.\n");
      print_error();
      return EINVAL;
    }

  if (streq(argv[1], "boot"))
    return main_boot(--argc, ++argv);
  else if (streq(argv[1], "disk"))
    return main_disk(--argc, ++argv);

  while ((c = getopt_long (argc, argv, "hv", longopts, NULL)) != -1)
    {
      switch (c)
        {
	case 'h':
          print_help();
          return 0;
        case 'v':
          printf("rdii-helper (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return EINVAL;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 0)
    {
      fprintf(stderr, "rdii-helper: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  return 0;
}
