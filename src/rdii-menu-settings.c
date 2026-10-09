// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "basics.h"
#include "nc-dialogs.h"
#include "rdii-menu.h"
#include "logger.h"
#include "keymap.h"
#include "is_linux_vt.h"

#define FILTER_BUF_LEN 50
#define URL_BUF_LEN 256

// Top-to-bottom field order of the Settings screen
enum focus
{
  FOCUS_CHECKBOX,         // "Copy hosts ssh keys into installed system" checkbox
  FOCUS_DOWNLOAD_SERVER,  // download server URL field
  FOCUS_LIST,             // keymap filter + list (selected_index is valid)
};

static void
update_filter(const char *filter_buf, const char **filtered_keymaps,
             int *filtered_count, int *selected_index, int *list_offset)
{
  *filtered_count = 0;
  for (int i = 0; i < total_keymaps; i++)
    {
      if (filter_buf[0] == '\0' || strstr(all_keymaps[i], filter_buf))
        filtered_keymaps[(*filtered_count)++] = all_keymaps[i];
    }

  if (*selected_index >= *filtered_count)
    *selected_index = *filtered_count > 0 ? 0 : -1;

  *list_offset = 0; // Reset scroll on new filter input
}

static void
draw_settings(const char *filter_buf, const char **filtered_keymaps,
             int filtered_count, enum focus focus, int selected_index,
             int *list_offset, bool preserve_ssh_hostkey, const char *url_buf)
{
  print_global_header_footer("Tab: Switch Field", SELECTION);
  print_title("Settings");

  int checkbox_row = 4;
  bool checkbox_focused = (focus == FOCUS_CHECKBOX);
  attron(COLOR_PAIR(checkbox_focused ? CP_SELECTED : CP_UNSELECTED));
  mvprintw(checkbox_row, 2, "[%s] Copy hosts ssh keys into installed system", preserve_ssh_hostkey ? "x" : " ");
  attroff(COLOR_PAIR(checkbox_focused ? CP_SELECTED : CP_UNSELECTED));

  int download_row = checkbox_row + 2; // +1 blank separator line
  bool download_focused = (focus == FOCUS_DOWNLOAD_SERVER);
  attron(COLOR_PAIR(download_focused ? CP_SELECTED : CP_UNSELECTED));
  mvprintw(download_row, 2, "Image Download Server: ");
  attroff(COLOR_PAIR(download_focused ? CP_SELECTED : CP_UNSELECTED));
  attron(COLOR_PAIR(download_focused ? CP_SELECTED : CP_UNSELECTED) | A_UNDERLINE);
  printw("%s", url_buf);
  attroff(COLOR_PAIR(download_focused ? CP_SELECTED : CP_UNSELECTED) | A_UNDERLINE);

  int filter_row = download_row + 2; // +1 blank separator line
  bool filter_focused = (focus == FOCUS_LIST);
  attron(COLOR_PAIR(filter_focused ? CP_SELECTED : CP_UNSELECTED));
  mvprintw(filter_row, 2, "Keymap Filter: ");
  attroff(COLOR_PAIR(filter_focused ? CP_SELECTED : CP_UNSELECTED));
  attron(COLOR_PAIR(filter_focused ? CP_SELECTED : CP_UNSELECTED) | A_UNDERLINE);
  printw("%s", filter_buf);
  attroff(COLOR_PAIR(filter_focused ? CP_SELECTED : CP_UNSELECTED) | A_UNDERLINE);

  int list_start_y = filter_row + 2; // +1 blank separator line
  int visible_lines = list_viewport_height(list_start_y);

  int list_selected = (focus == FOCUS_LIST) ? selected_index : -1;
  if (focus == FOCUS_LIST)
    *list_offset = scroll_offset_for_selection(selected_index, *list_offset, visible_lines);

  if (total_keymaps == 0)
    mvprintw(list_start_y, 2, "(Error: Could not find system keymaps)");
  else if (filtered_count == 0)
    mvprintw(list_start_y, 2, "(No keymaps match filter)");
  else
    render_scrollable_list(list_start_y, visible_lines, filtered_keymaps,
                           filtered_count, list_selected, *list_offset);

  switch (focus)
    {
    case FOCUS_CHECKBOX:
      curs_set(0);
      break;
    case FOCUS_DOWNLOAD_SERVER:
      move(download_row, 25 + strlen(url_buf));
      curs_set(1);
      break;
    case FOCUS_LIST:
    default:
      move(filter_row, 17 + strlen(filter_buf));
      curs_set(1);
      break;
    }

  refresh();
}

int
settings(char **keymap, bool *preserve_ssh_hostkey)
{
  _cleanup_free_ const char **filtered_keymaps = NULL;
  char filter_buf[FILTER_BUF_LEN] = "";
  char url_buf[URL_BUF_LEN] = "";
  int filtered_count = 0;
  int selected_index = 0;
  int saved_list_index = 0; // remembers the list position while another field has focus
  int list_offset = 0;
  enum focus focus = FOCUS_CHECKBOX;
  int ch;
  int r;

  MSG_FUNC("keymap='%s', preserve_ssh_hostkey=%i",
	   strempty(*keymap), *preserve_ssh_hostkey);

  if (!all_keymaps)
    {
      r = load_system_keymaps();
      if (r < 0)
	return -1;
    }

  filtered_keymaps = malloc((total_keymaps == 0 ? 1 : total_keymaps)
			    * sizeof(const char *));
  if (!filtered_keymaps)
    return -ENOMEM;

  {
    _cleanup_free_ char *system_keymap = NULL;
    const char *filter_seed = *keymap;

    if (isempty(filter_seed))
      {
	get_vconsole_keymap(&system_keymap);
	filter_seed = system_keymap;
      }

    if (!isempty(filter_seed) && strlen(filter_seed) < FILTER_BUF_LEN)
      strcpy(filter_buf, filter_seed);
  }
  update_filter(filter_buf, filtered_keymaps, &filtered_count, &selected_index, &list_offset);

  if (!isempty(rdii_download_server) && strlen(rdii_download_server) < URL_BUF_LEN)
    strcpy(url_buf, rdii_download_server);

  while (1)
    {
      draw_settings(filter_buf, filtered_keymaps, filtered_count, focus,
                   selected_index, &list_offset, *preserve_ssh_hostkey, url_buf);
      ch = getch();
      curs_set(0);

      if (ch == 27) // ESC key: leave the Settings screen
	break;
      else if (ch == '\n' || ch == KEY_ENTER)
	{
	  if (focus == FOCUS_CHECKBOX)
	    *preserve_ssh_hostkey = !*preserve_ssh_hostkey;
	  else if (focus == FOCUS_DOWNLOAD_SERVER)
	    {
	      const char *error_msg = NULL;

	      if (isempty(url_buf))
		continue;

	      if (!url_is_valid(url_buf, &error_msg) &&
		  !show_warning_popup("URL doesn't seem to be valid:",
				     error_msg, "Really use this download server?"))
		continue;

	      char *new_server = strdup(url_buf);
	      if (!new_server)
		return -ENOMEM;
	      rdii_download_server = new_server; // intentionally not freeing the previous value
	    }
	  else if (focus == FOCUS_LIST && filtered_count > 0)
	    {
	      if (is_linux_vt() ||
		  show_warning_popup("Keymaps can only be configured directly on a virtual console.",
				     NULL, "Continue?"))
		{
		  if (set_keymap(filtered_keymaps[selected_index]) == 0)
		    {
		      *keymap = mfree(*keymap);
		      *keymap = strdup(filtered_keymaps[selected_index]);
		      if (!*keymap)
			return -ENOMEM;
		    }
		}
	    }
	}
      else if (ch == KEY_UP)
	{
	  if (focus == FOCUS_LIST)
	    {
	      if (selected_index > 0)
		selected_index--;
	      else
		{
		  saved_list_index = selected_index;
		  focus = FOCUS_DOWNLOAD_SERVER;
		}
	    }
	  else if (focus == FOCUS_DOWNLOAD_SERVER)
	    focus = FOCUS_CHECKBOX;
	  // FOCUS_CHECKBOX is the top-most field, nothing above it
	}
      else if (ch == KEY_DOWN)
	{
	  if (focus == FOCUS_CHECKBOX)
	    focus = FOCUS_DOWNLOAD_SERVER;
	  else if (focus == FOCUS_DOWNLOAD_SERVER)
	    {
	      focus = FOCUS_LIST;
	      selected_index = (filtered_count > 0) ?
		(saved_list_index < filtered_count ? saved_list_index : 0) : -1;
	    }
	  else if (selected_index < filtered_count - 1)
	    selected_index++;
	  // else: already at the bottom of the list, the last field
	}
      else if (ch == '\t') // Tab: cycle focus forward
	{
	  if (focus == FOCUS_CHECKBOX)
	    focus = FOCUS_DOWNLOAD_SERVER;
	  else if (focus == FOCUS_DOWNLOAD_SERVER)
	    {
	      focus = FOCUS_LIST;
	      selected_index = (filtered_count > 0) ?
		(saved_list_index < filtered_count ? saved_list_index : 0) : -1;
	    }
	  else
	    {
	      saved_list_index = selected_index;
	      focus = FOCUS_CHECKBOX;
	    }
	}
      else if (ch == KEY_BTAB) // Shift-Tab: cycle focus backward
	{
	  if (focus == FOCUS_CHECKBOX)
	    {
	      focus = FOCUS_LIST;
	      selected_index = (filtered_count > 0) ?
		(saved_list_index < filtered_count ? saved_list_index : 0) : -1;
	    }
	  else if (focus == FOCUS_LIST)
	    {
	      saved_list_index = selected_index;
	      focus = FOCUS_DOWNLOAD_SERVER;
	    }
	  else
	    focus = FOCUS_CHECKBOX;
	}
      else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
	{
	  if (focus == FOCUS_LIST && strlen(filter_buf) > 0)
	    {
	      filter_buf[strlen(filter_buf) - 1] = '\0';
	      update_filter(filter_buf, filtered_keymaps, &filtered_count,
			    &selected_index, &list_offset);
	    }
	  else if (focus == FOCUS_DOWNLOAD_SERVER && strlen(url_buf) > 0)
	    url_buf[strlen(url_buf) - 1] = '\0';
	}
      else if (isprint(ch))
	{
	  if (focus == FOCUS_LIST && strlen(filter_buf) < FILTER_BUF_LEN - 1)
	    {
	      int len = strlen(filter_buf);
	      filter_buf[len] = (char)ch;
	      filter_buf[len + 1] = '\0';
	      update_filter(filter_buf, filtered_keymaps, &filtered_count,
			    &selected_index, &list_offset);
	    }
	  else if (focus == FOCUS_DOWNLOAD_SERVER && strlen(url_buf) < URL_BUF_LEN - 1)
	    {
	      int len = strlen(url_buf);
	      url_buf[len] = (char)ch;
	      url_buf[len + 1] = '\0';
	    }
	}
    }

  return 0;
}
