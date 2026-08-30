#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Host-pure collect-then-unlink for waterfall Clear (#FW-65).
 * No ESP_LOG, FreeRTOS, or spectrogram.h. Firmware supplies POSIX ops. */

#define WF_SEG_CLEAR_PASSES 8

typedef struct wf_dir_ops {
    void       *ctx;
    void      *(*opendir)(void *ctx, const char *path);
    /* Next name in dir, or NULL at end. Name is valid until the next call. */
    const char *(*readdir)(void *ctx, void *dir);
    int         (*closedir)(void *ctx, void *dir);
    /* 0 = removed, 1 = missing, -1 = other error. */
    int         (*unlink)(void *ctx, const char *path);
} wf_dir_ops_t;

bool wf_seg_name_index(const char *name, uint32_t *idx);
int  wf_seg_count_on_disk(const wf_dir_ops_t *ops, const char *dir);
int  wf_seg_delete_all(const wf_dir_ops_t *ops, const char *dir,
                       char *names, size_t name_stride, int name_cap,
                       int passes);
