/*
 * Virtio TEE Client
 *
 * Copyright 2022 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Author: Rijo Thomas <Rijo-john.Thomas@amd.com>
 *
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <linux/tee.h>
#include <string.h>
#include <sys/ioctl.h>
#include "virtio-tee-client.h"

#ifndef PATH_MAX
#define PATH_MAX 255
#endif

#define TEEC_MAX_DEV_SEQ        10

int teec_open_device(uint32_t *gen_caps)
{
    struct tee_ioctl_version_data vers;
    char devname[PATH_MAX];
    int fd = -1;
    size_t n;

    for (n = 0; n < TEEC_MAX_DEV_SEQ; n++) {
        snprintf(devname, sizeof(devname), "/dev/tee%zu", n);
        fd = open(devname, O_RDWR);
        if (fd >= 0) {
            break;
        }
    }

    if (fd < 0) {
        return -1;
    }

    memset(&vers, 0, sizeof(vers));

    if (ioctl(fd, TEE_IOC_VERSION, &vers)) {
        fprintf(stderr, "TEE_IOC_VERSION failed\n");
        goto err;
    }

    /* We can only handle GP TEEs */
    if (!(vers.gen_caps & TEE_GEN_CAP_GP)) {
        goto err;
    }

    *gen_caps = vers.gen_caps;
    return fd;

err:
    close(fd);
    return -1;
}

void teec_close_device(int fd)
{
    close(fd);
}
