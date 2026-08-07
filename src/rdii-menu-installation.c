// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <spawn.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <curl/curl.h>

#include "basics.h"
#include "download.h"
#include "mkdir_p.h"
#include "rdii-menu.h"
#include "logger.h"
#include "zap_partition_table.h"
#include "exec_cmd.h"
#include "rdii-ssh-hostkey.h"

extern char **environ;

static bool
verify_signature(const char *file, char *key, char **error)
{
  int r;

  MSG_FUNC("file='%s', key='%s'", file, key);

  print_global_header_footer(NULL);
  move(2,2);
  refresh();

  r = exec_cmd("gpgv", "gpgv", "--keyring", "/etc/systemd/import-pubring.gpg",
	       (char *)key, (char *)file, NULL);
  if (r < 0)
    {
      MSG_ERROR("Failed to run gpgv: %s", strerror(-r));
      if (error &&
	  (asprintf(error, "Failed to run gpgv: %s", strerror(-r)) < 0))
        *error = "Out of memory";
      return false;
    }
  if (r > 0)
    {
      if (r > 128) // aborted by signal
        {
          int sig = r - 128;
          MSG_ERROR("gpgv got terminated by signal %d (%s)",
                 sig, strsignal(sig));
          if (error &&
              (asprintf(error, "gpgv got terminated by signal %d (%s)",
			sig, strsignal(sig)) < 0))
            *error = "Out of memory";
        }
      else
	{
          MSG_ERROR("gpgv failed with exit code %i", r);
          if (error &&
              (asprintf(error, "gpgv failed with exit code %i", r) < 0))
	    *error = "Out of memory";
	}
      return false;
    }

  return true;
}

// Call sgdisk -e to adjust partition table to real disk size
static int
fix_partition_table(const char *device)
{
  int r;

  MSG_FUNC("device='%s'", device);

  r = exec_cmd("/usr/sbin/sgdisk", "sgdisk", "-e", (char *)device, NULL);

  if (r < 0)
    {
      show_error_popup("Adjusting partition table to real disk size failed.",
                       "Failed to start sgdisk:",
		       strerror(-r));
      return r;
    }
  if (r > 0)
    {
      if (r > 128) // aborted by signal
	{
	  int sig = r - 128;
          show_error_popup("Adjusting partition table to real disk size failed.",
			   "sgdisk was terminated by signal:",
			   strsignal(sig));
	}
      else
        show_error_popup("Adjusting partition table to real disk size failed.",
			 "sgdisk failed with:", strerror(r));

      keywait(8, 0, NULL, 0);
      return -ECHILD;
    }
  return 0;
}

static char * const *
decompression(const char *url)
{
  static char *decomp_cat_args[] = { "cat", NULL };
  static char *decomp_bz2_args[] = {"pbzip2", "-dc", NULL};
  static char *decomp_gz_args[] = {"pigz", "-dc",  NULL};
  static char *decomp_xz_args[] = {"xz", "-dc",  "-T0", NULL};
  static char *decomp_zst_args[] = {"zstd", "-dc",  "-T0", NULL};

  if (endswith(url, ".xz"))
    return decomp_xz_args;
  else if (endswith(url, ".zst"))
    return decomp_zst_args;
  else if (endswith(url, ".gz"))
    return decomp_gz_args;
  else if (endswith(url, ".bz2"))
    return decomp_bz2_args;
  else
    return decomp_cat_args;
}

static void
cleanup_pipes_and_actions(int all_pipes[], const int all_pipe_size,
                          posix_spawn_file_actions_t fa[], const int fa_size)
{
  for (int i = 0; i < all_pipe_size; i++)
    close(all_pipes[i]);
  for (int i = 0; i < fa_size; i++)
    posix_spawn_file_actions_destroy(&fa[i]);
}

// Terminate and reap the processes which have already been started when
// setting up the pipeline fails half way through. Without this they would
// keep running and end up as zombies, as nobody will ever wait for them.
static void
kill_and_reap(pid_t pids[], const int spawned)
{
  for (int i = 0; i < spawned; i++)
    {
      if (kill(pids[i], SIGKILL) != 0 && errno != ESRCH)
	MSG_ERROR("Cannot kill process %i: %s", i, strerror(errno));

      while (waitpid(pids[i], NULL, 0) == -1)
	{
	  if (errno == EINTR)
	    continue;
	  MSG_ERROR("waitpid(%i) failed: %s", i, strerror(errno));
	  break;
	}
    }
}

// Cleanup after an error while setting up the pipeline. The pipes are closed
// first, so that the already running processes get EOF or SIGPIPE and have a
// chance to terminate on their own before they get killed.
static void
cleanup_after_error(int all_pipes[], const int all_pipe_size,
		    posix_spawn_file_actions_t fa[], const int fa_size,
		    pid_t pids[], const int spawned)
{
  cleanup_pipes_and_actions(all_pipes, all_pipe_size, fa, fa_size);
  kill_and_reap(pids, spawned);
}

static int
wait_for_finish(pid_t pids[], const int pids_size)
{
  int first_error = 0;
  // Wait for all processes to finish
  for (int i = 0; i < pids_size; i++)
    {
      int status;

      if (waitpid(pids[i], &status, 0) == -1)
	{
	  int r = errno;
	  _cleanup_free_ char *err_msg = NULL;
	  if (asprintf(&err_msg, "waitpid(%i) failed: %s\n", i, strerror(r)) < 0)
            return -ENOMEM;
          reset_prog_mode();
          show_error_popup("Cannot finish installation correctly.",
			   err_msg, NULL);
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
	  if (WTERMSIG(status) != SIGPIPE)
	    {
              _cleanup_free_ char *err_msg = NULL;
              if (asprintf(&err_msg, "Process %i killed by signal %d", i, WTERMSIG(status)) < 0)
                return -ENOMEM;
              reset_prog_mode();
              show_error_popup("Cannot finish installation correctly.",
                               err_msg, NULL);
	      first_error = 1;
	    }
	}
      else
	{
          _cleanup_free_ char *err_msg = NULL;
          if (asprintf(&err_msg, "Process %i terminated abnormally", i) < 0)
            return -ENOMEM;
          reset_prog_mode();
          show_error_popup("Cannot finish installation correctly.",
                           err_msg, NULL);
	  first_error = 1;
	}
    }
  reset_prog_mode();
  if (first_error)
    keywait(LINES-3, 0, NULL, 0);

  return -first_error;
}

static int
write_net_image(const char *url, const char *device)
{
  char * const *decomp_args = decompression(url);
  int p_wget_tee[2], p_tee_sha[2], p_tee_decomp[2], p_decomp_dd[2];
  int r;

  MSG_FUNC("url='%s', device='%s'", url, device);
  MSG_INFO("decompressor=%s", decomp_args[0]);

  if (pipe(p_wget_tee) != 0 || pipe(p_tee_sha) != 0 ||
      pipe(p_tee_decomp) != 0 || pipe(p_decomp_dd) != 0)
    {
      r = errno;
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Pipe allocation failed: %s", strerror(r)) < 0)
        return -ENOMEM;
      show_error_popup("Cannot start image download.",
		       msg, NULL);
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
  const int all_pipe_size = (int)(sizeof(all_pipes) / sizeof(all_pipes[0]));

  pid_t pids[5];
  const int pids_size = (int)(sizeof(pids) / sizeof(pids[0]));
  posix_spawn_file_actions_t fa[5];
  const int fa_size =  (int)(sizeof(fa) / sizeof(fa[0]));
  for (int i = 0; i < fa_size; i++)
    posix_spawn_file_actions_init(&fa[i]);

  // Number of processes started so far, needed to clean them up on error
  int spawned = 0;

  // Process 1: wget
  char *wget_args[] = {"wget", "--tries=5", "-q", "-O", "-", (char *)url, NULL};
  posix_spawn_file_actions_adddup2(&fa[0], p_wget_tee[1], STDOUT_FILENO);
  for (int i = 0; i < all_pipe_size; i++)
    posix_spawn_file_actions_addclose(&fa[0], all_pipes[i]);
  reset_shell_mode();
  if (posix_spawnp(&pids[0], "wget", &fa[0], NULL, wget_args, environ) != 0)
    {
       _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting 'wget' failed: %s", strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }
  spawned++;

  // Process 2: tee
  _cleanup_free_ char *dev_fd_path = NULL;
  if (asprintf(&dev_fd_path, "/dev/fd/%d", p_tee_decomp[1]) < 0)
    {
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -ENOMEM;
    }
  char *tee_args[] = {"tee", dev_fd_path, NULL};

  posix_spawn_file_actions_adddup2(&fa[1], p_wget_tee[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&fa[1], p_tee_sha[1], STDOUT_FILENO);
  for (int i = 0; i < all_pipe_size; i++)
    {
      // Crucial: Leave p_tee_decomp[1] open so tee can write to it via /dev/fd/...
      if (all_pipes[i] != p_tee_decomp[1])
	posix_spawn_file_actions_addclose(&fa[1], all_pipes[i]);
    }
  if (posix_spawnp(&pids[1], "tee", &fa[1], NULL, tee_args, environ) != 0)
    {
       _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting 'tee' failed: %s", strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }
  spawned++;

  // Process 3: decompressor
  posix_spawn_file_actions_adddup2(&fa[2], p_tee_decomp[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&fa[2], p_decomp_dd[1], STDOUT_FILENO);
  for (int i = 0; i < all_pipe_size; i++)
    posix_spawn_file_actions_addclose(&fa[2], all_pipes[i]);
  if (posix_spawnp(&pids[2], decomp_args[0], &fa[2], NULL, decomp_args, environ) != 0)
    {
       _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting '%s' failed: %s", decomp_args[0], strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }
  spawned++;

  // Process 4: dd
  _cleanup_free_ char *dd_of_arg = NULL;
  if (asprintf(&dd_of_arg, "of=%s", device) < 0)
    {
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -ENOMEM;
    }
  char *dd_args[] = {"dd", dd_of_arg, "status=progress", "conv=fsync", "oflag=direct", NULL};
  posix_spawn_file_actions_adddup2(&fa[3], p_decomp_dd[0], STDIN_FILENO);
  for (int i = 0; i < all_pipe_size; i++)
    posix_spawn_file_actions_addclose(&fa[3], all_pipes[i]);
  if (posix_spawnp(&pids[3], "dd", &fa[3], NULL, dd_args, environ) != 0)
    {
       _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting 'dd' failed: %s", strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }
  spawned++;

  // Process 5: sha256sum
  char *sha_args[] = {"sha256sum", NULL};
  _cleanup_free_ char *written_sha256_fn = NULL;
  if (asprintf(&written_sha256_fn, "%s/written.sha256", rdii_tmp_dir) < 0)
    {
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -ENOMEM;
    }
  posix_spawn_file_actions_adddup2(&fa[4], p_tee_sha[0], STDIN_FILENO);
  posix_spawn_file_actions_addopen(&fa[4], STDOUT_FILENO, written_sha256_fn,
				   O_WRONLY | O_CREAT | O_TRUNC, 0644);
  for (int i = 0; i < all_pipe_size; i++)
    posix_spawn_file_actions_addclose(&fa[4], all_pipes[i]);
  if (posix_spawnp(&pids[4], "sha256sum", &fa[4], NULL, sha_args, environ) != 0)
    {
       _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting 'sha256sum' failed: %s", strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }

  // Close its copies of the pipes so the childs don't hang waiting for EOF
  cleanup_pipes_and_actions(all_pipes, all_pipe_size, fa, fa_size);

  return wait_for_finish(pids, pids_size);
}

static int
write_local_image(const char *file, const char *device)
{
  char * const *decomp_args = decompression(file);
  int p_pv_decomp[2], p_decomp_dd[2];
  int r;

  MSG_FUNC("file='%s', device='%s'", file, device);
  MSG_INFO("decompressor=%s", decomp_args[0]);

  if (pipe(p_pv_decomp) != 0 || pipe(p_decomp_dd) != 0)
    {
      r = errno;
       _cleanup_free_ char *msg = NULL;
       if (asprintf(&msg, "Pipe allocation failed: %s", strerror(r)) < 0)
        return -ENOMEM;
       show_error_popup("Cannot start installation process.",
                        msg, NULL);
      return -r;
    }

  // Array of all pipe ends. We must close unused ends in the child
  // processes so they receive EOF correctly when a process dies.
  int all_pipes[] =
    {
      p_pv_decomp[0], p_pv_decomp[1],
      p_decomp_dd[0], p_decomp_dd[1]
    };
  const int all_pipe_size = (int)(sizeof(all_pipes) / sizeof(all_pipes[0]));

  pid_t pids[3];
  const int pids_size = (int)(sizeof(pids) / sizeof(pids[0]));
  posix_spawn_file_actions_t fa[3];
  const int fa_size = (int)(sizeof(fa) / sizeof(fa[0]));
  for (int i = 0; i < fa_size; i++)
    posix_spawn_file_actions_init(&fa[i]);

  // Number of processes started so far, needed to clean them up on error
  int spawned = 0;

  // Process 1: pv
  char *pv_args[] = {"pv", (char *)file, NULL};
  posix_spawn_file_actions_adddup2(&fa[0], p_pv_decomp[1], STDOUT_FILENO);
  for (int i = 0; i < all_pipe_size; i++)
    posix_spawn_file_actions_addclose(&fa[0], all_pipes[i]);
  reset_shell_mode();
  if (posix_spawnp(&pids[0], "pv", &fa[0], NULL, pv_args, environ) != 0)
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting 'pv' failed: %s", strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }
  spawned++;

  // Process 2: decompressor
  posix_spawn_file_actions_adddup2(&fa[1], p_pv_decomp[0], STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&fa[1], p_decomp_dd[1], STDOUT_FILENO);
  for (int i = 0; i < all_pipe_size; i++)
    posix_spawn_file_actions_addclose(&fa[1], all_pipes[i]);
  if (posix_spawnp(&pids[1], decomp_args[0], &fa[1], NULL, decomp_args, environ) != 0)
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting '%s' failed: %s", decomp_args[0], strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }
  spawned++;

  // Process 3: dd
  _cleanup_free_ char *dd_of_arg = NULL;
  if (asprintf(&dd_of_arg, "of=%s", device) < 0)
    {
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -ENOMEM;
    }
  char *dd_args[] = {"dd", dd_of_arg, "bs=4M", "conv=fsync", "oflag=direct", NULL};
  posix_spawn_file_actions_adddup2(&fa[2], p_decomp_dd[0], STDIN_FILENO);
  for (int i = 0; i < all_pipe_size; i++)
    posix_spawn_file_actions_addclose(&fa[2], all_pipes[i]);
  if (posix_spawnp(&pids[2], "dd", &fa[2], NULL, dd_args, environ) != 0)
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "Starting 'dd' failed: %s", strerror(errno)) < 0)
        {
          cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
          return -ENOMEM;
        }
      reset_prog_mode();
      show_error_popup("Cannot start installation.", msg, NULL);
      cleanup_after_error(all_pipes, all_pipe_size, fa, fa_size, pids, spawned);
      return -1;
    }

  // Close its copies of the pipes so the childs don't hang waiting for EOF
  cleanup_pipes_and_actions(all_pipes, all_pipe_size, fa, fa_size);

  return wait_for_finish(pids, pids_size);
}

static bool
sha256_eq(const char *path1, const char *path2)
{
  _cleanup_fclose_ FILE *fp1 = NULL;
  _cleanup_fclose_ FILE *fp2 = NULL;
  int r;

  MSG_FUNC("path1='%s', path2='%s'", path1, path2);

  fp1 = fopen(path1, "r");
  if (!fp1)
    {
      r = errno;
      MSG_ERROR("Cannot open '%s': %s", path1, strerror(r));
      return false;
    }

  fp2 = fopen(path2, "r");
  if (!fp2)
    {
      r = errno;
      MSG_ERROR("Cannot open '%s': %s", path2, strerror(r));
      return false;
    }

  _cleanup_free_ char *hash1 = NULL;
  _cleanup_free_ char *hash2 = NULL;
  size_t len = 0;
  ssize_t nread;

  nread = getdelim(&hash1, &len, ' ', fp1);
  if (nread != 65) // includes trailing space
    {
      MSG_ERROR("Read '%s' failed - nread=%li (%s)", path2, nread, hash1);
      return false;
    }

  nread = getdelim(&hash2, &len, ' ', fp2);
  if (nread != 65) // includes trailing space
    {
      MSG_ERROR("Read '%s' failed - nread=%li (%s)", path2, nread, hash2);
      return false;
    }

  MSG_INFO("'%s' - '%s' - %i", hash1, hash2, streq(hash1, hash2));

  return streq(hash1, hash2);
}

/*
  Handling of mdraid:
  - first disk is "device"
  - second disk is "mdraid"
  - new device name is "/dev/md0"
*/
int
run_installation(const char *url, const char *device, const char *mdraid,
		 bool preserve_ssh_hostkey)
{
  _cleanup_free_ char *d_sha256_fn = NULL;
  _cleanup_free_ char *ssh_backup_dir = NULL;
  bool is_neturl = startswith(url, "https://") || startswith(url, "http://");
  int r;

  MSG_FUNC("url='%s', device='%s', mdraid='%s', preserve_ssh_hostkey=%s",
	   strna(url), strna(device), strna(mdraid),
           strbool(preserve_ssh_hostkey));

  if (is_device_mounted(device))
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "The device %s contains mounted partitions.",
		   device) < 0)
	return -ENOMEM;

      r = show_warning_popup("!!! CRITICAL WARNING: DRIVE IS CURRENTLY MOUNTED !!!",
			     msg,
			     "Proceeding may cause data loss or corruption.");
      if (r == 0)
	return -EINTR;
    }

  if (!isempty(mdraid) && is_device_mounted(mdraid))
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "The device %s contains mounted partitions.",
		   mdraid) < 0)
	return -ENOMEM;

      r = show_warning_popup("!!! CRITICAL WARNING: DRIVE IS CURRENTLY MOUNTED !!!",
			     msg,
			     "Proceeding may cause data loss or corruption.");
      if (r == 0)
	return -EINTR;
    }

  print_global_header_footer(NULL);
  move(2,0);

  // assume network url style
  // download hashes and signatures for verification
  if (is_neturl)
    {
      _cleanup_free_ char *sha256_url = NULL;

      MSG_INFO("Is network url");

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
	  d_sha256_fn = mfree(d_sha256_fn);
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
	      if (!show_warning_popup("Error downloading sha256.asc file:",
				      r < 0?strerror(-r):curl_easy_strerror(r),
				      "Continue without signature verification?"))
		return r;
	    }
	  else
	    {
              char *error_msg = NULL;
	      if (!verify_signature(d_sha256_fn, d_gpgasc, &error_msg) &&
		  !show_warning_popup ("Cannot verify signature.", error_msg,
				       "Continue without signature verification?"))
		return -1;
	    }
	}
    }
  // /path/to/file/*.raw.xz
  else if (startswith(url, "/"))
    {
      _cleanup_free_ char *sha256_file = NULL;

      MSG_INFO("Is a file url");

      if (asprintf(&sha256_file, "%s.sha256", url) < 0)
	return -ENOMEM;

      r = access(sha256_file, F_OK);
      if (r < 0)
	{
	  r = -errno;
	  if (!show_warning_popup("Cannot find sha256 file:",
				  strerror(-r),
				  "Continue without image verification?"))
	    return r;
	}
      else
	{
	  _cleanup_free_ char *gpgasc_file = NULL;

	  if (asprintf(&gpgasc_file, "%s.sha256.asc", url) < 0)
	    return -ENOMEM;

	  r = access(gpgasc_file, F_OK);
	  if (r != 0)
	    {
	      if (!show_warning_popup("Cannot find sha256.asc file:",
				      r < 0?strerror(-r):curl_easy_strerror(r),
				      "Continue without signature verification?"))
		return r;
	    }
	  else
	    {
              char *error_msg = NULL;
	      if (!verify_signature(sha256_file, gpgasc_file, &error_msg) &&
		  !show_warning_popup ("Cannot verify signature.", error_msg,
				       "Continue without signature verification?"))
                return -1;
	    }
	}
    }
  else
    {
      show_error_popup("Unknown URL format:", url, NULL);
      return -EINVAL;
    }

  _cleanup_free_ char *device_line = NULL;

  if (isempty(mdraid))
    {
      if (asprintf(&device_line, "will be written to %s", device) < 0)
	return -ENOMEM;
    }
  else
    {
      if (asprintf(&device_line, "will be written to %s and %s", device, mdraid) < 0)
	return -ENOMEM;
    }

  print_global_header_footer(NULL);
  refresh();
  if (!show_warning_popup("WARNING: PERMANENT DATA LOSS - Are you absolutely sure?",
			  url, device_line))
    return 1;

  if (preserve_ssh_hostkey)
    {
      if (asprintf(&ssh_backup_dir, "%s/ssh-backup", rdii_tmp_dir) < 0)
        return -ENOMEM;

      MSG_INFO("Attempting to backup SSH host keys from %s", device);
      r = rdii_ssh_hostkey_backup(device, ssh_backup_dir);
      if (r < 0)
        {
          MSG_WARN("SSH host key backup failed: %s", strerror(-r));
        }
      else if (r > 0)
        {
          MSG_INFO("Successfully backed up %d SSH host key(s)", r);
        }
    }

  print_global_header_footer(NULL);
  const char *start_installation_str = "Starting installation...";
  mvprintw(2, (COLS - strlen(start_installation_str)) / 2,
	   "%s", start_installation_str);
  move(4,0);
  refresh();

  if (!isempty(mdraid))
    {
      // Create /dev/md0

      r = exec_cmd("mdadm", "mdadm", "--create", "--verbose", "/dev/md0",
		   "--level=1", "--metadata=1.0", "--bitmap=internal",
		   "--raid-devices=2", "--run",
		   (char *)device, (char *)mdraid, NULL);
      if (r < 0)
	{
	  MSG_ERROR("Creating MD Raid failed (mdadm): %s",
		    strerror(-r));

	  show_error_popup("Creating MD Raid failed.",
			   "Failed to run mdadm:",
			   strerror(-r));
	  return r;
	}
      if (r > 0)
	{
	  if (r > 128) // aborted by signal
	    {
	      int sig = r - 128;
	      MSG_ERROR("mdadm got terminated by signal %d (%s)",
			sig, strsignal(sig));
	      show_error_popup("mdadm got terminated by signal",
			       strsignal(sig), NULL);
	    }
	  else
	    {
              _cleanup_free_ char *ret = NULL;
              MSG_ERROR("mdadm failed with exit code %i", r);
              if (asprintf(&ret, "mdadm failed with exit code %i", r) < 0)
                show_error_popup("mdadm failed with exit code",
                                 NULL, NULL);
              else
                show_error_popup(ret, NULL, NULL);
	    }
	  return false;
	}

      // set device to /dev/md0
      device = "/dev/md0";
    }

  if (is_neturl)
    {
      _cleanup_free_ char *written_sha256_fn = NULL;

      r = write_net_image(url, device);
      if (r != 0)
	return r;

      if (asprintf(&written_sha256_fn, "%s/written.sha256", rdii_tmp_dir) < 0)
	return -ENOMEM;

      if (d_sha256_fn && !sha256_eq(written_sha256_fn, d_sha256_fn))
	{
	  _cleanup_free_ char *errmsg = NULL;
	  show_error_popup("ERROR: SHA256 verification failed!",
			   "Wiping invalid data and aborting...", NULL);
	  if (zap_partition_tables(device, &errmsg) < 0)
	    show_error_popup("ERROR: wiping invalid data failed!",
			     errmsg, NULL);


	  return -EIO;
	}
    }
  else
    {
      r = write_local_image(url, device);
      if (r != 0)
	return r;
    }

  fix_partition_table(device);
  // Re-read partition table to update kernel view on disk
  _cleanup_close_ int fd = -EBADF;
  fd = open(device, O_RDWR | O_SYNC);
  if (fd > 0) // ignore error if we cannot open device
    ioctl(fd, BLKRRPART);

  if (preserve_ssh_hostkey && ssh_backup_dir)
    {
      MSG_INFO("Attempting to restore SSH host keys to %s", device);
      sleep(2);
      r = rdii_ssh_hostkey_restore(device, ssh_backup_dir);
      if (r < 0)
        {
          MSG_WARN("SSH host key restore failed: %s", strerror(-r));
        }
      else if (r > 0)
        {
          MSG_INFO("Successfully restored %d SSH host key(s)", r);
	  r = 0; // for final return from function
        }
    }

  keywait(LINES-3, 0, NULL, 0);

  return r;
}
