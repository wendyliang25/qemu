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
	VIRTIO_TEE_CMD_GET_VERSION,
	VIRTIO_TEE_CMD_OPEN_SESSION,
	VIRTIO_TEE_CMD_CLOSE_SESSION,
	VIRTIO_TEE_CMD_INVOKE_FUNC,
	VIRTIO_TEE_CMD_CANCEL_REQ,

	/* success responses */
	VIRTIO_TEE_RESP_OK_NODATA = 0x2100,
	VIRTIO_TEE_RESP_OK_OPEN_DEVICE,
	VIRTIO_TEE_RESP_OK_GET_VERSION,
	VIRTIO_TEE_RESP_OK_OPEN_SESSION,
	VIRTIO_TEE_RESP_OK_CLOSE_SESSION,
	VIRTIO_TEE_RESP_OK_INVOKE_FUNC,
	VIRTIO_TEE_RESP_OK_CANCEL_REQ,

	/* error responses */
	VIRTIO_TEE_RESP_ERR_UNSPECIFIED = 0x2200,
	VIRTIO_TEE_RESP_ERR_OPEN_DEVICE,
	VIRTIO_TEE_RESP_ERR_CLOSE_DEVICE,
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

#endif
