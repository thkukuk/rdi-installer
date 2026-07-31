// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <wchar.h>
#include <linux/kd.h>
#include <sys/ioctl.h>

#include "basics.h"
#include "mkdir_p.h"
#include "rdii-menu.h"
#include "logger.h"
#include "exec_cmd.h"
#include "zap_partition_table.h"

#define TITLE "Raw Disk Installer Version " VERSION

/* Returns true if the controlling terminal is a Linux virtual console.
   Only there can the keyboard mapping be changed with loadkeys(1);
   serial consoles and pseudo terminals fail KDGKBTYPE with ENOTTY. */
bool
is_linux_vt(void)
{
  char kbtype;

  _cleanup_close_ int fd = open("/dev/tty", O_RDWR|O_CLOEXEC|O_NOCTTY);
  if (fd < 0)
    return false;

  int r = ioctl(fd, KDGKBTYPE, &kbtype);
  close(fd);
  return r >= 0;
}

static void
init_colors(void)
{
  MSG_FUNC();

  start_color();
  use_default_colors();

  init_pair(CP_HEADER, COLOR_GREEN, COLOR_BLUE);
  init_pair(CP_SPLASH_BOX, COLOR_GREEN, COLOR_BLUE);
  init_pair(CP_WARNING, COLOR_WHITE, COLOR_RED);
  init_pair(CP_SELECTED, COLOR_GREEN, -1);

  if (COLORS >= 256)
    {
      init_pair(CP_TITLE, 21, -1);      // Blue color 21
      init_pair(CP_UNSELECTED, 8, -1);  // Gray color 8
    }
  else
    {
      init_pair(CP_TITLE, COLOR_BLUE, -1);
      init_pair(CP_UNSELECTED, COLOR_WHITE, -1);
    }

  // Footer: Default font
  init_pair(CP_FOOTER, -1, -1);
}

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
show_splash_screen(void)
{
  int width = strlen(TITLE) + 12;
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
  int text_x = (width - strlen(TITLE)) / 2;
  mvwprintw(win, text_y, text_x, "%s", TITLE);
  wrefresh(win);

  keywait(start_y+height, start_x, NULL, -1);

  delwin(win);
}

void
print_global_header_footer(const char *addkeys)
{
  MSG_FUNC("addkeys='%s'", strempty(addkeys));

  clear();
  // Draw Header (Green on Blue)
  attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', COLS); // Fill the top line background
  mvprintw(0, (COLS - strlen(TITLE)) / 2, TITLE);
  attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);

  // Draw Footer
  const char *footer_text = "Up/Down: Navigate | Enter: Select | ESC: Abort/Quit";
  attron(COLOR_PAIR(CP_FOOTER) | A_REVERSE);
  mvhline(LINES - 1, 0, ' ', COLS);
  if (addkeys)
    mvprintw(LINES - 1, 1, "%s | %s", addkeys, footer_text);
  else
    mvprintw(LINES - 1, 1, "%s", footer_text);
  attroff(COLOR_PAIR(CP_FOOTER) | A_REVERSE);
}

void
print_title(const char *title)
{
  MSG_FUNC("title='%s'", title);

  attron(COLOR_PAIR(CP_TITLE));
  mvprintw(2, 2, "%s", title);
  attroff(COLOR_PAIR(CP_TITLE));
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


// Returns 1 if YES, 0 if NO
int
show_warning_popup(const char *headline,
		   const char *descr_line1, const char *descr_line2)
{
  unsigned int height = 7;
  unsigned int width;

  MSG_FUNC("headline='%s', descr1='%s', descr2='%s'", strna(headline),
	   strna(descr_line1), strna(descr_line2));

  if (descr_line1)
    height++;

  if (descr_line2)
    height++;

  width = strlen(headline) + 6;
  if (descr_line1)
    {
      if (strlen(descr_line1) > (size_t)(COLS - 8))
	descr_line1 = truncate_middle(descr_line1, COLS-8);
      if (strlen(descr_line1) + 6 > width)
	width = strlen(descr_line1) + 6;
    }
  if (descr_line2)
    {
      if (strlen(descr_line2) > (size_t)(COLS - 8))
	descr_line2 = truncate_middle(descr_line2, COLS-8);
      if (strlen(descr_line2) + 6 > width)
	width = strlen(descr_line2) + 6;
    }

  int start_y = (LINES - height) / 2 - 2;
  int start_x = (COLS - width) / 2;

  WINDOW *win = newwin(height, width, start_y, start_x);
  wbkgd(win, COLOR_PAIR(CP_WARNING));
  keypad(win, TRUE); // Enable arrow keys for this specific window

  int btn_selected = 1; // 0 = YES, 1 = NO (Defaulting to NO for safety)
  int choice = -1;

  while (1)
    {
      box(win, 0, 0);
      mvwprintw(win, 2, (width - strlen(headline)) / 2, "%s", headline);
      if (descr_line1)
	mvwprintw(win, 4, (width - strlen(descr_line1)) / 2, "%s", descr_line1);
      if (descr_line2)
	mvwprintw(win, 5, (width - strlen(descr_line2)) / 2, "%s", descr_line2);

      if (btn_selected == 0)
	{
	  wattron(win, A_REVERSE);
	  mvwprintw(win, height - 2, width / 2 - 10, "[ YES ]");
	  wattroff(win, A_REVERSE);
        }
      else
	mvwprintw(win, height - 2, width / 2 - 10, "[ YES ]");

      if (btn_selected == 1)
	{
	  wattron(win, A_REVERSE);
	  mvwprintw(win, height - 2, width / 2 + 3, "[ NO ]");
	  wattroff(win, A_REVERSE);
        }
      else
	mvwprintw(win, height - 2, width / 2 + 3, "[ NO ]");

      wrefresh(win);

      // Handle input locally inside the popup
      int key = wgetch(win);
      if (key == KEY_LEFT || key == KEY_RIGHT || key == '\t')
	  btn_selected = 1 - btn_selected; // Toogle
      else if (key == '\n' || key == KEY_ENTER)
	{
	  choice = btn_selected;
	  break;
        }
      else if (key == 27) // ESC key aborts/defaults to NO
	{
	  choice = 1;
	  break;
        }
    }

    delwin(win);
    refresh();

    return (choice == 0); // Return 1 if YES was chosen, otherwise 0
}

void
show_error_popup(const char *headline,
		 const char *descr_line1, const char *descr_line2)
{
  int height = 7;
  int width = strlen(headline) + 6;

  MSG_FUNC("headline='%s', descr1='%s', descr2='%s'", strna(headline),
	   strna(descr_line1), strna(descr_line2));

  if (headline)
    height++;

  if (descr_line1)
    {
      height++;
      if ((int)(strlen(descr_line1) + 6) > width)
	width = strlen(descr_line1) + 6;
    }
  if (descr_line2)
    {
      height++;
      if ((int)(strlen(descr_line2) + 6) > width)
	width = strlen(descr_line2) + 6;
    }

  int start_y = (LINES - height) / 2 - 2;
  int start_x = (COLS - width) / 2;

  WINDOW *win = newwin(height, width, start_y, start_x);
  wbkgd(win, COLOR_PAIR(CP_WARNING));
  box(win, 0, 0);
  mvwprintw(win, 2, (width - strlen(headline)) / 2, "%s", headline);
  if (descr_line1)
    mvwprintw(win, 4, (width - strlen(descr_line1)) / 2, "%s", descr_line1);
  if (descr_line2)
    mvwprintw(win, 5, (width - strlen(descr_line2)) / 2, "%s", descr_line2);

  mvwprintw(win, height - 3, width / 2 - 3, "[ OK ]");
  wrefresh(win);

  while (1)
    {
      // Handle input locally inside the popup
      int key = wgetch(win);
      if (key == '\n' || key == 27) // RETURN || ESC
	break;
    }

  delwin(win);
}

void
show_info_popup(const char *headline, const char *descr)
{
  int height = 6;
  int width = strlen(headline) + 6;

  MSG_FUNC("headline='%s', descr='%s'", headline, strna(descr));

  if (descr)
    {
      height += 2;
      if ((int)(strlen(descr) + 6) > width)
	width = strlen(descr) + 6;
    }

  int start_y = (LINES - height) / 2 - 2;
  int start_x = (COLS - width) / 2;

  WINDOW *win = newwin(height, width, start_y, start_x);
  wbkgd(win, COLOR_PAIR(CP_SPLASH_BOX));
  keypad(win, TRUE);
  box(win, 0, 0);
  mvwprintw(win, 2, (width - strlen(headline)) / 2, "%s", headline);
  if (descr)
    mvwprintw(win, 4, (width - strlen(descr)) / 2, "%s", descr);
  mvwprintw(win, height - 2, width / 2 - 3, "[ OK ]");

  // Use wgetch/wrefresh on the popup window instead of keywait() which
  // calls refresh() on stdscr.  After clear(), stdscr has clearok set,
  // so a refresh() would flush blank stdscr cells over the popup window
  // in the virtual screen, making the popup invisible.
  wtimeout(win, 100);

  const char spinner[] = "|/-\\";
  int spinner_idx = 0;
  int elapsed_ms = 0;

  while (elapsed_ms < 30 * 1000)
    {
      mvwprintw(win, height - 2, width - 2, "%c", spinner[spinner_idx]);
      spinner_idx = (spinner_idx + 1) % 4;
      wrefresh(win);
      if (wgetch(win) != ERR)
	break;
      elapsed_ms += 100;
    }

  delwin(win);
  refresh();
}

int
choose_entry(int row, const char *options[], int num_options, int start)
{
  int selected = start;

  MSG_FUNC("row=%i, options[0]='%s', num_options=%i, start=%i",
	   row, options[0], num_options, start);

  while (1)
    {
      for (int i = 0; i < num_options; i++)
	{
	  int y = row + i;

	  if (i == selected)
	    {
	      attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
	      mvprintw(y, 2, "-> %s", options[i]);
	      attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
	    }
	  else
	    {
	      attron(COLOR_PAIR(CP_UNSELECTED));
	      mvprintw(y, 2, "   %s", options[i]);
	      attroff(COLOR_PAIR(CP_UNSELECTED));
	    }
	}

      refresh();

      int ch = getch();
      if (ch == 27) // 27 is the ASCII code for ESC
	{
	  MSG_INFO("Canceld with ESC");
	  return -ECANCELED;
	}
      else if (ch == KEY_UP)
	selected = (selected - 1 + num_options) % num_options;
      else if (ch == KEY_DOWN)
	selected = (selected + 1) % num_options;
      else if (ch == '\n' || ch == KEY_ENTER)
	{
	  MSG_INFO("Selected entry %i", selected);
	  return selected;
	}
    }

  MSG_ERROR("quit while loop without return!");

  // we should never reach this
  return -2;
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

  MSG_FUNC();

  while (1)
    {
      print_global_header_footer(NULL);
      selected = choose_entry(4, options, num_options, selected);
      switch(selected)
	{
	case 0: // Reboot
	  MSG_INFO("Calling exec_cmd(reboot)");
	  return exec_cmd("reboot", "reboot");
	case 1: // Try Again/Next Image
	  MSG_INFO("Try Again/Next Image selected");
	  return 1;
	case 2: // PowerOff
	  MSG_INFO("Calling exec_cmd(poweroff)");
	  return exec_cmd("poweroff", "poweroff");
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
	      int r = run_installation(image, device, mdraid, preserve_ssh_hostkey);
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
	  return exec_cmd("reboot", "reboot");
	  break;
	case 10: // PowerOff
	  return exec_cmd("poweroff", "poweroff");
	  break;
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

void
init_ncurses(void)
{
  // For correctly rendering the double borders
  setlocale(LC_ALL, "");

  MSG_FUNC();

  // Initialize ncurses
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE); // Enable arrow keys
  curs_set(0);          // Hide cursor
  set_escdelay(25);     // Set escape delay to 25 milliseconds

  if (has_colors())
    init_colors();
}

int
rdii_menu(const char *image0, const char *image1, const char *image2,
	  const char *device, const char *mdraid, bool preserve_ssh_hostkey)
{
  const char *image = NULL;
  int r;

  MSG_FUNC("image0='%s', image1='%s', image2='%s', device='%s', mdraid='%s', preserve_ssh_hostkey=%i",
	   strempty(image0), strempty(image1), strempty(image2),
	   strempty(device), strempty(mdraid), preserve_ssh_hostkey);

  show_splash_screen();

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
