//SPDX-License-Identifier: LGPL-2.1-or-later

#pragma once

/* Callback invoked once per whitespace-separated token found by
   foreach_cmdline_arg(). Return 0 to keep iterating, non-zero to stop;
   that value is then returned by foreach_cmdline_arg(). */
typedef int (*cmdline_arg_cb)(char *arg, void *userdata);

/* Splits "line" in place on spaces, honoring double-quoted sections (as
   used by /proc/cmdline and rdii-networkd's config file syntax), and
   invokes cb once for every token found. Returns 0 if all tokens were
   processed, or the first non-zero value returned by cb. */
extern int foreach_cmdline_arg(char *line, cmdline_arg_cb cb, void *userdata);
