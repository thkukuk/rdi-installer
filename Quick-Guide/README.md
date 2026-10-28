# Raw Disk Image Installer Quick Guide

![Select Settings](rdi-installer-settings-1.png)

The RDI-Installer offers a text UI for the correct settings.
In most cases, selecting an image and a target device before starting the installation is enough. But sometimes it is necessary to adjust at first some options.

![Select Keyboard](rdi-installer-settings-2.png)

In the settings dialog you can adjust:
* if the host ssh key should be backed up from an already installed system and copied back into the installed system.
* The URL from where images should be downloaded. In the "Select Image" dialog, this URL will be used to present existing images for installation. Under the URL a file SHA256SUMS is read to get the image names.
* The keyboard layout. This is only used for the installer itself to make it easier to type URLs and image names.

![Picture01](rdi-installer-01.png)

Now enter the `Select Image` dialog.

![Picture03](rdi-installer-03.png)

Three options to select an image are offered:
1. `Select image from download server` offers all images from the download URL in the settings dialog. To build a list of available images, `SHA256SUMS` and `SHA256SUMS.asc` are downloaded and verified with the openSUSE signing key.
2. With `Provide URL` a URL for an image can be entered.
3. `Use file selection` allows to browse for the image. Partitions with the label `images` will be auto-mounted below `/images`.

![Picture04](rdi-installer-04.png)

The dialog to select an image from a download server shows a list of all matching images. The dialog filters according to the extension of the image and the architecture of the system. There are three options:
* The default is `matching` and only images with the same architecture as the host are shown.
* `all` will show all images with a supported format.
* At last you can select per architecture, which allows to install e.g. an aarch64 image for a Raspberry Pi on an SD card on a x86-64 host.


![Picture05](rdi-installer-05.png)

After selecting an image for installation, a device needs to be selected, on which the image should be installed. The list contains the device name, the disk model if known, the interface, disk size and if the disk is the boot device, the default disk according to the firmware or if it is already mounted. Only disks with size of 10GB or greater are shown by default.

![Picture06](rdi-installer-06.png)

If an image and the target device got selected, RDI-Installer will automatically pre-select the "Start Installation" option.

![Picture07](rdi-installer-07.png)

Until now, no changes were made to the disk. If `YES` get selected, the destructive part starts and all data on the selected device gets overwritten.

![Picture08](rdi-installer-08.png)

You can follow the output of `dd` as it writes the image to disk.

![Picture09](rdi-installer-09.png)
