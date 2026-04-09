// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <stdarg.h>
#include <spawn.h>
#include <sys/wait.h>

#include "basics.h"
#include "exec_cmd.h"

extern char **environ;

/* Executes a command using posix_spawnp and waits for it to finish.
   Returns 0 on success, -errno on failure, > 0 exit code of the
   called command. */
int
exec_cmd(const char *cmd, ...)
{
  _cleanup_free_ char **argv = NULL;
  va_list args, args_copy;
  int argc;
  int r;

  argc = 0;
  va_start(args, cmd);
  va_copy(args_copy, args);
  while (va_arg(args_copy, const char *) != NULL)
    argc++;
  va_end(args_copy);

  argv = malloc((argc + 1) * sizeof(char *));
  if (!argv)
    {
      va_end(args);
      return -ENOMEM;
    }

  for (int i = 0; i < argc; i++)
    argv[i] = (char *)va_arg(args, const char *);
  argv[argc] = NULL;
  va_end(args);

  pid_t pid;
  r = posix_spawnp(&pid, cmd, NULL, NULL, argv, environ);

  if (r != 0)
    return -r;

  // Wait for the child process to complete
  int wait_status;
  if (waitpid(pid, &wait_status, 0) == -1)
    return -r;

  // Evaluate the exit status
  if (WIFEXITED(wait_status))
    {
      int exit_code = WEXITSTATUS(wait_status);
      if (exit_code != 0)
	return exit_code;
      return 0;
    }
  else if (WIFSIGNALED(wait_status))
    {
      int sig = WTERMSIG(wait_status);
      // Conventionally, shells return 128 + signal number for fatal signals
      return 128 + sig;
    }

  // fprintf(stderr, "Command '%s' terminated in an unknown state.\n", cmd);
  return -EIO;
}
