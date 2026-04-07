// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/mount.h>

#include "basics.h"

#include "zap_partition_table.h"

static int
error_handler(int err, const char *info, const char *device, char **error)
{
  if (error)
    {
      _cleanup_free_ char *msg = NULL;
      if (asprintf(&msg, "%s '%s': %s",
		   info, device, strerror(errno)) < 0)
	return -ENOMEM;
      *error = TAKE_PTR(msg);
    }
  return -err;
}

/* Zaps the MBR, Primary GPT, and Backup GPT on a block device,
   replicating `sgdisk --zap-all`.
   returns 0 on success, -errno on failure. */
int
zap_partition_tables(const char *device, char **error)
{
  _cleanup_close_ int fd = -EBADF;
  _cleanup_free_ char *zero_buf = NULL;

  fd = open(device, O_RDWR | O_SYNC);
  if (fd < 0)
    return error_handler(errno, "Error opening device", device, error);

  int sector_size = 0;
  if (ioctl(fd, BLKSSZGET, &sector_size) < 0)
    return error_handler(errno, "Error getting sector size for",
			 device, error);

  uint64_t disk_size = 0;
  if (ioctl(fd, BLKGETSIZE64, &disk_size) < 0)
    return error_handler(errno, "Error getting disk size for", device, error);

  /* Calculate wipe sizes based on standard GPT specifications:
     Primary: LBA 0 to 33 (34 sectors total)
     Backup: Last 33 sectors */
  ssize_t primary_wipe_size = 34 * sector_size;
  ssize_t backup_wipe_size = 33 * sector_size;

  if (disk_size < (uint64_t)(primary_wipe_size + backup_wipe_size))
    return 0; // no space for a partition table, nothing to do.

  zero_buf = calloc(1, primary_wipe_size);
  if (!zero_buf)
    return -ENOMEM;

  ssize_t written = pwrite(fd, zero_buf, primary_wipe_size, 0);
  if (written != primary_wipe_size)
    return error_handler(errno, "Failed to wipe primary GPT/MBR on",
			 device, error);

  off_t backup_offset = disk_size - backup_wipe_size;
  written = pwrite(fd, zero_buf, backup_wipe_size, backup_offset);
  if (written != backup_wipe_size)
    return error_handler(errno, "Failed to wipe backup GPT on",
			 device, error);

  fsync(fd);

  // Re-read partition table to update kernel view on disk
  ioctl(fd, BLKRRPART);

  return 0;
}
