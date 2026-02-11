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

./rdii-networkd -o "$TEMPDIR" -a rd.route=10.1.2.3/16:10.0.2.3:eth0

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-route-networkd-2/$cfg" ; then
       diff -u "../tests/tst-route-networkd-2/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
