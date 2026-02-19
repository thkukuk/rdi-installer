# SPDX-License-Identifier: MIT
# shellcheck shell=bash


select_target_device()
{
    local DEVICE_LIST

    # Find all storage devices
    DEVICE_LIST=$("$RDII_HELPER" disk)

    if [ -z "$DEVICE_LIST" ]; then
	gum style --foreground="$COLOR_WARNING" "No suitable drives found, collecting all available devices."
	$KEYWAIT -s 0
	DEVICE_LIST=$("$RDII_HELPER" disk --all)
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
