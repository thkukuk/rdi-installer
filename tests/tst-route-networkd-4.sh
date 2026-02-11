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

./rdii-networkd -o "$TEMPDIR" -a rd.route=[2001:DB8:3::/8]:[2001:DB8:2::1]:ens10

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-route-networkd-4/$cfg" ; then
       diff -u "../tests/tst-route-networkd-4/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
