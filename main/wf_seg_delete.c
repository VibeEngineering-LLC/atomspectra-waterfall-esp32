#include "wf_seg_delete.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

bool wf_seg_name_index(const char *name, uint32_t *idx)
{
    if (!name || !idx) return false;
    if (strncmp(name, "seg_", 4) != 0) return false;
    const char *p = name + 4;
    char *end = NULL;
    unsigned long v = strtoul(p, &end, 10);
    if (end == p) return false;
    if (strcmp(end, ".aswf") != 0) return false;
    *idx = (uint32_t)v;
    return true;
}

int wf_seg_count_on_disk(const wf_dir_ops_t *ops, const char *dir)
{
    if (!ops || !ops->opendir || !ops->readdir || !ops->closedir || !dir)
        return -1;
    void *d = ops->opendir(ops->ctx, dir);
    if (!d) return 0;
    int n = 0;
    const char *name;
    while ((name = ops->readdir(ops->ctx, d)) != NULL) {
        uint32_t idx;
        if (wf_seg_name_index(name, &idx)) n++;
    }
    ops->closedir(ops->ctx, d);
    return n;
}

int wf_seg_delete_all(const wf_dir_ops_t *ops, const char *dir,
                      char *names, size_t name_stride, int name_cap,
                      int passes)
{
    if (!ops || !ops->opendir || !ops->readdir || !ops->closedir ||
        !ops->unlink || !dir || !names || name_stride == 0 || name_cap <= 0)
        return -1;
    if (passes <= 0) passes = WF_SEG_CLEAR_PASSES;

    for (int pass = 0; pass < passes; pass++) {
        void *d = ops->opendir(ops->ctx, dir);
        if (!d) return 0;
        int n = 0;
        int overflow = 0;
        const char *name;
        while ((name = ops->readdir(ops->ctx, d)) != NULL) {
            uint32_t idx;
            if (!wf_seg_name_index(name, &idx)) continue;
            if (n < name_cap) {
                char *slot = names + (size_t)n * name_stride;
                snprintf(slot, name_stride, "%s", name);
                n++;
            } else {
                overflow = 1;
            }
        }
        ops->closedir(ops->ctx, d);
        if (n == 0 && !overflow) return 0;
        for (int i = 0; i < n; i++) {
            const char *base = names + (size_t)i * name_stride;
            char path[160];
            snprintf(path, sizeof(path), "%s/%s", dir, base);
            (void)ops->unlink(ops->ctx, path);
        }
        int left = wf_seg_count_on_disk(ops, dir);
        if (left < 0) return -1;
        if (left == 0) return 0;
        if (!overflow && left >= n)
            break;
    }
    return (wf_seg_count_on_disk(ops, dir) == 0) ? 0 : -1;
}
