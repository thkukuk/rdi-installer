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

./rdii-networkd -o "$TEMPDIR" -a ip=192.168.0.10::192.168.0.1:255.255.255.0::eth0:on:10.10.10.10:10.10.10.11:10.10.10.161

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-ip-networkd-12/$cfg" ; then
       diff -u "../tests/tst-ip-networkd-12/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
