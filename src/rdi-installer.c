// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <string.h>
#include <libeconf.h>

#include "basics.h"
#include "rm_rf.h"
#include "tmpfile-util.h"
#include "tmpfile-util.h"
#include "rdii-menu.h"
#include "logger.h"

const char *rdii_config = "/run/rdi-installer/rdii-config";
const char *rdii_tmp_dir = NULL;
const char *rdii_log = "/var/log/rdi-installer.log";

static int
read_config(const char *config, char **ret_device,
	    char **ret_url, char **ret_keymap)
{
  _cleanup_(econf_freeFilep) econf_file *key_file = NULL;
  _cleanup_free_ char *device = NULL;
  _cleanup_free_ char *url = NULL;
  _cleanup_free_ char *keymap = NULL;
  econf_err error;

  error = econf_readFile(&key_file, config,
			 "=", "#");
  if (error != ECONF_SUCCESS && error != ECONF_NOFILE)
    {
      show_error_popup("Failed to read config file:",
                       econf_errString(error));
      return -error;
    }

  error = econf_getStringValue(key_file, NULL, "rdii.device", &device);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return -error;

  error = econf_getStringValue(key_file, NULL, "rdii.url", &url);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return -error;

  error = econf_getStringValue(key_file, NULL, "rdii.keymap", &keymap);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return -error;

  if (ret_device)
    *ret_device = TAKE_PTR(device);
  if (ret_url)
    *ret_url = TAKE_PTR(url);
  if (ret_keymap)
    *ret_keymap = TAKE_PTR(keymap);

  return 0;
}

static char*
rm_rf_and_free(char *p)
{
  int r;

  r = rm_rf(p);

  if (r < 0)
    fprintf(stderr, "Removal of '%s' failed: %s\n", p, strerror(-r));

  return mfree(p);
}

static inline void
rm_rf_and_freep(char **p)
{
  if (*p)
    *p = rm_rf_and_free(*p);
}

int
main(void)
{
  _cleanup_(rm_rf_and_freep) char *rdii_tmp_dir_cleanup = NULL;
  _cleanup_free_ char *image = NULL;
  _cleanup_free_ char *device = NULL;
  int r;

  if (getuid())
    rdii_log = "rdii.log";
  r = log_init(rdii_log);
  if (r < 0)
    {
      fprintf(stderr, "Error initializing log file (%s): %s\n",
	      rdii_log, strerror(-r));
      return -r;
    }

  LOG_INFO("rdi-installer started");

  // XXX error check missing
  // XXX keymap ignored
  read_config(rdii_config, &device, &image, NULL);

  const char *tmpdir_template = "/tmp/rdi-installer-XXXXXX";
  r = mkdtemp_malloc(tmpdir_template, &rdii_tmp_dir_cleanup);
  if (r < 0)
    {
      LOG_ERROR("Failed to create temporary directory (%s): %s",
		tmpdir_template, strerror(-r));
      show_error_popup("Failed to create temporary directory:",
		       strerror(-r));
      return -r;
    }
  // we cannot make rdii_tmp_dir_cleanup global because of _cleanup_
  rdii_tmp_dir = rdii_tmp_dir_cleanup;

  r = rdii_menu(image, device);

  LOG_INFO("rdi-installer stopped (retval=%i)", r);

  log_close();

  return r;
}
