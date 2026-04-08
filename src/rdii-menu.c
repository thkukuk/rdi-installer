// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <wchar.h>
#include <libeconf.h>

#include "basics.h"
#include "rm_rf.h"
#include "mkdir_p.h"
#include "tmpfile-util.h"
#include "rdii-menu.h"
#include "logger.h"

#define TITLE "Raw Disk Installer Version " VERSION

const char *rdii_run_dir = "/run/rdi-installer";
const char *rdii_config = "/run/rdi-installer/rdii-config";
const char *rdii_tmp_dir = NULL;
const char *rdii_log = "/var/log/rdi-installer.log";

static void
init_colors(void)
{
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
	break;
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

  clear();
  refresh();

  WINDOW *win = newwin(height, width, start_y, start_x);
  wbkgd(win, COLOR_PAIR(CP_SPLASH_BOX));

  // box(win, 0, 0);

  // Define double-line Unicode characters for the border
  cchar_t ls, rs, ts, bs, tl, tr, bl, br;

  setcchar(&ls, L"║", WA_NORMAL, 0, NULL); // Left side
  setcchar(&rs, L"║", WA_NORMAL, 0, NULL); // Right side
  setcchar(&ts, L"═", WA_NORMAL, 0, NULL); // Top side
  setcchar(&bs, L"═", WA_NORMAL, 0, NULL); // Bottom side
  setcchar(&tl, L"╔", WA_NORMAL, 0, NULL); // Top-left corner
  setcchar(&tr, L"╗", WA_NORMAL, 0, NULL); // Top-right corner
  setcchar(&bl, L"╚", WA_NORMAL, 0, NULL); // Bottom-left corner
  setcchar(&br, L"╝", WA_NORMAL, 0, NULL); // Bottom-right corner

  wborder_set(win, &ls, &rs, &ts, &bs, &tl, &tr, &bl, &br);

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
show_warning_popup(const char *msg1, const char *msg2, const char *msg3)
{
  unsigned int height = 7;
  unsigned int width;

  if (msg2)
    height++;
  if (msg3)
    height++;

  width = strlen(msg1) + 6;
  if (msg2)
    {
      if (strlen(msg2) > (size_t)(COLS - 8))
	msg2 = truncate_middle(msg2, COLS-8);
      if (strlen(msg2) + 6 > width)
	width = strlen(msg2) + 6;
    }
  if (msg3)
    {
      if (strlen(msg3) > (size_t)(COLS - 8))
	msg3 = truncate_middle(msg3, COLS-8);
      if (strlen(msg3) + 6 > width)
	width = strlen(msg3) + 6;
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
      mvwprintw(win, 2, (width - strlen(msg1)) / 2, "%s", msg1);
      if (msg2)
	mvwprintw(win, 3, (width - strlen(msg2)) / 2, "%s", msg2);
      if (msg3)
	mvwprintw(win, 4, (width - strlen(msg3)) / 2, "%s", msg3);

      if (btn_selected == 0)
	{
	  wattron(win, A_REVERSE);
	  mvwprintw(win, height - 3, width / 2 - 10, "[ YES ]");
	  wattroff(win, A_REVERSE);
        }
      else
	mvwprintw(win, height - 3, width / 2 - 10, "[ YES ]");

      if (btn_selected == 1)
	{
	  wattron(win, A_REVERSE);
	  mvwprintw(win, height - 3, width / 2 + 3, "[ NO ]");
	  wattroff(win, A_REVERSE);
        }
      else
	mvwprintw(win, height - 3, width / 2 + 3, "[ NO ]");

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
show_error_popup(const char *msg1, const char *msg2)
{
  int height = 7 + (msg2?1:0);
  int width = strlen(msg1) + 6;
  if (msg2)
    if ((int)(strlen(msg2) + 6) > width)
      width = strlen(msg2) + 6;

  int start_y = (LINES - height) / 2 - 2;
  int start_x = (COLS - width) / 2;

  WINDOW *win = newwin(height, width, start_y, start_x);
  wbkgd(win, COLOR_PAIR(CP_WARNING));
  box(win, 0, 0);
  mvwprintw(win, 2, (width - strlen(msg1)) / 2, "%s", msg1);
  if (msg2)
    mvwprintw(win, 3, (width - strlen(msg2)) / 2, "%s", msg2);
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

int
choose_entry(int row, const char *options[], int num_options, int start)
{
  int selected = start;

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
	return -ECANCELED;
      else if (ch == KEY_UP)
	selected = (selected - 1 + num_options) % num_options;
      else if (ch == KEY_DOWN)
	selected = (selected + 1) % num_options;
      else if (ch == '\n' || ch == KEY_ENTER)
	return selected;
    }

  // we should never reach this
  return -2;
}

static int
read_config(const char *config, char **ret_device,
	    char **ret_url, char **ret_keymap)
{
  _cleanup_(econf_freeFilep) econf_file *key_file = NULL;
  _cleanup_free_ char *device = NULL;
  _cleanup_free_ char *url = NULL;
  _cleanup_free_ char *keymap = NULL;
  econf_err error;

  error = econf_readFile(&key_file, config,
			 "=", "#");
  if (error != ECONF_SUCCESS && error != ECONF_NOFILE)
    {
      show_error_popup("Failed to read config file:",
                       econf_errString(error));
      return -error;
    }

  error = econf_getStringValue(key_file, NULL, "rdii.device", &device);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return -error;

  error = econf_getStringValue(key_file, NULL, "rdii.url", &url);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return -error;

  error = econf_getStringValue(key_file, NULL, "rdii.keymap", &keymap);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return -error;

  if (ret_device)
    *ret_device = TAKE_PTR(device);
  if (ret_url)
    *ret_url = TAKE_PTR(url);
  if (ret_keymap)
    *ret_keymap = TAKE_PTR(keymap);

  return 0;
}

static int
show_main_menu(void)
{
  uint64_t minsize = 10 * 1000ULL * 1000 * 1000; // 10G min disk size
  _cleanup_free_ char *image_entry = NULL;
  _cleanup_free_ char *target_entry = NULL;
  _cleanup_free_ char *keymap_entry = NULL;
  const char *options[] = {
    "Select Image",
    "Select Target",
    "Select Keymap",
    "System Information",
    "Refresh",
    "Abort",
    "Start Installation"
  };
  int num_options = sizeof(options) / sizeof(options[0]);
  int selected = 0;
  _cleanup_free_ char *image = NULL;
  _cleanup_free_ char *device = NULL;

  // XXX error check missing
  // XXX keymap ignored
  read_config(rdii_config, &device, &image, NULL);
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

  while (1)
    {
      print_global_header_footer(NULL);
      print_title("Configuration Settings");

      selected = choose_entry(4, options, num_options, selected);
      switch(selected)
	{
	case 0: // Select Image
	  {
	    select_installation_source(image?image:"https://", &image);
	    if (!isempty(image))
	      {
		_cleanup_free_ char *cp = truncate_middle(image, COLS-22);

		image_entry = mfree(image_entry);
		if (asprintf(&image_entry, "Select Image (%s)", cp) < 0)
		  return -ENOMEM;
		options[0] = image_entry;
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
		options[1] = target_entry;
	      }
	  }
	  break;
	case 2: // Select Keymap
	  {
	    _cleanup_free_ char *keymap = NULL;
	    if (select_keymap(&keymap) == 0)
	      {
		keymap_entry = mfree(keymap_entry);
		if (asprintf(&keymap_entry, "Select Keymap (%s)",
			     strna(keymap)) < 0)
		  return -ENOMEM;
		options[2] = keymap_entry;
	      }
	  }
	  break;
	case 3: // System Information
	  show_sysinfo();
	  break;
	case 4: // Refresh
	  // loop will redraw screen
	  break;
	case 5: // Abort
	  return 0;
	  break;
	case 6: // Start Installation
	  if (isempty(image) || isempty(device))
	    show_error_popup("Installation image and target device are required!", NULL);

	  int r = run_installation(image, device);
	  if (r == 0)
	    return 0; // Quit, we are done
#if 0
	  else if (show_warning_popup("This will destroy all data, are you sure?",
				      image, device))
	    {
	      print_global_header_footer(NULL);
	      mvprintw(LINES / 2, (COLS - 22) / 2,
		       "Starting installation...");
	      refresh();
	      sleep(2); // Mock delay
	      return 0;
	    }
	  else
	    clear();
#endif
	  break;
	case -ECANCELED:
	  return 0;
	  break;
	default:
	  show_error_popup("Internal Error", NULL);
	  abort();
	  break;
	}
    }

  return 0;
}

static int
rdii_menu(void)
{
  int r;

  // For correctly rendering the double borders
  setlocale(LC_ALL, "");

  // Initialize ncurses
  initscr();
  cbreak();
  noecho();
  keypad(stdscr, TRUE); // Enable arrow keys
  curs_set(0);          // Hide cursor
  set_escdelay(25);     // Set escape delay to 25 milliseconds

  if (has_colors())
    init_colors();

  show_splash_screen();
  r = show_main_menu();

  endwin();

  return r;
}

static char*
rm_rf_and_free(char *p)
{
  int r;

  r = rm_rf(p);

  if (r < 0)
    fprintf(stderr, "Removal of '%s' failed: %s\n", p, strerror(-r));

  return mfree(p);
}

static inline void
rm_rf_and_freep(char **p)
{
  if (*p)
    *p = rm_rf_and_free(*p);
}

int
main(void)
{
  _cleanup_(rm_rf_and_freep) char *rdii_tmp_dir_cleanup = NULL;
  int r;

  if (getuid())
    rdii_log = "rdii.log";
  r = log_init(rdii_log);
  if (r < 0)
    {
      fprintf(stderr, "Error initializing log file (%s): %s\n",
	      rdii_log, strerror(-r));
      return -r;
    }

  LOG_INFO("rdi-installer started");

  const char *tmpdir_template = "/tmp/rdi-installer-XXXXXX";
  r = mkdtemp_malloc(tmpdir_template, &rdii_tmp_dir_cleanup);
  if (r < 0)
    {
      LOG_ERROR("Failed to create temporary directory (%s): %s",
		tmpdir_template, strerror(-r));
      show_error_popup("Failed to create temporary directory:",
		       strerror(-r));
      return -r;
    }
  // we cannot make rdii_tmp_dir_cleanup global because of _cleanup_
  rdii_tmp_dir = rdii_tmp_dir_cleanup;

  r = rdii_menu();

  LOG_INFO("rdi-installer stopped (retval=%i)", r);

  log_close();

  return r;
}
