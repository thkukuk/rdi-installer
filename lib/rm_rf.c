// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

#include "basics.h"

#include "rm_rf.h"

static int
rm_rf_at(int parent_fd, const char *name)
{
  struct stat st;
  int r;

  // Don't following symlinks
  if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
    return -errno;

  if (S_ISDIR(st.st_mode))
    {
      _cleanup_close_ int fd = -EBADF;

      fd = openat(parent_fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      if (fd < 0)
	return -errno;

      DIR *dir = fdopendir(fd);
      if (!dir)
	return -errno;
      fd = -EBADF;

      struct dirent *entry;
      int result = 0;

      while ((entry = readdir(dir)) != NULL)
	{
	  if (streq(entry->d_name, ".") || streq(entry->d_name, ".."))
	    continue;

	  r = rm_rf_at(dirfd(dir), entry->d_name);
	  if (r < 0)
	    result = r;
        }
      closedir(dir);

      if (result != 0)
	return result;

      if (unlinkat(parent_fd, name, AT_REMOVEDIR) < 0)
	return -errno;
    }
  else
    {
      if (unlinkat(parent_fd, name, 0) < 0)
	return -errno;
    }
  return 0;
}

/* Recursively deletes a file or directory safely using relative file
   descriptors.
   Returns 0 on success, -errno on failure. */
int
rm_rf(const char *path)
{
  return rm_rf_at(AT_FDCWD, path);
}
