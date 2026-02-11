// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Structure to hold the parsed ip= and ifcfg= configuration
typedef struct {
  const char *client_ip;
  const char *peer_ip;
  const char *gateway;
  const char *destination;
  int netmask;
  const char *hostname;
  const char *interface;
  const char *autoconf;
  int use_dns; // 0 = unset, 1 = false, 2 = true
  char *dns1;
  char *dns2;
  char *ntp;
  char *mtu;
  char *macaddr;
  char *domains;
} ip_t;

extern bool debug;

extern int return_syntax_error(int line, const char *value, const int ret);
