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

./rdii-networkd -o "$TEMPDIR" -a ip=10.99.37.44::10.99.10.1:255.255.0.0::eth0:off

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-ip-networkd-05/$cfg" ; then
       diff -u "../tests/tst-ip-networkd-05/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
