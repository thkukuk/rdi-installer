#!/bin/bash

# Test vlans

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

./rdii-networkd -o "$TEMPDIR" "ifcfg=eth0.66=10.0.1.1/24,10.0.1.254" "ifcfg=eth0.67=dhcp" "ifcfg=eth1.33=dhcp"

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-ifcfg-networkd-2/$cfg" ; then
       diff -u "../tests/tst-ifcfg-networkd-2/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
