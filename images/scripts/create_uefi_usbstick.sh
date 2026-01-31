#!/bin/bash
set -e

# Defaults
IMG_NAME=${1:-"uefi-usbstick.img"}
TEMP_PART="esp_partition.img" # XXX depend on IMG_NAME
ESP_SIZE_MB=1024          # 1 GB Size for the partition
SECTOR_SIZE=512           # Standard sector size
START_SECTOR=2048         # Standard 1MB alignment (2048 * 512 = 1MB)

# Calculate size in sectors
ESP_SECTORS=$(( (ESP_SIZE_MB * 1024 * 1024) / SECTOR_SIZE ))
TOTAL_SECTORS=$(( START_SECTOR + ESP_SECTORS + 2048 ))

# Calculate total disk size
TOTAL_SIZE=$(( TOTAL_SECTORS * SECTOR_SIZE ))

echo "--- Configuration ---"
echo "Image Name:    $IMG_NAME"
echo "ESP Size:      $ESP_SIZE_MB MB ($ESP_SECTORS sectors)"
echo "Total Size:    $(( TOTAL_SECTORS * SECTOR_SIZE / 1024 / 1024 )) MB"
echo "---------------------"

# Create temporary file with ESP partition
truncate -s ${ESP_SIZE_MB}M "$TEMP_PART"
/usr/sbin/mkfs.vfat -F 32 -n "EFI_SYSTEM" "$TEMP_PART"

# Create the main empty disk image (Sparse file)
truncate -s $TOTAL_SIZE "$IMG_NAME"

# -o: Clear partition table and create new GPT
# -n 1:start:end : Create partition 1
# -t 1:EF00 : Set type to EFI System Partition
# -c 1:name : Set partition name
if [ -x /usr/sbin/sgdisk ]; then
    /usr/sbin/sgdisk -o \
		     -n 1:$START_SECTOR:+$ESP_SECTORS \
		     -t 1:EF00 \
		     -c 1:"EFI System Partition" \
		     "$IMG_NAME" > /dev/null
elif [ -x /usr/sbin/parted ]; then
    /usr/sbin/parted -s "$IMG_NAME" \
		     mklabel gpt \
		     unit s \
		     mkpart '"EFI System Partition"' fat32 $START_SECTOR $((START_SECTOR + ESP_SECTORS - 1)) \
		     set 1 esp on
else
    echo "ERROR: neither sgdisk nor parted installed!" >&2
    exit 1
fi

# Inject the formatted partition into the disk image
dd if="$TEMP_PART" of="$IMG_NAME" bs="$SECTOR_SIZE" seek="$START_SECTOR" conv=notrunc,sparse,fsync status=progress oflag=direct

# Cleanup
rm "$TEMP_PART"

echo "Created $IMG_NAME with a bootable GPT table."
