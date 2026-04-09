// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <mntent.h>

#include "basics.h"
#include "devices.h"
#include "rdii-menu.h"

// Returns 1 if mounted, 0 if not mounted, -errno on error
static int
is_device_mounted(const char *device)
{
  FILE *fp = NULL;
  struct mntent *mnt;
  int mounted = 0;
  int r;

  if (isempty(device))
    return -EINVAL;

  // Open the mounted filesystems table
  fp = setmntent("/proc/mounts", "r");
  if (fp == NULL)
    {
      r = -errno;
      perror("setmntent"); // XXX
      return r;
    }

  // Iterate through all mount entries
  while ((mnt = getmntent(fp)) != NULL)
    {
      char *cp = startswith(mnt->mnt_fsname, device);

      if (cp)
	{
	  if (*cp == '\0') // exact match
	    {
	      mounted = 1;
	      break;
            }


	  // Check for e.g. /dev/sda1 or /dev/nvme0n1p1
	  if (isdigit(*cp) ||
	      (*cp == 'p' && isdigit(cp[1])))
	    {
	      mounted = 1;
	      break;
	    }
        }
    }

  endmntent(fp);
  return mounted;
}


static inline const char *strunknown(const char *s) {
        return s ?: "Unknown";
}

int
select_target_device(uint64_t minsize, char **device)
{
  _cleanup_free_ char **options = NULL;
  _cleanup_free_ int *mapping = NULL;
  _cleanup_(devices_freep) device_t *disk = NULL;
  int selected = 0;
  int count;
  int r;

  r = get_devices(&disk, &count);
  if (r < 0)
    return r;

  options = calloc(count, sizeof(char *));
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
      if (device && streq(disk[i].device, strempty(*device)))
	selected = n;
      // XXX we need to free this later
      if (asprintf(&options[n], "%s - %s (%s, %.1f GB)%s%s",
		   disk[i].device, strunknown(disk[i].model),
		   disk[i].bus, disk[i].size_gb,
		   disk[i].is_default_device?" [Default]":"",
		   disk[i].is_boot_device?" [Booted]":"") < 0)
	return -ENOMEM;
      mapping[n] = i;
      n++;
    }

  print_global_header_footer(NULL);
  print_title("Select Target Device");

  selected = choose_entry(4, (const char **)options, n, selected);
  if (selected < 0)
    return selected;

  if (is_device_mounted(disk[mapping[selected]].device))
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "The device %s contains mounted partitions.",
		   disk[mapping[selected]].device) < 0)
	return -ENOMEM;

      r = show_warning_popup("!!! CRITICAL WARNING: DRIVE IS CURRENTLY MOUNTED !!!",
			     msg,
			     "Proceeding may cause data loss or corruption.");
      if (r == 0)
	return 0;
    }

  *device = strdup(disk[mapping[selected]].device);
  if (!*device)
    return -ENOMEM;

  return 0;
}
