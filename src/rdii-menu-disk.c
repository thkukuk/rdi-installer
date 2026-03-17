// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "basics.h"
#include "devices.h"
#include "rdii-menu.h"

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
        {
	  *device = strdup(disk[selected].device);
	  if (!*device)
	    return -ENOMEM;
	  break;
	}
    }

  return 0;
}
