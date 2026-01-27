# SPDX-License-Identifier: MIT
# shellcheck shell=bash

set_keymap()
{
    local AVAILABLE_MAPS
    local SELECTED_MAP

    # list of installed keymaps
    AVAILABLE_MAPS=$(localectl --no-pager list-keymaps)

    # We use --value to pre-fill the search box with the current keymap.
    # This acts as a default; the user can press Enter to keep it
    # or Backspace to search for a new one.
    SELECTED_MAP=$(echo "$AVAILABLE_MAPS" | \
	gum filter \
		--limit=1 \
		--indicator="$CURSOR" \
		--indicator.foreground="$COLOR_FOREGROUND" \
		--selected-indicator.foreground="$COLOR_FOREGROUND" \
		--value "$DEFAULT_KEYMAP" \
		--placeholder "Search keymaps..." \
		--match.foreground="$COLOR_FOREGROUND" \
		--header "Select keyboard layout:" \
		--header.foreground="$COLOR_TITLE" )

    # Check if the user cancelled
    if [ -z "$SELECTED_MAP" ]; then
	echo "No keymap selected. Operation cancelled."
	return
    fi

    # Replace or add KEYMAP in /etc/vconsole.conf
    echo "Updating $VCONSOLE_FILE..."

    if grep -q "^KEYMAP=" "$VCONSOLE_FILE"; then
	sed -i "s|^KEYMAP=.*|KEYMAP=$SELECTED_MAP|" "$VCONSOLE_FILE"
    else
	echo "KEYMAP=$SELECTED_MAP" >> "$VCONSOLE_FILE"
    fi

    # Set the KEYMAP
    echo "Loading new keymap..."
    loadkeys "$SELECTED_MAP"

    DEFAULT_KEYMAP="$SELECTED_MAP"
}
