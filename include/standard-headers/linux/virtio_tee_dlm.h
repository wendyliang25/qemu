/* SPDX-License-Identifier: MIT */

/*
 * Virtio TEE DLM
 *
 * Copyright 2023 Advanced Micro Devices, Inc.
 */

#ifndef VIRTIO_TEE_DLM_H
#define VIRTIO_TEE_DLM_H

/* Should be same as value in amdtee_client_DLM_api.h */
#define DLM_MAX_STRING_LEN  256

/**
 * struct tee_ioctl_get_debug_token_data - Get Debug Token
 * @ret:               [out] return value
 * @debug_token_size:  [in/out] size of debug token in bytes
 * @debug_token:       [in] __user buffer pointer to save debug token
 *
 * Command to get debug authentication token from Secure OS.
 */
struct tee_ioctl_get_debug_token_data {
    __u32 ret;
    __u32 debug_token_size;
    __u64 debug_token;
} __attribute__((aligned(8)));

/**
 * TEE_IOC_DLM_GET_DEBUG_TOKEN - Get Debug Token from Secure OS
 *
 * Takes struct tee_ioctl_get_debug_token_data which contains user pointer
 * to dlm buffer (debug_token) and dlm buffer size
 */
#define TEE_IOC_DLM_GET_DEBUG_TOKEN    _IOR(TEE_IOC_MAGIC, TEE_IOC_BASE + 20, \
                                       struct tee_ioctl_get_debug_token_data)

/**
 * struct tee_ioctl_start_ta_debug_data - Start TA debug session
 * @ret:               [out] return value
 * @debug_token_size:  [in] size of debug token in bytes
 * @debug_token:       [in] __user buffer pointer to signed debug token.
 * @ta_uuid:           [in] UUID of TA for which debug session is requested
 * @dlm_session_id:    [out] DLM session id
 *
 * Command to start TA debug session. @debug_token is a signed debug token.
 * On success, this command generates a valid @dlm_session_id.
 */
struct tee_ioctl_start_ta_debug_data {
    __u32 ret;
    __u32 debug_token_size;
    __u64 debug_token;
    __u8 ta_uuid[16];
    __u32 dlm_session_id;
} __attribute__((aligned(8)));

/**
 * TEE_IOC_DLM_START_TA_DEBUG - Start a TA Debug Session
 *
 * Takes struct tee_ioctl_start_ta_debug_data and on success returns Debug Log
 * Message (DLM) session id
 */
#define TEE_IOC_DLM_START_TA_DEBUG _IOWR(TEE_IOC_MAGIC, TEE_IOC_BASE + 21, \
                                   struct tee_ioctl_start_ta_debug_data)

/**
 * struct tee_ioctl_fetch_debug_strings_data - Fetch debug string
 * @ret:               [out] return value
 * @dlm_session_id:    [in] DLM session id
 * @is_valid_string:   [out] 0: if @string is invalid, 1: if @string is valid
 * @string:            [out] debug string from TA
 *
 * Command to fetch a debug string from TA for the given DLM session id
 */
struct tee_ioctl_fetch_debug_strings_data {
    __u32 ret;
    __u32 dlm_session_id;
    __u32 is_valid_string;
    __u8 string[DLM_MAX_STRING_LEN];
} __attribute__((aligned(8)));

/**
 * TEE_IOC_DLM_FETCH_DEBUG_STRING - Fetch debug string output by TA
 *
 * Takes struct tee_ioctl_fetch_debug_strings_data which contains input
 * parameter @dlm_session_id (the TA DLM session id) and on success provides
 * output string (if any) from TA in parameter @string.
 */
#define TEE_IOC_DLM_FETCH_DEBUG_STRING _IOWR(TEE_IOC_MAGIC, TEE_IOC_BASE + 22,\
                                       struct tee_ioctl_fetch_debug_strings_data)

/**
 * struct tee_ioctl_stop_ta_debug_data - Stop TA debug session
 * @ret:                [out] return value
 * @dlm_session_id:     [in] DLM session id
 */
struct tee_ioctl_stop_ta_debug_data {
    __u32 ret;
    __u32 dlm_session_id;
} __attribute__((aligned(8)));

/**
 * TEE_IOC_DLM_STOP_TA_DEBUG - Stop TA debug session
 *
 * Takes struct tee_ioctl_stop_ta_debug_data
 */
#define TEE_IOC_DLM_STOP_TA_DEBUG  _IOW(TEE_IOC_MAGIC, TEE_IOC_BASE + 23,\
                                   struct tee_ioctl_stop_ta_debug_data)

#endif
