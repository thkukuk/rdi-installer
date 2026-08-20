// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/kd.h>
#include <sys/ioctl.h>

#include "basics.h"
#include "is_linux_vt.h"

/* Returns true if the controlling terminal is a Linux virtual console.
   Only there can the keyboard mapping be changed with loadkeys(1);
   serial consoles and pseudo terminals fail KDGKBTYPE with ENOTTY. */
bool
is_linux_vt(void)
{
  char kbtype;

  _cleanup_close_ int fd = open("/dev/tty", O_RDWR|O_CLOEXEC|O_NOCTTY);
  if (fd < 0)
    return false;

  int r = ioctl(fd, KDGKBTYPE, &kbtype);
  close(fd);
  return r >= 0;
}
