// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include "basics.h"
#include "nc-dialogs.h"
#include "keymap.h"
#include "select_keymap.h"
#include "logger.h"

#define MAX_FILTER_LEN 50
const char **filtered_keymaps = NULL;
int filtered_count = 0;
int selected_index = 0;
int list_offset = 0;
char filter_buf[MAX_FILTER_LEN] = "";

static void
update_filter(void)
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
draw_ui(void)
{
  print_global_header_footer(NULL, SELECTION);
  print_title("Keyboard Settings");

  attron(COLOR_PAIR(CP_UNSELECTED));
  mvprintw(4, 2, "Filter: ");
  attroff(COLOR_PAIR(CP_UNSELECTED));
  attron(COLOR_PAIR(CP_UNSELECTED) | A_UNDERLINE);
  printw("%s", filter_buf);
  attroff(COLOR_PAIR(CP_UNSELECTED) | A_UNDERLINE);

  int list_start_y = 6;
  int visible_lines = list_viewport_height(list_start_y);

  if (selected_index != -1)
    list_offset = scroll_offset_for_selection(selected_index, list_offset, visible_lines);

  if (total_keymaps == 0)
    mvprintw(list_start_y, 2, "(Error: Could not find system keymaps)");
  else if (filtered_count == 0)
    mvprintw(list_start_y, 2, "(No keymaps match filter)");
  else
    render_scrollable_list(list_start_y, visible_lines, filtered_keymaps,
                           filtered_count, selected_index, list_offset);
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
