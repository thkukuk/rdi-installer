#!/bin/bash
#
# Mount all filesystems with the given label (default: "images") under
# /<label>/<device-name>. Invoked by rdii-mount-img-part.service; stdout and
# stderr are captured by systemd and written to the journal under that
# service's unit, and a non-zero exit code marks the service as failed.

set -u
set -o pipefail

TARGET_LABEL=${1:-"images"}
MOUNT_BASE="/images"

log_info() {
    echo "$*"
}

log_err() {
    echo "$*" >&2
}

if ! command -v blkid >/dev/null 2>&1; then
    log_err "blkid command not found, cannot search for partitions labeled \"${TARGET_LABEL}\"."
    exit 1
fi

DEVICES=$(blkid -t LABEL="${TARGET_LABEL}" -o device)
STATUS=$?

# blkid exits with 2 when no matching device is found; that is not an error here.
if [ "$STATUS" -gt 2 ]; then
    log_err "blkid failed while searching for partitions labeled \"${TARGET_LABEL}\" (exit code ${STATUS})."
    exit 1
fi

if [ -z "$DEVICES" ]; then
    log_info "No partitions with label \"${TARGET_LABEL}\" found."
    exit 0
fi

# Root is normally read-only, so "/images" is expected to already exist
# (created at image build time). Handle the read-only root fallback cases
# before trying to create any mount points below it.
if [ ! -d "$MOUNT_BASE" ]; then
    if [ ! -w / ]; then
        log_err "\"${MOUNT_BASE}\" does not exist and \"/\" is not writable."
        exit 1
    fi
    if ! mkdir -p "$MOUNT_BASE"; then
        log_err "Failed to create \"${MOUNT_BASE}\"."
        exit 1
    fi
elif [ ! -w "$MOUNT_BASE" ]; then
    log_info "\"${MOUNT_BASE}\" is not writable, mounting tmpfs on it."
    if ! mount -t tmpfs tmpfs "$MOUNT_BASE"; then
        log_err "Failed to mount tmpfs on \"${MOUNT_BASE}\"."
        exit 1
    fi
fi

EXIT_CODE=0

while IFS= read -r DEV; do
    [ -n "$DEV" ] || continue

    if [ ! -b "$DEV" ]; then
        log_err "Skipping \"${DEV}\": not a block device."
        EXIT_CODE=1
        continue
    fi

    DEV_NAME=$(basename "$DEV")
    MOUNTPOINT="${MOUNT_BASE}/${DEV_NAME}"

    if mountpoint -q "$MOUNTPOINT" 2>/dev/null; then
        log_info "\"${DEV}\" is already mounted on \"${MOUNTPOINT}\", skipping."
        continue
    fi

    if ! mkdir -p "$MOUNTPOINT"; then
        log_err "Failed to create mount point \"${MOUNTPOINT}\" for \"${DEV}\"."
        EXIT_CODE=1
        continue
    fi

    if ! mount "$DEV" "$MOUNTPOINT"; then
        log_err "Failed to mount \"${DEV}\" on \"${MOUNTPOINT}\"."
        EXIT_CODE=1
        continue
    fi

    log_info "Successfully mounted \"${DEV}\" on \"${MOUNTPOINT}\"."
done <<< "$DEVICES"

exit "$EXIT_CODE"
