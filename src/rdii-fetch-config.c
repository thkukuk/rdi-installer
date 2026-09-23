// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <sys/sendfile.h>
#include <curl/curl.h>

#include "basics.h"
#include "cmdline-util.h"
#include "efivars.h"
#include "mkdir_p.h"
#include "download.h"
#include "logger.h"

#define CMDLINE_PATH "/proc/cmdline"

static const char *output_dir = "/run/rdi-installer";

static void
print_usage(FILE *stream)
{
  fprintf(stream, "Usage: rdii-fetch-config [--help]|[--version]|[...]\n");
}

static void
print_help(void)
{
  fprintf(stdout, "rdii-fetch-config - Download config from same place as the bootloader\n\n");
  print_usage(stdout);

  fputs("  -d, --debug       Print debug informations\n", stdout);
  fputs("  -l, --local-only  Don't use network, only local config files\n", stdout);
  fputs("  -o, --output      Directory in which to write config\n", stdout);
  fputs("  -u, --url         URL to download as rdii-config\n", stdout);
  fputs("  -h, --help        Give this help list\n", stdout);
  fputs("  -v, --version     Print program version\n", stdout);

  fputs("\nIf the kernel command line (/proc/cmdline) contains rdii.config=<url>,\n"
	"that URL is used and takes precedence over the boot source location.\n", stdout);
}

static void
print_error(void)
{
  MSG_ERROR("Try `rdii-fetch-config --help' for more information.");
}

static int
replace_suffix(const char *str, const char *suffix,
	       const char *new_suffix, char **ret)
{
  _cleanup_free_ char *new_str = NULL;

  if (isempty(str))
    return -EINVAL;

  size_t len = strlen(str);
  size_t suffix_len = strlen(suffix);

  if (len < suffix_len)
    return -ENOENT;

  if (!streq(str + len - suffix_len, suffix))
    return -ENOENT;

  size_t new_len = len - suffix_len + strlen(new_suffix) + 1;

  new_str = malloc(new_len);
  if (!new_str)
    return -ENOMEM;

  strncpy(new_str, str, len - suffix_len);
  new_str[len - suffix_len] = '\0';

  strcat(new_str, new_suffix);

  *ret = TAKE_PTR(new_str);

  return 0;
}

static int
copy_file(const char *src, const char *dst)
{
  _cleanup_close_ int src_fd = -EBADF;
  _cleanup_close_ int dst_fd = -EBADF;
  struct stat st;
  int r;

  src_fd = open(src, O_RDONLY|O_NOCTTY|O_CLOEXEC);
  if (src_fd == -1)
    return -errno;

  if (fstat(src_fd, &st) == -1)
    return -errno;

  dst_fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
  if (dst_fd == -1)
    return -errno;
  off_t total_bytes_to_copy = st.st_size;

  off_t offset = 0;
  while (offset < total_bytes_to_copy)
    {
      ssize_t bytes_sent = sendfile(dst_fd, src_fd, &offset, total_bytes_to_copy - offset);
      if (bytes_sent == -1)
	{
	  r = -errno;
	  unlink(dst);
	  return r;
        }
    }

  return 0;
}

static int
cmdline_config_url_cb(char *arg, void *userdata)
{
  char **ret = userdata;
  char *val;
  char *new_ret;

  if (!(val = startswith(arg, "rdii.config=")))
    return 0;

  // Strip quotes surrounding the value part
  if (val[0] == '"')
    {
      val++;
      size_t l = strlen(val);
      if (l > 0 && val[l-1] == '"')
	val[l-1] = '\0';
    }

  if (isempty(val))
    return -EINVAL;

  new_ret = strdup(val);
  if (!new_ret)
    return -ENOMEM;

  free(*ret);
  *ret = new_ret;

  /* Keep scanning: if rdii.config= appears more than once, the last
     occurrence wins. */
  return 0;
}

/* Reads /proc/cmdline and returns  the value of "rdii.config=<url>" if
   present, a negative errno in error case.
   Caller has to free the returned string. */
static int
get_cmdline_config_url(char **ret)
{
  _cleanup_fclose_ FILE *fp = NULL;
  _cleanup_free_ char *line = NULL;
  size_t line_size = 0;
  ssize_t nread;
  int r;

  *ret = NULL;

  fp = fopen(CMDLINE_PATH, "r");
  if (!fp)
    return -errno;

  nread = getline(&line, &line_size, fp);
  if (nread == -1)
    return -errno;

  if (nread > 0 && line[nread-1] == '\n')
    line[nread-1] = '\0';

  r = foreach_cmdline_arg(line, cmdline_config_url_cb, ret);
  if (r < 0)
    {
      free(*ret);
      *ret = NULL;
      return r;
    }

  return 0;
}

int
main(int argc, char **argv)
{
  _cleanup_efivars_ efivars_t *efi = NULL;
  _cleanup_free_ char *cfgfile = NULL;
  _cleanup_free_ char *cmdline_url = NULL;
  const char *arg_url = NULL;
  bool url_from_cmdline = false;
  bool no_network = false;
  int r;

  while (1)
    {
      int c;
      int option_index = 0;
      static struct option long_options[] =
        {
          {"debug",      no_argument,       NULL, 'd' },
	  {"local-only", no_argument,       NULL, 'l' },
	  {"output",     required_argument, NULL, 'o' },
	  {"url",        required_argument, NULL, 'u' },
          {"help",       no_argument,       NULL, 'h' },
          {"version",    no_argument,       NULL, 'v' },
          {NULL,         0,                 NULL, '\0'}
        };

      c = getopt_long (argc, argv, "dlo:u:hv",
                       long_options, &option_index);
      if (c == (-1))
        break;

      switch (c)
        {
        case 'd':
	  _efivars_debug = true;
          break;
	case 'l':
	  no_network = true;
	  break;
	case 'o':
	  output_dir = optarg;
	  break;
	case 'u':
	  arg_url = optarg;
	  break;
	case 'h':
          print_help();
          return 0;
        case 'v':
	  MSG_INFO("rdii-fetch-config (%s) %s", PACKAGE, VERSION);
          return 0;
        default:
          print_error();
          return 1;
        }
    }

  argc -= optind;
  argv += optind;

  if (argc > 0)
    {
      MSG_ERROR("rdii-fetch-config: Too many arguments.");
      print_error();
      return EINVAL;
    }

  if (isempty(arg_url))
    {
      // The kernel commandline takes precedence over guessing the
      // config location from the EFI boot source.
      r = get_cmdline_config_url(&cmdline_url);
      if (r == 0 && !isempty(cmdline_url))
	{
	  arg_url = cmdline_url;
	  url_from_cmdline = true;
	}
    }

  const char *local_path = NULL;

  if (!isempty(arg_url))
    {
      local_path = startswith(arg_url, "file://");
      if (local_path == NULL && arg_url[0] == '/')
	local_path = arg_url;
    }

  if (!isempty(arg_url) && no_network && local_path == NULL)
    {
      if (url_from_cmdline)
	{
	  MSG_INFO("Found rdii.config=%s on kernel cmdline but running with \"--local-only\", skipping", arg_url);
	  return 0;
	}
      MSG_ERROR("The options '--local-only' and '--url' cannot be used together.");
      print_error();
      return EINVAL;
    }

  if (url_from_cmdline)
    MSG_INFO("Preferring rdii.config=%s found on kernel cmdline", arg_url);

  r = mkdir_p(output_dir, 0755);
  if (r < 0)
    {
      MSG_ERROR("Error creating config directory '%s': %s",
	     output_dir, strerror(-r));
      return -r;
    }

  if (asprintf(&cfgfile, "%s/rdii-config", output_dir) < 0)
    {
      MSG_ERROR("Out of memory!");
      return ENOMEM;
    }

  if (!isempty(arg_url) && local_path != NULL)
    {
      MSG_INFO("Attempting copying %s...", local_path);
      r = copy_file(local_path, cfgfile);
      if (r < 0)
	{
	  MSG_ERROR("Error copying '%s' to '%s': %s",
		 local_path, cfgfile, strerror(-r));
	  return -r;
	}
      MSG_INFO("Copy successful! Saved to '%s'", cfgfile);
      return 0;
    }
  else if (!isempty(arg_url) && !no_network)
    {
      MSG_INFO("Attempting download (%s)...", arg_url);
      r = curl_download_file(arg_url, cfgfile);
      if (r != 0)
	{
	  MSG_ERROR("Error downloading '%s' and storing to '%s': %s",
		 arg_url, cfgfile, r < 0?strerror(-r):curl_easy_strerror(r));
	  return -r;
	}
      MSG_INFO("Download successful! Saved to '%s'", cfgfile);
      return 0;
    }
  else
    {
      // no url provided, try to guess one based on EFI boot values
      r = efi_get_boot_source(&efi);
      if (r < 0)
	{
	  MSG_ERROR("Couldn't get boot source: %s", strerror(-r));
	  return -r;
	}
      if (!isempty(efi->url))
	{
	  _cleanup_free_ char *config_url = NULL;

	  if (no_network)
	    {
	      MSG_INFO("Booted from network but run with \"--local-only\", skipping");
	      return 0;
	    }

	  r = replace_suffix(efi->url, ".efi", ".rdii-config", &config_url);
	  if (r < 0)
	    {
	      MSG_ERROR("Error in string manipulation: %s",
		     strerror(-r));
	      return -r;
	    }

	  MSG_INFO("Attempting download (%s)...", config_url);
	  r = curl_download_file(config_url, cfgfile);
	  if (r != 0 && r != CURLE_HTTP_RETURNED_ERROR)
	    {
	      MSG_ERROR("Error downloading '%s' and storing to '%s': %s",
		     config_url, cfgfile, r < 0?strerror(-r):curl_easy_strerror(r));
	      return -r;
	    }
	  MSG_INFO("Download successful! Saved to '%s'", cfgfile);
	}
      else if (!isempty(efi->partition) && !isempty(efi->image))
	{
	  _cleanup_free_ char *src_cfg = NULL;
	  _cleanup_free_ char *mod_img_name = NULL;

	  r = replace_suffix(efi->image, ".efi", ".rdii-config", &mod_img_name);
	  if (r < 0)
	    {
              MSG_ERROR("Error in string manipulation: %s",
		     strerror(-r));
	      return -r;
	    }

	  if (asprintf(&src_cfg, "/boot/efi%s", mod_img_name) < 0)
	    {
	      MSG_ERROR("Out of memory!");
	      return ENOMEM;
	    }

	  if (access(src_cfg, R_OK) != 0)
	    {
	      r = -errno;
	      // Be silent if file does not exist
	      if (r != -ENOENT)
		return -r;
	    }
	  else
	    {
	      MSG_INFO("Attempting copying %s...", src_cfg);
	      r = copy_file(src_cfg, cfgfile);
	      if (r < 0)
		{
		  MSG_ERROR("Error copying '%s' to '%s': %s",
			 src_cfg, cfgfile, strerror(-r));
		  return -r;
		}
	    }
	}
      else if (efi->is_pxe_boot)
	{
	  MSG_INFO("PXE Boot (%s), fetching config not possible.", efi->entry);
	}
      else
	{
	  MSG_ERROR("No config URL provided and boot source couldn't be determined.");
	  return ENOENT;
	}
    }
  return 0;
}
