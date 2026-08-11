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
#include "download.h"

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

  res = curl_global_init(CURL_GLOBAL_DEFAULT);
  if (res != CURLE_OK)
    {
      if (error)
        *error = curl_easy_strerror(res);
      return false;
    }

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
	      if (show_warning_popup("URL doesn't seem to be valid:",
				     error_msg, "Really use this URL?"))
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
  char *name;
  bool is_dir;
} entry;

/*
 * Custom cleanup function for array of 'entry' structs.
 */
static void free_entriesp(entry **p) {
    entry *entries = *p;
    if (!entries)
      return;

    /* Free inner heap-allocated strings first */
    for (size_t i = 0; entries[i].name != NULL; i++)
      free(entries[i].name);

    /* Free the array itself */
    free(entries);
}

#define _cleanup_entries_ _cleanup_(free_entriesp)

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
    if (endswith(name, exts[i]))
      return true;

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

/*
  Load directory entries, returns:
  >= 0 -> number of found entries
  < 0 -> -errno
*/
static int
load_directory(const char *path,
	       entry **entries_ret, size_t *entries_size_ret)
{
  _cleanup_entries_ entry *entries = NULL;
  _cleanup_closedir_ DIR *dir = NULL;
  struct dirent *ent;
  int capacity = 42;
  int count = 0;

  MSG_FUNC("path='%s'", path);

  entries = calloc(capacity, sizeof(entry));
  if (!entries)
    return -ENOMEM;

  dir = opendir(path);
  if (!dir)
    {
      int r = -errno;
      show_error_popup("Cannot open:", path, strerror(-r));
      return r;
    }

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

      if (count + 1 >= capacity) /* +1 to leave room for the NULL sentinel below */
	{
	  capacity *= 2;
	  entry *new_entries = realloc(entries, capacity * sizeof(entry));
	  if (!new_entries)
            return -ENOMEM;
	  entries = new_entries;
        }

      entries[count].name = strdup(ent->d_name);
      if (entries[count].name == NULL)
        return -ENOMEM;
      entries[count].is_dir = is_dir;
      count++;
    }

  /* explicitly terminate: realloc() growth above does not zero new
     memory, so we can't rely on it for the NULL sentinel free_entriesp() needs */
  entries[count].name = NULL;
  entries[count].is_dir = false;

  MSG_INFO("Starting qsort(%i)", count);
  qsort(&entries[0], count, sizeof(entry), compare_entries);
  MSG_INFO("Finished qsort()");

  *entries_ret = TAKE_PTR(entries);
  *entries_size_ret = capacity;

  MSG_INFO("Done (%i)", count);

  return count;
}

static int
get_file(const char *prefill, char **ret)
{
  _cleanup_free_ char *curr_dir = NULL;
  const char *prefill_base = NULL;
  int selected = 0;
  int r;

  MSG_FUNC("prefill='%s', ret='%s'", strna(prefill), strna(*ret));

  if (!ret)
    {
      MSG_ERROR("Internal error: variable ret not provided");
      return -EINVAL;
    }

  if (prefill)
    {
      curr_dir = strdup(prefill);
      if (!curr_dir)
	return -ENOMEM;
      curr_dir = dirname(curr_dir);

      const char *base = strrchr(prefill, '/');
      prefill_base = base ? base + 1 : prefill; /* If '/' is found, point past it; otherwise use full string */
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
      _cleanup_entries_ entry *entries = NULL;
      size_t size_entries = 0;
      _cleanup_free_ char **options = NULL;
      int num_options = 0;

      print_global_header_footer(NULL);
      print_title(curr_dir /*"Select Source Image"*/);

      MSG_INFO("Current directory='%s'", curr_dir);

      r = load_directory(curr_dir, &entries, &size_entries);
      if (r < 0)
        return r;

      // build options list for menu
      num_options = r;

      options = calloc(num_options, sizeof(char *));
      if (!options)
        return -ENOMEM;
      for (int i = 0; i < num_options; i++)
        {
          options[i] = entries[i].name;
          if (prefill_base && streq(options[i], prefill_base))
            selected = i;
        }

      selected = choose_entry(4, (const char **)options, num_options, selected);
      if (selected < 0) // canceld or error.
	{
	  MSG_INFO("get_file aborted: %i", -selected);
	  return selected;
	}

      MSG_INFO("Selected entry: %i (%s|%s)", selected, entries[selected].name,
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
	      MSG_INFO("curr_dir after strdup: '%s'", curr_dir);
	      if (!curr_dir)
		return -ENOMEM;
	      selected = 0;
	      prefill_base = NULL;
	    }
	  else
	    {
	      r = -errno;
	      MSG_ERROR("realpath(%s) failed: %s", new_path, strerror(-r));
	      return r;
	    }
	  MSG_INFO("New curr_dir='%s'", curr_dir);
	}
      else
	{
	  MSG_INFO("Selected image: '%s/%s'", curr_dir, entries[selected].name);
	  if (asprintf(ret, "%s/%s", curr_dir, entries[selected].name) < 0)
	    return -ENOMEM;
	  return 0;
	}
    }

  return -ENOSYS;
}

/*
 *  select an image from a remote SHA256SUMS listing
 */

/*
  Parse SHA256SUMS file and return the names of all supported images.
  Returns the number of found entries (>= 0), or a negative errno code.
*/
static int
parse_sha256sums(const char *path, char ***ret_names)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  char **names = NULL;
  int capacity = 0;
  int count = 0;

  MSG_FUNC("path='%s'", path);

  fp = fopen(path, "r");
  if (!fp)
    return -errno;

  while ((linelen = getline(&line, &linecap, fp)) > 0)
    {
      char *nl = strchr(line, '\n');
      if (nl)
	*nl = '\0';

      // A valid line is "<64 hex char hash><separator><filename>"
      if (linelen < 66)
	continue;

      char *name = strchr(line, ' ');
      while (*name == ' ')
	++name;

      if (isempty(name) || !is_supported_image(name))
	continue;

      if (count >= capacity)
	{
	  capacity = capacity ? capacity * 2 : 16;
	  char **new_names = realloc(names, capacity * sizeof(char *));
	  if (!new_names)
	    {
	      for (int i = 0; i < count; i++)
		free(names[i]);
	      free(names);
	      return -ENOMEM;
	    }
	  names = new_names;
	}

      names[count] = strdup(name);
      if (!names[count])
	{
	  for (int i = 0; i < count; i++)
	    free(names[i]);
	  free(names);
	  return -ENOMEM;
	}
      count++;
    }

  *ret_names = names;

  MSG_INFO("Found %i supported image(s) in '%s'", count, path);

  return count;
}

static int
get_url_from_list(char **ret)
{
  _cleanup_free_ char *sha256sums_url = NULL;
  _cleanup_free_ char *sha256sums_asc_url = NULL;
  _cleanup_free_ char *sha256sums_fn = NULL;
  _cleanup_free_ char *sha256sums_asc_fn = NULL;
  char **names = NULL;
  int num_names;
  int selected;
  int r = 0;

  MSG_FUNC();

  if (asprintf(&sha256sums_url, "%s/SHA256SUMS", rdii_download_server) < 0)
    return -ENOMEM;
  if (asprintf(&sha256sums_asc_url, "%s/SHA256SUMS.asc", rdii_download_server) < 0)
    return -ENOMEM;
  if (asprintf(&sha256sums_fn, "%s/SHA256SUMS", rdii_tmp_dir) < 0)
    return -ENOMEM;
  if (asprintf(&sha256sums_asc_fn, "%s/SHA256SUMS.asc", rdii_tmp_dir) < 0)
    return -ENOMEM;

  r = curl_download_file(sha256sums_url, sha256sums_fn);
  if (r != 0)
    {
      MSG_ERROR("Error downloading SHA256SUMS file:",
		r < 0 ? strerror(-r) : curl_easy_strerror(r));
      show_error_popup("Error downloading SHA256SUMS file:",
		       r < 0 ? strerror(-r) : curl_easy_strerror(r), NULL);
      return -EIO;
    }

  r = curl_download_file(sha256sums_asc_url, sha256sums_asc_fn);
  if (r != 0)
    {
      MSG_ERROR("Error downloading SHA256SUMS.asc file:",
		r < 0 ? strerror(-r) : curl_easy_strerror(r));
      if (!show_warning_popup("Error downloading SHA256SUMS.asc file:",
			      r < 0 ? strerror(-r) : curl_easy_strerror(r),
			      "Continue without signature verification?"))
	return -ECANCELED;
    }
  else
    {
      char *error_msg = NULL;
      if (!verify_signature(sha256sums_fn, sha256sums_asc_fn, &error_msg))
	{
	  MSG_WARN("Cannot verify SHA256SUMS signature: %s", error_msg);
	  if (!show_warning_popup("Cannot verify signature.", error_msg,
				  "Continue without signature verification?"))
	    {
	      MSG_ERROR("Canceld");
	      return -ECANCELED;
	    }
	}
    }

  num_names = parse_sha256sums(sha256sums_fn, &names);
  if (num_names < 0)
    {
      MSG_ERROR("Error parsing SHA256SUMS file: %s",
		strerror(-num_names));
      show_error_popup("Error parsing SHA256SUMS file:",
		       strerror(-num_names), NULL);
      return num_names;
    }
  if (num_names == 0)
    {
      MSG_INFO("No supported images found in %s", rdii_download_server);
      show_error_popup("No supported images found in SHA256SUMS file.",
			NULL, NULL);
      return -ENOENT;
    }

  print_global_header_footer(NULL);
  print_title("Select image from download server");

  selected = choose_entry(4, (const char **)names, num_names, 0);
  if (selected < 0)
    r = selected;
  else if (asprintf(ret, "%s/%s", rdii_download_server, names[selected]) < 0)
    r = -ENOMEM;
  else
    r = 0;

  for (int i = 0; i < num_names; i++)
    free(names[i]);
  free(names);

  return r;
}

void
select_installation_source(const char *prefill, char **ret)
{
  const char *options[] = {
    "Select image from download server",
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
	case 0: // select from downloaded list
	  r = get_url_from_list(ret);
	  if (r == 0)
	    return;
	  else if (r == -ECANCELED)
	    MSG_INFO("get_url_from_list() quit with %i: %s", r, strerror(-r));
	  else
	    MSG_ERROR("get_url_from_list() quit with %i: %s", r, strerror(-r));
	  break;
	case 1: // url
	  r = get_url(prefill?prefill:"https://", ret);
	  if (r == 0)
            return;
	  break;
	case 2: // local image
	  char **new = ret;
	  r = get_file(prefill, new);
	  if (r == 0)
	    {
	      *ret = *new;
	      return;
	    }
	  else
	    {
              if (r == -ECANCELED)
		MSG_INFO("get_file() quit with %i: %s", r, strerror(-r));
	      else
	        MSG_ERROR("get_file() quit with %i: %s", r, strerror(-r));
	    }
	  break;
	default:
          return;
	}
    }
}
