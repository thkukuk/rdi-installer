// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "rdii-networkd.h"

extern int parse_ip_arg(int nr, char *arg, ip_t *cfg);
extern int parse_nameserver_arg(int nr, char *arg, ip_t *cfg);
extern int parse_rd_route_arg(int nr, char *arg, ip_t *cfg);
extern int parse_rd_peerdns_arg(int nr, char *arg, ip_t *cfg);
extern int parse_vlan_arg(int nr, char *arg, ip_t *cfg);

extern int extract_word(char **str, const char *sep, bool required, char **ret);

typedef struct {
    const char *key;
    const char *value;
} kv_map_t;

extern const char *map_lookup(const kv_map_t *table, const char *input, const char *valid);
