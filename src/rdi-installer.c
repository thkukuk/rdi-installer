// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <getopt.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <libeconf.h>

#include "basics.h"
#include "rm_rf.h"
#include "tmpfile-util.h"
#include "tmpfile-util.h"
#include "nc-dialogs.h"
#include "rdii-menu.h"
#include "rdii-autoinstall.h"
#include "logger.h"
#include "select_keymap.h"
#include "is_linux_vt.h"

#define TITLE "Raw Disk Image Installer Version " VERSION

const char *rdii_config = "/run/rdi-installer/rdii-config";
const char *rdii_tmp_dir = NULL;
const char *rdii_log = "/var/log/rdi-installer.log";
const char *rdii_download_server = "https://download.opensuse.org/tumbleweed/appliances/";

static econf_err
read_config(const char *config, char **ret_device, char **ret_mdraid,
	    char **ret_url, char **ret_url1, char **ret_url2,
	    char **ret_keymap, char **ret_download_server,
	    bool *ret_preserve_ssh_hostkey, bool *ret_autoinstall,
	    char **ret_autoinstall_finish)
{
  _cleanup_(econf_freeFilep) econf_file *key_file = NULL;
  _cleanup_free_ char *device = NULL;
  _cleanup_free_ char *mdraid = NULL;
  _cleanup_free_ char *url = NULL;
  _cleanup_free_ char *url1 = NULL;
  _cleanup_free_ char *url2 = NULL;
  _cleanup_free_ char *keymap = NULL;
  _cleanup_free_ char *download_server = NULL;
  _cleanup_free_ char *autoinstall_finish = NULL;
  bool preserve_ssh_hostkey = false;
  bool autoinstall = false;
  econf_err error;

  error = econf_readFile(&key_file, config,
			 "=", "#");
  if (error == ECONF_NOFILE)
    {
      MSG_WARN("No rdi-installer configuration file found");
      return ECONF_SUCCESS;
    }

  if (error != ECONF_SUCCESS)
    return error;

  error = econf_getStringValue(key_file, NULL, "rdii.device", &device);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;
  error = econf_getStringValue(key_file, NULL, "rdii.mdraid", &mdraid);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;

  error = econf_getStringValue(key_file, NULL, "rdii.url", &url);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;
  error = econf_getStringValue(key_file, NULL, "rdii.url1", &url1);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;
  error = econf_getStringValue(key_file, NULL, "rdii.url2", &url2);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;

  error = econf_getStringValue(key_file, NULL, "rdii.keymap", &keymap);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;

  error = econf_getStringValue(key_file, NULL, "rdii.download_server", &download_server);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;

  error = econf_getBoolValue(key_file, NULL, "rdii.preserve-ssh-hostkey", &preserve_ssh_hostkey);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;
  // if new values get's added later: only do the assignment if a key
  // was really found, and only after reading the last variable
  if (error == ECONF_SUCCESS && ret_preserve_ssh_hostkey)
    *ret_preserve_ssh_hostkey = preserve_ssh_hostkey;

  error = econf_getBoolValue(key_file, NULL, "rdii.autoinstall", &autoinstall);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;
  if (error == ECONF_SUCCESS && ret_autoinstall)
    *ret_autoinstall = autoinstall;

  error = econf_getStringValue(key_file, NULL, "rdii.autoinstall.finish", &autoinstall_finish);
  if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
    return error;
  if (autoinstall_finish &&
      strcmp(autoinstall_finish, "reboot") != 0 &&
      strcmp(autoinstall_finish, "poweroff") != 0 &&
      strcmp(autoinstall_finish, "manual") != 0)
    MSG_WARN("No valid value for rdii.autoinstall.finish: %s", autoinstall_finish);

  // These settings only apply to an automatic installation; in the normal
  // interactive menu, popups always keep requiring explicit confirmation.
  if (autoinstall)
    {
      bool confirm_info_cfg = confirm_infos;
      error = econf_getBoolValue(key_file, NULL, "rdii.autoinstall.confirm_infos", &confirm_info_cfg);
      if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
	return error;
      if (error == ECONF_SUCCESS)
	confirm_infos = confirm_info_cfg;

      bool confirm_warnings_cfg = confirm_warnings;
      error = econf_getBoolValue(key_file, NULL, "rdii.autoinstall.confirm_warnings", &confirm_warnings_cfg);
      if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
	return error;
      if (error == ECONF_SUCCESS)
	confirm_warnings = confirm_warnings_cfg;

      bool confirm_errors_cfg = confirm_errors;
      error = econf_getBoolValue(key_file, NULL, "rdii.autoinstall.confirm_errors", &confirm_errors_cfg);
      if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
	return error;
      if (error == ECONF_SUCCESS)
	confirm_errors = confirm_errors_cfg;

      int32_t popup_timeout_cfg = popup_timeout;
      error = econf_getIntValue(key_file, NULL, "rdii.autoinstall.popup_timeout", &popup_timeout_cfg);
      if (error != ECONF_SUCCESS && error != ECONF_NOKEY)
	return error;
      if (error == ECONF_SUCCESS)
	popup_timeout = popup_timeout_cfg;
    }

  if (ret_device)
    *ret_device = TAKE_PTR(device);
  if (ret_mdraid)
    *ret_mdraid = TAKE_PTR(mdraid);
  if (ret_url)
    *ret_url = TAKE_PTR(url);
  if (ret_url1)
    *ret_url1 = TAKE_PTR(url1);
  if (ret_url2)
    *ret_url2 = TAKE_PTR(url2);
  if (ret_keymap)
    *ret_keymap = TAKE_PTR(keymap);
  if (ret_download_server)
    *ret_download_server = TAKE_PTR(download_server);
  if (ret_autoinstall_finish)
    *ret_autoinstall_finish = TAKE_PTR(autoinstall_finish);

  return ECONF_SUCCESS;
}

static char*
rm_rf_and_free(char *p)
{
  int r;

  r = rm_rf(p);

  if (r < 0)
    MSG_ERROR("Removal of '%s' failed: %s", p, strerror(-r));

  return mfree(p);
}

static inline void
rm_rf_and_freep(char **p)
{
  if (*p)
    *p = rm_rf_and_free(*p);
}

static void
validate_image_url(char **url)
{
  const char *error_msg = NULL;

  if (isempty(*url) ||
      (!startswith(*url, "https://") && !startswith(*url, "http://")))
    return;

  if (!url_is_valid(*url, &error_msg) &&
      !show_warning_popup("URL doesn't seem to be valid:",
			   error_msg, "Really use this URL?"))
    *url = mfree(*url);
}

/* verify if a device path exists and is a block device */
static bool
device_exists(const char *device, const char **error)
{
  struct stat st;

  if (stat(device, &st) < 0)
    {
      if (error)
	*error = strerror(errno);
      return false;
    }

  if (!S_ISBLK(st.st_mode))
    {
      if (error)
	*error = "not a block device";
      return false;
    }

  return true;
}

static void
validate_device(char **device)
{
  const char *error_msg = NULL;

  if (isempty(*device))
    return;

  if (!device_exists(*device, &error_msg) &&
      !show_warning_popup("Device doesn't seem to be available:",
			   error_msg, "Really use this device?"))
    *device = mfree(*device);
}

static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: rdi-installer [options]\n");
}

static void
print_help(void)
{
  fprintf(stdout, "rdi-installer - raw disk image installer\n\n");

  print_usage(stdout);

  fputs("  -c, --config <file>  Specify a different config file\n", stdout);
  fputs("  -h, --help           Give this help list\n", stdout);
  fputs("  -v, --version        Print program version\n", stdout);
}

static void
print_error(void)
{
  fprintf(stderr, "Try `rdi-installer --help' for more information.\n");
}

int
main(int argc, char **argv)
{
  _cleanup_(rm_rf_and_freep) char *rdii_tmp_dir_cleanup = NULL;
  _cleanup_free_ char *image = NULL;
  _cleanup_free_ char *image1 = NULL;
  _cleanup_free_ char *image2 = NULL;
  _cleanup_free_ char *device = NULL;
  _cleanup_free_ char *mdraid = NULL;
  _cleanup_free_ char *keymap = NULL;
  _cleanup_free_ char *download_server = NULL;
  _cleanup_free_ char *autoinstall_finish = NULL;
  bool preserve_ssh_hostkey = false;
  bool autoinstall = false;
  int r;
  econf_err conf_err;

  if (getuid())
    rdii_log = "rdii.log";
  r = log_init(NO_CONSOLE_LOG, rdii_log);
  if (r < 0)
    {
      fprintf(stderr, "Cannot initialize log file (%s): %s\n",
	      rdii_log, strerror(-r));
      return -r;
    }

  MSG_INFO("rdi-installer started");

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
          {"config",    required_argument, NULL, 'c' },
	  {"help",      no_argument,       NULL, 'h' },
	  {"version",   no_argument,       NULL, 'v' },
	  {NULL,        0,                 NULL, '\0'}
        };

      c = getopt_long(argc, argv, "c:fhv",
                      long_options, &option_index);
      if (c == (-1))
        break;
      switch (c)
        {
	case 'c':
          rdii_config = optarg;
          break;
	case 'f':
	  // ignore -f, it's added by agetty for autologin
	  break;
	case 'h':
          print_help();
          return 0;
        case 'v':
          printf("rdi-installer (%s) %s\n", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return 1;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 1) // 1 because of "hostname" argument from agetty
    {
      fprintf(stderr, "rdi-installer: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  init_ncurses(TITLE);

  conf_err = read_config(rdii_config, &device, &mdraid, &image, &image1, &image2, &keymap,
			 &download_server, &preserve_ssh_hostkey, &autoinstall,
			 &autoinstall_finish);
  if (conf_err != ECONF_SUCCESS)
    {
      show_error_popup("Failed to read config file:",
                       econf_errString(conf_err), NULL);
    }

  // Only needed to decide whether an automatic installation can start;
  // the interactive menu validates URLs/devices lazily as the user
  // selects or enters them, to avoid blocking on network I/O at startup.
  if (autoinstall)
    {
      validate_image_url(&image);
      validate_image_url(&image1);
      validate_image_url(&image2);

      validate_device(&device);
      validate_device(&mdraid);
    }

  if (download_server)
    rdii_download_server = download_server;

  if (keymap)
    {
      if (is_linux_vt())
        set_keymap(keymap);
      else
        MSG_INFO("Not a Linux VT, skipping loadkeys");
    }

  const char *tmpdir_template = "/tmp/rdi-installer-XXXXXX";
  r = mkdtemp_malloc(tmpdir_template, &rdii_tmp_dir_cleanup);
  if (r < 0)
    {
      show_error_popup("Failed to create temporary directory:",
		       tmpdir_template, strerror(-r));
      return -r;
    }

  // we cannot make rdii_tmp_dir_cleanup global because of _cleanup_
  rdii_tmp_dir = rdii_tmp_dir_cleanup;

  if (!autoinstall || !rdii_autoinstall(image, device, mdraid, preserve_ssh_hostkey,
					autoinstall_finish, &r))
    r = rdii_menu(TITLE, image, image1, image2, device, mdraid, keymap,
		  preserve_ssh_hostkey);

  MSG_INFO("rdi-installer stopped (retval=%i)", r);

  log_close();

  return r;
}
