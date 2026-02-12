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

./rdii-networkd -o "$TEMPDIR" -a vlan=vlan99:eth0 vlan=vlan98:eth0 ip=vlan98:any

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-vlan-networkd-1/$cfg" ; then
       diff -u "../tests/tst-vlan-networkd-1/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
