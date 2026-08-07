// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "basics.h"
#include "devices.h"
#include "rdii-menu.h"
#include "logger.h"

int
select_mdraid_devices(uint64_t minsize, char **device1, char **device2)
{
  _cleanup_str_array_ char **options = NULL;
  _cleanup_free_ int *mapping = NULL;
  _cleanup_(devices_freep) device_t *disk = NULL;
  int selected1 = 0;
  int selected2 = -1;
  int count;
  int r;

  r = get_devices(&disk, &count);
  if (r < 0)
    return r;

  options = calloc(count + 1, sizeof(char *)); /* +1 for the NULL terminator _cleanup_str_array_ relies on */
  if (!options)
    return -ENOMEM;

  mapping = calloc(count, sizeof(int));
  if (!mapping)
    return -ENOMEM;

  int n = 0;
  for (int i = 0; i < count; i++)
    {
      if (!isempty(disk[i].type) && !streq(disk[i].type, "disk"))
	continue;
      if (disk[i].size < minsize)
	continue;
      if (device1 && streq(disk[i].device, strempty(*device1)))
	selected1 = n;
      if (device2 && streq(disk[i].device, strempty(*device2)))
	selected2 = n;

      if (asprintf(&options[n], "%s - %s (%s, %.1f GB)%s%s",
		   disk[i].device, strunknown(disk[i].model),
		   disk[i].bus, disk[i].size_gb,
		   disk[i].is_default_device?" [Default]":"",
		   disk[i].is_boot_device?" [Booted]":"") < 0)
	return -ENOMEM;
      mapping[n] = i;
      n++;
    }

  if (n < 2)
    {
      show_error_popup("Insufficient Devices",
		       "At least two disks are required for mdraid.",
		       NULL);
      return -EINVAL;
    }

  // Select first device
  print_global_header_footer(NULL);
  print_title("Select First Disk for mdraid");

  selected1 = choose_entry(4, (const char **)options, n, selected1);
  if (selected1 < 0)
    return selected1;

  if (is_device_mounted(disk[mapping[selected1]].device))
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "The device %s contains mounted partitions.",
		   disk[mapping[selected1]].device) < 0)
	return -ENOMEM;

      r = show_warning_popup("!!! CRITICAL WARNING: DRIVE IS CURRENTLY MOUNTED !!!",
			     msg,
			     "Proceeding may cause data loss or corruption.");
      if (r == 0)
	return 0;
    }

  uint64_t size1 = disk[mapping[selected1]].size;

  if (selected2 == -1)
    {
      // Start selection at 0, but skip if it's the first selected device
      selected2 = 0;
      if (selected2 == selected1)
	selected2 = (selected2 + 1) % n;
    }

  while (1)
    {
      // Select second device
      print_global_header_footer(NULL);
      print_title("Select Second Disk for mdraid");

      selected2 = choose_entry(4, (const char **)options, n, selected2);
      if (selected2 < 0)
	return selected2;

      if (selected2 == selected1)
	{
	  show_error_popup("Invalid Selection",
			   "Cannot select the same disk twice.",
			   "Please select a different disk.");
	  continue;
	}

      break;
    }

  if (is_device_mounted(disk[mapping[selected2]].device))
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "The device %s contains mounted partitions.",
		   disk[mapping[selected2]].device) < 0)
	return -ENOMEM;

      r = show_warning_popup("!!! CRITICAL WARNING: DRIVE IS CURRENTLY MOUNTED !!!",
			     msg,
			     "Proceeding may cause data loss or corruption.");
      if (r == 0)
	return 0;
    }

  uint64_t size2 = disk[mapping[selected2]].size;

  // Check if disk sizes differ significantly (more than 5% difference)
  uint64_t size_diff = size1 > size2 ?
    size1 - size2 : size2 - size1;

  if (size_diff * 100 / (size1 > size2 ? size1 : size2) > 5)
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Disk 1: %.1f GB, Disk 2: %.1f GB",
		   disk[mapping[selected1]].size_gb,
		   disk[mapping[selected2]].size_gb) < 0)
	return -ENOMEM;

      r = show_warning_popup("WARNING: DISK SIZE MISMATCH",
			     msg,
			     "Continue?");
      if (r == 0)
	return 0;
    }

  *device1 = strdup(disk[mapping[selected1]].device);
  if (!*device1)
    return -ENOMEM;
  *device2 = strdup(disk[mapping[selected2]].device);
  if (!*device2)
    {
      free(*device1);
      *device1 = NULL;
      return -ENOMEM;
    }

  return 0;
}
