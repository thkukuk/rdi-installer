// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <blkid/blkid.h>

#include "basics.h"
#include "logger.h"
#include "rdii-ssh-hostkey.h"
#include "rdii-menu.h"

#define SSH_HOSTKEY_PATTERN "ssh_host_"

typedef struct {
  char *path;
  char *fstype;
} partition_info_t;

typedef struct {
  partition_info_t *partitions;
  int count;
} partition_list_t;

static void
free_partition_list(partition_list_t *pl)
{
  if (!pl || !pl->partitions)
    return;

  for (int i = 0; i < pl->count; i++)
    {
      free(pl->partitions[i].path);
      free(pl->partitions[i].fstype);
    }
  free(pl->partitions);
  pl->partitions = NULL;
  pl->count = 0;
}

#if 0
static inline void
free_partition_listp(partition_list_t **pl)
{
  if (*pl)
    free_partition_list(*pl);
  *pl = mfree(*pl);
}
#endif

static inline void
blkid_free_probep(blkid_probe *pr)
{
  if (*pr)
    blkid_free_probe(*pr);
  *pr = NULL;
}

static int
get_mount_point(char **ret_path)
{
  _cleanup_free_ char *mount_point = NULL;
  struct stat st;

  if (asprintf(&mount_point, "%s/rdii-ssh-mount", rdii_tmp_dir) < 0)
    return -ENOMEM;

  if (stat(mount_point, &st) < 0)
    {
      if (mkdir(mount_point, 0700) < 0)
        {
          int r = errno;
          MSG_ERROR("Failed to create mount point '%s': %s", mount_point, strerror(r));
          return -r;
        }
    }

  *ret_path = TAKE_PTR(mount_point);
  return 0;
}

static int
find_linux_partitions(const char *device, partition_list_t *pl_out)
{
  _cleanup_(blkid_free_probep) blkid_probe pr = NULL;
  blkid_partlist ls;
  int nparts, i;
  int max_parts = 10;
  int r = 0;

  MSG_FUNC("device='%s'", device);

  partition_info_t *partitions = calloc(max_parts, sizeof(partition_info_t));
  if (!partitions)
    return -ENOMEM;

  pr = blkid_new_probe_from_filename(device);
  if (!pr)
    {
      MSG_WARN("Failed to create blkid probe for '%s'", device);
      free(partitions);
      return -ENOENT;
    }

  blkid_probe_enable_partitions(pr, 1);
  blkid_probe_set_partitions_flags(pr, BLKID_PARTS_ENTRY_DETAILS);

  r = blkid_do_probe(pr);
  if (r != 0)
    {
      MSG_DEBUG("No partition table found on '%s'", device);
      pl_out->partitions = partitions;
      pl_out->count = 0;
      return 0;
    }

  ls = blkid_probe_get_partitions(pr);
  if (!ls)
    {
      MSG_DEBUG("Failed to get partition list for '%s'", device);
      pl_out->partitions = partitions;
      pl_out->count = 0;
      return 0;
    }

  nparts = blkid_partlist_numof_partitions(ls);
  MSG_DEBUG("Found %d partition(s) on %s", nparts, device);

  int count = 0;
  for (i = 0; i < nparts; i++)
    {
      blkid_partition par = blkid_partlist_get_partition(ls, i);
      if (!par)
        continue;

      int partno = blkid_partition_get_partno(par);
      _cleanup_free_ char *partname = NULL;

      // Handle partition naming: nvme0n1p1, mmcblk0p1 vs sda1, vda1
      if (strstr(device, "nvme") || strstr(device, "mmcblk") ||
          strstr(device, "loop"))
        {
          if (asprintf(&partname, "%sp%d", device, partno) < 0)
	    return -ENOMEM;
        }
      else
        {
          if (asprintf(&partname, "%s%d", device, partno) < 0)
	    return -ENOMEM;
        }

      _cleanup_(blkid_free_probep) blkid_probe pr_part = blkid_new_probe_from_filename(partname);
      if (!pr_part)
        continue;

      blkid_probe_enable_superblocks(pr_part, 1);
      blkid_probe_set_superblocks_flags(pr_part, BLKID_SUBLKS_TYPE);

      if (blkid_do_safeprobe(pr_part) == 0)
        {
          const char *type = NULL;
          if (blkid_probe_lookup_value(pr_part, "TYPE", &type, NULL) == 0)
            {
              if (streq(type, "ext4") || streq(type, "ext3") || streq(type, "ext2") ||
                  streq(type, "xfs") || streq(type, "btrfs"))
                {
                  if (count >= max_parts)
                    {
                      max_parts *= 2;
                      partition_info_t *new_parts = realloc(partitions, max_parts * sizeof(partition_info_t));
                      if (!new_parts)
                        {
                          for (int j = 0; j < count; j++)
                            {
                              free(partitions[j].path);
                              free(partitions[j].fstype);
                            }
                          free(partitions);
                          return -ENOMEM;
                        }
                      partitions = new_parts;
                    }
                  partitions[count].path = strdup(partname);
                  partitions[count].fstype = strdup(type);
                  if (!partitions[count].path || !partitions[count].fstype)
                    {
                      for (int j = 0; j <= count; j++)
                        {
                          free(partitions[j].path);
                          free(partitions[j].fstype);
                        }
                      free(partitions);
                      return -ENOMEM;
                    }
                  MSG_DEBUG("Found Linux partition: %s (type=%s)", partitions[count].path, type);
                  count++;
                }
            }
        }
    }

  pl_out->partitions = partitions;
  pl_out->count = count;

  MSG_INFO("Found %d Linux partition(s) on %s", count, device);
  return 0;
}

static int
copy_ssh_hostkeys(const char *src_dir, const char *dst_dir)
{
  _cleanup_closedir_ DIR *dir = NULL;
  struct dirent *entry;
  int copied = 0;

  MSG_FUNC("src_dir='%s', dst_dir='%s'", src_dir, dst_dir);

  dir = opendir(src_dir);
  if (!dir)
    {
      int r = errno;
      MSG_ERROR("Failed to open directory '%s': %s", src_dir, strerror(r));
      return -r;
    }

  while ((entry = readdir(dir)) != NULL)
    {
      if (strncmp(entry->d_name, SSH_HOSTKEY_PATTERN, strlen(SSH_HOSTKEY_PATTERN)) == 0)
        {
          _cleanup_free_ char *src_path = NULL;
          _cleanup_free_ char *dst_path = NULL;

          if (asprintf(&src_path, "%s/%s", src_dir, entry->d_name) < 0)
	    return -ENOMEM;

          if (asprintf(&dst_path, "%s/%s", dst_dir, entry->d_name) < 0)
	    return -ENOMEM;

          _cleanup_close_ int src_fd = -EBADF;
          _cleanup_close_ int dst_fd = -EBADF;
          struct stat st;

          src_fd = open(src_path, O_RDONLY);
          if (src_fd < 0)
            {
              int r = errno;
              MSG_WARN("Failed to open '%s': %s", src_path, strerror(r));
              continue;
            }

          if (fstat(src_fd, &st) < 0)
            {
              int r = errno;
              MSG_WARN("Failed to stat '%s': %s", src_path, strerror(r));
              continue;
            }

          dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
          if (dst_fd < 0)
            {
              int r = errno;
              MSG_ERROR("Failed to create '%s': %s", dst_path, strerror(r));
              return -r;
            }

	  /* Allocate exact buffer size on the heap */
          off_t file_size = st.st_size;
          char *buf = malloc(file_size);
          if (buf == NULL)
            {
              int r = errno;
              MSG_ERROR("Failed to allocate %ld bytes: %s",
			(long)file_size, strerror(r));
	      return -r;
	    }

          /* Read the entire file into memory */
          ssize_t total_read = 0;
          while (total_read < file_size)
            {
               ssize_t nread = read(src_fd, buf + total_read,
				    file_size - total_read);
	       if (nread <= 0)
                 {
                   if (nread < 0)
                     {
                       int r = errno;
		       MSG_ERROR("Failed to read from source: %s", strerror(r));
		       free(buf);
		       return -r;
		     }
		   break; /* Unexpected EOF (file may have shrunk) */
		 }
	       total_read += nread;
	    }

	  /* Write the entire buffer in one go */
	  ssize_t total_written = 0;
	  while (total_written < total_read)
            {
              ssize_t n = write(dst_fd, buf + total_written,
				total_read - total_written);
	      if (n < 0)
		{
                  int r = errno;
		  MSG_ERROR("Failed to write to '%s': %s", dst_path,
			    strerror(r));
		  free(buf);
		  return -r;
		}
	      total_written += n;
	    }

          /* Free allocated memory */
	  free(buf);

          MSG_INFO("Copied SSH host key: %s", entry->d_name);
          copied++;
        }
    }

  MSG_INFO("Copied %d SSH host key(s) from '%s' to '%s'", copied, src_dir, dst_dir);
  return copied;
}

static int
try_backup_from_partition(const char *partition, const char *fstype, const char *backup_dir)
{
  _cleanup_free_ char *ssh_dir = NULL;
  _cleanup_free_ char *mount_point = NULL;
  int r;

  MSG_FUNC("partition='%s', fstype='%s', backup_dir='%s'", partition, fstype, backup_dir);

  r = get_mount_point(&mount_point);
  if (r < 0)
    return r;

  r = mount(partition, mount_point, fstype, MS_RDONLY, NULL);
  if (r < 0)
    {
      r = errno;
      MSG_WARN("Failed to mount '%s': %s", partition, strerror(r));
      return -r;
    }

  if (asprintf(&ssh_dir, "%s/etc/ssh", mount_point) < 0)
    {
      umount(mount_point);
      return -ENOMEM;
    }

  struct stat st;
  if (stat(ssh_dir, &st) < 0 || !S_ISDIR(st.st_mode))
    {
      MSG_DEBUG("No /etc/ssh directory found on %s", partition);
      umount(mount_point);
      return -ENOENT;
    }

  r = copy_ssh_hostkeys(ssh_dir, backup_dir);
  umount(mount_point);

  if (r > 0)
    MSG_INFO("Backed up %d SSH host key(s) from %s", r, partition);

  return r;
}

int
rdii_ssh_hostkey_backup(const char *device, const char *backup_dir)
{
  _cleanup_(free_partition_list) partition_list_t pl = {0};
  int r;
  struct stat st;

  MSG_FUNC("device='%s', backup_dir='%s'", device, backup_dir);

  if (stat(backup_dir, &st) < 0)
    {
      if (mkdir(backup_dir, 0700) < 0)
        {
          r = errno;
          MSG_ERROR("Failed to create backup directory '%s': %s", backup_dir, strerror(r));
          return -r;
        }
    }

  r = find_linux_partitions(device, &pl);
  if (r < 0)
    return r;

  if (pl.count == 0)
    {
      MSG_INFO("No Linux partitions found on %s, skipping SSH key backup", device);
      return 0;
    }

  for (int i = 0; i < pl.count; i++)
    {
      r = try_backup_from_partition(pl.partitions[i].path, pl.partitions[i].fstype, backup_dir);
      if (r > 0)
        return r;
    }

  MSG_WARN("No SSH host keys found on any partition of %s", device);
  return 0;
}

static int
try_restore_to_partition(const char *partition, const char *fstype, const char *backup_dir)
{
  _cleanup_free_ char *ssh_dir = NULL;
  _cleanup_free_ char *mount_point = NULL;
  int r;
  DIR *dir;
  struct dirent *entry;
  bool has_hostkeys = false;

  MSG_FUNC("partition='%s', fstype='%s', backup_dir='%s'", partition, fstype, backup_dir);

  r = get_mount_point(&mount_point);
  if (r < 0)
    return r;

  r = mount(partition, mount_point, fstype, 0, NULL);
  if (r < 0)
    {
      r = errno;
      MSG_WARN("Failed to mount '%s': %s", partition, strerror(r));
      return -r;
    }

  if (asprintf(&ssh_dir, "%s/etc/ssh", mount_point) < 0)
    {
      umount(mount_point);
      return -ENOMEM;
    }

  struct stat st;
  if (stat(ssh_dir, &st) < 0 || !S_ISDIR(st.st_mode))
    {
      MSG_DEBUG("No /etc/ssh directory found on %s", partition);
      umount(mount_point);
      return -ENOENT;
    }

  dir = opendir(ssh_dir);
  if (dir)
    {
      while ((entry = readdir(dir)) != NULL)
        {
          if (strncmp(entry->d_name, SSH_HOSTKEY_PATTERN, strlen(SSH_HOSTKEY_PATTERN)) == 0)
            {
              has_hostkeys = true;
              break;
            }
        }
      closedir(dir);
    }

  if (has_hostkeys)
    {
      MSG_INFO("SSH host keys already exist on %s, skipping restore", partition);
      umount(mount_point);
      return -EEXIST;
    }

  r = copy_ssh_hostkeys(backup_dir, ssh_dir);
  umount(mount_point);

  if (r > 0)
    MSG_INFO("Restored %d SSH host key(s) to %s", r, partition);

  return r;
}

int
rdii_ssh_hostkey_restore(const char *device, const char *backup_dir)
{
  _cleanup_(free_partition_list) partition_list_t pl = {0};
  int r;
  struct stat st;

  MSG_FUNC("device='%s', backup_dir='%s'", device, backup_dir);

  if (stat(backup_dir, &st) < 0)
    {
      MSG_INFO("No SSH host key backup directory found, skipping restore");
      return 0;
    }

  DIR *dir = opendir(backup_dir);
  if (!dir)
    {
      MSG_INFO("Cannot open backup directory, skipping restore");
      return 0;
    }

  bool has_backup = false;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL)
    {
      if (strncmp(entry->d_name, SSH_HOSTKEY_PATTERN, strlen(SSH_HOSTKEY_PATTERN)) == 0)
        {
          has_backup = true;
          break;
        }
    }
  closedir(dir);

  if (!has_backup)
    {
      MSG_INFO("No SSH host keys in backup directory, skipping restore");
      return 0;
    }

  r = find_linux_partitions(device, &pl);
  if (r < 0)
    return r;

  if (pl.count == 0)
    {
      MSG_INFO("No Linux partitions found on %s, cannot restore SSH keys", device);
      return 0;
    }

  for (int i = 0; i < pl.count; i++)
    {
      r = try_restore_to_partition(pl.partitions[i].path, pl.partitions[i].fstype, backup_dir);
      if (r > 0)
        return r;
    }

  MSG_WARN("Could not restore SSH host keys to any partition of %s", device);
  return 0;
}
