/*
 * host_test.c - fs_aging core'unu PC'de sahte bir platformla test eder.
 *
 * Sahte platform dosya olusturmaz; yollari hash tablolarinda tutar ve su
 * tutarsizliklari hata olarak sayar:
 *   - ayni dosyanin iki kez olusturulmasi,
 *   - olmayan dosyanin silinmesi,
 *   - ust dizini olusturulmamis bir dizine dosya ya da dizin acilmasi.
 * Volume dolulugunu da takip eder (asilirsa NO_SPACE doner).
 *
 * Kullanim:
 *   host_test [-q] [--max-err X] [--expect-ops N] <profil> [volume_bytes]
 *             [confidence] [seed] [max_ops]
 *     varsayilan: volume 268435456 (256 MB), confidence 0, seed 42,
 *                 max_ops 1000. -q: yalnizca SUMMARY satiri basilir.
 *     --max-err X   : yas, boyut ve dizin hatalarindan (toplam varyasyon
 *                     mesafesi) biri X'i asarsa basarisiz.
 *     --expect-ops N: islem sayisi (stats.ops) tam N degilse basarisiz.
 *
 * Cikis kodu 0 ancak su durumda: sahte fs tutarsizligi yok, sahte fs'teki
 * veri ve dosya sayisi istatistiklerle ayni, durum DONE_* kodlarindan biri
 * ve (verildiyse) --max-err / --expect-ops kosullari saglaniyor.
 *
 * Derleme: TEST/Makefile (make -C TEST), ya da LIB_FS_AGING klasorunde:
 *   gcc -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wshadow -O2 \
 *       -ICORE/include CORE/src/fs_aging.c CORE/src/fs_aging_profiles.c \
 *       TEST/src/host_test.c -o host_test
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "fs_aging.h"
#include "fs_aging_profiles.h"

/* ---------------- Hash kumesi (acik adresleme, dogrusal sinama) ------ */

/* Anahtar yolun 64 bit FNV-1a ozetidir; 0 bos slot demektir. Silmede
 * mezar tasi birakilmaz (geri kaydirma), boylece uzun kosularda tablo
 * dolmaz. Kayit sayisi tablo boyutunun 3/4'unu gecemez. */
typedef struct {
    uint64_t *keys;
    uint64_t *vals;
    uint32_t  mask;
    uint32_t  count;
    uint32_t  limit;
} hset_t;

static int hset_init(hset_t *h, uint64_t min_entries)
{
    uint64_t size = 1024u;

    memset(h, 0, sizeof(*h));
    while (size < (min_entries * 2u)) {
        size <<= 1u;
    }
    if (size > (1ull << 30u)) {
        return -1;
    }
    h->keys = calloc((size_t)size, sizeof(uint64_t));
    h->vals = calloc((size_t)size, sizeof(uint64_t));
    if ((h->keys == NULL) || (h->vals == NULL)) {
        return -1;
    }
    h->mask  = (uint32_t)(size - 1u);
    h->limit = (uint32_t)((size / 4u) * 3u);
    return 0;
}

static void hset_free(hset_t *h)
{
    free(h->keys);
    free(h->vals);
    memset(h, 0, sizeof(*h));
}

static int64_t hset_find(const hset_t *h, uint64_t key)
{
    uint32_t i = (uint32_t)key & h->mask;
    while (h->keys[i] != 0u) {
        if (h->keys[i] == key) {
            return (int64_t)i;
        }
        i = (i + 1u) & h->mask;
    }
    return -1;
}

/* Anahtar yoksa ekler. Tablo sinirindaysa -1 doner. */
static int hset_insert(hset_t *h, uint64_t key, uint64_t val)
{
    uint32_t i = (uint32_t)key & h->mask;
    if (h->count >= h->limit) {
        return -1;
    }
    while (h->keys[i] != 0u) {
        i = (i + 1u) & h->mask;
    }
    h->keys[i] = key;
    h->vals[i] = val;
    h->count++;
    return 0;
}

static void hset_remove_at(hset_t *h, uint32_t i)
{
    uint32_t j = i;
    for (;;) {
        uint32_t home;
        j = (j + 1u) & h->mask;
        if (h->keys[j] == 0u) {
            break;
        }
        home = (uint32_t)h->keys[j] & h->mask;
        /* home, (i, j] cevriminde degilse kayit i'ye kaydirilabilir */
        if ((i <= j) ? ((home <= i) || (home > j))
                     : ((home <= i) && (home > j))) {
            h->keys[i] = h->keys[j];
            h->vals[i] = h->vals[j];
            i = j;
        }
    }
    h->keys[i] = 0u;
    h->vals[i] = 0u;
    h->count--;
}

/* ---------------- Sahte platform ------------------------------------ */

typedef struct {
    const char *mount;
    uint32_t    mount_len;
    uint64_t    volume;
    uint64_t    used;
    hset_t      files;      /* dosya yolu ozeti -> boyut */
    hset_t      dirs;       /* dizin yolu ozeti          */
    uint64_t    dirs_made;  /* make_dir cagrisi          */
    uint64_t    no_space;   /* NO_SPACE donusleri        */
    int         errors;
} fake_fs_t;

static uint64_t hash_n(const char *s, uint32_t n)
{
    uint64_t h = 1469598103934665603ull;
    uint32_t i;
    for (i = 0u; i < n; i++) {
        h ^= (uint64_t)(unsigned char)s[i];
        h *= 1099511628211ull;
    }
    return (h == 0u) ? 1u : h;
}

static uint32_t parent_len(const char *path)
{
    uint32_t n = (uint32_t)strlen(path);
    while ((n > 0u) && (path[n - 1u] != '/')) {
        n--;
    }
    return (n > 0u) ? (n - 1u) : 0u;
}

/* Ust dizin mount noktasi ya da olusturulmus bir dizin mi? */
static int parent_exists(const fake_fs_t *fs, const char *path)
{
    uint32_t n = parent_len(path);
    if ((n == fs->mount_len) && (strncmp(path, fs->mount, n) == 0)) {
        return 1;
    }
    return hset_find(&fs->dirs, hash_n(path, n)) >= 0;
}

static fs_aging_plat_result_t fake_create(void *u, const char *path, uint64_t size)
{
    fake_fs_t *fs  = u;
    uint64_t   key = hash_n(path, (uint32_t)strlen(path));

    if (hset_find(&fs->files, key) >= 0) {
        fprintf(stderr, "HATA: dosya zaten var: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    if (parent_exists(fs, path) == 0) {
        fprintf(stderr, "HATA: ust dizin yok: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    if ((fs->used + size) > fs->volume) {
        fs->no_space++;
        return FS_AGING_PLAT_NO_SPACE;
    }
    if (hset_insert(&fs->files, key, size) != 0) {
        fprintf(stderr, "HATA: sahte fs dosya tablosu dolu (%u kayit)\n",
                fs->files.count);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    fs->used += size;
    return FS_AGING_PLAT_OK;
}

static fs_aging_plat_result_t fake_delete(void *u, const char *path)
{
    fake_fs_t *fs   = u;
    int64_t    slot = hset_find(&fs->files, hash_n(path, (uint32_t)strlen(path)));

    if (slot < 0) {
        fprintf(stderr, "HATA: olmayan dosya siliniyor: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    fs->used -= fs->files.vals[slot];
    hset_remove_at(&fs->files, (uint32_t)slot);
    return FS_AGING_PLAT_OK;
}

static fs_aging_plat_result_t fake_mkdir(void *u, const char *path)
{
    fake_fs_t *fs  = u;
    uint64_t   key = hash_n(path, (uint32_t)strlen(path));

    fs->dirs_made++;
    if (parent_exists(fs, path) == 0) {
        fprintf(stderr, "HATA: ust dizin yok: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    if (hset_find(&fs->dirs, key) >= 0) {
        return FS_AGING_PLAT_OK;     /* var olan dizin: OK (sozlesme) */
    }
    if (hset_insert(&fs->dirs, key, 0u) != 0) {
        fprintf(stderr, "HATA: sahte fs dizin tablosu dolu (%u kayit)\n",
                fs->dirs.count);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    return FS_AGING_PLAT_OK;
}

static uint64_t fake_now_ms(void *u)
{
    (void)u;
    return (uint64_t)clock() * 1000u / (uint64_t)CLOCKS_PER_SEC;
}

static void fake_log(void *u, const char *msg)
{
    (void)u;
    printf("[log] %s\n", msg);
}

/* ---------------- Yardimcilar ---------------------------------------- */

static const char *status_name(fs_aging_status_t s)
{
    switch (s) {
    case FS_AGING_RUNNING:             return "RUNNING";
    case FS_AGING_DONE_CONVERGED:      return "DONE_CONVERGED";
    case FS_AGING_DONE_TOLERANCE:      return "DONE_TOLERANCE";
    case FS_AGING_DONE_WORKLOAD_LIMIT: return "DONE_WORKLOAD_LIMIT";
    case FS_AGING_DONE_TIME_LIMIT:     return "DONE_TIME_LIMIT";
    case FS_AGING_ERR_PARAM:           return "ERR_PARAM";
    case FS_AGING_ERR_POOL_FULL:       return "ERR_POOL_FULL";
    case FS_AGING_ERR_PATH_TOO_LONG:   return "ERR_PATH_TOO_LONG";
    case FS_AGING_ERR_PLATFORM:        return "ERR_PLATFORM";
    default:                           return "UNKNOWN";
    }
}

static int is_done(fs_aging_status_t s)
{
    return (s == FS_AGING_DONE_CONVERGED) || (s == FS_AGING_DONE_TOLERANCE) ||
           (s == FS_AGING_DONE_WORKLOAD_LIMIT) || (s == FS_AGING_DONE_TIME_LIMIT);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if ((s[0] < '0') || (s[0] > '9')) {
        return -1;
    }
    errno = 0;
    v = strtoull(s, &end, 10);
    if ((errno != 0) || (end == s) || (*end != '\0')) {
        return -1;
    }
    *out = (uint64_t)v;
    return 0;
}

static int parse_float(const char *s, fs_aging_float_t *out)
{
    char *end = NULL;
    double v;

    errno = 0;
    v = strtod(s, &end);
    if ((errno != 0) || (end == s) || (*end != '\0')) {
        return -1;
    }
    *out = (fs_aging_float_t)v;
    return 0;
}

/* Toplam varyasyon mesafesi: 0.5 * sum|gercek - hedef| */
static fs_aging_float_t dist_error(const fs_aging_ctx_t *ctx, fs_aging_dist_t d)
{
    fs_aging_float_t sum = 0.0;
    uint32_t i;
    uint32_t n = fs_aging_bin_count(ctx, d);

    for (i = 0u; i < n; i++) {
        fs_aging_float_t ideal  = 0.0;
        fs_aging_float_t actual = 0.0;
        (void)fs_aging_get_bin(ctx, d, i, &ideal, &actual);
        sum += (actual > ideal) ? (actual - ideal) : (ideal - actual);
    }
    return 0.5 * sum;
}

static void print_dist(const fs_aging_ctx_t *ctx, fs_aging_dist_t d, const char *name)
{
    uint32_t i;
    uint32_t n = fs_aging_bin_count(ctx, d);

    printf("%s dagilimi (hedef / gercek):\n", name);
    for (i = 0u; i < n; i++) {
        fs_aging_float_t ideal  = 0.0;
        fs_aging_float_t actual = 0.0;
        (void)fs_aging_get_bin(ctx, d, i, &ideal, &actual);
        printf("  %2u: %.4f / %.4f\n", i, ideal, actual);
    }
}

/* Dizin tablosu icin ust sinir: her dizin satiri en fazla
 * (derinlik - 1) ortak ust dizin + yaprak dizinler uretir. */
static uint64_t max_dirs(const fs_aging_profile_t *pr)
{
    uint64_t n = 0u;
    uint32_t i;
    for (i = 0u; i < pr->dir_count; i++) {
        n += (uint64_t)pr->dirs[i].depth + (uint64_t)pr->dirs[i].sibling_dirs + 1u;
    }
    return n;
}

static void usage(void)
{
    uint32_t i;
    fprintf(stderr, "kullanim: host_test [-q] [--max-err X] [--expect-ops N] <profil>"
                    " [volume_bytes] [confidence] [seed] [max_ops]\nprofiller:");
    for (i = 0u; i < FS_AGING_PROFILE_COUNT; i++) {
        fprintf(stderr, " %s", fs_aging_profiles[i]->name);
    }
    fprintf(stderr, "\n");
}

/* ---------------- Test ---------------------------------------------- */

int main(int argc, char **argv)
{
    static fs_aging_ctx_t ctx;
    const fs_aging_profile_t *pr;
    fake_fs_t fs;
    fs_aging_platform_t plat;
    fs_aging_config_t cfg;
    fs_aging_stats_t st;
    fs_aging_status_t s;
    fs_aging_file_t *pool;
    uint64_t cap64;
    uint64_t seed64   = 42u;
    uint64_t maxops64 = 1000u;
    uint32_t cap;
    uint32_t max_ops;
    int      quiet = 0;
    int      a     = 1;
    int      ok;
    int      quality_ok;
    int      have_max_err    = 0;
    int      have_expect_ops = 0;
    fs_aging_float_t max_err = 0.0;
    uint64_t expect_ops      = 0u;
    clock_t  t0;
    fs_aging_float_t err_age;
    fs_aging_float_t err_size;
    fs_aging_float_t err_dir;
    fs_aging_float_t live_ratio;

    memset(&cfg, 0, sizeof(cfg));
    cfg.mount_point    = "/fs0";
    cfg.volume_bytes   = 256ull << 20u;
    cfg.fill_ratio     = 0.8;
    cfg.overwrite_runs = 0u;
    cfg.confidence     = 0.0;

    /* Secenekler profil adindan once gelir */
    while ((a < argc) && (argv[a][0] == '-')) {
        if (strcmp(argv[a], "-q") == 0) {
            quiet = 1;
            a++;
        } else if ((strcmp(argv[a], "--max-err") == 0) && ((a + 1) < argc) &&
                   (parse_float(argv[a + 1], &max_err) == 0) && (max_err >= 0.0)) {
            have_max_err = 1;
            a += 2;
        } else if ((strcmp(argv[a], "--expect-ops") == 0) && ((a + 1) < argc) &&
                   (parse_u64(argv[a + 1], &expect_ops) == 0)) {
            have_expect_ops = 1;
            a += 2;
        } else {
            fprintf(stderr, "HATA: gecersiz secenek: %s\n", argv[a]);
            usage();
            return 2;
        }
    }
    if ((a >= argc) || ((argc - a) > 5)) {
        usage();
        return 2;
    }
    pr = fs_aging_profile_find(argv[a]);
    if (pr == NULL) {
        fprintf(stderr, "HATA: bilinmeyen profil: %s\n", argv[a]);
        usage();
        return 2;
    }
    if ((((a + 1) < argc) && (parse_u64(argv[a + 1], &cfg.volume_bytes) != 0)) ||
        (((a + 2) < argc) && (parse_float(argv[a + 2], &cfg.confidence) != 0)) ||
        (((a + 3) < argc) && (parse_u64(argv[a + 3], &seed64) != 0)) ||
        (((a + 4) < argc) && (parse_u64(argv[a + 4], &maxops64) != 0))) {
        fprintf(stderr, "HATA: gecersiz sayi argumani\n");
        usage();
        return 2;
    }
    if ((seed64 > 0xFFFFFFFFu) || (maxops64 == 0u) || (maxops64 > 0xFFFFFFFFu)) {
        fprintf(stderr, "HATA: seed 32 bit, max_ops 1..4294967295 olmali\n");
        return 2;
    }
    cfg.seed = (uint32_t)seed64;
    max_ops  = (uint32_t)maxops64;

    /* Havuz: dogrudan tahmin (FILL icin guvenli, ek pay yok). 0 ise hicbir
     * boyut volume'e sigmiyor; init icin en az 1 kayit verilir. */
    cap64 = fs_aging_estimate_files(pr, cfg.volume_bytes, cfg.fill_ratio);
    if (cap64 == 0u) {
        cap64 = 1u;
    }
    if (cap64 > 0xFFFFFFFEu) {
        cap64 = 0xFFFFFFFEu;
    }
    cap  = (uint32_t)cap64;
    pool = calloc(cap, sizeof(*pool));

    memset(&fs, 0, sizeof(fs));
    fs.mount     = cfg.mount_point;
    fs.mount_len = (uint32_t)strlen(cfg.mount_point);
    fs.volume    = cfg.volume_bytes;
    if ((pool == NULL) || (hset_init(&fs.files, cap) != 0) ||
        (hset_init(&fs.dirs, max_dirs(pr)) != 0)) {
        fprintf(stderr, "HATA: bellek ayrilamadi (havuz %u kayit)\n", cap);
        return 1;
    }

    plat.create_file = fake_create;
    plat.delete_file = fake_delete;
    plat.make_dir    = fake_mkdir;
    plat.now_ms      = fake_now_ms;
    plat.log         = (quiet != 0) ? NULL : fake_log;
    plat.user        = &fs;

    if (quiet == 0) {
        printf("Profil %s, volume %llu byte, confidence %.3f, seed %u, max_ops %u\n",
               pr->name, (unsigned long long)cfg.volume_bytes, cfg.confidence,
               cfg.seed, max_ops);
        printf("Havuz %u kayit (%llu byte), dosya tablosu %u slot\n", cap,
               (unsigned long long)((uint64_t)cap * sizeof(*pool)),
               fs.files.mask + 1u);
    }

    t0 = clock();
    s = fs_aging_init(&ctx, pr, &cfg, &plat, pool, cap);
    if (s == FS_AGING_RUNNING) {
        do {
            s = fs_aging_step(&ctx, max_ops);   /* ARINC'te bir periyot */
        } while (s == FS_AGING_RUNNING);
    } else if (quiet == 0) {
        printf("init: %s\n", fs_aging_status_str(s));
    } else {
        /* init hatasi SUMMARY'de raporlanir */
    }

    fs_aging_get_stats(&ctx, &st);
    st.status  = s;
    err_age    = dist_error(&ctx, FS_AGING_DIST_AGE);
    err_size   = dist_error(&ctx, FS_AGING_DIST_SIZE);
    err_dir    = dist_error(&ctx, FS_AGING_DIST_DIR);
    live_ratio = (fs_aging_float_t)st.live_bytes /
                 ((fs_aging_float_t)cfg.volume_bytes * cfg.fill_ratio);

    ok = (fs.errors == 0) && (fs.used == st.live_bytes) &&
         (fs.files.count == st.live_files) && is_done(st.status);

    /* Istege bagli kalite kapisi: dagilim hatalari ve beklenen islem sayisi */
    quality_ok = 1;
    if ((have_max_err != 0) &&
        ((err_age > max_err) || (err_size > max_err) || (err_dir > max_err))) {
        fprintf(stderr, "HATA: dagilim hatasi siniri asildi (yas=%.4f boyut=%.4f"
                        " dizin=%.4f, sinir %.4f)\n", err_age, err_size, err_dir, max_err);
        quality_ok = 0;
    }
    if ((have_expect_ops != 0) && (st.ops != expect_ops)) {
        fprintf(stderr, "HATA: islem sayisi %llu, beklenen %llu\n",
                (unsigned long long)st.ops, (unsigned long long)expect_ops);
        quality_ok = 0;
    }
    if (quality_ok == 0) {
        ok = 0;
    }

    if (quiet == 0) {
        printf("\nSonuc: %s\n", fs_aging_status_str(st.status));
        printf("  islem=%llu olusturma=%llu silme=%llu canli_dosya=%u\n",
               (unsigned long long)st.ops, (unsigned long long)st.creates,
               (unsigned long long)st.deletes, st.live_files);
        printf("  canli=%llu MB yuk=%llu MB hedef_islem=%llu yas_hatasi=%.4f\n",
               (unsigned long long)(st.live_bytes >> 20u),
               (unsigned long long)(st.workload_bytes >> 20u),
               (unsigned long long)st.convergence_ops, st.age_error);
        printf("  dizin cagrisi=%llu sahte_fs_dolu=%llu MB tutarsizlik=%d\n",
               (unsigned long long)fs.dirs_made, (unsigned long long)(fs.used >> 20u),
               fs.errors);
        printf("  sahte_fs_dosya=%u sahte_fs_dizin=%u no_space=%llu sure=%llu ms\n\n",
               fs.files.count, fs.dirs.count, (unsigned long long)fs.no_space,
               (unsigned long long)((uint64_t)(clock() - t0) * 1000u /
                                    (uint64_t)CLOCKS_PER_SEC));

        print_dist(&ctx, FS_AGING_DIST_AGE,  "Yas");
        print_dist(&ctx, FS_AGING_DIST_SIZE, "Boyut");
        print_dist(&ctx, FS_AGING_DIST_DIR,  "Dizin");
        printf("\n");
    }

    printf("SUMMARY profile=%s volume=%llu seed=%u status=%s ops=%llu"
           " live_target=%.4f age_err=%.4f size_err=%.4f dir_err=%.4f"
           " tutarsizlik=%d result=%s\n",
           pr->name, (unsigned long long)cfg.volume_bytes, cfg.seed,
           status_name(st.status), (unsigned long long)st.ops, live_ratio,
           err_age, err_size, err_dir, fs.errors, (ok != 0) ? "PASS" : "FAIL");
    if ((ok == 0) && (fs.used != st.live_bytes)) {
        fprintf(stderr, "HATA: sahte fs %llu byte, istatistik %llu byte\n",
                (unsigned long long)fs.used, (unsigned long long)st.live_bytes);
    }
    if ((ok == 0) && (fs.files.count != st.live_files)) {
        fprintf(stderr, "HATA: sahte fs %u dosya, istatistik %u dosya\n",
                fs.files.count, st.live_files);
    }

    hset_free(&fs.files);
    hset_free(&fs.dirs);
    free(pool);
    return (ok != 0) ? 0 : 1;
}
