// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <ftw.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <libeconf.h>

#include "basics.h"
#include "rdii-menu.h"

static int
get_vconsole_keymap(char **ret)
{
  _cleanup_(econf_freeFilep) econf_file *key_file = NULL;
  _cleanup_free_ char *keymap = NULL;
  econf_err error;

  error = econf_readFile(&key_file, "/etc/vconsole.conf", "=", "#");
  if (error != ECONF_SUCCESS)
    {
      show_error_popup("Failed to read /etc/vconsole.conf:",
		       econf_errString(error));
      return -error;
    }

  error = econf_getStringValue(key_file, NULL, "KEYMAP", &keymap);
  if (error != ECONF_SUCCESS)
    {
      if (error == ECONF_NOKEY)
	return 0;
      else
	return -error;
    }

  *ret = TAKE_PTR(keymap);
  return 0;
}

extern char **environ;

static int
set_keymap(const char *keymap)
{
  pid_t pid;
  int status;
  int r;

  char *argv[] = {"loadkeys", (char *)keymap, NULL};

  print_global_header_footer(NULL);
  move(2,2);
  refresh();

  r = posix_spawnp(&pid, "loadkeys", NULL, NULL, argv, environ);
  if (r != 0)
    {
      fprintf(stderr, "Failed to spawn loadkeys: %s\n", strerror(r)); // XXX
      return -r;
    }

  if (waitpid(pid, &status, 0) == -1)
    {
      r = errno;
      perror("waitpid failed"); // XXX
      return -r;
    }

  if (WIFEXITED(status))
    {
      if (WEXITSTATUS(status)) // loadkeys quit with error
	keywait(8, 0, NULL, 0);
      return WEXITSTATUS(status);
    }
  else
    {
      fprintf(stderr, "loadkeys terminated abnormally\n"); // XXX
      return -1;
    }
}

// Dynamic list of available keymaps
char **all_keymaps = NULL;
int total_keymaps = 0;
int capacity_keymaps = 0;

#define MAX_FILTER_LEN 50
const char **filtered_keymaps = NULL;
int filtered_count = 0;
int selected_index = 0;
int list_offset = 0;
char filter_buf[MAX_FILTER_LEN] = "";

static int
compare_strings(const void *a, const void *b)
{
  return strcmp(*(const char **)a, *(const char **)b);
}

static int
process_file(const char *fpath, const struct stat *sb _unused_,
	     int typeflag, struct FTW *ftwbuf)
{
  if (typeflag != FTW_F) // If it's not a regular file
    return 0;

  const char *filename = fpath + ftwbuf->base;

  // Look for .map or .kmap extensions (ignoring the .gz part)
  if (strstr(filename, ".map") == NULL &&
      strstr(filename, ".kmap") == NULL)
    return 0;

  if (total_keymaps >= capacity_keymaps)
    {
      capacity_keymaps = capacity_keymaps == 0 ? 128 : capacity_keymaps * 2;
      all_keymaps = realloc(all_keymaps, capacity_keymaps * sizeof(char *));
    }

  // Duplicate and clean the filename
  _cleanup_free_ char *clean_name = strdup(filename);
  // XXX OOM

  // Strip the extension (.map or .kmap and anything after)
  char *dot = strstr(clean_name, ".kmap");
  if (!dot)
    dot = strstr(clean_name, ".map");
  if (dot)
    *dot = '\0';

  // Prevent exact duplicates in the list
  int is_duplicate = 0;
  for (int i = 0; i < total_keymaps; i++)
    {
      if (streq(all_keymaps[i], clean_name))
	{
	  is_duplicate = 1;
	  break;
	}
    }

  if (!is_duplicate)
    all_keymaps[total_keymaps++] = TAKE_PTR(clean_name);

  return 0;
}

static int
load_system_keymaps()
{
  int r;

  r = nftw("/usr/share/kbd/keymaps", process_file, 20, FTW_PHYS);
  if (r < 0)
    {
      show_error_popup("nftw('/usr/share/kbd/keymaps') failed", NULL);
      return -1;
    }

  if (total_keymaps > 1)
    qsort(all_keymaps, total_keymaps, sizeof(char *), compare_strings);

  return 0;
}

static void
update_filter()
{
  filtered_count = 0;
  for (int i = 0; i < total_keymaps; i++)
    {
      if (filter_buf[0] == '\0' ||
	  strstr(all_keymaps[i], filter_buf))
	filtered_keymaps[filtered_count++] = all_keymaps[i];
    }
  if (selected_index >= filtered_count)
    selected_index = filtered_count > 0 ? 0 : -1;

  list_offset = 0; // Reset scroll on new filter input
}

static void
draw_ui()
{
  print_global_header_footer(NULL);
  print_title("Keyboard Settings");

  attron(COLOR_PAIR(CP_UNSELECTED));
  mvprintw(4, 2, "Filter: ");
  attroff(COLOR_PAIR(CP_UNSELECTED));
  attron(COLOR_PAIR(CP_UNSELECTED) | A_UNDERLINE);
  printw("%s", filter_buf);
  attroff(COLOR_PAIR(CP_UNSELECTED) | A_UNDERLINE);

  int list_start_y = 6;
  int visible_lines = LINES - list_start_y - 2;

  // Adjust scrolling offset
  if (selected_index >= list_offset + visible_lines)
    list_offset = selected_index - visible_lines + 1;
  else if (selected_index < list_offset && selected_index != -1)
    list_offset = selected_index;

  if (total_keymaps == 0)
    mvprintw(list_start_y, 2, "(Error: Could not find system keymaps)");
  else if (filtered_count == 0)
    mvprintw(list_start_y, 2, "(No keymaps match filter)");
  else
    {
      for (int i = 0; i < visible_lines && (list_offset + i) < filtered_count; i++)
	{
	  int current_item = list_offset + i;

	  if (current_item == selected_index)
	    {
	      attron(COLOR_PAIR(CP_SELECTED));
	      mvprintw(list_start_y + i, 2, "-> %s", filtered_keymaps[current_item]);
	      attroff(COLOR_PAIR(CP_SELECTED));
            }
	  else
	    {
	      attron(COLOR_PAIR(CP_UNSELECTED));
	      mvprintw(list_start_y + i, 2, "   %s", filtered_keymaps[current_item]);
	      attroff(COLOR_PAIR(CP_UNSELECTED));
            }
        }
    }
  move(4, 10 + strlen(filter_buf));
  curs_set(1);
  refresh();
}

int
select_keymap(char **ret)
{
  _cleanup_free_ char *keymap = NULL;
  int ch;
  int running = 1;
  int r;

  // Dynamically fetch keymaps from the OS
  if (!all_keymaps)
    {
      r = load_system_keymaps();
      if (r < 0)
	return -1;
    }

  filtered_keymaps = malloc((total_keymaps == 0 ? 1 : total_keymaps)
			    * sizeof(char*));
  if (!filtered_keymaps)
    return -ENOMEM;

  get_vconsole_keymap(&keymap);
  if (keymap && strlen(keymap) < MAX_FILTER_LEN)
    strcpy(filter_buf, keymap);
  update_filter();

  while (running)
    {
      draw_ui();
      ch = getch();

      switch (ch)
	{
	case 27: // ESC key
	  keymap = mfree(keymap);
	  running = 0;
	  break;
	case '\n':
	case KEY_ENTER:
	  if (filtered_count > 0 && selected_index != -1)
	    {
	      keymap = mfree(keymap);
	      keymap = strdup(filtered_keymaps[selected_index]);
	      if (!keymap)
		return -ENOMEM;
	      running = 0;
	    }
	  break;
	case KEY_UP:
	  if (selected_index > 0)
	    selected_index--;
	  break;
	case KEY_DOWN:
	  if (selected_index < filtered_count - 1)
	    selected_index++;
	  break;
	case KEY_BACKSPACE:
	case 127:
	case '\b':
	  if (strlen(filter_buf) > 0)
	    {
	      filter_buf[strlen(filter_buf) - 1] = '\0';
	      update_filter();
	    }
	  break;
	default:
	  if (isprint(ch) && strlen(filter_buf) < MAX_FILTER_LEN - 1)
	    {
	      int len = strlen(filter_buf);
	      filter_buf[len] = (char)ch;
	      filter_buf[len + 1] = '\0';
	      update_filter();
	    }
	  break;
        }
      curs_set(0);
    }

  filtered_keymaps = mfree(filtered_keymaps);

  if (!isempty(keymap))
    {
      r = set_keymap(keymap);
      if (ret && r == 0)
	*ret = TAKE_PTR(keymap);
      return r;
    }

  return 0;
}
