#!/bin/bash

# Imports additional gpg public keys into the keyring used by rdi-installer
# to verify the signature of downloaded raw disk images.
#
# Kernel cmdline / config file argument:
#
# rdii.pubkey=<url>     URL (http://, https://, ftp://) to an ASCII armored
#                        gpg public key, e.g.
#                        https://download.opensuse.org/repositories/devel:/microos:/images/Tumbleweed/repodata/repomd.xml.key
# rdii.pubkey=<base64>  Base64 encoded ASCII armored gpg public key, imported
#                        directly without downloading anything.
#
# May be specified more than once to import several keys.

RDII_CONFIG_FILE="/run/rdi-installer/rdii-config"
KEYRING="/etc/systemd/import-pubring.gpg"

cmdline=$(cat /proc/cmdline)

declare -a PUBKEYS=()

if [ -f "${RDII_CONFIG_FILE}" ]; then
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            rdii.pubkey=*)
                PUBKEYS+=("${line#rdii.pubkey=}")
                ;;
        esac
    done < "${RDII_CONFIG_FILE}"
fi

for ARG in $cmdline; do
    case "$ARG" in
        rdii.pubkey=*)
            PUBKEYS+=("${ARG#rdii.pubkey=}")
            ;;
    esac
done

if [ ${#PUBKEYS[@]} -eq 0 ]; then
    exit 0
fi

mkdir -p "$(dirname "$KEYRING")"

for key in "${PUBKEYS[@]}"; do
    # Strip surrounding quotes, in case the value was quoted on the
    # kernel cmdline.
    key="${key%\"}"
    key="${key#\"}"

    if [ -z "$key" ]; then
        continue
    fi

    WORKDIR=$(mktemp -d)
    mkdir -m 700 "$WORKDIR/gnupg"

    case "$key" in
        http://*|https://*|ftp://*)
            echo "rdii-pubkey-setup: Downloading public key from $key..."
            if ! wget -q -O "$WORKDIR/key.asc" "$key"; then
                echo "rdii-pubkey-setup: Failed to download public key from $key" >&2
                rm -rf "$WORKDIR"
                continue
            fi
            ;;
        *)
            echo "rdii-pubkey-setup: Decoding Base64 encoded public key..."
            if ! base64 -d <<<"$key" > "$WORKDIR/key.asc" 2>/dev/null; then
                echo "rdii-pubkey-setup: Failed to decode Base64 public key" >&2
                rm -rf "$WORKDIR"
                continue
            fi
            ;;
    esac

    if gpg --homedir "$WORKDIR/gnupg" --no-options --batch --import \
           --no-default-keyring --keyring "$KEYRING" "$WORKDIR/key.asc"; then
        echo "rdii-pubkey-setup: Imported public key into $KEYRING"
    else
        echo "rdii-pubkey-setup: Failed to import public key into $KEYRING" >&2
    fi

    rm -rf "$WORKDIR"
done

exit 0
