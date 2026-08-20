// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

extern const char *rdii_tmp_dir;
extern const char *rdii_download_server;

extern int is_device_mounted(const char *device);

extern void keywait(int y, int x, const char *text, int sec);

extern int select_target_device(uint64_t minsize, char **device);
extern int select_mdraid_devices(uint64_t minsize, char **device1, char **device2);
extern void select_installation_source(const char *prefill, char **ret);
extern int show_sysinfo(void);
extern bool verify_signature(const char *file, char *key, char **error);
extern int run_installation(const char *url, const char *device,
		            const char *mdraid, bool preserve_ssh_hostkey);
extern int rdii_menu(const char *title, const char *image,
		     const char *image1, const char *image2,
		     const char *device, const char *mdraid, bool preserve_ssh_hostkey);

static inline void cleanup_string_array(char ***p) {
    if (!p || !*p)
        return;
    char **arr = *p;
    for (size_t i = 0; arr[i] != NULL; i++) {
        free(arr[i]);
    }
    free(arr);
}
#define _cleanup_str_array_ _cleanup_(cleanup_string_array)

