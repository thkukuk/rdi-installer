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
#define DT_HARDWARE       0x01
#define DT_ACPI           0x02
#define DT_MESSAGING      0x03
#define DT_MEDIA          0x04
#define DT_BIOS_BOOT      0x05 // XXX not implemented
#define DT_END            0x7f

/* Device Path SubTypes */
#define DST_HARD_DRIVE    0x01
#define DST_MEDIA_FILE    0x04
#define DST_MSG_MAC_ADDR  0x0b
#define DST_MSG_IPV4      0x0c
#define DST_MSG_IPV6      0x0d
#define DST_MSG_URI       0x18

typedef struct {
    uint8_t type;
    uint8_t sub_type;
    uint16_t length;
} __attribute__((packed)) efi_device_path_header_t;

// UEFI GUID Structure
typedef struct {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
} __attribute__((packed)) efi_guid_t;

typedef struct {
  uint8_t mac_addr[32];
  uint8_t if_type;
} __attribute__((packed)) mac_addr_device_path_t;

typedef struct {
  uint8_t LocalIp[4];
  uint8_t RemoteIp[4]; // Sometimes used for the server
  uint16_t LocalPort;
  uint16_t RemotePort;
  uint16_t Protocol;
  uint8_t StaticIpAddress;
  uint8_t GatewayIp[4];
  uint8_t SubnetMask[4];
} __attribute__((packed)) ipv4_device_path_t;

typedef struct {
    uint8_t function;
    uint8_t device;
} __attribute__((packed)) pci_device_path_t;

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
efi_boot_systemd_stub(efivars_t **res)
{
  _cleanup_free_ char *loader_url = NULL;
  _cleanup_free_ char *loader_dev = NULL;
  _cleanup_free_ char *loader_img = NULL;
  _cleanup_free_ char *loader_entry = NULL;
  int r;

  /* Check for selected boot entry */
  r = read_efi_var_string("LoaderEntrySelected", SYSTEMD_VENDOR_GUID,
			  &loader_entry);
  if (r < 0 && r != -ENOENT)
    return r;

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

  (*res)->url = TAKE_PTR(loader_url);
  (*res)->device = TAKE_PTR(loader_dev);
  (*res)->image = TAKE_PTR(loader_img);
  (*res)->entry = TAKE_PTR(loader_entry);

  return 0;
}

/* Parse standard BootCurrent -> BootXXXX */
static int
parse_device_path(char *data, size_t limit, efivars_t **res)
{
  _cleanup_free_ char *loader_url = NULL;
  _cleanup_free_ char *loader_dev = NULL;
  _cleanup_free_ char *loader_img = NULL;
  bool pxe_boot = false;
  size_t offset = 0;
  int r;

  while (offset < limit)
    {
      efi_device_path_header_t *head = (efi_device_path_header_t *)(data + offset);

      if (head->type == DT_END)
	{
	  if (head->sub_type) // End of Device Path
	    break;
	  if (_efivars_debug)
	    printf("Unexpected: type=0x7F, subtype=0x%02X\n", head->sub_type);
	}

      if (head->length < 4)
	{
	  if (_efivars_debug)
	    printf("length too short: type=%02X, subtype=%02X, length=%i\n",
		   head->type, head->sub_type, head->length);
	  break;
	}

      if (offset + head->length > limit)
	{
	  if (_efivars_debug)
	    printf("length bigger than limit: type=%02X, subtype=%02X, length=%i, limit=%lu\n",
		   head->type, head->sub_type, head->length, limit);
	  break;
	}

      if (head->type == DT_MEDIA)       /* hard disk */
	{
	  switch(head->sub_type)
	    {
	    case DST_HARD_DRIVE: // UUID of hard disk
	      {
		/* The Signature (UUID) is at offset 24 within this
		   node structure: Header(4) + PartitionNumber(4) +
		   StartLBA(8) + SizeLBA(8) + Signature(16) ... */
		if (head->length >= 42)
		  {
		    efi_guid_t *guid = (efi_guid_t*)(data + offset + 24);

		    if (asprintf(&loader_dev,
				 "%s%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x",
				 "/dev/disk/by-partuuid/",
				 guid->data1, guid->data2, guid->data3,
				 guid->data4[0], guid->data4[1],
				 guid->data4[2], guid->data4[3], guid->data4[4],
				 guid->data4[5], guid->data4[6], guid->data4[7]) < 0)
		      return -ENOMEM;

		    if (_efivars_debug)
		      printf("Partition UUID: %s\n", loader_dev);
		  }
		else if (_efivars_debug)
		  printf("DST_HARD_DRIVE: length (%d) too small (< 42)\n", head->length);
	      }
	      break;
	    case DST_MEDIA_FILE: // Disk Boot (File Path Node)
		r = utf16_to_utf8(data + offset + 4, head->length - 4, &loader_img);
		if (r < 0)
		  return r;
		break;
	    default:
	      if (_efivars_debug)
		printf("Unknown sub-type of DT_MEDIA: %02X\n", head->sub_type);
	      break;
	    }
	}
      else if (head->type == DT_MESSAGING) // PXE or HTTP Boot (URI Node)
	{
	  switch (head->sub_type)
	    {
	    case DST_MSG_URI:
	      r = utf16_to_utf8(data + offset + 4, head->length - 4, &loader_url);
	      if (r < 0)
		return r;
	      break;
	    case DST_MSG_MAC_ADDR:
	      if (_efivars_debug)
		{
		  mac_addr_device_path_t *mac = (mac_addr_device_path_t *)(data + offset + 4);
		  printf("MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
			 mac->mac_addr[0], mac->mac_addr[1],
			 mac->mac_addr[2], mac->mac_addr[3],
			 mac->mac_addr[4], mac->mac_addr[5]);
		}
	      pxe_boot = true;
	      break;
	    case DST_MSG_IPV4: // XXX print all fields
	      {
		ipv4_device_path_t *ipv4 = (ipv4_device_path_t *)(data + offset + 4);

		if (_efivars_debug)
		  printf("Remote IP: %d.%d.%d.%d\n",
			 ipv4->RemoteIp[0], ipv4->RemoteIp[1],
			 ipv4->RemoteIp[2], ipv4->RemoteIp[3]);
		if (!ipv4->RemoteIp[0] && !ipv4->RemoteIp[1] &&
		    !ipv4->RemoteIp[2] && !ipv4->RemoteIp[3])
		  pxe_boot = true; // IP 0.0.0.0 normally means PXE boot
	      }
	      break;
	    default:
	      if (_efivars_debug)
		printf("Unknown sub-type of DT_MESSAGING: %02X\n", head->sub_type);
	      break;
	    }
	}
      else if (head->type == DT_HARDWARE)
	{
	  if (head->sub_type == 0x01) // XXX -> 0x01 should be a define
	    {
	      pci_device_path_t *pci = (pci_device_path_t *)(data + offset + 4);
	      if (_efivars_debug)
		printf("Pci(Device=0x%x, Function=0x%x)\n", pci->device, pci->function);
	    }
	  else if (_efivars_debug)
	    printf("Unsupportd: DT_HARDWARE, subtype: %02X\n", head->sub_type);
	}
      else if (head->type == DT_ACPI)
	{
	  if (_efivars_debug)
	    printf("Unsupported: DT_ACPI, subtype: %02X\n", head->sub_type);
	}
      else if (_efivars_debug)
	printf("Unknown device path type: %02X, subtype: %02X\n",
	       head->type, head->sub_type);

      offset += head->length;
    }

  if (isempty(loader_url) && isempty(loader_dev) && isempty(loader_img) &&
      !pxe_boot)
    return -ENOENT;

  (*res)->url = TAKE_PTR(loader_url);
  (*res)->device = TAKE_PTR(loader_dev);
  (*res)->image = TAKE_PTR(loader_img);
  (*res)->is_pxe_boot = pxe_boot;

  return 0;
}

static int
efi_boot_current(efivars_t **res)
{
  _cleanup_free_ char *data = NULL;
  size_t size;
  int r;

  if (_efivars_debug)
    printf("Trying efi_boot_current()...\n");

  r = read_efi_var("BootCurrent", EFI_GLOBAL_VARIABLE_GUID, &data, &size);
  if (r < 0)
    return r;

  if (!data || size != 2)
    return -ENOENT;

  uint16_t boot_index = data[0] | (data[1] << 8);
  data = mfree(data);

  char boot_var_name[9];
  snprintf(boot_var_name, sizeof(boot_var_name), "Boot%04X", boot_index);

  if (_efivars_debug)
    printf("Reading %s\n", boot_var_name);

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
  _cleanup_free_ char *efi_desc = malloc(size - offset + 1);
  int i = 0;

  if (efi_desc == NULL)
    return -ENOMEM;

  // Skip Description (find double null terminator for UTF-16)
  while (offset + 1 < size)
    {
      efi_desc[i++] = data[offset];
      efi_desc[i++] = data[offset+1];
      offset+=2;
      if (data[offset-2] == 0 && data[offset-1] == 0)
	break;
    }
  if (i > 0)
    {
      _cleanup_free_ char *str = NULL;

      r = utf16_to_utf8(efi_desc, i, &str);
      if (r < 0)
	return r;

      if (_efivars_debug)
	printf("Description='%s'\n", str);

      (*res)->entry = TAKE_PTR(str);
    }

  if (offset < size)
    return parse_device_path(data + offset, size - offset, res);

  return -ENOENT;
}

int
efi_get_default_boot_partition(char **res_part)
{
  _cleanup_free_ char *data = NULL;
  size_t size;
  int r;

  if (_efivars_debug)
    printf("efi_get_default_boot_partition() called...\n");

  r = read_efi_var("BootOrder", EFI_GLOBAL_VARIABLE_GUID, &data, &size);
  if (r < 0)
    return r;

  if (!data || size < 2)
    return -ENOENT;

  // this can be more than one entry, but we only look at the first
  uint16_t boot_index = data[0] | (data[1] << 8);
  data = mfree(data);

  char boot_var_name[9];
  snprintf(boot_var_name, sizeof(boot_var_name), "Boot%04X", boot_index);

  if (_efivars_debug)
    printf("Reading %s\n", boot_var_name);

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

  if (offset >= size)
    return -ENOENT;

  _cleanup_efivars_ efivars_t *efi = calloc(1, sizeof (efivars_t));
  if (!efi)
    return -ENOMEM;

  r = parse_device_path(data + offset, size - offset, &efi);
  if (r < 0 && r != -ENODEV)
    return r;

  if (isempty(efi->device))
    return -ENODEV;

  if (_efivars_debug)
    printf("EFI default boot device: %s\n", strna(efi->device));

  (*res_part) = TAKE_PTR(efi->device);

  return 0;
}

// Return source of booted binary
int
efi_get_boot_source(efivars_t **res)
{
  _cleanup_efivars_ efivars_t *efi = NULL;
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

  efi = calloc(1, sizeof(efivars_t));
  if (!efi)
    return -ENOMEM;

  r = efi_boot_systemd_stub(&efi);
  if (r < 0 && _efivars_debug)
    fprintf(stderr, "efi_boot_systemd_stub: %s\n", strerror(-r));
  if (r == -ENOENT)
    r = efi_boot_current(&efi);
  if (r < 0)
    return r;

  r = efi_get_default_boot_partition(&(efi->def_efi_partition));
  if (r < 0)
    return r;

  *res = TAKE_PTR(efi);

  return 0;
}

efivars_t *
efivars_free(efivars_t *var)
{
  if (!var)
    return NULL;

  var->device = mfree(var->device);
  var->url = mfree(var->url);
  var->image = mfree(var->image);
  var->entry = mfree(var->entry);
  var->def_efi_partition = mfree(var->def_efi_partition);

  return NULL;
}
