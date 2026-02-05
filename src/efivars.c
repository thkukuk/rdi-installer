// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <fcntl.h>
#include <sys/stat.h>

#include "basics.h"
#include "efivars.h"

/*
 * Determines the boot source of a UKI/EFI binary by parsing EFI
 * variables in /sys/firmware/efi/efivars/.
 * Supports:
 * - HTTP Boot (prints URL)
 * - Local Disk Boot (prints Partition UUID or Device Path)
 */

bool _efivars_debug = false;

/* GUIDs */
#define EFI_GLOBAL_VARIABLE_GUID "8be4df61-93ca-11d2-aa0d-00e098032b8c"
#define SYSTEMD_VENDOR_GUID      "4a67b082-0a4c-41cf-b6c7-440b29bb8c4f"

/* Path to efivars */
#define EFIVARS_PATH "/sys/firmware/efi/efivars"

/* Device Path Types */
#define DT_HARDWARE    0x01
#define DT_MESSAGING   0x03
#define DT_MEDIA       0x04

/* Device Path SubTypes */
#define DST_HARD_DRIVE 0x01
#define DST_MEDIA_FILE 0x04
#define DST_MSG_URI    0x18

typedef struct {
    uint8_t type;
    uint8_t sub_type;
    uint16_t length;
} __attribute__((packed)) efi_device_path_header_t;

// UEFI GUID Structure
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} __attribute__((packed)) efi_guid_t;


/* Simple helper to convert UTF-16LE to ASCII/UTF-8 for
   printing. We assume that the string only contains ASCII
   characters. The resulting string will be NUL terminated. */
static int
utf16_to_utf8(const char *data, size_t len, char **ret)
{
  _cleanup_free_ char *utf8 = NULL;

  // Even lengths are wrong
  if (len % 2 != 0)
    return -EINVAL;

  utf8 = malloc(len/2 + 1);
  if (!utf8)
    return -ENOMEM;

  for (size_t i = 0; i < len/2; i++)
    {
      uint16_t c = data[i*2] | (data[(i*2) + 1] << 8);
      if (c == 0)
	{
	  utf8[i] = '\0';
	  break;
	}
      else if (c < 128)
	{
	  // convert DOS backlash to UNIX '/'
	  if (c == '\\')
	    utf8[i] = '/';
	  else
	    utf8[i] = c;
	}
      else
	return -ERANGE;
    }

  utf8[len/2] = '\0';
  *ret = TAKE_PTR(utf8);

  return 0;
}

/* Check if the file is a regular file. */
static int
stat_verify_regular(const struct stat *st)
{
  if (S_ISDIR(st->st_mode))
    return -EISDIR;

  if (S_ISLNK(st->st_mode))
    return -ELOOP;

  if (!S_ISREG(st->st_mode))
    return -EBADFD;

  return 0;
}

/* Reads an EFI variable and returns malloc'd buffer containing the data */
int
read_efi_var(const char *name, const char *guid, char **ret_str,
	     size_t *ret_size)
{
  _cleanup_free_ char *path = NULL;
  size_t size = 0;
  int r;

  if (asprintf(&path, "%s/%s-%s", EFIVARS_PATH, name, guid) < 0)
    return -ENOMEM;

  _cleanup_close_ int fd = open(path, O_RDONLY|O_NOCTTY|O_CLOEXEC);
  if (fd < 0)
    {
      r = -errno;
      if (_efivars_debug && r != -ENOENT)
	fprintf(stderr, "Couldn't open '%s': %s\n", path, strerror(-r));
      return r;
    }

  _cleanup_free_ char *buf = NULL;
  struct stat st;

  if (fstat(fd, &st) < 0)
    {
      r = -errno;
      if (_efivars_debug)
	fprintf(stderr, "fstat(%s) failed: %s\n", path, strerror(-r));
      return -r;
    }

  r = stat_verify_regular(&st);
  if (r < 0)
    {
      if (_efivars_debug)
	fprintf(stderr, "EFI variable '%s' is not a regular file: %s",
		path, strerror(-r));
      return r;
    }

  size = st.st_size;
  buf = malloc(size);
  if (!buf)
    return -ENOMEM;

  ssize_t bytes_read = read(fd, buf, size);
  if (bytes_read < 0)
    {
      r = -errno;
      if (_efivars_debug)
	fprintf(stderr, "Error reading '%s': %s\n", path, strerror(-r));
      return r;
    }

  /* * efivarfs files start with 4 bytes of Attributes.
   * We need to strip them to get the actual payload.
   */
  size = size - 4;
  _cleanup_free_ char *data = malloc(size);
  if (!data)
    return -ENOMEM;

  // Copy payload (skip first 4 bytes)
  memcpy(data, buf + 4, size);

  *ret_str = TAKE_PTR(data);
  *ret_size = size;

  return 0;
}

/* Reads an EFI variable, converts from utf16 to utf8 and returns a
   malloc'd NUL terminated string */
int
read_efi_var_string(const char *name, const char *guid, char **ret)
{
  _cleanup_free_ char *data = NULL;
  size_t size;
  int r;

  r = read_efi_var(name, guid, &data, &size);
  if (r < 0)
    return r;

  return utf16_to_utf8(data, size, ret);
}

/* Use systemd-stub specific variables */
static int
efi_boot_systemd_stub(char **url, char **device, char **image)
{
  _cleanup_free_ char *loader_url = NULL;
  _cleanup_free_ char *loader_dev = NULL;
  _cleanup_free_ char *loader_img = NULL;
  int r;

  /* Check for Network Boot URL */
  r = read_efi_var_string("LoaderDeviceURL", SYSTEMD_VENDOR_GUID,
			  &loader_url);
  if (r < 0 && r != -ENOENT)
    return r;

  /* Check for Disk Boot (Partition UUID) */
  r = read_efi_var_string("LoaderDevicePartUUID", SYSTEMD_VENDOR_GUID,
			  &loader_dev);
  if (r < 0 && r != -ENOENT)
    return r;

  if (loader_dev)
    {
      // /dev/disk/by-partuuid is lowercase
      char *p = loader_dev;
      for ( ; *p; ++p) *p = tolower(*p);

      _cleanup_free_ char *str = NULL;
      if (asprintf(&str, "/dev/disk/by-partuuid/%s", loader_dev) < 0)
	return -ENOMEM;
      loader_dev = mfree(loader_dev);
      loader_dev = TAKE_PTR(str);

      r = read_efi_var_string("LoaderImageIdentifier", SYSTEMD_VENDOR_GUID,
			      &loader_img);
      if (r < 0 && r != -ENOENT)
	return r;
    }

  if (isempty(loader_url) && isempty(loader_dev) && isempty(loader_img))
    return -ENOENT;

  *url = TAKE_PTR(loader_url);
  *device = TAKE_PTR(loader_dev);
  *image = TAKE_PTR(loader_img);

  return 0;
}

/* Parse standard BootCurrent -> BootXXXX */
static int
parse_device_path(char *data, size_t limit, char **url, char **device, char **image)
{
  _cleanup_free_ char *loader_url = NULL;
  _cleanup_free_ char *loader_dev = NULL;
  _cleanup_free_ char *loader_img = NULL;
  size_t offset = 0;
  int r;

  while (offset < limit)
    {
      efi_device_path_header_t *head = (efi_device_path_header_t *)(data + offset);

      if (head->type == 0x7F) // End of Device Path
	break;

      if (head->length < 4)
	break;

      if (offset + head->length > limit)
	break;

    /* Case: HTTP Boot (URI Node) */
    if (head->type == DT_MESSAGING && head->sub_type == DST_MSG_URI)
      {
	r = utf16_to_utf8(data + offset + 4, head->length - 4, &loader_url);
	if (r < 0)
	  return r;
      }

    /* Case: UUID of hard disk */
    if (head->type == DT_MEDIA && head->sub_type == DST_HARD_DRIVE)
      {
	// The Signature (UUID) is at offset 24 within this node structure
	// Struct: Header(4) + PartitionNumber(4) + StartLBA(8) +
	//         SizeLBA(8) + Signature(16) ...
	if (head->length >= 42)
	  {
	    efi_guid_t *guid = (efi_guid_t*)(data + offset + 24);

	    if (asprintf(&loader_dev,
			 "%s%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
			 "/dev/disk/by-partuuid/",
			 guid->Data1, guid->Data2, guid->Data3,
			 guid->Data4[0], guid->Data4[1],
			 guid->Data4[2], guid->Data4[3], guid->Data4[4],
			 guid->Data4[5], guid->Data4[6], guid->Data4[7]) < 0)
	      return -ENOMEM;

	    printf("Partition UUID: %s\n", loader_dev);
	  }
      }

    /* Case: Disk Boot (File Path Node) */
    if (head->type == DT_MEDIA && head->sub_type == DST_MEDIA_FILE)
      {
	r = utf16_to_utf8(data + offset + 4, head->length - 4, &loader_img);
	if (r < 0)
	  return r;
      }

    offset += head->length;
    }

  if (isempty(loader_url) && isempty(loader_dev) && isempty(loader_img))
    return -ENOENT;

  *url = TAKE_PTR(loader_url);
  *device = TAKE_PTR(loader_dev);
  *image = TAKE_PTR(loader_img);

  return 0;
}

static int
efi_boot_current(char **url, char **device, char **image)
{
  _cleanup_free_ char *data = NULL;
  size_t size;
  int r;

  r = read_efi_var("BootCurrent", EFI_GLOBAL_VARIABLE_GUID, &data, &size);
  if (r < 0)
    return r;

  if (!data || size != 2)
    return -ENOENT;

  uint16_t boot_index = data[0] | (data[1] << 8);
  data = mfree(data);

  char boot_var_name[9];
  snprintf(boot_var_name, sizeof(boot_var_name), "Boot%04X", boot_index);

  r = read_efi_var(boot_var_name, EFI_GLOBAL_VARIABLE_GUID, &data, &size);
  if (r < 0)
    return r;

  if (!data)
    return -ENOENT;

  /*
   * BootXXXX Format:
   * - Attributes (4 bytes)
   * - FilePathListLength (2 bytes)
   * - Description (Null-terminated UTF-16 string)
   * - FilePathList (Device Path)
   */
  if (size < 6)
    return -EINVAL;

  size_t offset = 6;

  // Skip Description (find double null terminator for UTF-16)
  while (offset + 1 < size)
    {
      offset+=2;
      if (data[offset-2] == 0 && data[offset-1] == 0)
	break;
    }

  if (offset < size)
    {
      parse_device_path(data + offset, size - offset, url, device, image);
      return 0;
    }

  return -ENOENT;
}

// Return source of booted binary
int
efi_get_boot_source(char **url, char **device, char **image)
{
  _cleanup_free_ char *loader_url = NULL;
  _cleanup_free_ char *loader_dev = NULL;
  _cleanup_free_ char *loader_img = NULL;
  int r;

  if (access(EFIVARS_PATH, F_OK) != 0)
    {
      r = -errno;
      if (_efivars_debug)
	  fprintf(stderr, "Error: %s is not accessible: %s\n",
		  EFIVARS_PATH, strerror(-r));
      if (r == -ENOENT)
	return -EOPNOTSUPP;
      return r;
    }

  r = efi_boot_systemd_stub(&loader_url, &loader_dev, &loader_img);
  if (r < 0 && _efivars_debug)
    fprintf(stderr, "efi_boot_systemd_stub: %s\n", strerror(-r));
  if (r == -ENOENT)
    r = efi_boot_current(&loader_url, &loader_dev, &loader_img);

  *url = TAKE_PTR(loader_url);
  *device = TAKE_PTR(loader_dev);
  *image = TAKE_PTR(loader_img);

  return r;
}
