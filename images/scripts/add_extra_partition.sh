#!/bin/bash

set -e

IMG_NAME=${1}
PART_SIZE=${2:-"8G"}
LABEL=${3:-"images"}
FS_TYPE=${4:-"ext4"}
SGDISK="/usr/sbin/sgdisk"
NEW_PART_NR=2

usage() {
    echo "Usage: $0 <image_file> [[[<size>] <label>] <fs_type>]"
    echo "  <size>    is 'xxxM' or 'xxxG'"
    echo "  <label>   is the filesystem label, defaults to $LABEL"
    echo "  <fs_type> must be 'xfs' or 'ext4'"
    echo "Example: $0 disk.img 16G ext4"
}

get_next_part_nr()
{
    local last_partnr

    last_partnr=$("$SGDISK" -p "$IMG_NAME" 2>/dev/null | awk '/^[[:space:]]*[0-9]/ {print $1}' | sort -rn | head -1)
    if [ $? -ne 0 ] || [ -z "$last_partnr" ]; then
	echo "Error: Failed to read partition table from '$IMG_NAME'."
	exit 1
    fi

    ((last_partnr++))

    NEW_PART_NR="$last_partnr"
}

if [ "$1" == "--help" ] || [ "$1" == "-h" ]; then
    usage
    exit 0
fi

if [ -z "$IMG_NAME" ]; then
    echo "Error: no image provided."
    usage
    exit 1
fi

if [ ! -f "$IMG_NAME" ]; then
    echo "Error: File '$IMG_NAME' not found."
    usage
    exit 1
fi

if [[ "$FS_TYPE" != "xfs" && "$FS_TYPE" != "ext4" ]]; then
    echo "Error: Filesystem must be xfs or ext4."
    usage
    exit 1
fi

get_next_part_nr

echo "Extending image file..."
truncate -s "+$PART_SIZE" "$IMG_NAME"
# and some more for GPT overhead
#truncate -s "+$PART_EXTRA_SPACE" "$IMG_NAME"

# Move the GPT backup header to the end of the file
echo "Relocating GPT backup header..."
$SGDISK -e "$IMG_NAME" > /dev/null

echo "Creating new $PART_SIZE partition..."
# -n 2:0:+8G -> New partition #2, start at first available sector (0), size +PART_SIZE
# -t 2:8300  -> Set type of partition 2 to Linux Filesystem (8300)
$SGDISK -n "${NEW_PART_NR}":0:+"$PART_SIZE" -c "${NEW_PART_NR}":"data" -t "${NEW_PART_NR}":8300 "$IMG_NAME" > /dev/null

# Get the starting sector of partition 2
START_SECTOR=$($SGDISK -i "$NEW_PART_NR" "$IMG_NAME" | grep "First sector:" | awk '{print $3}')
# Get the logical sector size (usually 512, but best to check)
SECTOR_SIZE=$($SGDISK -p "$IMG_NAME" | grep "Sector size" | awk '{print $4}' | grep -o '[0-9]*')
# If detection fails, default to 512
if [ -z "$SECTOR_SIZE" ]; then SECTOR_SIZE=512; fi

# Calculate byte offset
OFFSET_BYTES=$((START_SECTOR * SECTOR_SIZE))

echo "Partition starts at sector $START_SECTOR (Sector size: $SECTOR_SIZE)"
echo "Byte offset: $OFFSET_BYTES"

echo "Formatting partition with $FS_TYPE..."
if [ "$FS_TYPE" == "ext4" ]; then
    # -F: Force (work on a file)
    # -E offset=...: Specify exactly where in the file the FS starts
    # "PART_SIZE": Explicitly tell mkfs the size, so it doesn't overwrite the GPT header at the end
    /usr/sbin/mkfs.ext4 -q -F -L "$LABEL" -E offset="$OFFSET_BYTES" "$IMG_NAME" "$PART_SIZE"
elif [ "$FS_TYPE" == "xfs" ]; then
    # -d file: Tells xfs we are working on a file, not a block device
    # name=...: The filename
    # offset=...: Where to start writing
    # size=...: Limit the size (crucial so it doesn't overrun)
    /usr/sbin/mkfs.xfs -f -L "$LABEL" -d file,name="$IMG_NAME",offset="$OFFSET_BYTES",size="$PART_SIZE" > /dev/null
fi
