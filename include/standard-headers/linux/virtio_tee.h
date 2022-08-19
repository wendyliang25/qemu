/* SPDX-License-Identifier: MIT */

/*
 * Virtio TEE Device
 *
 * Copyright 2022 Advanced Micro Devices, Inc.
 */

#ifndef VIRTIO_TEE_HW_H
#define VIRTIO_TEE_HW_H

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/tee.h>

#define TEE_MAX_PARAMS				4

/* Must be same as in GP TEE specification */
#define TEE_OP_PARAM_TYPE_NONE                  0
#define TEE_OP_PARAM_TYPE_VALUE_INPUT           1
#define TEE_OP_PARAM_TYPE_VALUE_OUTPUT          2
#define TEE_OP_PARAM_TYPE_VALUE_INOUT           3
#define TEE_OP_PARAM_TYPE_INVALID               4
#define TEE_OP_PARAM_TYPE_MEMREF_INPUT          5
#define TEE_OP_PARAM_TYPE_MEMREF_OUTPUT         6
#define TEE_OP_PARAM_TYPE_MEMREF_INOUT          7

/**
 * Encode the param_types according to the supplied types.
 *
 * @param p0 The first param type.
 * @param p1 The second param type.
 * @param p2 The third param type.
 * @param p3 The fourth param type.
 */
#define TEE_PARAM_TYPES(p0, p1, p2, p3) \
	((p0) | ((p1) << 4) | ((p2) << 8) | ((p3) << 12))

/**
 * Get the i_th param type from the param_type.
 *
 * @param p The param_type.
 * @param i The i-th parameter to get the type for.
 */
#define TEE_PARAM_TYPE_GET(p, i) (((p) >> ((i) * 4)) & 0xF)

/*
 * Open session and close session is supported
 */
#define VIRTIO_TEE_F_OC_SESSION			0

/*
 * Invoke function is supported
 */
#define VIRTIO_TEE_F_INVOKE_FUNC		1

/*
 * Cancel request is supported
 */
#define VIRTIO_TEE_F_CANCEL_REQ			2

enum virtio_tee_cmd_type {
	VIRTIO_TEE_CMD_UNDEFINED = 0,

	/* commands */
	VIRTIO_TEE_CMD_OPEN_DEVICE = 0x2000,
	VIRTIO_TEE_CMD_CLOSE_DEVICE,
	VIRTIO_TEE_CMD_REGISTER_MEM,
	VIRTIO_TEE_CMD_UNREGISTER_MEM,
	VIRTIO_TEE_CMD_OPEN_SESSION,
	VIRTIO_TEE_CMD_CLOSE_SESSION,
	VIRTIO_TEE_CMD_INVOKE_FUNC,
	VIRTIO_TEE_CMD_CANCEL_REQ,

	/* success responses */
	VIRTIO_TEE_RESP_OK_NODATA = 0x2100,
	VIRTIO_TEE_RESP_OK_OPEN_DEVICE,
	VIRTIO_TEE_RESP_OK_OPEN_SESSION,
	VIRTIO_TEE_RESP_OK_INVOKE_FUNC,
	VIRTIO_TEE_RESP_OK_CANCEL_REQ,

	/* error responses */
	VIRTIO_TEE_RESP_ERR_UNSPECIFIED = 0x2200,
	VIRTIO_TEE_RESP_ERR_OPEN_DEVICE,
	VIRTIO_TEE_RESP_ERR_CLOSE_DEVICE,
	VIRTIO_TEE_RESP_ERR_REGISTER_MEM,
	VIRTIO_TEE_RESP_ERR_UNREGISTER_MEM,
	VIRTIO_TEE_RESP_ERR_OPEN_SESSION,
	VIRTIO_TEE_RESP_ERR_CLOSE_SESSION,
	VIRTIO_TEE_RESP_ERR_INVOKE_FUNC,
};

#define ERROR_RESPONSE(x) (le32_to_cpu(x) >= VIRTIO_TEE_RESP_ERR_UNSPECIFIED)

struct virtio_tee_hdr {
	__le32 type;
};

/* VIRTIO_TEE_RESP_OK_OPEN_DEVICE */
struct virtio_tee_resp_open_device {
	struct virtio_tee_hdr hdr;
	__le32 fd;
	__le32 gen_caps;
};

/* VIRTIO_TEE_CMD_CLOSE_DEVICE */
struct virtio_tee_cmd_close_device {
	struct virtio_tee_hdr hdr;
	__le32 fd;
};

/* VIRTIO_TEE_CMD_REGISTER_MEM */
struct virtio_tee_cmd_register_mem {
	struct virtio_tee_hdr hdr;
	__le64 addr;
	__le32 size;
	__le32 fd;
};

/* VIRTIO_TEE_CMD_UNREGISTER_MEM */
struct virtio_tee_cmd_unregister_mem {
	struct virtio_tee_hdr hdr;
	__le64 addr;
	__le32 size;
};

struct virtio_tee_shm {
	__le64 addr;
	__le32 offset;
	__le32 size;
};

struct virtio_tee_memref {
	struct virtio_tee_shm shm;
	__le32 size;
};

struct virtio_tee_value {
	__le64 a;
	__le64 b;
	__le64 c;
};

union virtio_tee_op_param {
	struct virtio_tee_memref mref;
	struct virtio_tee_value val;
};

struct virtio_tee_operation {
	__le32 param_types;
	union virtio_tee_op_param params[TEE_MAX_PARAMS];
};

/* VIRTIO_TEE_CMD_OPEN_SESSION */
struct virtio_tee_cmd_open_session {
	struct virtio_tee_hdr hdr;
	__le32 fd;
	__u8 uuid[TEE_IOCTL_UUID_LEN];
	__u8 clnt_uuid[TEE_IOCTL_UUID_LEN];
	__le32 clnt_login;
	__le32 cancel_id;
	__le32 num_params;
	struct virtio_tee_operation op;
};

/* VIRTIO_TEE_RESP_OK_OPEN_SESSION */
struct virtio_tee_resp_open_session {
	struct virtio_tee_hdr hdr;
	struct virtio_tee_operation op;
	__le32 session;
	__le32 ret;
	__le32 ret_origin;
};

/* VIRTIO_TEE_CMD_CLOSE_SESSION */
struct virtio_tee_cmd_close_session {
	struct virtio_tee_hdr hdr;
	__le32 fd;
	__le32 session;
};

/* VIRTIO_TEE_CMD_INVOKE_FUNC */
struct virtio_tee_cmd_invoke_func {
	struct virtio_tee_hdr hdr;
	__le32 fd;
	__le32 func;
	__le32 session;
	__le32 cancel_id;
	__le32 num_params;
	struct virtio_tee_operation op;
};

/* VIRTIO_TEE_RESP_OK_INVOKE_FUNC */
struct virtio_tee_resp_invoke_func {
	struct virtio_tee_hdr hdr;
	struct virtio_tee_operation op;
	__le32 ret;
	__le32 ret_origin;
};
#endif
