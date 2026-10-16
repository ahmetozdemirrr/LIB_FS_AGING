/*
 * host_test.c - fs_aging core'unu PC'de sahte bir platformla test eder.
 *
 * Sahte platform dosya olusturmaz; yollari bir hash tablosunda tutar,
 * olmayan dosyanin silinmesini veya ayni dosyanin iki kez olusturulmasini
 * hata olarak yakalar ve volume dolulugunu takip eder.
 *
 * Derleme (LIB_FS_AGING klasorunde):
 *   gcc -std=c99 -Wall -Wextra -O2 -ICORE/include \
 *       CORE/src/fs_aging.c TEST/src/host_test.c -o host_test
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fs_aging.h"

/* ---------------- Agrawal profili (geriatrix/profiles/agrawal) ------- */

static const fs_aging_age_bin_t agrawal_ages[] = {
    {10.74, 1}, {31, 2}, {85.98, 8}, {240, 20}, {687.87, 44},
    {1945.59, 75}, {5502.98, 144}, {15768, 146}, {44023.9, 23}, {122640, 1}
};
static const fs_aging_size_bin_t agrawal_sizes[] = {
    {8, 1}, {32, 7}, {128, 16}, {512, 40}, {2048, 50}, {8192, 46},
    {32768, 35}, {131072, 20}, {524288, 10}, {2097152, 4}, {8388608, 1}
};
static const fs_aging_dir_bin_t agrawal_dirs[] = {
    {1, 1, 1}, {2, 2, 5}, {3, 14, 13}, {4, 15, 19}, {5, 23, 30},
    {6, 24, 36}, {7, 42, 32}, {8, 22, 26}, {9, 15, 22}, {10, 12, 17},
    {11, 7, 8}, {12, 4, 5}, {13, 3, 2}, {14, 2, 1}, {15, 1, 1}
};
static const fs_aging_profile_t agrawal = {
    "agrawal",
    agrawal_ages,  sizeof(agrawal_ages)  / sizeof(agrawal_ages[0]),
    agrawal_sizes, sizeof(agrawal_sizes) / sizeof(agrawal_sizes[0]),
    agrawal_dirs,  sizeof(agrawal_dirs)  / sizeof(agrawal_dirs[0])
};

/* ---------------- Sahte platform ------------------------------------ */

#define TABLE_BITS 20u
#define TABLE_SIZE (1u << TABLE_BITS)

typedef struct {
    uint64_t  volume;
    uint64_t  used;
    uint64_t *keys;       /* 0 = bos, 1 = silinmis (tombstone) */
    uint64_t *sizes;
    uint64_t  dirs_made;
    int       errors;
} fake_fs_t;

static uint64_t hash_path(const char *s)
{
    uint64_t h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return (h < 2u) ? h + 2u : h;
}

static int64_t find_slot(fake_fs_t *fs, uint64_t key, int for_insert)
{
    uint64_t i = key & (TABLE_SIZE - 1u);
    int64_t  tomb = -1;
    uint32_t n;
    for (n = 0; n < TABLE_SIZE; n++, i = (i + 1u) & (TABLE_SIZE - 1u)) {
        if (fs->keys[i] == key) return (int64_t)i;
        if (fs->keys[i] == 1u && tomb < 0) tomb = (int64_t)i;
        if (fs->keys[i] == 0u) return for_insert ? (tomb >= 0 ? tomb : (int64_t)i) : -1;
    }
    return for_insert ? tomb : -1;
}

static fs_aging_plat_result_t fake_create(void *u, const char *path, uint64_t size)
{
    fake_fs_t *fs = u;
    uint64_t key = hash_path(path);
    int64_t slot;
    if (find_slot(fs, key, 0) >= 0) {
        fprintf(stderr, "HATA: dosya zaten var: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    if (fs->used + size > fs->volume) return FS_AGING_PLAT_NO_SPACE;
    slot = find_slot(fs, key, 1);
    if (slot < 0) { fprintf(stderr, "HATA: test tablosu dolu\n"); return FS_AGING_PLAT_ERROR; }
    fs->keys[slot] = key;
    fs->sizes[slot] = size;
    fs->used += size;
    return FS_AGING_PLAT_OK;
}

static fs_aging_plat_result_t fake_delete(void *u, const char *path)
{
    fake_fs_t *fs = u;
    int64_t slot = find_slot(fs, hash_path(path), 0);
    if (slot < 0) {
        fprintf(stderr, "HATA: olmayan dosya siliniyor: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    fs->used -= fs->sizes[slot];
    fs->keys[slot] = 1u;
    return FS_AGING_PLAT_OK;
}

static fs_aging_plat_result_t fake_mkdir(void *u, const char *path)
{
    (void)path;
    ((fake_fs_t *)u)->dirs_made++;
    return FS_AGING_PLAT_OK;
}

static uint64_t fake_now_ms(void *u)
{
    (void)u;
    return (uint64_t)clock() * 1000u / CLOCKS_PER_SEC;
}

static void fake_log(void *u, const char *msg)
{
    (void)u;
    printf("[log] %s\n", msg);
}

/* ---------------- Test ---------------------------------------------- */

static void print_dist(const fs_aging_ctx_t *ctx, fs_aging_dist_t d, const char *name)
{
    uint32_t i, n = fs_aging_bin_count(ctx, d);
    printf("%s dagilimi (hedef / gercek):\n", name);
    for (i = 0; i < n; i++) {
        fs_aging_float_t ideal, actual;
        fs_aging_get_bin(ctx, d, i, &ideal, &actual);
        printf("  %2u: %.4f / %.4f\n", i, ideal, actual);
    }
}

int main(int argc, char **argv)
{
    static fs_aging_ctx_t ctx;
    fake_fs_t fs;
    fs_aging_platform_t plat;
    fs_aging_config_t cfg;
    fs_aging_stats_t st;
    fs_aging_status_t s;
    fs_aging_file_t *pool;
    uint32_t cap;

    memset(&cfg, 0, sizeof(cfg));
    cfg.mount_point    = "/fs0";
    cfg.volume_bytes   = (argc > 1) ? strtoull(argv[1], NULL, 10) : 256ull << 20;
    cfg.fill_ratio     = 0.8;
    cfg.seed           = 42;
    cfg.overwrite_runs = 1000;
    cfg.confidence     = (argc > 2) ? atof(argv[2]) : 0.0;

    memset(&fs, 0, sizeof(fs));
    fs.volume = cfg.volume_bytes;
    fs.keys   = calloc(TABLE_SIZE, sizeof(uint64_t));
    fs.sizes  = calloc(TABLE_SIZE, sizeof(uint64_t));

    plat.create_file = fake_create;
    plat.delete_file = fake_delete;
    plat.make_dir    = fake_mkdir;
    plat.now_ms      = fake_now_ms;
    plat.log         = fake_log;
    plat.user        = &fs;

    cap = fs_aging_estimate_files(&agrawal, cfg.volume_bytes, cfg.fill_ratio);
    cap += cap / 4u;
    pool = calloc(cap, sizeof(*pool));
    printf("Volume %llu byte, havuz %u kayit (%zu byte)\n",
           (unsigned long long)cfg.volume_bytes, cap, (size_t)cap * sizeof(*pool));

    s = fs_aging_init(&ctx, &agrawal, &cfg, &plat, pool, cap);
    if (s != FS_AGING_RUNNING) {
        printf("init: %s\n", fs_aging_status_str(s));
        return 1;
    }

    do {
        s = fs_aging_step(&ctx, 1000);   /* ARINC'te bir periyot */
    } while (s == FS_AGING_RUNNING);

    fs_aging_get_stats(&ctx, &st);
    printf("\nSonuc: %s\n", fs_aging_status_str(st.status));
    printf("  islem=%llu olusturma=%llu silme=%llu canli_dosya=%u\n",
           (unsigned long long)st.ops, (unsigned long long)st.creates,
           (unsigned long long)st.deletes, st.live_files);
    printf("  canli=%llu MB yuk=%llu MB hedef_islem=%llu yas_hatasi=%.4f\n",
           (unsigned long long)(st.live_bytes >> 20),
           (unsigned long long)(st.workload_bytes >> 20),
           (unsigned long long)st.convergence_ops, st.age_error);
    printf("  dizin cagrisi=%llu sahte_fs_dolu=%llu MB tutarsizlik=%d\n\n",
           (unsigned long long)fs.dirs_made, (unsigned long long)(fs.used >> 20),
           fs.errors);

    print_dist(&ctx, FS_AGING_DIST_AGE,  "Yas");
    print_dist(&ctx, FS_AGING_DIST_SIZE, "Boyut");
    print_dist(&ctx, FS_AGING_DIST_DIR,  "Dizin");

    return (fs.errors == 0 && fs.used == st.live_bytes &&
            st.status != FS_AGING_ERR_PLATFORM) ? 0 : 1;
}