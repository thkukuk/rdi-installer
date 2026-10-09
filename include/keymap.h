// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Dynamic list of available system keymaps, populated by load_system_keymaps()
extern char **all_keymaps;
extern int total_keymaps;

extern int set_keymap(const char *keymap);
extern int get_vconsole_keymap(char **ret);
extern int load_system_keymaps(void);
