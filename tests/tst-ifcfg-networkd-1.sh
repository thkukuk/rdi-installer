#!/bin/bash

set -e

cleanup()
{
    local exit_code=$?

    if [ -n "$TEMPDIR" ] && [ -d "$TEMPDIR" ]; then
        rm -rf "$TEMPDIR"
    fi

    exit $exit_code
}

trap cleanup EXIT


TEMPDIR=$(mktemp -d)

./ifcfg-networkd -o "$TEMPDIR" "ifcfg=*=dhcp" "ifcfg=00:11:22:33:44:55=dhcp,rfc2132" ifcfg='"eth1=192.168.0.2/24 192.158.10.12/24,192.168.0.1,8.8.8.8,mydomain.com"'

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if cmp "$TEMPDIR/$cfg" "tst-ifcfg-networkd-1/$cfg" ; then
       diff -u "tst-ifcfg-networkd-1/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
