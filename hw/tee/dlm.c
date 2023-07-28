/*
 * Virtio TEE DLM (Debug Log Message) module
 *
 * Copyright 2023 Advanced Micro Devices, Inc.
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
 * Author: Jeshwanth Kumar <jeshwanthkumar.nk@amd.com>
 * Author: Rijo Thomas <Rijo-john.Thomas@amd.com>
 *
 */


#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include "dlm_api.h"
#include "virtio-tee-client.h"

static bool gInterrupted = false;
volatile bool gStopped = false;

static TEEC_Result write_token_to_file(DLM_DebugToken *token)
{
    FILE *fp = fopen(TOKEN_FILE, "wb");
    if (NULL == fp) {
        printf("Cannot open %s for write\n", TOKEN_FILE);
        return TEEC_ERROR_GENERIC;
    }

    if (fwrite(token->buffer, sizeof(uint8_t), token->size, fp) <
        token->size) {
        printf("Write failed\n");
        fclose(fp);
        return TEEC_ERROR_GENERIC;
    }

    printf("Debug Token writen to %s\n", TOKEN_FILE);
    fclose(fp);
    return TEEC_SUCCESS;
}

TEEC_Result dlm_get_debug_token(int fd)
{
    DLM_DebugToken token;
    TEEC_Result res = TEEC_SUCCESS;

    token.buffer = NULL;
    token.size = 0;

    res = teec_dlm_get_debug_token(fd, &token);
    if (res != TEEC_ERROR_SHORT_BUFFER) {
        printf("Expected TEEC_ERROR_SHORT_BUFFER. status = %u\n", res);
        return res;
    }

    printf("Requested token size = %u\n", token.size);
    token.buffer = (uint8_t *) malloc(token.size * sizeof(uint8_t));
    if (NULL == token.buffer) {
        printf("Not enough memory\n");
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    res = teec_dlm_get_debug_token(fd, &token);
    if (res != TEEC_SUCCESS) {
        printf("GetDebugToken failed. status = 0x%x\n", res);
        goto out;
    }

    res = write_token_to_file(&token);
out:
    free(token.buffer);
    return res;
}

static TEEC_Result read_signed_token_file(DLM_DebugToken *token)
{
    struct stat st;
    size_t size;
    uint8_t *buffer;

    token->buffer = NULL;
    token->size = 0;
    FILE *fp = fopen(SIGNED_TOKEN_FILE, "rb");

    if (NULL == fp) {
        printf("Failed to open file %s\n", SIGNED_TOKEN_FILE);
        return TEEC_ERROR_NO_DATA;
    }

    if (stat(SIGNED_TOKEN_FILE, &st) == 0) {
        size = st.st_size;
    } else {
        printf("Failed to read size of %s\n", SIGNED_TOKEN_FILE);
        fclose(fp);
        return TEEC_ERROR_GENERIC;
    }

    printf("%s size = %zu\n", SIGNED_TOKEN_FILE, size);

    buffer = (uint8_t *) malloc(sizeof(uint8_t) * size);
    if (NULL == buffer) {
        printf("Out of memory\n");
        fclose(fp);
        return TEEC_ERROR_OUT_OF_MEMORY;
    }

    if (fread(buffer, sizeof(uint8_t), size, fp) < size) {
        printf("%s read failed\n", SIGNED_TOKEN_FILE);
        fclose(fp);
        free(buffer);
        return TEEC_ERROR_GENERIC;
    }

    token->buffer = buffer;
    token->size = size;
    fclose(fp);
    return TEEC_SUCCESS;
}

static void* fetch_strings(void *arg)
{
    DLM_Context *dlm_ctx = (DLM_Context *)arg;
    DLM_SessionID id = dlm_ctx->id;
    TEEC_Result res;
    struct timespec req;

    printf("FetchStrings thread started\n");

    req.tv_sec = SLEEP_INTERVAL_SEC;
    req.tv_nsec = SLEEP_INTERVAL_NSEC;

    gStopped = false;
    gInterrupted = false;

    while(1) {
        DLM_String string;

        string.is_valid = false;
        res = teec_dlm_fetch_debug_strings(dlm_ctx->fd, id, &string);
        if (TEEC_SUCCESS != res) {
            printf("FetchDebugStrings failed. status = 0x%x",res);
            break;
        }

        if (true == gInterrupted)
            break;

        nanosleep(&req, NULL);
    }

    printf("Exiting ... FetchStrings thread");
    gStopped = true;
    return NULL;
}


TEEC_Result dlm_start_session(DLM_Context *dlm_ctx, const uint8_t *ta)
{
    DLM_DebugToken token;
    TEEC_Result res = TEEC_SUCCESS;
    pthread_t tid;

    if (!dlm_ctx || !ta ) {
        printf("NULL parameter\n");
        return TEEC_ERROR_BAD_PARAMETERS;
    }

    dlm_ctx->id = INVALID_DLM_SID;

    res = read_signed_token_file(&token);
    if (res != TEEC_SUCCESS) {
        return res;
    }

    res = teec_dlm_start_ta_debug(dlm_ctx, ta, &token);
    if (res != TEEC_SUCCESS) {
        printf("StartTADebug failed with status = 0x%x\n", res);
        goto cleanup;
    }

    if (0 == pthread_create(&tid, NULL, fetch_strings, dlm_ctx)) {
        printf("FetchStrings thread created\n");
    } else {
        printf("Failed to create FetchStrings thread\n");
        res = TEEC_ERROR_GENERIC;
        goto cleanup;
    }

cleanup:
    free(token.buffer);
    return res;
}

void dlm_stop_session(DLM_Context *dlm_ctx)
{
    gInterrupted = true;

    while (gStopped == false){
        usleep(1000);
    }

    if (dlm_ctx->id != INVALID_DLM_SID)
        teec_dlm_stop_ta_debug(dlm_ctx);
}
