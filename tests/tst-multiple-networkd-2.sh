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

./rdii-networkd -o "$TEMPDIR" -a ip=192.168.0.10:192.168.0.2:192.168.0.1:255.255.255.0:hogehoge:eth0:on nameserver=10.1.2.3

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-multiple-networkd-2/$cfg" ; then
       diff -u "../tests/tst-multiple-networkd-2/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
