#!/bin/bash

# Kernel cmdline arguments:
#
# ssh=1             Enable sshd.service
# ssh.password=xyz  Set as root password and enable PermitRootLogin
# ssh.key=base64    Public ssh key base64 encoded

RDII_CONFIG_FILE="/run/rdi-installer/rdii-config"

cmdline=$(cat /proc/cmdline)

# Initialize variables
ENABLE_SSH=0
ROOT_PASS=""
SSH_PUB_KEY_B64=""

if [ -f "${RDII_CONFIG_FILE}" ]; then
    ENABLE_SSH=$(sed -n 's/^ssh=\([^ ]*\).*/\1/p' $RDII_CONFIG_FILE)
    ROOT_PASS=$(sed -n 's/^ssh\.password=\([^ ]*\).*/\1/p' $RDII_CONFIG_FILE)
    SSH_PUB_KEY_B64=$(sed -n 's/^ssh\.key=\([^ ]*\).*/\1/p' $RDII_CONFIG_FILE)

    if [ -z "$ENABLE_SSH" ]; then
	ENABLE_SSH=0
    fi
fi

for ARG in $cmdline; do
    case "$ARG" in
        ssh=1)
            ENABLE_SSH=1
            ;;
	ssh=0)
	    ENABLE_SSH=0
	    ;;
        ssh.password=*)
            # Extract value after the first '='
            ROOT_PASS="${ARG#*=}"
            ;;
	ssh.key=*)
            # Extract Base64 string
            SSH_PUB_KEY_B64="${ARG#*=}"
            ;;
    esac
done

if [ "$ENABLE_SSH" -eq 1 ]; then
    echo "Boot argument ssh=1 detected. Configuring SSH..."

    if [ -n "$ROOT_PASS" ]; then
        echo "Setting root password and enabling root login..."

        echo "root:$ROOT_PASS" | chpasswd

	mkdir -p /etc/ssh/sshd_config.d
	echo "PermitRootLogin yes" > /etc/ssh/sshd_config.d/50-permit-root-login.conf
    fi

    if [ -n "$SSH_PUB_KEY_B64" ]; then
        echo "Deploying SSH Public Key..."

        # Create .ssh directory with correct permissions
        mkdir -m 0700 /root/.ssh

	if [ ! -f /root/.ssh/authorized_keys ]; then
	    install -m 600 /dev/null /root/.ssh/authorized_keys
	fi
        echo "$SSH_PUB_KEY_B64" | base64 -d >> /root/.ssh/authorized_keys

	# Make sure labels are correct if we use SELinux
	if command -v selinuxenabled >/dev/null 2>&1; then
	    if selinuxenabled; then
		restorecon -R /root/.ssh
	    fi
        fi
        echo "SSH Public key deployed."
    fi

    systemctl enable --now sshd.service
    echo "SSH service enabled and started."
fi
