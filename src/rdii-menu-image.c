// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <libgen.h>
#include <stdbool.h>
#include <curl/curl.h>

#include "basics.h"
#include "logger.h"
#include "rdii-menu.h"

#define OFFSET 2

/*
 *  URL as installation source
 */

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

/*
 *  local file as installation source
 */

typedef struct {
  char name[PATH_MAX]; // XXX use pointer to save memory, means create free function, too
  bool is_dir;
} entry;

static bool
is_supported_image(const char *name)
{
  const char *exts[] = {
    ".img",     ".raw",
    ".img.gz",  ".raw.gz",
    ".img.bz2", ".raw.bz2",
    ".img.xz",  ".raw.xz",
    ".img.zst", ".raw.zst"
  };
  const size_t num_exts = sizeof(exts) / sizeof(exts[0]);

  for (size_t i = 0; i < num_exts; i++)
    {
      if (endswith(name, exts[i]))
	return true;
    }
  return false;
}

// Sort directories first (with .. the very first), then alphabetically
static int
compare_entries(const void *a, const void *b)
{
  entry *entry_a = (entry *)a;
  entry *entry_b = (entry *)b;

  // ".." should always be at the top
  if (streq(entry_a->name, "..")) return -1;
  if (streq(entry_b->name, "..")) return 1;

  if (entry_a->is_dir != entry_b->is_dir)
    return entry_b->is_dir ? -1 : 1;

  return strcmp(entry_a->name, entry_b->name);
}

static inline void
closedirp(DIR **p)
{
  if (*p)
    {
      closedir(*p);
      *p = NULL;
    }
}

/*
  Load directory entries, returns:
  >= 0 -> number of found entries
  < 0 -> -errno
*/
static int
load_directory(const char *path,
	       entry **entries_ret, size_t *entries_size_ret)
{
  _cleanup_free_ entry *entries = NULL;
  _cleanup_(closedirp) DIR *dir;
  struct dirent *ent;
  int capacity = 42;
  int count = 0;

  LOG_FUNC("path='%s'", path);

  entries = malloc(capacity * sizeof(entry));
  if (!entries)
    return -ENOMEM;

  dir = opendir(path);
  if (!dir)
    return -errno;

  while ((ent = readdir(dir)) != NULL)
    {
      // Ignore current directory "."
      if (streq(ent->d_name, "."))
	continue;

      bool is_dir = false;

      if (ent->d_type == DT_DIR)
	is_dir = true;

      if (!is_dir && !is_supported_image(ent->d_name))
	continue;

      if (count >= capacity)
	{
	  capacity *= 2;
	  entry *new_entries = realloc(entries, capacity * sizeof(entry));
	  if (!new_entries)
	    return -ENOMEM;
	  entries = new_entries;
        }

      size_t result = strlcpy(entries[count].name, ent->d_name, sizeof(entries[count].name));
      if (result >= sizeof(entries[count].name))
	return -ENAMETOOLONG;
      entries[count].is_dir = is_dir;
      count++;
    }

  LOG_INFO("Starting qsort(%i)", count);
  qsort(&entries[0], count, sizeof(entry), compare_entries);
  LOG_INFO("Finished qsort()");

  *entries_ret = TAKE_PTR(entries);
  *entries_size_ret = capacity;

  LOG_INFO("Done (%i)", count);

  return count;
}

static int
get_file(const char *prefill, char **ret)
{
  _cleanup_free_ char **options = NULL;
  _cleanup_free_ entry *entries = NULL;
  _cleanup_free_ char *curr_dir = NULL;
  size_t size_entries = 0;
  int selected = 0;
  int num_options = 0;
  int r;

  LOG_FUNC("prefill='%s', ret='%s'", strna(prefill), strna(*ret));

  if (!ret)
    {
      LOG_ERROR("Internal error: variable ret not provided");
      return -EINVAL;
    }

  if (prefill)
    {
      curr_dir = strdup(prefill);
      if (!curr_dir)
	return -ENOMEM;
      curr_dir = dirname(curr_dir);
      // XXX get image name and set "selected"
    }
  else
    {
      if (access("/images", R_OK) == 0)
	curr_dir = strdup("/images");
      else
	curr_dir = strdup("/");
      if (!curr_dir)
	return -ENOMEM;
    }

  while (1)
    {
      print_global_header_footer(NULL);
      print_title(curr_dir /*"Select Source Image"*/);

      LOG_INFO("Current directory='%s'", curr_dir);

      if (entries)
	entries = mfree(entries);

      r = load_directory(curr_dir, &entries, &size_entries);
      if (r < 0)
	return r;

      // build options list for menu
      num_options = r;
      if (options)
	options = mfree(options);

      options = calloc(num_options, sizeof(char *));
      for (int i = 0; i < num_options; i++)
	options[i] = basename(entries[i].name);

      selected = choose_entry(4, (const char **)options, num_options, selected);
      if (selected < 0) // canceld or error.
	{
	  LOG_INFO("get_file aborted: %i", -selected);
	  return selected;
	}

      LOG_INFO("Selected entry: %i (%s|%s)", selected, entries[selected].name,
	       strbool(entries[selected].is_dir));

      if (entries[selected].is_dir)
	{
	  _cleanup_free_ char *new_path = NULL;
	  if (asprintf(&new_path, "%s/%s", curr_dir, entries[selected].name) < 0)
	    return -ENOMEM;

	  char resolved_path[PATH_MAX];
	  if (realpath(new_path, resolved_path))
	    {
	      curr_dir = mfree(curr_dir);
	      curr_dir = strdup(resolved_path);
	      LOG_INFO("curr_dir after strdup: '%s'", curr_dir);
	      if (!curr_dir)
		return -ENOMEM;
	      selected = 0;
	    }
	  else
	    {
	      r = -errno;
	      LOG_ERROR("realpath(%s) failed: %s", new_path, strerror(-r));
	      return r;
	    }
	  LOG_INFO("New curr_dir='%s'", curr_dir);
	}
      else
	{
	  LOG_INFO("Selected image: '%s/%s'", curr_dir, entries[selected].name);
	  if (asprintf(ret, "%s/%s", curr_dir, entries[selected].name) < 0)
	    return -ENOMEM;
	  return 0;
	}
    }

  return -ENOSYS;
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
	  r = get_url(prefill?prefill:"https://", ret);
	  if (r == 0)
	    return 0;
	  break;
	case 1: // local image
	  char **new = ret;
	  r = get_file(prefill, new);
	  if (r == 0)
	    {
	      *ret = *new;
	      return 0;
	    }
	  else
	    LOG_ERROR("get_file() quit with %i: %s", r, strerror(-r));
	  break;
	default:
	  return 0;
	  break;
	}
    }
  return 0;
}
