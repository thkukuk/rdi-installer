// SPDX-License-Identifier: LGPL-2.1-or-later

#include "config.h"

#include "cmdline-util.h"

int
foreach_cmdline_arg(char *line, cmdline_arg_cb cb, void *userdata)
{
  char *cp = line;
  char *arg_start = cp;
  int in_quote = 0;

  while (*cp)
    {
      if (*cp == '"')
	in_quote = !in_quote;

      if (cp[1] == '\0' || (*cp == ' ' && !in_quote))
	{
	  int r;

	  if (*cp == ' ')
	    *cp = '\0'; // Terminate current arg

	  r = cb(arg_start, userdata);
	  if (r != 0)
	    return r;

	  arg_start = cp + 1;
	}
      cp++;
    }

  return 0;
}
