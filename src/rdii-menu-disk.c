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
  _cleanup_(devices_freep) device_t *disk = NULL;
  int selected = 0;
  int count;
  int r;

  r = get_devices(&disk, &count);
  if (r < 0)
    return r;

  while (1)
    {
      int entries_shown = 0;
      print_global_header_footer(NULL);
      print_title("Select Target Device");

      for (int i = 0; i < count; i++)
	{
	  int y = 4 + i;

	  if (!isempty(disk[i].type) && !streq(disk[i].type, "disk"))
            continue;
	  if (disk[i].size < minsize)
            continue;

	  if (i == selected)
            {
              attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
              mvprintw(y, 2, "-> %s - %s (%s, %.1f GB)",
		       disk[i].device, strunknown(disk[i].model),
		       disk[i].bus, disk[i].size_gb);
              attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
            }
          else
            {
              attron(COLOR_PAIR(CP_UNSELECTED));
              mvprintw(y, 2, "   %s - %s (%s, %.1f GB)",
		       disk[i].device, strunknown(disk[i].model),
		       disk[i].bus, disk[i].size_gb);
              attroff(COLOR_PAIR(CP_UNSELECTED));
            }
	  entries_shown++;
	}
      refresh();

      // Handle Input
      int ch = getch();
      if (ch == 27) // 27 is the ASCII code for ESC
        return -ECANCELED;
      else if (ch == KEY_UP)
        selected = (selected - 1 + entries_shown) % entries_shown;
      else if (ch == KEY_DOWN)
        selected = (selected + 1) % entries_shown;
      else if (ch == '\n') // Enter key
	break;
    }

  if (is_device_mounted(disk[selected].device))
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "The device %s contains mounted partitions.",
		   disk[selected].device) < 0)
	return -ENOMEM;

      r = show_warning_popup("!!! CRITICAL WARNING: DRIVE IS CURRENTLY MOUNTED !!!",
			     msg,
			     "Proceeding may cause data loss or corruption.");
      if (r == 0)
	return 0;
    }

  *device = strdup(disk[selected].device);
  if (!*device)
    return -ENOMEM;

  return 0;
}
