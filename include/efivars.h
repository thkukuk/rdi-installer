// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Determines the boot source of a UKI/EFI binary by parsing EFI
 * variables in /sys/firmware/efi/efivars/.
 * Supports:
 * - HTTP Boot (prints URL)
 * - Local Disk Boot (prints Partition UUID or Device Path)
 */

#pragma once

typedef struct {
  char *device;
  char *url;
  char *image;
  char *entry;
  bool is_pxe_boot;
  char *def_efi_partition;
} efivars_t;

extern efivars_t *efivars_free(efivars_t *var);
/* helper so that __attribute__((cleanup(efivars_free)) may be used */
static __inline__ void efivars_freep(efivars_t **var) {
  if (*var)
    *var = efivars_free(*var);
}
#define _cleanup_efivars_ __attribute__((__cleanup__(efivars_freep)))

// enables output on stderr with more concret error messages
extern bool _efivars_debug;

// Reads an EFI variable and returns malloc'd buffer containing the data
extern int read_efi_var(const char *name, const char *guid,
		        char **ret_str, size_t *ret_size);
// Reads an EFI variable, returns NUL terminated string.
extern int read_efi_var_string(const char *name, const char *guid,
		               char **ret);
// Returns the url/device/path from where the image to loaded
extern int efi_get_boot_source(efivars_t **var);
// Returns device of default EFI boot device if it is a disk
extern int efi_get_default_boot_partition(char **res_part);

