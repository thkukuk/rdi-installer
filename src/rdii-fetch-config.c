// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <getopt.h>
#include <sys/sendfile.h>
#include <curl/curl.h>

#include "basics.h"
#include "efivars.h"
#include "mkdir_p.h"
#include "download.h"

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
}

static void
print_error(void)
{
  fputs("Try `rdii-fetch-config --help' for more information.\n", stderr);
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
	  break;
        }
    }

  return r;
}

int
main(int argc, char **argv)
{
  _cleanup_free_ char *loader_url = NULL;
  _cleanup_free_ char *loader_dev = NULL;
  _cleanup_free_ char *loader_img = NULL;
  _cleanup_free_ char *cfgfile = NULL;
  const char *arg_url = NULL;
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
          printf("rdii-fetch-config (%s) %s\n", PACKAGE, VERSION);
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
      fprintf(stderr, "rdii-fetch-config: Too many arguments.\n");
      print_error();
      return EINVAL;
    }

  if (!isempty(arg_url) && no_network)
    {
      fprintf(stderr, "The options '--local-only' and '--url' cannot be used together.\n");
      print_error();
      return EINVAL;
    }

  r = mkdir_p(output_dir, 0755);
  if (r < 0)
    {
      fprintf(stderr, "Error creating config directory '%s': %s\n",
	      output_dir, strerror(-r));
      return -r;
    }

  if (asprintf(&cfgfile, "%s/rdii-config", output_dir) < 0)
    {
      fputs("Out of memory!\n", stderr);
      return ENOMEM;
    }

  if (!isempty(arg_url) && !no_network)
    {
      printf("Attempting download (%s)...\n", arg_url);
      r = curl_download_config(arg_url, cfgfile);
      if (r < 0)
	{
	  fprintf(stderr, "Error downloading '%s' and storing to '%s': %s\n",
		  arg_url, cfgfile, curl_easy_strerror(-r));
	  return -r;
	}
      return 0;
    }
  else
    {
      // no url provided, try to guess one based on EFI boot values
      r = efi_get_boot_source(&loader_url, &loader_dev, &loader_img);
      if (r < 0)
	{
	  fprintf(stderr, "Couldn't get boot source: %s\n", strerror(-r));
	  return -r;
	}
      if (!isempty(loader_url))
	{
	  _cleanup_free_ char *config_url = NULL;

	  if (no_network)
	    {
	      printf("Booted from network but run with \"--local-only\", skipping\n");
	      return 0;
	    }

	  r = replace_suffix(loader_url, ".efi", ".rdii-config", &config_url);
	  if (r < 0)
	    {
	      fprintf(stderr, "Error in string manipulation: %s\n",
		      strerror(-r));
	      return -r;
	    }

	  printf("Attempting download (%s)...\n", config_url);
	  r = curl_download_config(config_url, cfgfile);
	  if (r < 0 && r != -CURLE_HTTP_RETURNED_ERROR)
	    {
	      fprintf(stderr, "Error downloading '%s' and storing to '%s': %s\n",
		      config_url, cfgfile, curl_easy_strerror(-r));
	      return -r;
	    }
	}
      else if (!isempty(loader_dev) && !isempty(loader_img))
	{
	  _cleanup_free_ char *src_cfg = NULL;
	  _cleanup_free_ char *mod_img_name = NULL;

	  r = replace_suffix(loader_img, ".efi", ".rdii-config", &mod_img_name);
	  if (r < 0)
	    {
	      fprintf(stderr, "Error in string manipulation: %s\n",
		      strerror(-r));
	      return -r;
	    }

	  if (asprintf(&src_cfg, "/boot/efi%s", mod_img_name) < 0)
	    {
	      fputs("Out of memory!\n", stderr);
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
	      printf("Attempting copying %s...\n", src_cfg);
	      r = copy_file(src_cfg, cfgfile);
	      if (r < 0)
		{
		  fprintf(stderr, "Error copying '%s' to '%s': %s\n",
			  src_cfg, cfgfile, strerror(-r));
		  return -r;
		}
	    }
	}
      else
	{
	  fprintf(stderr, "No config URL provided and boot source couldn't be determined.\n");
	  return ENOENT;
	}
    }
  return 0;
}
