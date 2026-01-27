# SPDX-License-Identifier: MIT
# shellcheck shell=bash

select_image()
{
    if [[ $EUID -eq 0 ]]; then
	mount /dev/disk/by-label/images /mnt > /dev/null
    fi

    clear_and_print_title
    SOURCE_IMAGE=$(gum file /mnt \
		       --file \
                       --header="Select Image" \
                       --header.foreground="$COLOR_TITLE" \
                       --cursor "${CURSOR}" \
                       --cursor.foreground="$COLOR_FOREGROUND" \
		       --selected.foreground="$COLOR_FOREGROUND")
}
