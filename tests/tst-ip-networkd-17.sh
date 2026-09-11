#!/bin/bash

# Test empty client-IP/peer/gateway/netmask/hostname fields in the long-form
# "ip=" syntax, e.g. "ip=:::::<interface>:dhcp" (see bug #1: extract_ip_addr()
# must not treat an empty-but-present field as a syntax error, and must not
# leave *ret uninitialized when it rejects one).

set -e

# Source shared test utilities
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/test_utils.sh"

# Initialize temp dir and register exit trap
TEMPDIR=$(mktemp -d)
enable_cleanup_trap

# Execute networkd test command
./rdii-networkd -o "$TEMPDIR" -a \
		"ip=:::::eth0:dhcp"

# Run bidirectional assertion against reference directory
REF_DIR="../tests/tst-ip-networkd-17"
assert_dirs_match "$TEMPDIR" "$REF_DIR"
