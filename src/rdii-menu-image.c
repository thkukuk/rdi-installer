// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "basics.h"
#include "rdii-menu.h"

#define OFFSET 2

int
select_installation_source(const char *prefill, char **ret)
{
  char url[1024];

  if (prefill)
    {
      if (strlen(prefill) + 1 > sizeof(url))
	return -EOVERFLOW;

      strcpy(url, prefill);
    }
  else
    url[0] = '\0';

  int current_pos = strlen(url);

  print_global_header_footer(NULL);
  print_title("Please enter the image URL");

  mvprintw(4, 0, "> %s", url);
  move(4, current_pos+OFFSET); // Set cursor to the end of the prefix
  curs_set(1);
  refresh();

  while (1)
    {
      int ch = getch();
      if (ch == 27) // <ESC>
	{
	  curs_set(0);
	  return -ECANCELED;
	}
      else if (ch == '\n') // <RETURN>
	break;
      else if (ch == KEY_LEFT)
	{
	  if (current_pos > 0)
	    current_pos--;
	  move(4, current_pos+OFFSET);
	}
      else if (ch == KEY_RIGHT)
	{
	  if (current_pos < strlen(url))
	    current_pos++;
	  move(4, current_pos+OFFSET);
	}
      else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
	{
	  if (current_pos > 0)
	    {
	      current_pos--;
	      url[current_pos] = '\0';
	      mvprintw(4, current_pos+OFFSET, " ");
	      move(4, current_pos+OFFSET);
	    }
        }
      else if (current_pos < sizeof(url) - 1 && ch >= 32 && ch <= 126)
	{
	  url[current_pos] = ch;
	  current_pos++;
	  url[current_pos] = '\0';

	  // Echo the typed character to the screen
	  addch(ch);
        }
      refresh();
    }

  if (ret)
    {
      *ret = strdup(url);
      if (!*ret)
	return -ENOMEM;
    }

  curs_set(0);
  return 0;
}
