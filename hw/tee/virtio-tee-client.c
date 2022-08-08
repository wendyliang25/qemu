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
#include <errno.h>
#include <string.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <sys/mman.h>
#include "qemu/osdep.h"
#include "virtio-tee-client.h"

#ifndef PATH_MAX
#define PATH_MAX 255
#endif

#define TEEC_MAX_DEV_SEQ        10

/*
 * Internal flags of TEEC_SharedMemory::internal.flags
 */
#define SHM_FLAG_BUFFER_ALLOCED         (1u << 0)
#define SHM_FLAG_SHADOW_BUFFER_ALLOCED  (1u << 1)

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

static int teec_shm_alloc(int fd, size_t size, int *id)
{
    int shm_fd;
    struct tee_ioctl_shm_alloc_data data;

    memset(&data, 0, sizeof(data));
    data.size = size;
    shm_fd = ioctl(fd, TEE_IOC_SHM_ALLOC, &data);
    if (shm_fd < 0) {
        return -1;
    }
    *id = data.id;
    return shm_fd;
}

static int teec_shm_register(int fd, void *buf, size_t size, int *id)
{
    int shm_fd = 0;
    struct tee_ioctl_shm_register_data data;

    memset(&data, 0, sizeof(data));

    data.addr = (uintptr_t)buf;
    data.length = size;

    shm_fd = ioctl(fd, TEE_IOC_SHM_REGISTER, &data);
    if (shm_fd < 0) {
        return -1;
    }
    *id = data.id;

    return shm_fd;
}

static void *teec_paged_aligned_alloc(size_t sz)
{
    void *p = NULL;

    if (!posix_memalign(&p, (size_t) qemu_real_host_page_size, sz)) {
        return p;
    }

    return NULL;
}

int teec_register_shared_memory(TEEC_Context *ctx, TEEC_SharedMemory *shm)
{
    int res = 0;
    int fd;
    size_t s;

    if (!ctx || !shm) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (!shm->flags || (shm->flags & ~(TEEC_MEM_INPUT | TEEC_MEM_OUTPUT))) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (!shm->buffer) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    s = shm->size;
    if (!s) {
        s = 8;
    }

    if (ctx->reg_mem) {
        fd = teec_shm_register(ctx->fd, shm->buffer, s, &shm->id);
        if (fd >= 0) {
            shm->registered_fd = fd;
            shm->shadow_buffer = NULL;
            shm->internal.flags = 0;
            goto out;
        }

        /*
         * If we're here TEE_IOC_SHM_REGISTER failed, probably
         * because some read-only memory was supplied and the Linux
         * kernel doesn't like that at the moment.
         *
         * The error could also have some other origin. In any case
         * we're not making matters worse by trying to allocate and
         * register a shadow buffer before giving up.
         */
        shm->shadow_buffer = teec_paged_aligned_alloc(s);
        if (!shm->shadow_buffer) {
            return TEEC_ERROR_OUT_OF_MEMORY;
        }
        fd = teec_shm_register(ctx->fd, shm->shadow_buffer, s,
                               &shm->id);
        if (fd >= 0) {
            shm->registered_fd = fd;
            shm->internal.flags = SHM_FLAG_SHADOW_BUFFER_ALLOCED;
            goto out;
        }

        if (errno == ENOMEM) {
            res = TEEC_ERROR_OUT_OF_MEMORY;
        } else {
            res = TEEC_ERROR_GENERIC;
        }
        free(shm->shadow_buffer);
        shm->shadow_buffer = NULL;
        return res;
    } else {
        fd = teec_shm_alloc(ctx->fd, s, &shm->id);
        if (fd < 0) {
            return TEEC_ERROR_OUT_OF_MEMORY;
        }

        shm->shadow_buffer = mmap(NULL, s, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, fd, 0);
        close(fd);
        if (shm->shadow_buffer == (void *)MAP_FAILED) {
            shm->id = -1;
            return TEEC_ERROR_OUT_OF_MEMORY;
        }
        shm->registered_fd = -1;
        shm->internal.flags = 0;
    }

out:
    shm->alloced_size = s;
    return TEEC_SUCCESS;
}

void teec_release_shared_memory(TEEC_SharedMemory *shm)
{
    if (!shm || shm->id == -1) {
        return;
    }

    if (shm->shadow_buffer) {
        if (shm->registered_fd >= 0) {
            if (shm->internal.flags &
                SHM_FLAG_SHADOW_BUFFER_ALLOCED) {
                free(shm->shadow_buffer);
            }
            close(shm->registered_fd);
        } else {
            munmap(shm->shadow_buffer, shm->alloced_size);
        }
    } else if (shm->buffer) {
        if (shm->registered_fd >= 0) {
            if (shm->internal.flags & SHM_FLAG_BUFFER_ALLOCED) {
                free(shm->buffer);
            }
            close(shm->registered_fd);
        } else {
            munmap(shm->buffer, shm->alloced_size);
        }
    } else if (shm->registered_fd >= 0) {
        close(shm->registered_fd);
    }

    shm->id = -1;
    shm->shadow_buffer = NULL;
    shm->buffer = NULL;
    shm->registered_fd = -1;
    shm->internal.flags = 0;
}
