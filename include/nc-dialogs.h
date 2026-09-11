// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <ncursesw/curses.h>

// Color Pair definitions
#define CP_HEADER 1
#define CP_SPLASH_BOX 2
#define CP_TITLE 3
#define CP_SELECTED 4
#define CP_UNSELECTED 5
#define CP_FOOTER 6
#define CP_WARNING 7

#define KEY_F1	(KEY_F0+(1))
#define SELECTION true
#define NO_SELECTION false

extern void print_global_header_footer(const char *addkeys, const bool selection);
extern void print_title(const char *title);
extern int show_warning_popup(const char *headline,
			      const char *descr_line1,
			      const char *descr_line2);
extern void show_error_popup(const char *headline,
			     const char *descr_line1,
			     const char *descr_line2);
extern void show_info_popup(const char *headline, const char *descr);

extern void show_help_dialog(const char *title, const char *text);

extern int choose_entry(int row, const char *options[], int num_options,
                        int start, const char *title, const char *help_text);
extern void init_ncurses(const char *title);
