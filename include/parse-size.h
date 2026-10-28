// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <stdint.h>

/* Parses a size given as plain number of bytes, or a number followed by
   an optional "B" and a K/M/G/T suffix (binary units, e.g. "500G",
   "500GB", "1T", "1TB"). Returns 0 on success, or a negative errno. */
extern int parse_size(const char *str, uint64_t *res);
