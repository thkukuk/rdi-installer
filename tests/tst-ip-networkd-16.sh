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

./rdii-networkd -o "$TEMPDIR" -a ip=[2001:1234:56:8f63::10]:[2001:1234:56:8f63::2]:[2001:1234:56:8f63::1]:64:hogehoge:eth0:on

for cfg in "${TEMPDIR}"/*; do
    cfg=$(basename "$cfg")
    if ! cmp "$TEMPDIR/$cfg" "../tests/tst-ip-networkd-16/$cfg" ; then
       diff -u "../tests/tst-ip-networkd-16/$cfg" "$TEMPDIR/$cfg"
       exit 1
    fi
done
