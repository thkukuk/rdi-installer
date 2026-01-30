# SPDX-License-Identifier: MIT
# shellcheck shell=bash


select_target_device()
{
    local MIN_SIZE_BYTES
    local SCRIPT_DIR

    # Minimale größe einer Festplatte
    MIN_SIZE_BYTES=10000000000 # 10GB

    # Identify current device, it cannot be used as target device
    # XXX Must be better doable...
    SCRIPT_DIR=$(dirname "$(realpath "$0")")
    CURRENT_PARTITION=$(df --output=source "$SCRIPT_DIR" | tail -n 1)
    # Avoid that lsblk prints on error message about known rootfs
    if [ "$CURRENT_PARTITION" != "rootfs" ]; then
	CURRENT_DISK_NAME=$(lsblk -no pkname "$CURRENT_PARTITION")
    fi
    [ -z "$CURRENT_DISK_NAME" ] && CURRENT_DISK_NAME=$(basename "$CURRENT_PARTITION")

    # Find all storage devices
    local DEVICE_LIST=""

    while read -r dev size_bytes type transport model; do
	if [[ "$type" != "disk" ]]; then continue; fi
	if [[ "$dev" == *"$CURRENT_DISK_NAME"* ]]; then continue; fi
	if [ "$size_bytes" -lt "$MIN_SIZE_BYTES" ]; then continue; fi

	human_size=$(numfmt --to=iec --suffix=B "$size_bytes")
	[ -z "$model" ] && model="Unknown"

	DEVICE_LIST+="$dev  - $model ($transport, $human_size)\n"
    done < <(lsblk -dnp -b -o NAME,SIZE,TYPE,TRAN,MODEL)

    if [ -z "$DEVICE_LIST" ]; then
	gum style --foreground="$COLOR_WARNING" "No suitable drives >10GB found."
	$KEYWAIT -s 0
	if [ -z "$RDII_DEBUG" ]; then
	    return
	else
	    DEVICE_LIST="/dev/dummy0 - DUMMY (none, 999TB)\n"
	    DEVICE_LIST+="/dev/dummy1 - DUMMY (none, 20GB)\n"
	    clear_and_print_title
	fi
    fi

    SELECTION_STRING=$(echo -e "$DEVICE_LIST" | \
			   gum choose \
			       --header="Select Target Device" \
			       --header.foreground="$COLOR_TITLE" \
			       --cursor="${CURSOR} " \
			       --cursor.foreground="$COLOR_FOREGROUND")

    if [ -z "$SELECTION_STRING" ]; then
	gum style --foreground="$COLOR_TEXT" "Cancelled."
	return
    fi

    SELECTED_DEV=$(echo "$SELECTION_STRING" | awk '{print $1}')

    # Check if any mountpoints exist for this device or its children
    IS_MOUNTED=$(lsblk -n -o MOUNTPOINT "$SELECTED_DEV" | grep -v "^$")

    clear_and_print_title
    if [ -n "$IS_MOUNTED" ]; then
	gum style \
	    --width 78 \
	    --align="center" \
            --border double \
            --border-foreground $COLOR_WARNING \
            --foreground $COLOR_WARNING \
            --padding "1" \
            "$WARN_SIGN CRITICAL WARNING: DRIVE IS CURRENTLY MOUNTED $WARN_SIGN" \
            "The device $SELECTED_DEV contains mounted partitions." \
            "Proceeding may cause data loss or corruption."
    else
	gum style \
	    --width 78 \
	    --align="center" \
            --border normal \
            --border-foreground $COLOR_WARNING \
	    --foreground $COLOR_WARNING \
            --padding "1" \
            "$WARN_SIGN SAFETY CHECK $WARN_SIGN"
    fi

    # Show Layout
    gum style --foreground "$COLOR_TEXT" "Current Layout of $SELECTED_DEV:"
    lsblk -o NAME,SIZE,TYPE,FSTYPE,MOUNTPOINT "$SELECTED_DEV" | \
	gum style --foreground 250

    echo ""
    if gum confirm \
	   --default \
	   --prompt.foreground "$COLOR_WARNING" \
	   --selected.background "$COLOR_WARNING" \
	   --selected.foreground "$COLOR_GRAY" \
	   --unselected.background "$COLOR_BACKGROUND" \
	   --unselected.foreground "$COLOR_GRAY" \
	   "Are you sure you want to use $SELECTED_DEV?"; then
	gum style --foreground "$COLOR_TEXT" "Confirmed. Proceeding with $SELECTED_DEV..."
	TARGET_DEVICE="$SELECTED_DEV"
    else
	gum style --foreground "$COLOR_TEXT" "Aborted."
	$KEYWAIT -t "" -s 2
    fi
}
