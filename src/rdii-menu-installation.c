// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <curl/curl.h>

#include "basics.h"
#include "download.h"
#include "mkdir_p.h"
#include "rdii-menu.h"
#include "logger.h"

extern char **environ;

// XXX add char **error parameter and change to bool return value
static int
verify_signature(const char *file, const char *key)
{
  pid_t pid;
  int status;
  int r;

  char *argv[] = {"gpgv", "--keyring", "/etc/systemd/import-pubring.gpg",
		  (char *)key, (char *)file, NULL};

  print_global_header_footer(NULL);
  move(2,2);
  refresh();

  r = posix_spawnp(&pid, "gpgv", NULL, NULL, argv, environ);
  if (r != 0)
    {
      fprintf(stderr, "Failed to spawn gpgv: %s\n", strerror(r)); // XXX
      return -r;
    }

  if (waitpid(pid, &status, 0) == -1)
    {
      r = errno;
      perror("waitpid failed"); // XXX
      return -r;
    }

  if (WIFEXITED(status))
    {
      if (WEXITSTATUS(status)) // Signature doesn't match
        keywait(8, 0, NULL, 0);
      return WEXITSTATUS(status);
    }
  else
    {
      fprintf(stderr, "gpgv terminated abnormally\n"); // XXX
      return -1; // XXX
    }
}

static int
write_net_image(const char *url, const char *device)
{
  char **decomp_args;
  char *decomp_cat_args[] = { "cat", NULL };
  char *decomp_bz2_args[] = {"pbzip2", "-dc", NULL};
  char *decomp_gz_args[] = {"pigz", "-dc",  NULL};
  char *decomp_xz_args[] = {"xz", "-dc",  "-T0", NULL};
  char *decomp_zst_args[] = {"zstd", "-dc",  "-T0", NULL};

  int p_wget_tee[2], p_tee_sha[2], p_tee_decomp[2], p_decomp_dd[2];
  int r;

  if (endswith(url, ".xz"))
    decomp_args = decomp_xz_args;
  else if (endswith(url, ".zst"))
    decomp_args = decomp_zst_args;
  else if (endswith(url, ".gz"))
    decomp_args = decomp_gz_args;
  else if (endswith(url, ".bz2"))
    decomp_args = decomp_bz2_args;
  else
    decomp_args = decomp_cat_args;

  if (pipe(p_wget_tee) != 0 || pipe(p_tee_sha) != 0 ||
      pipe(p_tee_decomp) != 0 || pipe(p_decomp_dd) != 0)
    {
      r = errno;
      show_error_popup("pipe allocation failed", strerror(r));
      return -r;
    }

  // Array of all pipe ends. We must close unused ends in the child
  // processes so they receive EOF correctly when a process dies.
  int all_pipes[] =
    {
      p_wget_tee[0], p_wget_tee[1],
      p_tee_sha[0], p_tee_sha[1],
      p_tee_decomp[0], p_tee_decomp[1],
      p_decomp_dd[0], p_decomp_dd[1]
    };

  pid_t pids[5];
  posix_spawn_file_actions_t fa[5];
  for (int i = 0; i < 5; i++)
    posix_spawn_file_actions_init(&fa[i]);

  // Process 1: wget
  char *wget_args[] = {"wget", "--tries=5", "-q", "-O", "-", (char *)url, NULL};
  posix_spawn_file_actions_adddup2(&fa[0], p_wget_tee[1], STDOUT_FILENO);
  for (int i = 0; i < 8; i++) // XXX calculate 8
    posix_spawn_file_actions_addclose(&fa[0], all_pipes[i]);
  if (posix_spawnp(&pids[0], "wget", &fa[0], NULL, wget_args, environ) != 0)
    {
      fprintf(stderr, "Starting 'wget' failed: %s", strerror(errno));
      goto err;
    }

  // Process 2: tee
  _cleanup_free_ char *dev_fd_path = NULL;
  if (asprintf(&dev_fd_path, "/dev/fd/%d", p_tee_decomp[1]) < 0)
    return -ENOMEM;
  char *tee_args[] = {"tee", dev_fd_path, NULL};

  posix_spawn_file_actions_adddup2(&fa[1], p_wget_tee[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&fa[1], p_tee_sha[1], STDOUT_FILENO);
  for (int i = 0; i < 8; i++)
    {
      // Crucial: Leave p_tee_decomp[1] open so tee can write to it via /dev/fd/...
      if (all_pipes[i] != p_tee_decomp[1])
	posix_spawn_file_actions_addclose(&fa[1], all_pipes[i]);
    }
  if (posix_spawnp(&pids[1], "tee", &fa[1], NULL, tee_args, environ) != 0)
    {
      fprintf(stderr, "Starting 'tee' failed: %s", strerror(errno));
      goto err;
    }

  // Process 3: decompressor
  posix_spawn_file_actions_adddup2(&fa[2], p_tee_decomp[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&fa[2], p_decomp_dd[1], STDOUT_FILENO);
  for (int i = 0; i < 8; i++)
    posix_spawn_file_actions_addclose(&fa[2], all_pipes[i]);
  if (posix_spawnp(&pids[2], decomp_args[0], &fa[2], NULL, decomp_args, environ) != 0)
    {
      fprintf(stderr, "Starting '%s' failed: %s", decomp_args[0], strerror(errno));
      goto err;
    }

  // Process 4: dd
  _cleanup_free_ char *dd_of_arg = NULL;
  if (asprintf(&dd_of_arg, "of=%s", device) < 0)
    return -ENOMEM;
  char *dd_args[] = {"dd", dd_of_arg, "status=progress", "conv=fsync", "oflag=direct", NULL};
  posix_spawn_file_actions_adddup2(&fa[3], p_decomp_dd[0], STDIN_FILENO);
  for (int i = 0; i < 8; i++)
    posix_spawn_file_actions_addclose(&fa[3], all_pipes[i]);
  if (posix_spawnp(&pids[3], "dd", &fa[3], NULL, dd_args, environ) != 0)
    {
      fprintf(stderr, "Starting 'dd' failed: %s", strerror(errno));
      goto err;
    }

  // Process 5: sha256sum
  char *sha_args[] = {"sha256sum", NULL};
  _cleanup_free_ char *written_sha256_fn = NULL;
  if (asprintf(&written_sha256_fn, "%s/written.sha256", rdii_tmp_dir) < 0)
    return -ENOMEM;
  posix_spawn_file_actions_adddup2(&fa[4], p_tee_sha[0], STDIN_FILENO);
  posix_spawn_file_actions_addopen(&fa[4], STDOUT_FILENO, written_sha256_fn,
				   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  for (int i = 0; i < 8; i++)
    posix_spawn_file_actions_addclose(&fa[4], all_pipes[i]);
  if (posix_spawnp(&pids[4], "sha256sum", &fa[4], NULL, sha_args, environ) != 0)
    {
      fprintf(stderr, "Starting 'sha256sum' failed: %s", strerror(errno));
      goto err;
    }

  // Close its copies of the pipes so the childs don't hang waiting for EOF
  for (int i = 0; i < 8; i++)
    close(all_pipes[i]);
  for (int i = 0; i < 5; i++)
    posix_spawn_file_actions_destroy(&fa[i]);

  int first_error = 0;
  // Wait for all processes to finish
  for (int i = 0; i < 5; i++)
    {
      int status;

      if (waitpid(pids[i], &status, 0) == -1)
	{
	  r = errno;
	  fprintf(stderr, "waitpid(%i) failed: %s\n", i, strerror(r)); // XXX show_error
	  return -r;
	}

      if (WIFEXITED(status))
	{
	  if (WEXITSTATUS(status) && first_error == 0)
	    first_error = WEXITSTATUS(status);
	}
      else if (WIFSIGNALED(status))
	{
	  // ignore SIGPIPE, follow up error
	  if (WTERMSIG(status) != 13)
	    {
	      fprintf(stderr, "Process %i killed by signal %d\n", i, WTERMSIG(status));
	      first_error = 1;
	    }
	}
      else
	{
	  fprintf(stderr, "Process %i terminated abnormally\n", i); // XXX
	  first_error = 1;
	}
    }

  if (first_error)
    keywait(LINES-3, 0, NULL, 0);

  return -first_error;

  // XXX use _cleanup_ for this
 err:
  keywait(LINES-3, 0, NULL, 0);
  for (int i = 0; i < 8; i++)
    close(all_pipes[i]);
  for (int i = 0; i < 5; i++)
    posix_spawn_file_actions_destroy(&fa[i]);
  return -1;
}

static bool
sha256_eq(const char *path1, const char *path2)
{
  _cleanup_fclose_ FILE *fp1 = NULL;
  _cleanup_fclose_ FILE *fp2 = NULL;
  int r;

  LOG_FUNC("path1='%s', path2='%s'", path1, path2);

  fp1 = fopen(path1, "r");
  if (!fp1)
    {
      r = errno;
      LOG_ERROR("Cannot open '%s': %s", path1, strerror(r));
      fprintf(stderr, "Cannot open '%s': %s", path1, strerror(r));
      return false;
    }

  fp2 = fopen(path2, "r");
  if (!fp2)
    {
      r = errno;
      LOG_ERROR("Cannot open '%s': %s", path2, strerror(r));
      fprintf(stderr, "Cannot open '%s': %s", path2, strerror(r));
      return false;
    }

  _cleanup_free_ char *hash1 = NULL;
  _cleanup_free_ char *hash2 = NULL;
  size_t len = 0;
  ssize_t nread;

  nread = getdelim(&hash1, &len, ' ', fp1);
  if (nread != 65) // includes trailing space
    {
      LOG_ERROR("Read '%s' failed - nread=%li (%s)", path2, nread, hash1);
      return false;
    }

  nread = getdelim(&hash2, &len, ' ', fp2);
  if (nread != 65) // includes trailing space
    {
      LOG_ERROR("Read '%s' failed - nread=%li (%s)", path2, nread, hash2);
      return false;
    }

  LOG_INFO("'%s' - '%s' - %i\n", hash1, hash2, streq(hash1, hash2));

  return streq(hash1, hash2);
}

int
run_installation(const char *url, const char *device)
{
  _cleanup_free_ char *d_sha256_fn = NULL;
  bool is_neturl = startswith(url, "https://") || startswith(url, "http://");
  int r;

  print_global_header_footer(NULL);
  move(2,0);

  // assume network url style
  if (is_neturl)
    {
      _cleanup_free_ char *sha256_url = NULL;

      if (asprintf(&sha256_url, "%s.sha256", url) < 0)
	return -ENOMEM;

      if (asprintf(&d_sha256_fn, "%s/image.sha256", rdii_tmp_dir) < 0)
	return -ENOMEM;

      r = curl_download_file(sha256_url, d_sha256_fn);
      if (r != 0)
	{
	  if (!show_warning_popup("Error downloading sha256 file:",
				  r < 0?strerror(-r):curl_easy_strerror(r),
				  "Continue without image verification?"))
	    return r;
	}
      else
	{
	  _cleanup_free_ char *gpgasc_url = NULL;
	  _cleanup_free_ char *d_gpgasc = NULL;

	  if (asprintf(&gpgasc_url, "%s.sha256.asc", url) < 0)
	    return -ENOMEM;

	  if (asprintf(&d_gpgasc, "%s/image.sha256.asc", rdii_tmp_dir) < 0)
	    return -ENOMEM;

	  r = curl_download_file(gpgasc_url, d_gpgasc);
	  if (r != 0)
	    {
	      show_error_popup("Error downloading sha256.asc file:",
			       r < 0?strerror(-r):curl_easy_strerror(r));
	      return r;
	    }

	  r = verify_signature(d_sha256_fn, d_gpgasc);
	  if (r < 0)
	    return r;
	}
    }
  else
    {
      /* XXX implement reading local file */
    }

  _cleanup_free_ char *device_line = NULL;

  if (asprintf(&device_line, "will be written to %s", device) < 0)
    return -ENOMEM;

  print_global_header_footer(NULL);
  refresh();
  if (!show_warning_popup("WARNING: PERMANENT DATA LOSS - Are you absolutely sure?",
			  url, device_line))
    return 1;

  print_global_header_footer(NULL);
  const char *start_installation_str = "Starting installation...";
  mvprintw(2, (COLS - strlen(start_installation_str)) / 2,
	   "%s", start_installation_str);
  move(4,0);
  refresh();

  r = write_net_image(url, device);
  if (r != 0)
    return r;

  if (is_neturl)
    {
      _cleanup_free_ char *written_sha256_fn = NULL;
      if (asprintf(&written_sha256_fn, "%s/written.sha256", rdii_tmp_dir) < 0)
	return -ENOMEM;

      if (!sha256_eq(written_sha256_fn, d_sha256_fn))
	{
	  show_error_popup("ERROR: SHA256 verification failed!",
			   "Wiping invalid data and aborting...");
	  return -EIO;
	}

    }

  return r;
}
