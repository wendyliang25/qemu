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

#ifndef VIRTIO_TEE_CLIENT
#define VIRTIO_TEE_CLIENT

#include <fcntl.h>
#include <stdint.h>
#include <stdbool.h>

#ifndef PATH_MAX
#define PATH_MAX 255
#endif

#define TEEC_MAX_DEV_SEQ        10

/**
 * Flag constants indicating the data transfer direction of memory in
 * TEEC_Parameter. TEEC_MEM_INPUT signifies data transfer direction from the
 * client application to the TEE. TEEC_MEM_OUTPUT signifies data transfer
 * direction from the TEE to the client application. Type is uint32_t.
 *
 * TEEC_MEM_INPUT   The Shared Memory can carry data from the client
 *                  application to the Trusted Application.
 * TEEC_MEM_OUTPUT  The Shared Memory can carry data from the Trusted
 *                  Application to the client application.
 */
#define TEEC_MEM_INPUT   0x00000001
#define TEEC_MEM_OUTPUT  0x00000002

/**
 * Return values. Type is TEEC_Result
 *
 * TEEC_SUCCESS                 The operation was successful.
 * TEEC_ERROR_GENERIC           Non-specific cause.
 * TEEC_ERROR_ACCESS_DENIED     Access privileges are not sufficient.
 * TEEC_ERROR_CANCEL            The operation was canceled.
 * TEEC_ERROR_ACCESS_CONFLICT   Concurrent accesses caused conflict.
 * TEEC_ERROR_EXCESS_DATA       Too much data for the requested operation was
 *                              passed.
 * TEEC_ERROR_BAD_FORMAT        Input data was of invalid format.
 * TEEC_ERROR_BAD_PARAMETERS    Input parameters were invalid.
 * TEEC_ERROR_BAD_STATE         Operation is not valid in the current state.
 * TEEC_ERROR_ITEM_NOT_FOUND    The requested data item is not found.
 * TEEC_ERROR_NOT_IMPLEMENTED   The requested operation should exist but is not
 *                              yet implemented.
 * TEEC_ERROR_NOT_SUPPORTED     The requested operation is valid but is not
 *                              supported in this implementation.
 * TEEC_ERROR_NO_DATA           Expected data was missing.
 * TEEC_ERROR_OUT_OF_MEMORY     System ran out of resources.
 * TEEC_ERROR_BUSY              The system is busy working on something else.
 * TEEC_ERROR_COMMUNICATION     Communication with a remote party failed.
 * TEEC_ERROR_SECURITY          A security fault was detected.
 * TEEC_ERROR_SHORT_BUFFER      The supplied buffer is too short for the
 *                              generated output.
 * TEEC_ERROR_TARGET_DEAD       Trusted Application has panicked
 *                              during the operation.
 */

/**
 *  Standard defined error codes.
 */
#define TEEC_SUCCESS                0x00000000
#define TEEC_ERROR_GENERIC          0xFFFF0000
#define TEEC_ERROR_ACCESS_DENIED    0xFFFF0001
#define TEEC_ERROR_CANCEL           0xFFFF0002
#define TEEC_ERROR_ACCESS_CONFLICT  0xFFFF0003
#define TEEC_ERROR_EXCESS_DATA      0xFFFF0004
#define TEEC_ERROR_BAD_FORMAT       0xFFFF0005
#define TEEC_ERROR_BAD_PARAMETERS   0xFFFF0006
#define TEEC_ERROR_BAD_STATE        0xFFFF0007
#define TEEC_ERROR_ITEM_NOT_FOUND   0xFFFF0008
#define TEEC_ERROR_NOT_IMPLEMENTED  0xFFFF0009
#define TEEC_ERROR_NOT_SUPPORTED    0xFFFF000A
#define TEEC_ERROR_NO_DATA          0xFFFF000B
#define TEEC_ERROR_OUT_OF_MEMORY    0xFFFF000C
#define TEEC_ERROR_BUSY             0xFFFF000D
#define TEEC_ERROR_COMMUNICATION    0xFFFF000E
#define TEEC_ERROR_SECURITY         0xFFFF000F
#define TEEC_ERROR_SHORT_BUFFER     0xFFFF0010
#define TEEC_ERROR_EXTERNAL_CANCEL  0xFFFF0011
#define TEEC_ERROR_TARGET_DEAD      0xFFFF3024

/**
 * struct TEEC_Context - Represents a connection between a client application
 * and a TEE.
 */
typedef struct {
    /* Implementation defined */
    int fd;
    bool reg_mem;
} TEEC_Context;

/**
 * struct TEEC_SharedMemory - Memory to transfer data between a client
 * application and trusted code.
 *
 * @param buffer      The memory buffer which is to be, or has been, shared
 *                    with the TEE.
 * @param size        The size, in bytes, of the memory buffer.
 * @param flags       Bit-vector which holds properties of buffer.
 *                    The bit-vector can contain either or both of the
 *                    TEEC_MEM_INPUT and TEEC_MEM_OUTPUT flags.
 *
 * A shared memory block is a region of memory allocated in the context of the
 * client application memory space that can be used to transfer data between
 * that client application and a trusted application. The user of this struct
 * is responsible to populate the buffer pointer.
 */
typedef struct {
    void *buffer;
    size_t size;
    uint32_t flags;
    /*
     * Implementation-Defined
     */
    int id;
    size_t alloced_size;
    void *shadow_buffer;
    int registered_fd;
    union {
        bool dummy;
        uint8_t flags;
    } internal;
} TEEC_SharedMemory;

int teec_open_device(uint32_t *gen_caps);

void teec_close_device(int fd);

int teec_register_shared_memory(TEEC_Context *context,
                                TEEC_SharedMemory *sharedMem);

void teec_release_shared_memory(TEEC_SharedMemory *sharedMemory);

#endif
