# Raw Disk Image Installer (rdi-installer)

This project contains of several utilities and images for
an image installer, which main purpose is to have a comfortable and
robust tool to boot on bare metal and install a raw disk image on
that hardware.

## Images

This project will build the folowing images:

* `rdi-installer-<version>.<arch>.efi` is an EFI binary file that can be stored on an ESP partition, on a tftpboot server or on a http server and booted directly from the UEFI firmware from disk, PXE or HTTP boot.
* `rdi-installer-sdboot-<version>.<arch>.img` is a disk image which can be written to an USB stick and uses `systemd-boot` as bootloader with the classical linux kernel and initrd setup. This image allows to modify the kernel cmdline. There is a script [add_extra_partition.sh](images/scripts/add_extra_partition.sh) with extends the image with an additional partition. This can be used to create a parition `images`, on which raw disk images can be copied and the installer will automatically mount the partition and provides the images on it as installation source. Or a `combustion` partition with a combustion config to personalize the image later. The script can create as many partitions with a filesystem, label and size as needed.

Images will be build in the [home:kukuk:mkosi-images](https://build.opensuse.org/project/monitor/home:kukuk:mkosi-images) OBS project.

**Secure Boot:**
The `rdi-installer-sdboot-<version>.<arch>.img` image uses `shim` and `systemd-boot` signed with official keys. The EFI binary is signed with the deel project key. To be able to boot it the key of the devel project needs to be enrolled in the UEFI firmware.

## Options

The options can be provided either via the kernel cmdline during boot or with a configuration file.

| Parameter | Format | Description |
| --------- | ------ | ----------- |
| rdii.url  | http url | Specifies a the URL under which the to be installed image can be downloaded |
| rdii.device | /dev/... | Device on which the image should be installed |
| rdii.keymap | name | Configures the key mapping table for the keyboard |

## Utilities

### keywait

Simple utility that pauses execution until the user presses a key
or a specified timeout period elapses, whichever happens first.

### rdii-networkd

`rdii-networkd` is a systemd service which parses network configuration
parameters and generates transient configuration files for
[systemd-networkd(8)](https://manpages.opensuse.org/systemd-networkd.8).

The program processes input in the following order:

1. Configuration file (specified by --config)
2. Command line arguments
3. /proc/cmdline (kernel boot parameters)

Note: The dracut-style options (**ip=**, etc.) are not evaluated when
reading from `/proc/cmdline` by default, because
[systemd-network-generator(8)](https://manpages.opensuse.org/systemd-network-generator.8)
handles them already.

#### ifcfg option

* DHCP Configuration: `ifcfg=interface=dhcp*[,rfc2132]`
    * dhcp - Enables both IPv4 and IPv6 DHCP.
    * dhcp4 - Enables only IPv4 DHCP.
    * dhcp6 - Enables only IPv6 DHCP.
    * rfc2132 - Configures the DHCP client to send the MAC address as the client identifier. This maps to ClientIdentifier=mac in the generated systemd configuration.

* Static Configuration: `ifcfg=interface=IP_LIST,GW_LIST,DNS_LIST,DOMAIN_LIST`

Lists (IPs, Gateways, DNS, Domains) are space-separated. If a list
contains spaces, the entire `ifcfg` string must be quoted on the kernel
command line.

* _IP\_LIST_ - IP addresses in address/prefix notation (e.g., 192.168.1.5/24).
* _GW\_LIST_ - List of default gateways.
* _DNS\_LIST_ - List of DNS servers.

The _interface_ specifier supports:
* Exact interface names (e.g. `eth0`).
* Exact interface names with .VlanID for vlan (e.g. `eth0.42`).
* MAC addresses (e.g. `12:34:56:78:9A:BC`).
* Shell globs (e.g. `eth*`, `*:BC`).

Vlans can be setup by adding a vlan id to the _interface_
(e.g. `eth0.42`). The interface will be configured for **tagged only** setups.

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
