// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <ncursesw/curses.h>

#include "basics.h"
#include "exec_cmd.h"
#include "logger.h"
#include "rdii-menu.h"
#include "rdii-autoinstall.h"
#include "nc-dialogs.h"

static int
finish_reboot_or_poweroff(const char *cmd)
{
  int r;

  MSG_INFO("Calling exec_cmd(%s)", cmd);

  r = exec_cmd(true, cmd, cmd, NULL);
  if (r != 0)
    show_error_popup("Error while calling:", cmd, NULL);

  return r;
}

/* Starts the installation without user interaction if a valid image
   and device are already defined.
   "autoinstall_finish" decides what happens after a successful
   installation: "reboot"/"poweroff" do so directly, anything else
   (including NULL/"manual") falls back to show_post_menu().
   Returns true if the installation was handled here (*ret holds the
   value the caller should return), false if the caller should fall
   back to the interactive menu. */
bool
rdii_autoinstall(const char *image, const char *device, const char *mdraid,
		 bool preserve_ssh_hostkey, const char *autoinstall_finish,
		 int *ret)
{
  int r;

  if (isempty(image) || isempty(device))
    return false;

  MSG_INFO("rdii.autoinstall is set, starting installation automatically:");
  MSG_INFO("  image: %s", image);
  MSG_INFO("  device: %s", device);
  MSG_INFO("  mdraid: %s", (mdraid ? mdraid : "not set"));
  MSG_INFO("  preserve_ssh_hostkey: %d", preserve_ssh_hostkey);

  r = run_installation(image, device, mdraid, preserve_ssh_hostkey);
  if (r == 0)
    {
      if (!isempty(autoinstall_finish) && streq(autoinstall_finish, "reboot"))
	r = finish_reboot_or_poweroff("reboot");
      else if (!isempty(autoinstall_finish) && streq(autoinstall_finish, "poweroff"))
	r = finish_reboot_or_poweroff("poweroff");
      else
	r = show_post_menu();

      if (r == 0)
	{
	  endwin();
	  if (ret)
	    *ret = 0;
	  return true;
	}
    }

  return false;
}
