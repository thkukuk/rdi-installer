// SPDX-License-Identifier: GPL-2.0-or-later

/*
 * Determines the boot source of a UKI/EFI binary by parsing EFI
 * variables in /sys/firmware/efi/efivars/.
 * Supports:
 * - HTTP Boot (prints URL)
 * - Local Disk Boot (prints Partition UUID or Device Path)
 */

#pragma once

// enables output on stderr with more concret error messages
extern bool _efivars_debug;

// Reads an EFI variable and returns malloc'd buffer containing the data 
extern int read_efi_var(const char *name, const char *guid,
		        char **ret_str, size_t *ret_size);
// Reads an EFI variable, returns NUL terminated string.
extern int read_efi_var_string(const char *name, const char *guid, 
		               char **ret);
// Returns the url/device/path from where the image to loaded
extern int efi_get_boot_source(char **url, char **device, char **image);

