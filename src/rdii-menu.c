// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <wchar.h>

#include "basics.h"
#include "exec_cmd.h"
#include "nc-dialogs.h"
#include "rdii-menu.h"
#include "logger.h"
#include "select_keymap.h"
#include "zap_partition_table.h"
#include "is_linux_vt.h"

void
keywait(int y, int x, const char *text, int sec)
{
  const char spinner[] = "|/-\\";
  int spinner_idx = 0;
  int elapsed_ms = 0;
  const char *msg = "Press any key...";

  MSG_FUNC("y=%i, x=%i, text='%s', sec=%i", y, x, strempty(text), sec);

  if (text)
    msg = text;

  if (sec < 0)
    sec = 5;

  // Set 100ms timeout for getch() to allow animation
  timeout(100);

  while (elapsed_ms < (sec*1000) || sec == 0)
    {
      if (sec)
	{
	  mvprintw(y, x, "%s %c", msg, spinner[spinner_idx]);
	  spinner_idx = (spinner_idx + 1) % 4;
	}
      else
	mvprintw(y, x, "%s", msg);
      refresh();
      if (getch() != ERR)
	{
	  MSG_INFO("keywait: key pressed");
	  break;
	}
      elapsed_ms += 100;
    }

  // Reset timeout back to blocking mode for the main menu
  timeout(-1);
}

static void
show_splash_screen(const char *name)
{
  int width = strlen(name) + 12;
  int height = 7;
  int start_x = (COLS - width) / 2;
  int start_y = (LINES - height) / 2 - 2;

  MSG_FUNC();

  clear();
  refresh();

  WINDOW *win = newwin(height, width, start_y, start_x);
  wbkgd(win, COLOR_PAIR(CP_SPLASH_BOX));

  /* The linux console (TERM=linux) does not support the wide-character
     interface used by wborder_set, so fall back to ACS single-line border. */
  const char *term = getenv("TERM");
  if (term && strcmp(term, "linux") == 0)
    {
      box(win, 0, 0);
    }
  else
    {
      cchar_t ls, rs, ts, bs, tl, tr, bl, br;

      setcchar(&ls, L"║", WA_NORMAL, 0, NULL);
      setcchar(&rs, L"║", WA_NORMAL, 0, NULL);
      setcchar(&ts, L"═", WA_NORMAL, 0, NULL);
      setcchar(&bs, L"═", WA_NORMAL, 0, NULL);
      setcchar(&tl, L"╔", WA_NORMAL, 0, NULL);
      setcchar(&tr, L"╗", WA_NORMAL, 0, NULL);
      setcchar(&bl, L"╚", WA_NORMAL, 0, NULL);
      setcchar(&br, L"╝", WA_NORMAL, 0, NULL);

      wborder_set(win, &ls, &rs, &ts, &bs, &tl, &tr, &bl, &br);
    }

  int text_y = height / 2;
  int text_x = (width - strlen(name)) / 2;
  mvwprintw(win, text_y, text_x, "%s", name);
  wrefresh(win);

  keywait(start_y+height, start_x, NULL, -1);

  delwin(win);
}

static char*
truncate_middle(const char *str, size_t max_len)
{
  size_t len = strlen(str);

  if (len <= max_len)
    return strdup(str); // Yes, we ignore OOM...

  char *result = malloc(max_len + 1);
  if (result == NULL)
    return NULL;

  // If max_len is too small to even fit the "...", fallback to a hard truncation at the end
  if (max_len < 3)
    {
      strncpy(result, str, max_len);
      result[max_len] = '\0';
      return result;
    }

  // Calculate how many characters from the original string we can keep
  size_t keep_len = max_len - 3;
  size_t prefix_len = keep_len / 2;
  size_t suffix_len = keep_len - prefix_len;

  // Construct the new string: [prefix] + "..." + [suffix]
  strncpy(result, str, prefix_len);
  char *cp = stpcpy(result + prefix_len, "...");
  stpcpy(cp, str + (len - suffix_len));

  return result;
}


static int
show_post_menu(void)
{
  const char *options[] = {
    "Reboot",
    "Try Again/Next Image",
    "PowerOff",
    "Exit"
  };
  int num_options = sizeof(options) / sizeof(options[0]);
  int selected = 0;
  int r;

  MSG_FUNC();

  while (1)
    {
      print_global_header_footer(NULL);
      selected = choose_entry(4, options, num_options, selected);
      switch(selected)
	{
	case 0: // Reboot
	  MSG_INFO("Calling exec_cmd(reboot)");
	  r = exec_cmd(true, "reboot", "reboot", NULL);
	  if (r != 0)
	    keywait(LINES-3, 0, NULL, 0);
	  return r;
	case 1: // Try Again/Next Image
	  MSG_INFO("Try Again/Next Image selected");
	  return 1;
	case 2: // PowerOff
	  MSG_INFO("Calling exec_cmd(poweroff)");
	  r = exec_cmd(true, "poweroff", "poweroff", NULL);
	  if (r != 0)
	    keywait(LINES-3, 0, NULL, 0);
	  return r;
	case 3: // Exit
	  MSG_INFO("Quit");
	  return 0;
	default:
	  MSG_WARN("Menu returned with %i", selected);
	  return -EIO;
	}
    }
}

static int
show_main_menu(const char *def_image, const char *def_device, const char *def_mdraid, bool preserve_ssh_hostkey)
{
  uint64_t minsize = 10 * 1000ULL * 1000 * 1000; // 10G min disk size
  _cleanup_free_ char *image_entry = NULL;
  _cleanup_free_ char *target_entry = NULL;
  _cleanup_free_ char *mdraid_entry = NULL;
  _cleanup_free_ char *keymap_entry = NULL;
  _cleanup_free_ char *image = NULL;
  _cleanup_free_ char *device = NULL; // standard device or first device of mdraid
  _cleanup_free_ char *mdraid = NULL; // second device for mdraid
  const char *options[] = {
    "Select Image",
    "Select Target",
    "Enable MD Devices (Raid1)",
    "Select Keymap",
    "System Information",
    "Start Installation",
    "Destroy Partition Table",
    "Refresh Screen",
    "Abort",
    "Reboot System",
    "PowerOff"
  };
  int num_options = sizeof(options) / sizeof(options[0]);
  int selected = 0;
  int r;

  if (def_image)
    {
      image = strdup(def_image);
      if (!image)
	return -ENOMEM;
    }
  if (def_device)
    {
      device = strdup(def_device);
      if (!device)
	return -ENOMEM;
    }
  if (def_mdraid)
    {
      mdraid = strdup(def_mdraid);
      if (!mdraid)
	return -ENOMEM;
    }

  // Adjust menu entries
  if (!isempty(image))
    {
      _cleanup_free_ char *cp = truncate_middle(image, COLS-22);

      if (asprintf(&image_entry, "%s (%s)", options[0], cp) < 0)
	return -ENOMEM;
      options[0] = image_entry;
    }
  if (!isempty(device))
    {
      if (asprintf(&target_entry, "%s (%s)", options[1], device) < 0)
	return -ENOMEM;
      options[1] = target_entry;
    }

  if (!isempty(image) && !isempty(device))
    selected = 5;

  while (1)
    {
      print_global_header_footer(NULL);
      print_title("Configuration Settings");

      selected = choose_entry(4, options, num_options, selected);
      switch(selected)
	{
	case 0: // Select Image
	  {
	    select_installation_source(image, &image);
	    if (!isempty(image))
	      {
		_cleanup_free_ char *cp = truncate_middle(image, COLS-22);

		image_entry = mfree(image_entry);
		if (asprintf(&image_entry, "Select Image (%s)", cp) < 0)
		  return -ENOMEM;
		options[selected] = image_entry;
		// if we need a device pre-select that, else pre-select installation
		if (isempty(device))
		  selected = 1;
		else
		  selected = 5;
	      }
	  }
	  break;
	case 1: // Select Target
	  {
	    select_target_device(minsize, &device);
	    if (!isempty(device))
	      {
		target_entry = mfree(target_entry);
		if (asprintf(&target_entry, "Select Target (%s)", device) < 0)
		  return -ENOMEM;
		options[selected] = target_entry;
		// if we have an image, too, pre-select installation, else pre-select image
		if (isempty(image))
		  selected = 0;
		else
		  selected = 5;
	      }
	  }
	  break;
	case 2: // Enable mdraid
	  {
	    if (select_mdraid_devices(minsize, &device, &mdraid) == 0)
	      {
		if (!isempty(device) && !isempty(mdraid))
		  {
		    mdraid_entry = mfree(mdraid_entry);
		    if (asprintf(&mdraid_entry, "Enable MD Devices (Raid1) (%s, %s)",
				 device, mdraid) < 0)
		      return -ENOMEM;
		    options[selected] = mdraid_entry;
		  }
	      }
	  }
	  break;
	case 3: // Select Keymap
	  {
	    if (is_linux_vt() ||
		show_warning_popup("Keymaps can only be configured directly on a virtual console.",
				   NULL, "Continue?"))
	      {
		_cleanup_free_ char *keymap = NULL;
		if (select_keymap(&keymap) == 0)
		  {
		    keymap_entry = mfree(keymap_entry);
		    if (asprintf(&keymap_entry, "Select Keymap (%s)",
				 strna(keymap)) < 0)
		      return -ENOMEM;
		    options[selected] = keymap_entry;
		  }
	      }
	  }
	  break;
	case 4: // System Information
	  show_sysinfo();
	  break;
	case 5: // Start Installation
	  if (isempty(image) || isempty(device))
	    show_error_popup("Installation image and target device are required!",
			     NULL, NULL);
	  else
	    {
	      r = run_installation(image, device, mdraid, preserve_ssh_hostkey);
	      if (r == 0)
		{
		  r = show_post_menu();
		  if (r == 0)
		    return 0;
		}
	    }
	  break;
	case 6: // Destroy partition table
	  select_target_device(0, &device);
	  if (!isempty(device))
	    {
	      _cleanup_free_ char *errmsg = NULL;

	      if (!show_warning_popup("WARNING: PERMANENT DATA LOSS - Are you absolutely sure?",
				      device, NULL))
		break;

	      if (zap_partition_tables(device, &errmsg) < 0)
		{
		  MSG_ERROR("ERROR: Destroying partition table failed: %s", errmsg);
		  show_error_popup("ERROR: Destroying partition table failed!",
				   errmsg, NULL);
		}
	      else
		{
		  MSG_INFO("Destroying partition table on device '%s' successful.", device);
		  print_global_header_footer(NULL); // remove warning popup
		  refresh();
		  show_info_popup("Destroying partition table was successful", NULL);
		}
	    }
	  break;
	case 7: // Refresh Screen
	  // loop will redraw screen
	  break;
	case 8: // Abort
	  return 0;
	  break;
	case 9: // Reboot
	  r = exec_cmd(true, "reboot", "reboot", NULL);
	  if (r != 0)
	    keywait(LINES-3, 0, NULL, 0);
	  return r;
	case 10: // PowerOff
	  r = exec_cmd(true, "poweroff", "poweroff", NULL);
	  if (r != 0)
	    keywait(LINES-3, 0, NULL, 0);
	  return r;
	case -ECANCELED:
	  return 0;
	  break;
	default:
          show_error_popup("Internal Error", NULL, NULL);
	  abort();
	  break;
	}
    }

  return 0;
}

static int
select_image(const char *image1, const char *image2,
	     const char *image3)
{
  _cleanup_free_ char **options = NULL;

  MSG_FUNC("image1='%s', image2='%s', image3='%s'",
	   strempty(image1), strempty(image2), strempty(image3));

  options = calloc(3, sizeof(char *));
  if (!options)
    return -ENOMEM;

  options[0] = truncate_middle(strna(image1), COLS-8);
  options[1] = truncate_middle(strna(image2), COLS-8);
  options[2] = truncate_middle(strna(image3), COLS-8);

  print_global_header_footer(NULL);
  print_title("Select Installation Source");

  return choose_entry(4, (const char **)options, 3, 0);
}

int
rdii_menu(const char *title, const char *image0, const char *image1,
	  const char *image2, const char *device, const char *mdraid,
	  bool preserve_ssh_hostkey)
{
  const char *image = NULL;
  int r;

  MSG_FUNC("image0='%s', image1='%s', image2='%s', device='%s', mdraid='%s', preserve_ssh_hostkey=%i",
	   strempty(image0), strempty(image1), strempty(image2),
	   strempty(device), strempty(mdraid), preserve_ssh_hostkey);

  show_splash_screen(title);

  if (!isempty(image0) && (!isempty(image1) || !isempty(image2)))
    {
      r = select_image(image0, image1, image2);
      switch(r)
        {
        case 0:
          image = image0;
          break;
        case 1:
          image = image1;
          break;
        case 2:
          image = image2;
          break;
        case -ECANCELED:
          endwin();
          return 0;
	default:
          MSG_ERROR("select_image() failed: %s", strerror(-r));
          show_error_popup("Internal Error", NULL, NULL);
          endwin();
          return r;
        }
    }
  else
    image = image0;

  r = show_main_menu(image, device, mdraid, preserve_ssh_hostkey);
  endwin();

  return r;
}
