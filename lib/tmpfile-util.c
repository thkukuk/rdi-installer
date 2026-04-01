/* SPDX-License-Identifier: LGPL-2.1-or-later */

// based on systemd v258

#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <fcntl.h>

#include "basics.h"
#include "tmpfile-util.h"

int mkdtemp_malloc(const char *template, char **ret) {
        _cleanup_ (freep) char *p = NULL;

        assert(ret);

        if (template)
                p = strdup(template);
        else
		return -EINVAL;

        if (!p)
                return -ENOMEM;

        if (!mkdtemp(p))
                return -errno;

        *ret = TAKE_PTR(p);
        return 0;
}
