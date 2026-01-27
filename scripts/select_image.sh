# SPDX-License-Identifier: MIT
# shellcheck shell=bash

select_image()
{
    local IMAGE_LIST=()

    clear_and_print_title
    SOURCE_IMAGE=$(gum file . \
		       --file \
                       --header="Select Image" \
                       --header.foreground="$COLOR_TITLE" \
                       --cursor "${CURSOR}" \
                       --cursor.foreground="$COLOR_FOREGROUND" \
		       --selected.foreground="$COLOR_FOREGROUND")
}
