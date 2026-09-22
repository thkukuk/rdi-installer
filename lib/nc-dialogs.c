// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>

#include "basics.h"
#include "logger.h"
#include "nc-dialogs.h"

static const char *header_title = NULL;

// Confirmation and timeout behaviour for the popups below, configurable via
// rdii.autoinstall.confirm_infos / confirm_warnings / confirm_errors /
// popup_timeout. Defaults reproduce the previous, purely interactive
// behaviour: every popup is shown and blocks until the user responds.
bool confirm_infos = true;
bool confirm_warnings = true;
bool confirm_errors = true;
int popup_timeout = 0; // 0 = wait forever

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
print_global_header_footer(const char *addkeys, const bool selection)
{
  MSG_FUNC("addkeys='%s'", strempty(addkeys));

  clear();
  // Draw Header (Green on Blue)
  attron(COLOR_PAIR(CP_HEADER) | A_BOLD);
  mvhline(0, 0, ' ', COLS); // Fill the top line background
  mvprintw(0, (COLS - strlen(strempty(header_title))) / 2, "%s", strempty(header_title));
  attroff(COLOR_PAIR(CP_HEADER) | A_BOLD);

  // Draw Footer
  const char *footer_text = (selection ? "Up/Down: Navigate | Enter: Select | ESC: Abort/Quit" :
                             "ESC: Abort/Quit");
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
  MSG_FUNC("title='%s'", strna(title));

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

  // Only auto-dismiss when confirmation isn't required; otherwise wait
  // indefinitely for an explicit answer, regardless of popup_timeout.
  if (!confirm_warnings && popup_timeout > 0)
    wtimeout(win, popup_timeout * 1000);

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
      if (key == ERR) // popup_timeout expired without any input
	{
	  choice = 1; // Confirmation not required: default to NO for safety
	  break;
	}
      else if (key == KEY_LEFT || key == KEY_RIGHT || key == '\t')
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

  // Only auto-dismiss when confirmation isn't required; otherwise wait
  // indefinitely for an explicit answer, regardless of popup_timeout.
  if (!confirm_errors && popup_timeout > 0)
    wtimeout(win, popup_timeout * 1000);

  while (1)
    {
      // Handle input locally inside the popup
      int key = wgetch(win);
      if (key == '\n' || key == 27 || key == ERR) // RETURN || ESC || timeout
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
  // Only auto-dismiss when confirmation isn't required; otherwise wait
  // indefinitely for an explicit answer, regardless of popup_timeout.
  int timeout_ms = (!confirm_infos && popup_timeout > 0) ? popup_timeout * 1000 : -1;

  while (timeout_ms < 0 || elapsed_ms < timeout_ms)
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
static int
clamp_scroll_offset(int offset, int line_count, int text_win_h)
{
  int max_offset = line_count - text_win_h;
  if (max_offset < 0) max_offset = 0;
  if (offset > max_offset) offset = max_offset;
  if (offset < 0) offset = 0;
  return offset;
}

// Appends a wrapped line (given as a slice of a wide-character string, so
// multibyte characters can never be split across two lines) to a growable
// array of multibyte C strings.  Returns 0 and leaves *lines_ptr/*cap_ptr
// untouched on allocation failure.
static int
append_wrapped_line(char ***lines_ptr, int *cap_ptr, int *count_ptr,
                     const wchar_t *wstr, size_t wlen)
{
  if (*count_ptr == *cap_ptr)
    {
      int new_cap = *cap_ptr * 2;
      char **grown = realloc(*lines_ptr, sizeof(char *) * new_cap);
      if (!grown)
        return 0;
      *lines_ptr = grown;
      *cap_ptr = new_cap;
    }

  wchar_t *wtmp = malloc((wlen + 1) * sizeof(wchar_t));
  if (!wtmp)
    return 0;
  memcpy(wtmp, wstr, wlen * sizeof(wchar_t));
  wtmp[wlen] = L'\0';

  size_t mb_len = wcstombs(NULL, wtmp, 0);
  if (mb_len == (size_t)-1)
    {
      free(wtmp);
      return 0;
    }

  char *mb_buf = malloc(mb_len + 1);
  if (!mb_buf)
    {
      free(wtmp);
      return 0;
    }
  wcstombs(mb_buf, wtmp, mb_len + 1);
  free(wtmp);

  (*lines_ptr)[*count_ptr] = mb_buf;
  (*count_ptr)++;
  return 1;
}

void show_help_dialog(const char *title, const char *text) {
  int max_y, max_x;
  getmaxyx(stdscr, max_y, max_x);

  if (!text)
    return;

  // Use maximum available width (with a 2-character margin on each side)
  // and up to 80% screen height for better vertical proportions.
  int width = max_x - 2;
  int height = max_y * 0.8;

  // Fallback bounds for smaller terminals
  if (width < 20) width = max_x;
  if (height < 8) height = 8;
  if (height > max_y) height = max_y;

  int start_y = (max_y - height) / 2;
  int start_x = (max_x - width) / 2;

  int prev_cursor = curs_set(0);

  // Save current mouse mask and enable mouse events
  mmask_t old_mouse_mask;
  mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, &old_mouse_mask);

  // Create main window.  The text is rendered into a pad so that
  // scrolling only has to move the pad's viewport instead of
  // re-blitting every visible line by hand on each redraw.
  WINDOW *help_win = newwin(height, width, start_y, start_x);
  int text_win_h = height - 4;
  int text_win_w = width - 4;
  if (text_win_h < 1) text_win_h = 1;
  if (text_win_w < 1) text_win_w = 1;
  int text_y = start_y + 2;
  int text_x = start_x + 2;

  keypad(help_win, TRUE);

  // --- Convert to wide characters first, so word-wrapping below
  // operates on whole characters and can never split a multibyte
  // UTF-8 sequence across two lines. ---
  size_t wtext_len = mbstowcs(NULL, text, 0);
  wchar_t *wtext = NULL;
  if (wtext_len == (size_t)-1)
    {
      // Invalid sequence for the current locale: fall back to widening
      // the raw bytes so the text is still shown instead of lost.
      wtext_len = strlen(text);
      wtext = malloc((wtext_len + 1) * sizeof(wchar_t));
      if (wtext)
        {
          for (size_t i = 0; i < wtext_len; i++)
            wtext[i] = (unsigned char)text[i];
          wtext[wtext_len] = L'\0';
        }
    }
  else
    {
      wtext = malloc((wtext_len + 1) * sizeof(wchar_t));
      if (wtext)
        mbstowcs(wtext, text, wtext_len + 1);
    }

  if (!wtext)
    {
      MSG_ERROR("show_help_dialog: out of memory converting text");
      delwin(help_win);
      mousemask(old_mouse_mask, NULL);
      curs_set(prev_cursor);
      return;
    }

  // --- Line Wrapping Logic ---
  int lines_cap = 64;
  char **lines = malloc(sizeof(char *) * lines_cap);
  int line_count = 0;
  int oom = (lines == NULL);

  const wchar_t *wptr = wtext;
  while (!oom && *wptr)
    {
      if (*wptr == L'\n')
        {
          if (!append_wrapped_line(&lines, &lines_cap, &line_count, L"", 0))
            oom = 1;
          else
            wptr++;
          continue;
        }

      int len = 0;
      int break_point = -1;

      while (wptr[len] && wptr[len] != L'\n' && len < text_win_w)
        {
          if (iswspace(wptr[len]))
            {
              break_point = len;
            }
          len++;
        }

      if (len == text_win_w && wptr[len] != L'\0' && wptr[len] != L'\n' && break_point > 0)
        {
          len = break_point;
        }

      if (!append_wrapped_line(&lines, &lines_cap, &line_count, wptr, (size_t)len))
        {
          oom = 1;
          continue;
        }

      wptr += len;
      if (*wptr == L' ' || *wptr == L'\n')
        {
          wptr++;
        }
    }

  free(wtext);

  if (oom)
    {
      MSG_ERROR("show_help_dialog: out of memory wrapping text");
      for (int i = 0; i < line_count; i++)
        free(lines[i]);
      free(lines);
      delwin(help_win);
      mousemask(old_mouse_mask, NULL);
      curs_set(prev_cursor);
      return;
    }

  // Render the wrapped lines into a pad once; the event loop below
  // only ever changes which slice of the pad is visible.
  int pad_h = line_count > text_win_h ? line_count : text_win_h;
  WINDOW *pad = newpad(pad_h, text_win_w);
  if (!pad)
    {
      MSG_ERROR("show_help_dialog: newpad() failed");
      for (int i = 0; i < line_count; i++)
        free(lines[i]);
      free(lines);
      delwin(help_win);
      mousemask(old_mouse_mask, NULL);
      curs_set(prev_cursor);
      return;
    }

  for (int i = 0; i < line_count; i++)
    {
      mvwprintw(pad, i, 0, "%s", lines[i]);
      free(lines[i]);
    }
  free(lines);

  int scroll_offset = 0;
  int ch;

  // --- Main Event Loop ---
  while (1)
    {
      werase(help_win);
      box(help_win, 0, 0);

      if (title)
        {
          int title_x = (width - (int)strlen(title) - 2) / 2;
          if (title_x < 0) title_x = 0;
          mvwprintw(help_win, 0, title_x, " %s ", title);
        }

      const char *footer = (line_count > text_win_h) ? " Up/Down/Wheel: Scroll | Press any key to exit " :
        "  Press any key to exit ";

      int footer_x = (width - (int)strlen(footer)) / 2;
      if (footer_x < 0) footer_x = 0;
      mvwprintw(help_win, height - 1, footer_x, "%s", footer);

      // Draw scroll bar if text overflows window height
      if (line_count > text_win_h)
        {
          int track_height = text_win_h;
          int bar_size = (track_height * text_win_h) / line_count;
          if (bar_size < 1) bar_size = 1;

          int max_offset = line_count - text_win_h;
          int bar_pos = (scroll_offset * (track_height - bar_size)) / max_offset;

          // Draw track
          for (int y = 0; y < track_height; y++)
            {
              mvwaddch(help_win, 2 + y, width - 1, ACS_VLINE);
            }

          // Draw scroll bar handle
          wattron(help_win, A_REVERSE);
          for (int y = 0; y < bar_size; y++)
            {
              mvwaddch(help_win, 2 + bar_pos + y, width - 1, ' ');
            }
          wattroff(help_win, A_REVERSE);
        }

      // Stage the frame (border/title/footer/scrollbar) first, then
      // the pad's visible slice on top, so a single doupdate() paints
      // both without the blank interior of help_win clobbering the pad.
      wnoutrefresh(help_win);
      pnoutrefresh(pad, scroll_offset, 0, text_y, text_x,
                   text_y + text_win_h - 1, text_x + text_win_w - 1);
      doupdate();

      // Input processing
      ch = wgetch(help_win);

      if (ch == KEY_MOUSE)
        {
          MEVENT event;
          if (getmouse(&event) == OK)
            {
              // Mouse Wheel Up
              if (event.bstate & BUTTON4_PRESSED)
                {
                  scroll_offset = clamp_scroll_offset(scroll_offset - 3, line_count, text_win_h);
                }
              // Mouse Wheel Down
              else if (event.bstate & BUTTON5_PRESSED)
                {
                  scroll_offset = clamp_scroll_offset(scroll_offset + 3, line_count, text_win_h);
                }
            }
        }
      else if (ch == KEY_UP)
        {
          scroll_offset = clamp_scroll_offset(scroll_offset - 1, line_count, text_win_h);
        }
      else if (ch == KEY_DOWN)
        {
          scroll_offset = clamp_scroll_offset(scroll_offset + 1, line_count, text_win_h);
        }
      else if (ch == KEY_NPAGE)
        { // Page Down
          scroll_offset = clamp_scroll_offset(scroll_offset + text_win_h, line_count, text_win_h);
        }
      else if (ch == KEY_PPAGE)
        { // Page Up
          scroll_offset = clamp_scroll_offset(scroll_offset - text_win_h, line_count, text_win_h);
        }
      else
        {
          // Exit loop on any non-navigation key press
          break;
        }
    }

  delwin(pad);

  // Restore previous mouse state
  mousemask(old_mouse_mask, NULL);

  // Cleanup ncurses resources
  delwin(help_win);

  curs_set(prev_cursor);
  touchwin(stdscr);
  refresh();
}

int
choose_entry(int row, const char *options[], int num_options, int start,
             const char *title, const char *help_text)
{
  int selected = start;
  int scroll_offset = 0;

  MSG_FUNC("row=%i, options[0]='%s', num_options=%i, start=%i, title=%s",
           row, options[0], num_options, start, title);

  print_global_header_footer((help_text ? "F1: Help" : NULL),
                             SELECTION);
  print_title((title ? title : ""));

  while (1)
    {
      int max_y, max_x;
      getmaxyx(stdscr, max_y, max_x);

      // Reserve the last line for the footer drawn by print_global_header_footer()
      int max_visible = max_y - row - 1;
      if (max_visible < 1) max_visible = 1;

      // Adjust scroll offset to keep 'selected' visible
      if (selected < scroll_offset)
        scroll_offset = selected;
      else if (selected >= scroll_offset + max_visible)
        scroll_offset = selected - max_visible + 1;

      // Leave room for the scrollbar column so long entries can't overwrite it
      bool show_scrollbar = num_options > max_visible;
      int text_width = (show_scrollbar ? max_x - 2 : max_x) - 2 - 3;
      if (text_width < 1) text_width = 1;

      // Render visible options
      for (int i = 0; i < max_visible; i++)
        {
          int item_idx = scroll_offset + i;
          int y = row + i;

          // Clear line to avoid leftover artifacts from previously rendered text
          move(y, 0);
          clrtoeol();

          if (item_idx < num_options)
            {
              if (item_idx == selected)
                {
                  attron(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                  mvprintw(y, 2, "-> %.*s", text_width, options[item_idx]);
                  attroff(COLOR_PAIR(CP_SELECTED) | A_BOLD);
                }
              else
                {
                  attron(COLOR_PAIR(CP_UNSELECTED));
                  mvprintw(y, 2, "   %.*s", text_width, options[item_idx]);
                  attroff(COLOR_PAIR(CP_UNSELECTED));
                }
            }
        }

      // Render scrollbar on right margin if list exceeds viewport
      if (show_scrollbar)
        {
          int sb_x = max_x - 2;
          int thumb_pos = (selected * (max_visible - 1)) / (num_options - 1);

          for (int i = 0; i < max_visible; i++)
            {
              mvaddch(row + i, sb_x, (i == thumb_pos) ? ACS_CKBOARD : '|');
            }
        }

      refresh();

      int ch = getch();
      if (ch == 27) // ESC
        {
          MSG_INFO("Canceled with ESC");
          return -ECANCELED;
        }
      else if (ch == KEY_UP)
        selected = (selected - 1 + num_options) % num_options;
      else if (ch == KEY_DOWN)
        selected = (selected + 1) % num_options;
      else if (ch == KEY_PPAGE) // Page Up
        selected = (selected - max_visible < 0) ? 0 : selected - max_visible;
      else if (ch == KEY_NPAGE) // Page Down
        selected = (selected + max_visible >= num_options) ? num_options - 1 : selected + max_visible;
      else if (ch == KEY_F1)
        show_help_dialog(title, help_text);
      else if (ch == '\n' || ch == KEY_ENTER)
        {
          MSG_INFO("Selected entry %i", selected);
          return selected;
        }
    }

  MSG_ERROR("quit while loop without return!");
  return -2;
}


void
init_ncurses(const char *title)
{
  // For correctly rendering the double borders
  setlocale(LC_ALL, "");

  MSG_FUNC("title='%s'", strna(title));

  header_title = title;

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
