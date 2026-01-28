# SPDX-License-Identifier: MIT
# shellcheck shell=bash

local_fs()
{
    clear_and_print_title
    SOURCE_IMAGE=$(gum file /mnt \
		       --file \
                       --header="Select Image" \
                       --header.foreground="$COLOR_TITLE" \
                       --cursor "${CURSOR}" \
                       --cursor.foreground="$COLOR_FOREGROUND" \
		       --selected.foreground="$COLOR_FOREGROUND")
}

query_url()
{
    local TIMEOUT=5

    clear_and_print_title
    URL=$(gum input \
	      --header="Please enter the image URL:" \
	      --header.foreground="$COLOR_TITLE" \
              --cursor.foreground="$COLOR_FOREGROUND" \
	      --placeholder="https://")

    if [[ -z "$URL" ]]; then
	gum style --foreground="$COLOR_TEXT" "Cancelled."
	$KEYWAIT -t "" -s 1
	return
    fi

    REGEX='^(https?|ftp)://[-A-Za-z0-9\+&@#/%?=~_|!:,.;]*[-A-Za-z0-9\+&@#/%=~_|]$'
    if [[ ! $URL =~ $REGEX ]]; then
        gum style \
	    --foreground=$COLOR_WARNING \
	    "Error: Invalid URL syntax."
	$KEYWAIT -s 0
	return
    fi

    # XXX better error check
    if ! gum spin \
	 --spinner=globe \
	 --title "Pinging $URL..." \
	 --title.foreground="$COLOR_TITLE" -- \
         curl -o /dev/null --silent --head --fail --max-time $TIMEOUT \
	 "$URL"; then
        gum style \
            --foreground=$COLOR_WARNING \
            "Unreachable: $URL"

        gum style \
	     --foreground=$COLOR_TEXT \
	    "The URL has valid syntax, but the file does not exist or the server is not responding (Timeout: ${TIMEOUT}s)."
	$KEYWAIT -s 0
        return
    fi
    SOURCE_IMAGE="$URL"
}

select_image()
{
    local SELECTED_IMG
    local PROCESSED_DEVICES
    local OLD_PATH
    local IMAGE_LIST="Provide URL\nUse file selection\n"

    mkdir -p "${TEMP_DIR}/mount"

    # lsblk "PATH" should be "DEVICE" to avoid a conflict
    local OLD_PATH=$PATH
    declare -A PROCESSED_DEVICES
    while read -r line; do
	# Evaluate the line to set variables: $PATH, $LABEL, $MOUNTPOINT
	eval "$line"
	DEVICE=$PATH
	PATH=$OLD_PATH

	# Skip if we have already processed this device path
	if [[ -n "${PROCESSED_DEVICES[$DEVICE]}" ]]; then
            continue
	fi
	PROCESSED_DEVICES[$DEVICE]=1

	# ${LABEL,,} converts the label to lowercase for comparison
	if [[ "${LABEL,,}" == "images" ]]; then
            echo "Found target partition: $DEVICE (Label: $LABEL)"

            SEARCH_DIR=""
            TEMP_MOUNT_DIR=""

            if [ -n "$MOUNTPOINT" ]; then
		echo "Partition is already mounted at: $MOUNTPOINT"
		SEARCH_DIR="$MOUNTPOINT"
            else
		TEMP_MOUNT_DIR="${TEMP_DIR}/mount"

		# Try to mount the device to the temp directory
		if mount -r "$DEVICE" "$TEMP_MOUNT_DIR" 2>/dev/null ; then
                    SEARCH_DIR="$TEMP_MOUNT_DIR"
		else
                    rmdir "$TEMP_MOUNT_DIR"
                    continue
		fi
            fi

            echo "Scanning for .raw and .img files..."
	    while IFS= read -r -d '' file; do
		if [ -n "$TEMP_MOUNT_DIR" ]; then
		    IMAGE_LIST+="$(basename "$file") ($DEVICE)\n"
		else
		    IMAGE_LIST+="$file\n"
		fi
	    done < <(find "$SEARCH_DIR" -maxdepth 1 -type f \( -name "*.raw" -o -name "*.img" \) -print0)

	    if [ -n "$TEMP_MUONT_DIR" ]; then
		echo "Unmounting temporary mount..."
		umount "$TEMP_MOUNT_DIR"
		rmdir "$TEMP_MOUNT_DIR"
            fi
        fi
    done < <(lsblk -P -o PATH,LABEL,MOUNTPOINT)
    PATH=$OLD_PATH

    clear_and_print_title
    SELECTED_IMG=$(echo -e "$IMAGE_LIST" | \
                       gum choose \
                           --header="Select Source Image" \
                           --header.foreground="$COLOR_TITLE" \
                           --cursor="${CURSOR} " \
                           --cursor.foreground="$COLOR_FOREGROUND")

    if [ -z "$SELECTED_IMG" ]; then
        gum style --foreground="$COLOR_TEXT" "Cancelled."
        return
    fi

    case "$SELECTED_IMG" in
	"Provide URL")
	    query_url
	    ;;
	"Use file selection")
	    local_fs
	    ;;
	*)
	    SOURCE_IMAGE=$SELECTED_IMG
	    ;;
    esac
}
