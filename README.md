# Raw Disk Image Installer (rdi-installer)

This project contains of several utilities and images for
an image installer, which main purpose is to have a comfortable and
robust tool to boot on bare metal and install a raw disk image on
that hardware.

## Images

This project will build several images:

* `rdi-installer-<version>.<arch>.efi` is an EFI binary file that can be stored on an ESP partition and booted directly from the UEFI firmware.
* `rdi-installer-<version>.<arch>.img` is a disk image which can be written to an USB stick and contains the EFI binary file. There is a script `add_extra_partition.sh` with extends the image with a second partition with the filesystem label "images". A raw disk image with the OS can be copied to this partition and the installer will automatically mount the partition and provides the images on it as installation source.
* `rdi-installer-<version>.<arch>.efi` is a disk image which can be written to an USB stick and uses `systemd-boot` as bootloader with the classical linux kernel and initrd setup. This image allows to modify the kernel cmdline.

**Secure Boot:**
Currently Secure Boot needs be disabled, since the EFI binary file is not signed with the Microsoft Key.

## Utilities

### keywait

Simple utility that pauses execution until the user presses a key
or a specified timeout period elapses, whichever happens first.

### rdii-ssh-setup

This script provides a systemd service and a bash script that parses
the Linux kernel command line (`/proc/cmdline`) at boot time. It allows
for the dynamic enabling of the SSH daemon, setting of the root password,
and injection of SSH public keys via boot parameters.

**Security Warning:**
> **Plaintext Passwords:** Passing `ssh.password` via the kernel command line is **insecure** as it can be read by any user via `/proc/cmdline` and may appear in logs. Use `ssh.key` whenever possible.

| Parameter | Format | Description |
| --------- | ------ | ----------- |
| ssh=1     | N/A    | Enables and starts the sshd service immediately. |
| ssh.key   | Base64 Encoded | Decodes the string and appends it to `/root/.ssh/authorized_keys`.|
| ssh.password | Plaintext | (Insecure) Sets the root password to the provided string. Sets PermitRootLogin yes.|
