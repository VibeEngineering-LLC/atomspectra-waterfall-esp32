/* #FW-65: real wf_seg_delete_all against a LittleFS skip-on-unlink fake. */
#include "wf_seg_delete.h"
#include "test_util.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define FAKE_MAX 16
#define FAKE_DIR "/wf"

typedef struct {
    char names[FAKE_MAX][32];
    int  n;
    int  cursor;
    int  open;
    int  skip_on_unlink; /* 1 = LittleFS compact skip; 0 = POSIX */
} fake_fs_t;

static void fake_fill(fake_fs_t *fs, int n)
{
    memset(fs, 0, sizeof(*fs));
    fs->n = n;
    fs->skip_on_unlink = 1;
    for (int i = 0; i < n; i++)
        snprintf(fs->names[i], sizeof(fs->names[i]), "seg_%05d.aswf", i);
}

static const char *fake_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void *fake_opendir(void *ctx, const char *path)
{
    (void)path;
    fake_fs_t *fs = ctx;
    fs->cursor = 0;
    fs->open = 1;
    return fs;
}

static const char *fake_readdir(void *ctx, void *dir)
{
    (void)dir;
    fake_fs_t *fs = ctx;
    if (fs->cursor >= fs->n) return NULL;
    return fs->names[fs->cursor++];
}

static int fake_closedir(void *ctx, void *dir)
{
    (void)dir;
    fake_fs_t *fs = ctx;
    fs->open = 0;
    return 0;
}

static int fake_unlink(void *ctx, const char *path)
{
    fake_fs_t *fs = ctx;
    const char *base = fake_basename(path);
    for (int i = 0; i < fs->n; i++) {
        if (strcmp(fs->names[i], base) != 0) continue;
        memmove(fs->names[i], fs->names[i + 1],
                (size_t)(fs->n - i - 1) * sizeof(fs->names[0]));
        fs->n--;
        if (i < fs->cursor) {
            if (fs->open && fs->skip_on_unlink)
                ; /* leave cursor: walker skips the slid-in neighbor */
            else
                fs->cursor--;
        }
        return 0;
    }
    return 1;
}

static wf_dir_ops_t fake_ops(fake_fs_t *fs)
{
    wf_dir_ops_t o = {
        .ctx = fs,
        .opendir = fake_opendir,
        .readdir = fake_readdir,
        .closedir = fake_closedir,
        .unlink = fake_unlink,
    };
    return o;
}

/* Unlink during an open walk — the old broken Clear. */
static void broken_walk_unlink(fake_fs_t *fs)
{
    wf_dir_ops_t ops = fake_ops(fs);
    void *d = ops.opendir(ops.ctx, FAKE_DIR);
    const char *name;
    while ((name = ops.readdir(ops.ctx, d)) != NULL) {
        uint32_t idx;
        if (!wf_seg_name_index(name, &idx)) continue;
        char path[64];
        snprintf(path, sizeof(path), "%s/%s", FAKE_DIR, name);
        ops.unlink(ops.ctx, path);
    }
    ops.closedir(ops.ctx, d);
}

static void test_broken_walk_leaves_remainder(void)
{
    fake_fs_t fs;
    fake_fill(&fs, 8);
    broken_walk_unlink(&fs);
    CHECK(fs.n > 0);
    CHECK(fs.n == 4);
}

static void test_delete_all_empties_same_fake(void)
{
    fake_fs_t fs;
    fake_fill(&fs, 8);
    char names[8][32];
    wf_dir_ops_t ops = fake_ops(&fs);
    int rc = wf_seg_delete_all(&ops, FAKE_DIR, (char *)names, 32, 8,
                               WF_SEG_CLEAR_PASSES);
    CHECK(rc == 0);
    CHECK(fs.n == 0);
}

void wf_seg_clear_suite(void)
{
    printf("wf_seg_clear: walk compact leaves remainder\n");
    test_broken_walk_leaves_remainder();
    printf("wf_seg_clear: collect then unlink empties\n");
    test_delete_all_empties_same_fake();
}
