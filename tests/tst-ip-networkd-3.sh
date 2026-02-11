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

./rdii-networkd -o "$TEMPDIR" -a ip=eth0:dhcp:1530

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-ip-networkd-03/$cfg" ; then
       diff -u "../tests/tst-ip-networkd-03/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
