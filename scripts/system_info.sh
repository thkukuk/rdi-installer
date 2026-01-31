# SPDX-License-Identifier: MIT
# shellcheck shell=bash

system_information()
{
    #    INFO_CPU=$(lscpu | grep -E 'Model name|Socket\(s\)|Thread\(s\) per core|Core\(s\) per socket')
    INFO_CPU="CPU:      $(grep model.name /proc/cpuinfo | sort -u | sed -e 's|model name.*:||g')"
    INFO_VENDOR="Vendor:    $(cat /sys/class/dmi/id/bios_vendor)"
    INFO_VERSION="Version:   $(cat /sys/class/dmi/id/bios_version)"
    INFO_BIOS_DATE="Bios Date: $(cat /sys/class/dmi/id/bios_date)"
    INFO_MEMORY=$(free -h)
    INFO_IPV4="IPv4:      $(ip -4 addr show | grep -oP '(?<=inet\s)\d+(\.\d+){3}' | grep -v '127.0.0.1' | tr '\n' ' ')"
#    INFO_IPV6="IPv6: $(ip -6 addr show | grep -oP '(?<=inet6\s)[\da-f:]+' | grep -v '::1' | tr '\n' ' ')"

    gum style \
	--align="left" \
	--foreground="$COLOR_FOREGROUND" \
	"$INFO_CPU" \
	"$INFO_VENDOR" \
	"$INFO_VERSION" \
	"$INFO_BIOS_DATE" \
	"$INFO_MEMORY" \
	"$INFO_IPV4"
#	"$INFO_IPV6"
    echo ""
    $KEYWAIT -s 0
}
