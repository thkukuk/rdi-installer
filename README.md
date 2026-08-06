# Raw Disk Image Installer (rdi-installer)

This project consists of various utilities and a UKI image for an raw disk image installer, which main purpose is to have a comfortable and robust tool to boot on bare metal and install a raw disk image on that hardware.

## Images

This project will build the folowing images:

* `rdi-installer-<version>.<arch>.efi` (UKI EFI binary)
* `rdi-installer-sdboot-<version>.<arch>.img` (USB stick)

Images will be build in the [home:kukuk:mkosi-images](https://build.opensuse.org/project/monitor/home:kukuk:mkosi-images) OBS project.

### rdi-installer-&lt;version&gt;.&lt;arch&gt;.efi

This is a UKI EFI binary file that can be stored on an ESP partition (e.g. <ESP>/EFI/Linux/ will automatically be sourced by `systemd-boot`), on a tftpboot server or on a http server.
It can be booted directly from the UEFI firmware from the hard disk, via PXE, or HTTP. The kernel command line cannot be modified with UKI binary files. Since the image is *not* signed with an official Microsoft key, Secure Boot will only work if the key used is registered in the UEFI firmware.

This binary runs completley from a RAM disk in memory. The data can be modified, but a reboot will reset it.


###  rdi-installer-sdboot-&lt;version&gt;.&lt;arch&gt;.img

This is a disk image which can be written to an USB stick and uses
`shim` and `systemd-boot` as bootloader with the classical linux kernel
and initrd setup. This image allows to modify the kernel cmdline during boot.

This system also runs completley from a RAM disk in memory. The data can be modified, but a reboot will reset it.

There is a script [add_extra_partition.sh](images/scripts/add_extra_partition.sh) with extends the image with an additional partition. This can be used to create a parition `images`, on which raw disk images can be copied and the installer will automatically mount the partition and provides the images on it as installation source. Or a `combustion` partition with a combustion config to personalize the image later. The script can create as many partitions with a filesystem, label and size as needed.

Usage:
```
add_extra_partition.sh <image name> [<part size> [<label> [<fs type>]]]
```

## Hardware Requirements

Currently only x86-64 systems with UEFI firmware and at minimum 2GB of
memory are supported.

## Compressed Raw Images

Raw Images compressed with xz, gzip, bzip2 or zstd are supported. The images will be decompressed on the fly while writing to disk.

## Raw Image Verification

The `rdi-installer` application tries to download a gpg signed sha256 hash for an image and uses that to verify the image. If the image URL is `https://download.example.org/example-image.raw.xz`, attempts will be made to also download the files `https://download.example.org/example-image.raw.xz.sha256` and `https://download.example.org/example-image.raw.xz.sha256.asc`.

Signature verification is done by `gpgv` with a keyring at `/etc/systemd/import-pubring.gpg`. The signing key must be imported into that keyring for verification to succeed. If the signature file cannot be downloaded or verification fails, the installer will warn the user but still allow the installation to proceed.

## Options

The options can be provided either via the kernel cmdline during boot or with a configuration file.

| Parameter | Format | Description |
| --------- | ------ | ----------- |
| rdii.url  | http url/local file | Specifies a the URL or the filename under which the to be installed image can be downloaded |
| rdii.device | /dev/... | Device on which the image should be installed |
| rdii.mdraid | /dev/... | Second device together with rdii.device for MD Raid 1 |
| rdii.keymap | name | Configures the key mapping table for the keyboard. Only applied when running on a Linux virtual console; ignored on serial consoles and pseudo terminals |
| rdii.preserve-ssh-hostkey | true/false/yes/no/1/0 | Preserves SSH host keys from the old installation and restores them to the new installation |
| rdii.download_server | http url | Base URL from where raw disk images presented in a list to select from can be downloaded |

With `rdii.url1` and `rdii.url2` additional images can be specified. At the start of `rdi-installer`, the user has to selected the one he wants to install.

### SSH Host Key Preservation

The `rdii.preserve-ssh-hostkey` option enables automatic preservation of SSH host keys during installation. When enabled, the installer will:

1. Before writing the new image, scan all partitions on the target device with a supported filesystem (ext2, ext3, ext4, xfs, btrfs) for `/etc/ssh/ssh_host_*` files
2. Back up any found SSH host keys to a temporary directory
3. After writing and mounting the new image, restore the backed-up keys to the new installation's `/etc/ssh/` directory (only if no host keys already exist in the new installation)

This feature is useful when reinstalling a system and you want to avoid SSH "host key changed" warnings for clients that previously connected to the machine.

### MD Raid (Raid 1)

During installation a MD Raid can be created and used as device for the image. If `rdii.mdraid` is set, `rdii.device` is the first device of the MD Raid and `rdii.mdraid` is the second device. `rdi-intaller` does not make any modifications to the image, it needs to contain already everything to assembly the MD device during boot.

### Configuration file

The rdii-config configuration file is used by rdi-installer,
rdii-networkd, rdii-proxy-setup.service and rdii-ssh-setup.service to provision installation
sources and targets, network interfaces, proxys and remote SSH access.
It follows a simple syntax:
* One key=value parameter is defined per line.
* Empty lines are ignored.
* Everything behind the comment character # is ignored.

If the configuration file is not specified on the command line, the tools will look for `/run/rdi-installer/rdii-config`.

During boot phase `rdii-fetch-config` will lookup the source of the installer and try to fetch
a configuration file from that location, too, and store it as `/run/rdi-installer/rdii-config`.
For this the `.efi` suffix will be replaced with `.rdii-config`.
So if your system did boot via UEFI HTTP the image `http://192.168.122.1/rdi-installer.x86-64.efi`,
`rdii-fetch-config` will look for `http://192.168.122.1/rdi-installer.x86-64.rdii-config`.
If the UKI image got booted from `<ESP>/EFI/Linux/` as `rdi-installer.efi`, `rdii-fetch-config` will
look for `<ESP>/EFI/Linux/rdi-installer.rdii-config`.

An example configuration file may look like:

```
rdii.device=/dev/vda
rdii.url=https://download.opensuse.org/tumbleweed/appliances/Tumbleweed-OEM.x86_64-KDE.raw.xz
rdii.url1=https://download.opensuse.org/tumbleweed/appliances/openSUSE-MicroOS.x86_64-SelfInstall.raw.xz
rdii.keymap=de-nodeadkeys
rdii.preserve-ssh-hostkey=true
ssh=1
ssh.key=ZXhhbXBsZSBzc2ggcHVibGljIGtleQo=
```

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

#### ip= and dracut-style options

The following dracut-style options are supported. They are always parsed from
the config file and from command line arguments. They are only parsed from
`/proc/cmdline` when `--parse-all` is specified.

* `ip=<autoconf>` — Global DHCP/autoconf shorthand. Valid values for _autoconf_:
    * `dhcp` — IPv4 DHCP only
    * `dhcp6` / `either6` — IPv6 DHCP only
    * `on` / `any` — Both IPv4 and IPv6 DHCP
    * `auto6` — IPv6 stateless autoconf (RA only)
    * `link6` — Link-local addressing only
    * `none` / `off` — No automatic configuration

* `ip=<interface>:<autoconf>[:[<mtu>][:<macaddr>]]` — Per-interface DHCP/autoconf.

* Full static configuration:
  `ip=<client-IP>:[<peer>]:<gateway-IP>:<netmask>:<hostname>:<interface>:<autoconf>[:[<dns1>[:<dns2>[:<ntp>]]]`
    * _client-IP_ — Client IP address (IPv4 or IPv6 in `[brackets]`)
    * _peer_ — Optional peer IP (may be empty)
    * _gateway-IP_ — Default gateway
    * _netmask_ — Subnet mask in dotted notation (e.g. `255.255.0.0`) or CIDR prefix length
    * _hostname_ — Client hostname (may be empty)
    * _interface_ — Network interface name or MAC address
    * _autoconf_ — DHCP mode (see above) or `none`/`off` to disable
    * _dns1_, _dns2_ — Optional DNS server addresses
    * _ntp_ — Optional NTP server address

* `nameserver=<IP>` — Add a DNS server.

* `rd.peerdns=<0|1>` — Control whether DHCP-supplied DNS servers are used.
  `0` disables, `1` enables.

* `rd.route=<destination>:<gateway>[:<interface>]` — Add a static route.
  _destination_ is in `network/prefix` notation (IPv6 destinations may be wrapped in `[brackets]`).

* `vlan=<vlanname>:<physdev>` — Configure a VLAN. _vlanname_ determines the VLAN
  ID and supports four naming styles: `vlan0005`, `vlan5`, `eth0.0005`, `eth0.5`.

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

### rdii-proxy-setup

This script provides a systemd service and shell script that parses
the Linux kernel command line (`/proc/cmdline`) and config file at
boot time and setups the /etc/sysconfig/proxy file.
The supported proxy URL format is protocol://[user[:password]@]host[:port].

The format for the kernel command line and config file is:
``` sh
proxy=protocol://[user[:password]@]host[:port]
```

Example: `proxy=http://192.168.122.1:3128`

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
