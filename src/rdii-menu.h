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

extern void print_global_header_footer(const char *addkeys);
extern void print_title(const char *title);
extern int show_warning_popup(const char *msg1, const char *msg2,
			      const char *msg3);
extern void show_error_popup(const char *errmsg);

extern void keywait(int y, int x, const char *text, int sec);

extern int select_keymap(char **device);
extern int select_target_device(uint64_t minsize, char **device);
extern int select_installation_source(const char *prefill, char **ret);
