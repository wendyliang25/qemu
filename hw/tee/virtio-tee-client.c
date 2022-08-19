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
#include <pthread.h>
#include "qemu/osdep.h"
#include "virtio-tee-client.h"

#ifndef PATH_MAX
#define PATH_MAX 255
#endif

#define TEEC_MAX_DEV_SEQ        10

/* Helpers to access memref parts of a struct tee_ioctl_param */
#define MEMREF_SHM_ID(p)        ((p)->c)
#define MEMREF_SHM_OFFS(p)      ((p)->a)
#define MEMREF_SIZE(p)          ((p)->b)

/*
 * Internal flags of TEEC_SharedMemory::internal.flags
 */
#define SHM_FLAG_BUFFER_ALLOCED         (1u << 0)
#define SHM_FLAG_SHADOW_BUFFER_ALLOCED  (1u << 1)

static pthread_mutex_t teec_mutex = PTHREAD_MUTEX_INITIALIZER;

static void teec_mutex_lock(pthread_mutex_t *mu)
{
    pthread_mutex_lock(mu);
}

static void teec_mutex_unlock(pthread_mutex_t *mu)
{
    pthread_mutex_unlock(mu);
}

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

static TEEC_Result ioctl_errno_to_res(int err)
{
    switch (err) {
    case ENOMEM:
        return TEEC_ERROR_OUT_OF_MEMORY;
    default:
        return TEEC_ERROR_GENERIC;
    }
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

TEEC_Result teec_allocate_shared_memory(TEEC_Context *ctx,
                                        TEEC_SharedMemory *shm)
{
    int fd;
    size_t s;

    if (!ctx || !shm) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (!shm->flags || (shm->flags & ~(TEEC_MEM_INPUT | TEEC_MEM_OUTPUT))) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    s = shm->size;
    if (!s) {
        s = 8;
    }

    if (ctx->reg_mem) {
        shm->buffer = teec_paged_aligned_alloc(s);
        if (!shm->buffer) {
            return TEEC_ERROR_OUT_OF_MEMORY;
        }

        fd = teec_shm_register(ctx->fd, shm->buffer, s, &shm->id);
        if (fd < 0) {
            free(shm->buffer);
            shm->buffer = NULL;
            return TEEC_ERROR_OUT_OF_MEMORY;
        }
        shm->registered_fd = fd;
    } else {
        fd = teec_shm_alloc(ctx->fd, s, &shm->id);
        if (fd < 0) {
            return TEEC_ERROR_OUT_OF_MEMORY;
        }

        shm->buffer = mmap(NULL, s, PROT_READ | PROT_WRITE,
                           MAP_SHARED, fd, 0);
        close(fd);
        if (shm->buffer == (void *)MAP_FAILED) {
            shm->id = -1;
            return TEEC_ERROR_OUT_OF_MEMORY;
        }
        shm->registered_fd = -1;
    }

    shm->shadow_buffer = NULL;
    shm->alloced_size = s;
    shm->internal.flags = SHM_FLAG_BUFFER_ALLOCED;
    return TEEC_SUCCESS;
}

TEEC_Result teec_register_shared_memory(TEEC_Context *ctx,
                                        TEEC_SharedMemory *shm)
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

static
TEEC_Result teec_pre_process_tmpref(TEEC_Context *ctx,
                                    uint32_t param_type,
                                    TEEC_TempMemoryReference *tmpref,
                                    struct tee_ioctl_param *param,
                                    TEEC_SharedMemory *shm)
{
    TEEC_Result res;

    switch (param_type) {
    case TEEC_MEMREF_TEMP_INPUT:
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
        shm->flags = TEEC_MEM_INPUT;
        break;
    case TEEC_MEMREF_TEMP_OUTPUT:
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
        shm->flags = TEEC_MEM_OUTPUT;
        break;
    case TEEC_MEMREF_TEMP_INOUT:
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
        shm->flags = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;
        break;
    default:
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    shm->size = tmpref->size;

    res = teec_allocate_shared_memory(ctx, shm);
    if (res != TEEC_SUCCESS) {
        return res;
    }

    memcpy(shm->buffer, tmpref->buffer, tmpref->size);
    MEMREF_SIZE(param) = tmpref->size;
    MEMREF_SHM_ID(param) = shm->id;
    return TEEC_SUCCESS;
}

static
TEEC_Result teec_pre_process_whole(TEEC_RegisteredMemoryReference *memref,
                                   struct tee_ioctl_param *param)
{
    const uint32_t inout = TEEC_MEM_INPUT | TEEC_MEM_OUTPUT;
    uint32_t flags;
    TEEC_SharedMemory *shm;

    if (!memref || !memref->parent) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    flags = memref->parent->flags & inout;

    if (flags == inout) {
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
    } else if (flags & TEEC_MEM_INPUT) {
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
    } else if (flags & TEEC_MEM_OUTPUT) {
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
    } else {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    shm = memref->parent;
    /*
     * We're using a shadow buffer in this reference, copy the real buffer
     * into the shadow buffer if needed. We'll copy it back once we've
     * returned from the call to secure world.
     */
    if (shm->shadow_buffer && (flags & TEEC_MEM_INPUT)) {
        memcpy(shm->shadow_buffer, shm->buffer, shm->size);
    }

    MEMREF_SHM_ID(param) = shm->id;
    MEMREF_SIZE(param) = shm->size;
    return TEEC_SUCCESS;
}

static
TEEC_Result teec_pre_process_partial(uint32_t param_type,
                                     TEEC_RegisteredMemoryReference *memref,
                                     struct tee_ioctl_param *param)
{
    uint32_t req_shm_flags;
    TEEC_SharedMemory *shm;

    switch (param_type) {
    case TEEC_MEMREF_PARTIAL_INPUT:
        req_shm_flags = TEEC_MEM_INPUT;
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INPUT;
        break;
    case TEEC_MEMREF_PARTIAL_OUTPUT:
        req_shm_flags = TEEC_MEM_OUTPUT;
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_OUTPUT;
        break;
    case TEEC_MEMREF_PARTIAL_INOUT:
        req_shm_flags = TEEC_MEM_OUTPUT | TEEC_MEM_INPUT;
        param->attr = TEE_IOCTL_PARAM_ATTR_TYPE_MEMREF_INOUT;
        break;
    default:
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    if (!memref || !memref->parent) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    shm = memref->parent;

    if ((shm->flags & req_shm_flags) != req_shm_flags) {
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    /*
     * We're using a shadow buffer in this reference, copy the real buffer
     * into the shadow buffer if needed. We'll copy it back once we've
     * returned from the call to secure world.
     */
    if (shm->shadow_buffer && param_type != TEEC_MEMREF_PARTIAL_OUTPUT) {
        memcpy((char *)shm->shadow_buffer + memref->offset,
               (char *)shm->buffer + memref->offset, memref->size);
    }

    MEMREF_SHM_ID(param) = shm->id;
    MEMREF_SHM_OFFS(param) = memref->offset;
    MEMREF_SIZE(param) = memref->size;
    return TEEC_SUCCESS;
}

static
void teec_post_process_tmpref(uint32_t param_type,
                              TEEC_TempMemoryReference *tmpref,
                              struct tee_ioctl_param *param,
                              TEEC_SharedMemory *shm)
{
    if (param_type != TEEC_MEMREF_TEMP_INPUT) {
        if (MEMREF_SIZE(param) <= tmpref->size && tmpref->buffer) {
            memcpy(tmpref->buffer, shm->buffer, MEMREF_SIZE(param));
        }

        tmpref->size = MEMREF_SIZE(param);
    }
}

static
void teec_post_process_whole(TEEC_RegisteredMemoryReference *memref,
                             struct tee_ioctl_param *param)
{
    TEEC_SharedMemory *shm = memref->parent;

    if (shm->flags & TEEC_MEM_OUTPUT) {

        /*
         * We're using a shadow buffer in this reference, copy back
         * the shadow buffer into the real buffer now that we've
         * returned from secure world.
         */
        if (shm->shadow_buffer && MEMREF_SIZE(param) <= memref->size) {
            memcpy(shm->buffer, shm->shadow_buffer, MEMREF_SIZE(param));
        }

        memref->size = MEMREF_SIZE(param);
    }
}

static
void teec_post_process_partial(uint32_t param_type,
                               TEEC_RegisteredMemoryReference *memref,
                               struct tee_ioctl_param *param)
{
    if (param_type != TEEC_MEMREF_PARTIAL_INPUT) {
        TEEC_SharedMemory *shm = memref->parent;

        /*
         * We're using a shadow buffer in this reference, copy back
         * the shadow buffer into the real buffer now that we've
         * returned from secure world.
         */
        if (shm->shadow_buffer && MEMREF_SIZE(param) <= memref->size) {
            memcpy((char *)shm->buffer + memref->offset,
                   (char *)shm->shadow_buffer + memref->offset,
                   MEMREF_SIZE(param));
        }

        memref->size = MEMREF_SIZE(param);
    }
}

static
void teec_post_process_operation(TEEC_Operation *operation,
                                 struct tee_ioctl_param *params,
                                 TEEC_SharedMemory *shms)
{
    size_t n;

    if (!operation) {
        return;
    }

    for (n = 0; n < TEEC_CONFIG_PAYLOAD_REF_COUNT; n++) {
        uint32_t param_type;

        param_type = TEEC_PARAM_TYPE_GET(operation->paramTypes, n);

        switch (param_type) {
        case TEEC_VALUE_INPUT:
            break;
        case TEEC_VALUE_OUTPUT:
        case TEEC_VALUE_INOUT:
            operation->params[n].value.a = params[n].a;
            operation->params[n].value.b = params[n].b;
            break;
        case TEEC_MEMREF_TEMP_INPUT:
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            teec_post_process_tmpref(param_type,
                                     &operation->params[n].tmpref,
                                     params + n,
                                     shms + n);
            break;
        case TEEC_MEMREF_WHOLE:
            teec_post_process_whole(&operation->params[n].memref,
                                    params + n);
            break;
        case TEEC_MEMREF_PARTIAL_INPUT:
        case TEEC_MEMREF_PARTIAL_OUTPUT:
        case TEEC_MEMREF_PARTIAL_INOUT:
            teec_post_process_partial(param_type,
                                      &operation->params[n].memref,
                                      params + n);
            break;
        default:
            break;
        }
    }
}

static
TEEC_Result teec_pre_process_operation(TEEC_Context *ctx,
                                       TEEC_Operation *operation,
                                       struct tee_ioctl_param *params,
                                       TEEC_SharedMemory *shms)
{
    TEEC_Result res;
    size_t n;

    memset(shms, 0, sizeof(TEEC_SharedMemory) * TEEC_CONFIG_PAYLOAD_REF_COUNT);

    for (n = 0; n < TEEC_CONFIG_PAYLOAD_REF_COUNT; n++) {
        shms[n].id = -1;
    }

    if (!operation) {
        memset(params, 0,
               sizeof(struct tee_ioctl_param) * TEEC_CONFIG_PAYLOAD_REF_COUNT);
        return TEEC_SUCCESS;
    }

    for (n = 0; n < TEEC_CONFIG_PAYLOAD_REF_COUNT; n++) {
        uint32_t param_type;

        param_type = TEEC_PARAM_TYPE_GET(operation->paramTypes, n);

        switch (param_type) {
        case TEEC_NONE:
            params[n].attr = param_type;
            break;
        case TEEC_VALUE_INPUT:
        case TEEC_VALUE_OUTPUT:
        case TEEC_VALUE_INOUT:
            params[n].attr = param_type;
            params[n].a = operation->params[n].value.a;
            params[n].b = operation->params[n].value.b;
            break;
        case TEEC_MEMREF_TEMP_INPUT:
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            res = teec_pre_process_tmpref(ctx, param_type,
                                          &operation->params[n].tmpref,
                                          params + n,
                                          shms + n);
            if (res != TEEC_SUCCESS) {
                return res;
            }
            break;
        case TEEC_MEMREF_WHOLE:
            res = teec_pre_process_whole(&operation->params[n].memref,
                                         params + n);
            if (res != TEEC_SUCCESS) {
                return res;
            }
            break;
        case TEEC_MEMREF_PARTIAL_INPUT:
        case TEEC_MEMREF_PARTIAL_OUTPUT:
        case TEEC_MEMREF_PARTIAL_INOUT:
            res = teec_pre_process_partial(param_type,
                                           &operation->params[n].memref,
                                           params + n);
            if (res != TEEC_SUCCESS) {
                return res;
            }
            break;
        default:
            return TEEC_ERROR_BAD_PARAMETERS;
        }
    }

    return TEEC_SUCCESS;
}

static
void teec_free_temp_refs(TEEC_Operation *operation,
                         TEEC_SharedMemory *shms)
{
    size_t n;

    if (!operation) {
        return;
    }

    for (n = 0; n < TEEC_CONFIG_PAYLOAD_REF_COUNT; n++) {
        switch (TEEC_PARAM_TYPE_GET(operation->paramTypes, n)) {
        case TEEC_MEMREF_TEMP_INPUT:
        case TEEC_MEMREF_TEMP_OUTPUT:
        case TEEC_MEMREF_TEMP_INOUT:
            teec_release_shared_memory(shms + n);
            break;
        default:
            break;
        }
    }
}

TEEC_Result teec_open_session(TEEC_Context *ctx, TEEC_Session *session,
                              const uint8_t uuid[],
                              uint8_t connection_method,
                              const void *connection_data,
                              TEEC_Operation *operation, uint32_t *ret_origin)
{
    uint64_t buf[(sizeof(struct tee_ioctl_open_session_arg) +
            TEEC_CONFIG_PAYLOAD_REF_COUNT * sizeof(struct tee_ioctl_param)) /
            sizeof(uint64_t)] = { 0 };
    TEEC_SharedMemory shm[TEEC_CONFIG_PAYLOAD_REF_COUNT];
    struct tee_ioctl_buf_data buf_data;
    struct tee_ioctl_open_session_arg *arg;
    struct tee_ioctl_param *params;
    TEEC_Result res;
    uint32_t eorig;
    int rc;

    (void)&connection_data;

    if (!ctx || !session) {
        eorig = TEEC_ORIGIN_API;
        res = TEEC_ERROR_BAD_PARAMETERS;
        goto out;
    }

    if (connection_method != TEEC_LOGIN_PUBLIC) {
        eorig = TEEC_ORIGIN_API;
        res = TEEC_ERROR_NOT_SUPPORTED;
        goto out;
    }

    buf_data.buf_ptr = (uintptr_t)buf;
    buf_data.buf_len = sizeof(buf);

    arg = (struct tee_ioctl_open_session_arg *)buf;
    arg->num_params = TEEC_CONFIG_PAYLOAD_REF_COUNT;
    params = (struct tee_ioctl_param *)(arg + 1);

    memcpy(arg->uuid, uuid, 16);
    arg->clnt_login = TEEC_LOGIN_PUBLIC;

    res = teec_pre_process_operation(ctx, operation, params, shm);
    if (res != TEEC_SUCCESS) {
        eorig = TEEC_ORIGIN_API;
        goto out_free_temp_refs;
    }

    rc = ioctl(ctx->fd, TEE_IOC_OPEN_SESSION, &buf_data);
    if (rc) {
        fprintf(stderr, "TEE_IOC_OPEN_SESSION failed");
        eorig = TEEC_ORIGIN_COMMS;
        res = ioctl_errno_to_res(errno);
        goto out_free_temp_refs;
    }

    res = arg->ret;
    eorig = arg->ret_origin;
    if (res == TEEC_SUCCESS) {
        session->ctx = ctx;
        session->session_id = arg->session;
    }
    teec_post_process_operation(operation, params, shm);

out_free_temp_refs:
    teec_free_temp_refs(operation, shm);
out:
    if (ret_origin) {
        *ret_origin = eorig;
    }

    return res;
}

void teec_close_session(TEEC_Session *session)
{
    struct tee_ioctl_close_session_arg arg;

    if (!session) {
        return;
    }

    arg.session = session->session_id;

    if (ioctl(session->ctx->fd, TEE_IOC_CLOSE_SESSION, &arg)) {
        fprintf(stderr, "Failed to close session 0x%x", session->session_id);
    }
}

TEEC_Result teec_invoke_command(TEEC_Session *session, uint32_t cmd_id,
                                TEEC_Operation *operation,
                                uint32_t *error_origin)
{
    uint64_t buf[(sizeof(struct tee_ioctl_invoke_arg) +
                 TEEC_CONFIG_PAYLOAD_REF_COUNT *
                 sizeof(struct tee_ioctl_param)) / sizeof(uint64_t)] = { 0 };
    struct tee_ioctl_buf_data buf_data;
    struct tee_ioctl_invoke_arg *arg;
    struct tee_ioctl_param *params;
    TEEC_Result res;
    uint32_t eorig;
    TEEC_SharedMemory shm[TEEC_CONFIG_PAYLOAD_REF_COUNT];
    int rc;

    if (!session) {
        eorig = TEEC_ORIGIN_API;
        res = TEEC_ERROR_BAD_PARAMETERS;
        goto out;
    }

    buf_data.buf_ptr = (uintptr_t)buf;
    buf_data.buf_len = sizeof(buf);

    arg = (struct tee_ioctl_invoke_arg *)buf;
    arg->num_params = TEEC_CONFIG_PAYLOAD_REF_COUNT;
    params = (struct tee_ioctl_param *)(arg + 1);

    arg->session = session->session_id;
    arg->func = cmd_id;

    if (operation) {
        teec_mutex_lock(&teec_mutex);
        operation->session = session;
        teec_mutex_unlock(&teec_mutex);
    }

    res = teec_pre_process_operation(session->ctx, operation, params, shm);
    if (res != TEEC_SUCCESS) {
        eorig = TEEC_ORIGIN_API;
        goto out_free_temp_refs;
    }

    rc = ioctl(session->ctx->fd, TEE_IOC_INVOKE, &buf_data);
    if (rc) {
        fprintf(stderr, "TEE_IOC_INVOKE failed\n");
        eorig = TEEC_ORIGIN_COMMS;
        res = ioctl_errno_to_res(errno);
        goto out_free_temp_refs;
    }

    res = arg->ret;
    eorig = arg->ret_origin;
    teec_post_process_operation(operation, params, shm);

out_free_temp_refs:
    teec_free_temp_refs(operation, shm);
out:
    if (error_origin) {
        *error_origin = eorig;
    }
    return res;
}
