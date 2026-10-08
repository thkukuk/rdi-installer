// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <ftw.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <libeconf.h>

#include "basics.h"
#include "exec_cmd.h"
#include "nc-dialogs.h"
#include "keymap.h"
#include "logger.h"

int
set_keymap(const char *keymap)
{
  int r;

  print_global_header_footer(NULL, NO_SELECTION);
  move(2,2);
  refresh();

  MSG_INFO("Set keymap to %s", keymap);
  r = exec_cmd(true, "loadkeys", "loadkeys", keymap, NULL);
  if (r != 0)
    show_error_popup("Cannot set keymap with the call 'loadkeys' to:", keymap, NULL);
  return r;
}

int
get_vconsole_keymap(char **ret)
{
  _cleanup_(econf_freeFilep) econf_file *key_file = NULL;
  _cleanup_free_ char *keymap = NULL;
  econf_err error;

  error = econf_readFile(&key_file, "/etc/vconsole.conf", "=", "#");
  if (error != ECONF_SUCCESS)
    {
      show_error_popup("Cannot read console keymap",
		       "Failed to read /etc/vconsole.conf:", econf_errString(error));
      return -error;
    }

  error = econf_getStringValue(key_file, NULL, "KEYMAP", &keymap);
  if (error != ECONF_SUCCESS)
    {
      if (error == ECONF_NOKEY)
	return 0;
      else
	return -error;
    }

  *ret = TAKE_PTR(keymap);
  return 0;
}

// Dynamic list of available keymaps
char **all_keymaps = NULL;
int total_keymaps = 0;
static int capacity_keymaps = 0;

static int
compare_strings(const void *a, const void *b)
{
  return strcmp(*(const char **)a, *(const char **)b);
}

static int
process_file(const char *fpath, const struct stat *sb _unused_,
	     int typeflag, struct FTW *ftwbuf)
{
  if (typeflag != FTW_F) // If it's not a regular file
    return 0;

  const char *filename = fpath + ftwbuf->base;

  // Look for .map or .kmap extensions (ignoring the .gz part)
  if (strstr(filename, ".map") == NULL &&
      strstr(filename, ".kmap") == NULL)
    return 0;

  if (total_keymaps >= capacity_keymaps)
    {
      int new_capacity = capacity_keymaps == 0 ? 128 : capacity_keymaps * 2;
      char **tmp = realloc(all_keymaps, new_capacity * sizeof(char *));
      if (!tmp)
        {
          MSG_ERROR("Out of memory!");
          return -ENOMEM;
        }
      all_keymaps = tmp;
      capacity_keymaps = new_capacity;
    }

  // Duplicate and clean the filename
  _cleanup_free_ char *clean_name = strdup(filename);
  if (!clean_name)
    {
      MSG_ERROR("Out of memory!");
      return -ENOMEM;
    }

  // Strip the extension (.map or .kmap and anything after)
  char *dot = strstr(clean_name, ".kmap");
  if (!dot)
    dot = strstr(clean_name, ".map");
  if (dot)
    *dot = '\0';

  // Prevent exact duplicates in the list
  int is_duplicate = 0;
  for (int i = 0; i < total_keymaps; i++)
    {
      if (streq(all_keymaps[i], clean_name))
	{
	  is_duplicate = 1;
	  break;
	}
    }

  if (!is_duplicate)
    all_keymaps[total_keymaps++] = TAKE_PTR(clean_name);

  return 0;
}

int
load_system_keymaps(void)
{
  int r;

  r = nftw("/usr/share/kbd/keymaps", process_file, 20, FTW_PHYS);
  if (r < 0)
    {
      show_error_popup("Cannot read available keymapts", "nftw('/usr/share/kbd/keymaps') failed", NULL);
      return -1;
    }

  if (total_keymaps > 1)
    qsort(all_keymaps, total_keymaps, sizeof(char *), compare_strings);

  return 0;
}
