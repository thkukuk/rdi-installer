// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

extern bool rdii_autoinstall(const char *image, const char *device,
                             const char *mdraid, bool preserve_ssh_hostkey,
                             const char *autoinstall_finish, int *ret);
