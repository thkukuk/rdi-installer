// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <curl/curl.h>

#include "basics.h"
#include "rdii-menu.h"

#define OFFSET 2

/* verify if an URL exists
  (curl -o /dev/null --silent --show-error --head --fail --max-time $TIMEOUT "$URL") */
static bool
url_is_valid(const char *url, const char **error)
{
  long timeout = 5; // 5 seconds
  CURL *curl;
  CURLcode res;
  bool is_valid = false;

  curl_global_init(CURL_GLOBAL_DEFAULT); // XXX Don't ignore return code

  curl = curl_easy_init();
  if(curl)
    {
      curl_easy_setopt(curl, CURLOPT_URL, url);

      // --head
      curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
      // fail silently on HTTP errors >= 400
      curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
      curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);

      res = curl_easy_perform(curl);

      if(res == CURLE_OK)
	is_valid = true;
      else if (error)
	*error = curl_easy_strerror(res);

      curl_easy_cleanup(curl);
    }
  else if (error)
    *error = "curl_easy_init() failed";

  curl_global_cleanup();

  return is_valid;
}

static int
get_url(const char *prefill, char **ret)
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

  print_global_header_footer(NULL);
  print_title("Please enter the image URL");

  mvprintw(4, 0, "> ");
  curs_set(1);
  refresh();

  int width = COLS - 4;
  int pos = strlen(url);
  int offset = 0;

  while (1)
    {
      int len = strlen(url);

      if (pos < offset)
	offset = pos;
      if (pos >= offset + width)
	offset = pos - width + 1;

      mvprintw(4, OFFSET, "%-*.*s", width, width, url + offset);
      move (4, OFFSET + pos - offset);
      refresh();

      int ch = getch();
      if (ch == 27) // <ESC>
	{
	  curs_set(0);
	  return -ECANCELED;
	}
      else if (ch == '\n' || ch == KEY_ENTER) // <RETURN>
	{
	  const char *error_msg = NULL;

	  curs_set(0);
	  if (!url_is_valid(url, &error_msg))
	    {
	      if (show_warning_popup("URL doesn't seem to be valid:", error_msg, "Really use this URL?"))
		break;
	      // Redraw screen
	      print_global_header_footer(NULL);
	      print_title("Please enter the image URL");

	      mvprintw(4, 0, "> ");
	      curs_set(1);
	    }
	  else
	    break;
	}
      else if (ch == KEY_LEFT)
	{
	  if (pos > 0)
	    pos--;
	}
      else if (ch == KEY_RIGHT)
	{
	  if (pos < (int)strlen(url))
	    pos++;
	}
      else if (ch == KEY_HOME)
	pos = 0;
      else if (ch == KEY_END)
	pos = len;
      else if (ch == KEY_BACKSPACE || ch == 127 || ch == '\b')
	{
	  if (pos > 0)
	    {
	      memmove(&url[pos - 1], &url[pos], len - pos + 1);
	      pos--;
	    }
        }
      else if (ch == KEY_DC)
	{
	  if (pos < len)
	    memmove(&url[pos], &url[pos + 1], len - pos);
	}
      else if (pos < (int)sizeof(url) - 1 && isprint(ch))
	{
	  memmove(&url[pos + 1], &url[pos], len - pos + 1);
	  url[pos] = ch;
	  pos++;
        }
    }

  if (ret)
    {
      *ret = strdup(url);
      if (!*ret)
	return -ENOMEM;
    }

  return 0;
}

int
select_installation_source(const char *prefill, char **ret)
{
  const char *options[] = {
    "Provide URL",
    "Use file selection"
  };
  int num_options = sizeof(options) / sizeof(options[0]);
  int selected = 0;
  int r;

  while (1)
    {
      print_global_header_footer(NULL);
      print_title("Select Source Image");

      selected = choose_entry(4, options, num_options, selected);
      switch(selected)
	{
	case 0: // url
	  r = get_url(prefill, ret);
	  if (r == 0)
	    return 0;
	  break;
	case 1: // local image
	  break;
	default:
	  return 0;
	  break;
	}
    }
  return 0;
}
