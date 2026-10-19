/*
 * edge_tests.c - fs_aging core'u icin kenar durum testleri.
 *
 * Her senaryo ayri bir test fonksiyonudur ve PASS / FAIL / SKIP basar.
 * Herhangi bir test basarisizsa program 1 ile cikar.
 *
 * Sahte platform host_test.c'dekiyle ayni tutarlilik kontrollerini yapar
 * (cift olusturma, olmayan dosyayi silme, ust dizini olmayan yol) ve ek
 * olarak:
 *   - gercek kapasiteyi config volume'unden kucuk tutabilir (erken NO_SPACE),
 *   - N. create / delete / make_dir cagrisinda ERROR donebilir,
 *   - hic dosya yokken donen NO_SPACE'leri ve en yuksek dosya sayisini sayar,
 *   - tum cagri dizisinin (tur, yol, boyut, sonuc) ozetini tutar.
 *
 * FS_AGING_PATH_MAX varsayilan (256) ile derlenince tum testler kosar.
 * Daha kucuk bir degerle derlenince (Makefile: edge_tests_path64,
 * -DFS_AGING_PATH_MAX=64u) yalnizca yol uzunlugu testleri kosar, digerleri
 * SKIP basar; cunku context yapisi bu degere bagli oldugundan core da ayni
 * degerle derlenmelidir.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs_aging.h"
#include "fs_aging_profiles.h"

/* Varsayilan yol siniriyla mi derlendi? Degilse sadece yol testleri. */
static const int g_full_suite = ((FS_AGING_PATH_MAX) >= 256u) ? 1 : 0;

/* Sonsuz donguye karsi adim siniri */
#define MAX_STEPS 50000000u

/* Gercek cikmaz log mesajlari (fs_aging.c) */
#define MSG_FILL_DEAD      "fs_aging: bos volume'e hicbir profil boyutu sigmiyor"
#define MSG_STABLE_NOFIT   "fs_aging: volume'e sigan profil boyutu yok ve silinecek dosya yok"
#define MSG_STABLE_NOSPACE "fs_aging: olusturma NO_SPACE aldi ve silinecek dosya yok"

/* ---------------- Hash kumesi (host_test.c ile ayni) ----------------- */

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
    uint64_t    capacity;          /* gercek kapasite                    */
    uint64_t    used;
    hset_t      files;
    hset_t      dirs;
    int         errors;            /* tutarsizlik sayisi                 */

    uint64_t    calls;             /* tum callback cagrilari             */
    uint64_t    n_create;
    uint64_t    n_delete;
    uint64_t    n_mkdir;
    uint64_t    no_space;
    uint64_t    no_space_empty;    /* hic dosya yokken donen NO_SPACE     */
    uint32_t    max_files;         /* ayni anda en fazla dosya sayisi     */
    uint64_t    call_hash;         /* cagri dizisinin FNV-1a ozeti        */

    uint64_t    fail_create_at;    /* N. cagrida ERROR, 0 = hic           */
    uint64_t    fail_delete_at;
    uint64_t    fail_mkdir_at;
    uint64_t    nospace_from;      /* N. create'ten itibaren NO_SPACE, 0 = hic */

    char        log_prev[128];     /* sondan bir onceki log mesaji        */
    char        log_last[128];     /* son log mesaji                      */
} fake_fs_t;

static uint64_t fnv_bytes(uint64_t h, const void *data, size_t n)
{
    const unsigned char *p = data;
    size_t i;
    for (i = 0u; i < n; i++) {
        h ^= (uint64_t)p[i];
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t hash_n(const char *s, uint32_t n)
{
    uint64_t h = fnv_bytes(1469598103934665603ull, s, n);
    return (h == 0u) ? 1u : h;
}

/* Cagri ozetine bir kayit ekler: tur, yol, boyut, sonuc */
static void record_call(fake_fs_t *fs, char kind, const char *path,
                        uint64_t size, fs_aging_plat_result_t r)
{
    unsigned char tail[9];
    uint32_t i;

    for (i = 0u; i < 8u; i++) {
        tail[i] = (unsigned char)(size >> (8u * i));
    }
    tail[8] = (unsigned char)r;
    fs->call_hash = fnv_bytes(fs->call_hash, &kind, 1u);
    fs->call_hash = fnv_bytes(fs->call_hash, path, strlen(path) + 1u);
    fs->call_hash = fnv_bytes(fs->call_hash, tail, sizeof(tail));
    fs->calls++;
}

static uint32_t parent_len(const char *path)
{
    uint32_t n = (uint32_t)strlen(path);
    while ((n > 0u) && (path[n - 1u] != '/')) {
        n--;
    }
    return (n > 0u) ? (n - 1u) : 0u;
}

static int parent_exists(const fake_fs_t *fs, const char *path)
{
    uint32_t n = parent_len(path);
    if ((n == fs->mount_len) && (strncmp(path, fs->mount, n) == 0)) {
        return 1;
    }
    return hset_find(&fs->dirs, hash_n(path, n)) >= 0;
}

static fs_aging_plat_result_t create_impl(fake_fs_t *fs, const char *path,
                                          uint64_t size)
{
    uint64_t key = hash_n(path, (uint32_t)strlen(path));

    if (fs->n_create == fs->fail_create_at) {
        return FS_AGING_PLAT_ERROR;              /* enjekte edilen hata */
    }
    if (hset_find(&fs->files, key) >= 0) {
        fprintf(stderr, "  HATA: dosya zaten var: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    if (parent_exists(fs, path) == 0) {
        fprintf(stderr, "  HATA: ust dizin yok: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    if (((fs->used + size) > fs->capacity) ||
        ((fs->nospace_from != 0u) && (fs->n_create >= fs->nospace_from))) {
        fs->no_space++;
        if (fs->files.count == 0u) {
            fs->no_space_empty++;
        }
        return FS_AGING_PLAT_NO_SPACE;
    }
    if (hset_insert(&fs->files, key, size) != 0) {
        fprintf(stderr, "  HATA: sahte fs dosya tablosu dolu\n");
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    fs->used += size;
    if (fs->files.count > fs->max_files) {
        fs->max_files = fs->files.count;
    }
    return FS_AGING_PLAT_OK;
}

static fs_aging_plat_result_t fake_create(void *u, const char *path, uint64_t size)
{
    fake_fs_t *fs = u;
    fs_aging_plat_result_t r;

    fs->n_create++;
    r = create_impl(fs, path, size);
    record_call(fs, 'C', path, size, r);
    return r;
}

static fs_aging_plat_result_t delete_impl(fake_fs_t *fs, const char *path)
{
    int64_t slot;

    if (fs->n_delete == fs->fail_delete_at) {
        return FS_AGING_PLAT_ERROR;
    }
    slot = hset_find(&fs->files, hash_n(path, (uint32_t)strlen(path)));
    if (slot < 0) {
        fprintf(stderr, "  HATA: olmayan dosya siliniyor: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    fs->used -= fs->files.vals[slot];
    hset_remove_at(&fs->files, (uint32_t)slot);
    return FS_AGING_PLAT_OK;
}

static fs_aging_plat_result_t fake_delete(void *u, const char *path)
{
    fake_fs_t *fs = u;
    fs_aging_plat_result_t r;

    fs->n_delete++;
    r = delete_impl(fs, path);
    record_call(fs, 'D', path, 0u, r);
    return r;
}

static fs_aging_plat_result_t mkdir_impl(fake_fs_t *fs, const char *path)
{
    uint64_t key = hash_n(path, (uint32_t)strlen(path));

    if (fs->n_mkdir == fs->fail_mkdir_at) {
        return FS_AGING_PLAT_ERROR;
    }
    if (parent_exists(fs, path) == 0) {
        fprintf(stderr, "  HATA: ust dizin yok: %s\n", path);
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    if (hset_find(&fs->dirs, key) >= 0) {
        return FS_AGING_PLAT_OK;
    }
    if (hset_insert(&fs->dirs, key, 0u) != 0) {
        fprintf(stderr, "  HATA: sahte fs dizin tablosu dolu\n");
        fs->errors++;
        return FS_AGING_PLAT_ERROR;
    }
    return FS_AGING_PLAT_OK;
}

static fs_aging_plat_result_t fake_mkdir(void *u, const char *path)
{
    fake_fs_t *fs = u;
    fs_aging_plat_result_t r;

    fs->n_mkdir++;
    r = mkdir_impl(fs, path);
    record_call(fs, 'M', path, 0u, r);
    return r;
}

/* Log: core'un son iki mesaji saklanir (mesaj yalnizca cagri suresince
 * gecerlidir, kopyalanir). Bitiste son mesaj durum metnidir, bir
 * oncekisi hatanin nedenidir. */
static void fake_log(void *u, const char *msg)
{
    fake_fs_t *fs = u;
    memcpy(fs->log_prev, fs->log_last, sizeof(fs->log_prev));
    (void)strncpy(fs->log_last, msg, sizeof(fs->log_last) - 1u);
    fs->log_last[sizeof(fs->log_last) - 1u] = '\0';
}

static uint64_t max_dirs(const fs_aging_profile_t *pr)
{
    uint64_t n = 0u;
    uint32_t i;
    for (i = 0u; i < pr->dir_count; i++) {
        n += (uint64_t)pr->dirs[i].depth + (uint64_t)pr->dirs[i].sibling_dirs + 1u;
    }
    return n;
}

/* ---------------- Test altyapisi ------------------------------------ */

static int g_failed_total;
static int g_passed_total;
static int g_skipped_total;
static int g_cur_failed;

static void check(int cond, const char *fmt, ...)
{
    va_list ap;
    if (cond != 0) {
        return;
    }
    g_cur_failed = 1;
    printf("    beklenmeyen: ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static void test_begin(void)
{
    g_cur_failed = 0;
}

static void test_end(const char *name)
{
    if (g_cur_failed != 0) {
        g_failed_total++;
        printf("[FAIL] %s\n", name);
    } else {
        g_passed_total++;
        printf("[PASS] %s\n", name);
    }
    fflush(stdout);
}

static void test_skip(const char *name, const char *why)
{
    g_skipped_total++;
    printf("[SKIP] %s (%s)\n", name, why);
    fflush(stdout);
}

static const char *st_name(fs_aging_status_t s)
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

static fs_aging_config_t base_config(uint64_t volume, uint32_t seed)
{
    fs_aging_config_t c;
    memset(&c, 0, sizeof(c));
    c.mount_point    = "/fs0";
    c.volume_bytes   = volume;
    c.fill_ratio     = 0.8;
    c.seed           = seed;
    c.overwrite_runs = 0u;
    c.time_limit_s   = 0u;
    c.confidence     = 0.0;
    return c;
}

/* ---------------- Kosu yardimcisi ------------------------------------ */

typedef struct {
    const fs_aging_profile_t *profile;
    fs_aging_config_t cfg;
    uint32_t max_ops;
    uint32_t pool_cap;           /* 0: fs_aging_estimate_files()    */
    uint64_t fs_capacity;        /* 0: cfg.volume_bytes             */
    uint64_t fail_create_at;
    uint64_t fail_delete_at;
    uint64_t fail_mkdir_at;
    uint64_t nospace_from;       /* N. create'ten itibaren NO_SPACE  */
    int      watch;              /* adim basina cagri izleme (max_ops=1) */
    int      stop_at_stable;     /* STABLE'a gecilince dur (yalniz FILL) */
} run_opts_t;

typedef struct {
    fs_aging_status_t init_status;
    fs_aging_status_t status;
    fs_aging_stats_t  st;
    uint32_t          nbins[3];
    fs_aging_float_t  ideal [3][FS_AGING_MAX_BINS];
    fs_aging_float_t  actual[3][FS_AGING_MAX_BINS];

    uint64_t calls;
    uint64_t call_hash;
    uint64_t n_create;
    uint64_t n_delete;
    uint64_t n_mkdir;
    uint64_t no_space;
    uint64_t no_space_empty;
    uint32_t max_files;          /* sahte fs'te en fazla dosya       */
    uint32_t pool_cap;           /* kullanilan havuz kapasitesi      */
    uint64_t stable_empty_steps; /* STABLE'da canli dosya 0 olan adim */
    uint64_t stable_pool_full;   /* STABLE'da havuz dolu olan adim   */
    int      fs_errors;
    uint64_t fs_used;
    uint32_t fs_files;
    uint64_t steps;
    int      step_limit_hit;
    uint64_t calls_after_end;    /* bitmis ctx'e ek step'teki cagrilar */
    fs_aging_status_t status_after_end;
    char     err_msg[128];       /* bitiste durum metninden onceki log */

    /* STABLE'a gecilen adimda sum(count_age) == live_files mi? */
    int      rebin_checked;
    int      rebin_sum_bad;
    uint64_t rebin_sum;
    uint32_t rebin_live;

    /* watch != 0 iken doldurulur */
    uint64_t max_calls_setup;    /* SETUP adiminda en fazla cagri    */
    uint64_t max_calls_work;     /* FILL/STABLE adiminda en fazla    */
    uint64_t two_call_steps;     /* NO_SPACE + silme iceren adimlar  */
    uint64_t rebin_steps;        /* cagrisiz yas kovasi adimlari     */
    uint32_t max_rebin_advance;  /* bir adimda taranan en fazla kayit */
    uint32_t rebin_pool;         /* yerlestirme sirasindaki havuz    */
    uint32_t max_setup_depth;    /* profildeki en buyuk derinlik     */
    int      step_violation;
} run_result_t;

static int consistent(const run_result_t *r)
{
    return (r->fs_errors == 0) && (r->fs_used == r->st.live_bytes) &&
           (r->fs_files == r->st.live_files);
}

static int run_aging(const run_opts_t *o, run_result_t *r)
{
    static fs_aging_ctx_t ctx;
    fake_fs_t fs;
    fs_aging_platform_t plat;
    fs_aging_file_t *pool;
    uint64_t cap64;
    uint32_t cap;
    uint32_t d;
    uint32_t i;

    memset(r, 0, sizeof(*r));
    if (o->pool_cap != 0u) {
        cap64 = o->pool_cap;
    } else {
        /* API'nin dondurdugu deger dogrudan havuz kapasitesi (ek pay yok) */
        cap64 = fs_aging_estimate_files(o->profile, o->cfg.volume_bytes,
                                        o->cfg.fill_ratio);
    }
    if (cap64 == 0u) {
        cap64 = 1u;
    }
    cap = (uint32_t)cap64;
    r->pool_cap = cap;

    memset(&fs, 0, sizeof(fs));
    fs.mount          = o->cfg.mount_point;
    fs.mount_len      = (uint32_t)strlen(o->cfg.mount_point);
    fs.capacity       = (o->fs_capacity != 0u) ? o->fs_capacity : o->cfg.volume_bytes;
    fs.call_hash      = 1469598103934665603ull;
    fs.fail_create_at = o->fail_create_at;
    fs.fail_delete_at = o->fail_delete_at;
    fs.fail_mkdir_at  = o->fail_mkdir_at;
    fs.nospace_from   = o->nospace_from;
    pool = calloc(cap, sizeof(*pool));
    if ((pool == NULL) || (hset_init(&fs.files, cap) != 0) ||
        (hset_init(&fs.dirs, max_dirs(o->profile)) != 0)) {
        printf("    bellek ayrilamadi\n");
        exit(2);
    }

    plat.create_file = fake_create;
    plat.delete_file = fake_delete;
    plat.make_dir    = fake_mkdir;
    plat.now_ms      = NULL;      /* zaman sonuclari etkilemesin */
    plat.log         = fake_log;
    plat.user        = &fs;

    for (i = 0u; i < o->profile->dir_count; i++) {
        if (o->profile->dirs[i].depth > r->max_setup_depth) {
            r->max_setup_depth = o->profile->dirs[i].depth;
        }
    }

    r->init_status = fs_aging_init(&ctx, o->profile, &o->cfg, &plat, pool, cap);
    r->status = r->init_status;
    while ((r->status == FS_AGING_RUNNING) && (r->steps < MAX_STEPS) &&
           !((o->stop_at_stable != 0) && (ctx.phase == FS_AGING_PHASE_STABLE))) {
        uint64_t c0 = fs.calls;
        uint64_t n0 = fs.no_space;
        uint64_t k0 = fs.n_create;
        uint64_t e0 = fs.n_delete;
        fs_aging_phase_t ph0 = ctx.phase;
        uint32_t ra0 = ctx.rebin_active;
        uint32_t cu0 = ctx.rebin_cursor;

        r->status = fs_aging_step(&ctx, o->max_ops);
        r->steps++;

        /* max_ops=1 iken adim sonu durumu her islemden sonraki durumdur */
        if (ctx.phase == FS_AGING_PHASE_STABLE) {
            if (ctx.live_files == 0u) {
                r->stable_empty_steps++;
            }
            if (ctx.live_files >= cap) {
                r->stable_pool_full++;
            }
        }

        /* Yas kovasi yerlestirme her canli kaydi tam bir kez saymali.
         * STABLE'da bu esitlik korunur; gecis adiminda bir kez bakilir. */
        if ((ph0 != FS_AGING_PHASE_STABLE) && (ctx.phase == FS_AGING_PHASE_STABLE) &&
            (r->rebin_checked == 0)) {
            uint64_t sum = 0u;
            for (i = 0u; i < o->profile->age_count; i++) {
                sum += ctx.count_age[i];
            }
            r->rebin_checked = 1;
            r->rebin_sum     = sum;
            r->rebin_live    = ctx.live_files;
            r->rebin_sum_bad = (sum != (uint64_t)ctx.live_files) ? 1 : 0;
        }

        if (o->watch != 0) {
            uint64_t dc = fs.calls - c0;
            if (ph0 == FS_AGING_PHASE_SETUP) {
                if (dc > r->max_calls_setup) {
                    r->max_calls_setup = dc;
                }
                if ((fs.n_create != k0) || (fs.n_delete != e0)) {
                    r->step_violation = 1;
                }
            } else if ((ph0 == FS_AGING_PHASE_FILL) &&
                       ((ra0 != 0u) || (ctx.rebin_active != 0u) ||
                        (ctx.phase == FS_AGING_PHASE_STABLE))) {
                /* yas kovasi yerlestirme adimi: cagri yok, sinirli tarama */
                uint32_t adv = ctx.rebin_cursor - ((ra0 != 0u) ? cu0 : 0u);
                r->rebin_steps++;
                r->rebin_pool = ctx.pool_high_water;
                if (adv > r->max_rebin_advance) {
                    r->max_rebin_advance = adv;
                }
                if (dc != 0u) {
                    r->step_violation = 1;
                }
            } else {
                if (dc > r->max_calls_work) {
                    r->max_calls_work = dc;
                }
                if (dc == 2u) {
                    /* kabul edilen ikililer: NO_SPACE donen olusturma ve
                     * yerine yapilan silme; ya da FILL'de bos volume'de
                     * NO_SPACE donen olusturma ve en kucuk boyut denemesi */
                    r->two_call_steps++;
                    if (!((((fs.no_space - n0) == 1u) && ((fs.n_create - k0) == 1u) &&
                           ((fs.n_delete - e0) == 1u)) ||
                          ((ph0 == FS_AGING_PHASE_FILL) && ((fs.no_space - n0) >= 1u) &&
                           ((fs.n_create - k0) == 2u) && (fs.n_delete == e0)))) {
                        r->step_violation = 1;
                    }
                } else if (dc > 2u) {
                    r->step_violation = 1;
                } else {
                    /* 0 ya da 1 cagri */
                }
            }
        }
    }
    r->step_limit_hit = ((r->status == FS_AGING_RUNNING) &&
                         !((o->stop_at_stable != 0) &&
                           (ctx.phase == FS_AGING_PHASE_STABLE))) ? 1 : 0;

    /* Bitmis context'e ek step: durum ayni kalmali, cagri yapilmamali.
     * Yalniz FILL kosusunda ctx bitmedigi icin atlanir. */
    if (o->stop_at_stable != 0) {
        r->status_after_end = r->status;
    } else {
        uint64_t c0 = fs.calls;
        r->status_after_end = fs_aging_step(&ctx, o->max_ops);
        r->calls_after_end  = fs.calls - c0;
    }

    fs_aging_get_stats(&ctx, &r->st);
    for (d = 0u; d < 3u; d++) {
        r->nbins[d] = fs_aging_bin_count(&ctx, (fs_aging_dist_t)d);
        for (i = 0u; i < r->nbins[d]; i++) {
            (void)fs_aging_get_bin(&ctx, (fs_aging_dist_t)d, i,
                                   &r->ideal[d][i], &r->actual[d][i]);
        }
    }
    r->calls     = fs.calls;
    r->call_hash = fs.call_hash;
    r->n_create  = fs.n_create;
    r->n_delete  = fs.n_delete;
    r->n_mkdir   = fs.n_mkdir;
    r->no_space  = fs.no_space;
    r->no_space_empty = fs.no_space_empty;
    r->max_files = fs.max_files;
    r->fs_errors = fs.errors;
    r->fs_used   = fs.used;
    r->fs_files  = fs.files.count;
    memcpy(r->err_msg, fs.log_prev, sizeof(r->err_msg));

    hset_free(&fs.files);
    hset_free(&fs.dirs);
    free(pool);
    return 0;
}

static void print_run(const char *label, const run_result_t *r)
{
    printf("    %s: %s islem=%llu canli_dosya=%u canli=%llu cagri=%llu"
           " no_space=%llu adim=%llu ozet=%016llx\n",
           label, st_name(r->status), (unsigned long long)r->st.ops,
           r->st.live_files, (unsigned long long)r->st.live_bytes,
           (unsigned long long)r->calls, (unsigned long long)r->no_space,
           (unsigned long long)r->steps, (unsigned long long)r->call_hash);
}

/* Ortak son kontroller: tutarlilik ve bitmis ctx'in hareketsizligi */
static void check_common(const run_result_t *r)
{
    check(r->step_limit_hit == 0, "adim siniri asildi (%u)", MAX_STEPS);
    check(consistent(r), "tutarsizlik: hata=%d sahte_fs=%llu/%u istatistik=%llu/%u",
          r->fs_errors, (unsigned long long)r->fs_used, r->fs_files,
          (unsigned long long)r->st.live_bytes, r->st.live_files);
    check(r->calls_after_end == 0u, "bitmis ctx'e step %llu cagri yapti",
          (unsigned long long)r->calls_after_end);
    check(r->status_after_end == r->status, "bitmis ctx durumu degisti: %s -> %s",
          st_name(r->status), st_name(r->status_after_end));
    check(r->rebin_sum_bad == 0, "yas kovasi yerlestirme eksik: sum(count_age)=%llu,"
          " canli_dosya=%u", (unsigned long long)r->rebin_sum, r->rebin_live);
}

static int same_results(const run_result_t *a, const run_result_t *b)
{
    uint32_t d;
    uint32_t i;

    if ((a->status != b->status) || (a->st.phase != b->st.phase) ||
        (a->st.ops != b->st.ops) || (a->st.creates != b->st.creates) ||
        (a->st.deletes != b->st.deletes) || (a->st.live_bytes != b->st.live_bytes) ||
        (a->st.workload_bytes != b->st.workload_bytes) ||
        (a->st.live_files != b->st.live_files) ||
        (a->st.convergence_ops != b->st.convergence_ops) ||
        (a->st.age_error != b->st.age_error) ||
        (a->calls != b->calls) || (a->call_hash != b->call_hash)) {
        return 0;
    }
    for (d = 0u; d < 3u; d++) {
        if (a->nbins[d] != b->nbins[d]) {
            return 0;
        }
        for (i = 0u; i < a->nbins[d]; i++) {
            if ((a->ideal[d][i] != b->ideal[d][i]) ||
                (a->actual[d][i] != b->actual[d][i])) {
                return 0;
            }
        }
    }
    return 1;
}

/* ---------------- Yardimci profiller -------------------------------- */

/* Kucuk dosyali profil: tek boyut (4 KB), derinlik 0 ve 1 (3 kardes).
 * Hic NO_SPACE olusmaz ve her SETUP islemi tek make_dir yapar; boylece
 * max_ops=1 ile her adim en fazla 1 platform cagrisi yapmalidir. Havuz
 * birden fazla FS_AGING_REBIN_CHUNK'a yayilir. */
static const fs_aging_age_bin_t  tiny_ages[]  = { { 1.0, 1.0 }, { 2.0, 1.0 } };
static const fs_aging_size_bin_t tiny_sizes[] = { { 4096u, 1.0 } };
static const fs_aging_dir_bin_t  tiny_dirs[]  = { { 0u, 1.0, 0u }, { 1u, 1.0, 3u } };
static const fs_aging_profile_t  tiny_profile = {
    "tiny", tiny_ages, 2u, tiny_sizes, 1u, tiny_dirs, 2u
};

/* Karisik boyutlu profil: cok sayida 4 KB, az sayida 1 MB dosya. Gercek
 * kapasite hedefin biraz ustundeyse 1 MB'lik olusturmalar hem FILL
 * sonunda hem STABLE'da NO_SPACE alir, 4 KB'lik olanlar ilerlemeyi surdurur. */
static const fs_aging_size_bin_t mix_sizes[] = { { 4096u, 9.0 }, { 1048576u, 1.0 } };
static const fs_aging_profile_t  mix_profile = {
    "mix", tiny_ages, 2u, mix_sizes, 2u, tiny_dirs, 2u
};

/* Derinlik 0 profil: en uzun yol = strlen(mount) + 23 */
static const fs_aging_dir_bin_t  flat_dirs[]  = { { 0u, 1.0, 0u } };
static const fs_aging_profile_t  flat_profile = {
    "flat", tiny_ages, 2u, tiny_sizes, 1u, flat_dirs, 1u
};

/* Hicbir yola sigamayacak kadar derin satir: init'teki yol hesabi
 * derinlikle orantili donmemeli, hemen PATH_TOO_LONG vermeli */
static const fs_aging_dir_bin_t  huge_depth_dirs[] = { { 0xFFFFFFFFu, 1.0, 0u } };
static const fs_aging_profile_t  huge_depth_profile = {
    "huge_depth", tiny_ages, 2u, tiny_sizes, 1u, huge_depth_dirs, 1u
};

/* 16 MB volume'e hic sigmayan 64 MB'lik boyut + 4 KB: FILL'in ilk
 * islemleri bos volume'de NO_SPACE alabilir (silinecek dosya yok) */
static const fs_aging_size_bin_t bigmix_sizes[] = { { 4096u, 3.0 }, { 67108864u, 1.0 } };
static const fs_aging_profile_t  bigmix_profile = {
    "bigmix", tiny_ages, 2u, bigmix_sizes, 2u, tiny_dirs, 2u
};

/* 16 MB volume'e hicbir boyutu sigmayan profil: gercek cikmaz */
static const fs_aging_size_bin_t nofit_sizes[] = { { 33554432u, 1.0 }, { 67108864u, 1.0 } };
static const fs_aging_profile_t  nofit_profile = {
    "nofit", tiny_ages, 2u, nofit_sizes, 2u, tiny_dirs, 2u
};

/* Tek boyut = volume (16 MB): FILL tek dosyayla biter; STABLE'da
 * pick_size_stable() bu boyutu sigmaz sayar (live + size >= volume),
 * dosya silinince olusturulacak boyut da kalmaz: STABLE cikmazi. Yas
 * esikleri hedef islem sayisini (T = 1000 x FILL islemi) buyutur; boylece
 * tam yakinsama cikmazdan once gelmez. */
static const fs_aging_age_bin_t  whole_ages[]  = { { 1.0, 1.0 }, { 1000.0, 1.0 } };
static const fs_aging_size_bin_t whole_sizes[] = { { 16777216u, 1.0 } };
static const fs_aging_profile_t  whole_profile = {
    "whole", whole_ages, 2u, whole_sizes, 1u, tiny_dirs, 2u
};

/* Tek boyut = volume'un yarisi (8 MB), fill 0.5: FILL tek dosyayla biter,
 * STABLE'da dosya silinince 8 MB yine sigar; platform ikinci create'ten
 * itibaren NO_SPACE donerse STABLE'da canli dosya yokken NO_SPACE cikmazi. */
static const fs_aging_size_bin_t half_sizes[] = { { 8388608u, 1.0 } };
static const fs_aging_profile_t  half_profile = {
    "half", whole_ages, 2u, half_sizes, 1u, tiny_dirs, 2u
};

/* En kucuk boyutun agirligi cok kucuk: bos volume'de 8 KB NO_SPACE alinca
 * ayni islemde 4 KB denenir (ornekleme ile 4 KB'yi beklemek gerekmez). */
static const fs_aging_size_bin_t rare_sizes[] = { { 4096u, 0.0001 }, { 8192u, 1.0 } };
static const fs_aging_profile_t  rare_profile = {
    "rare", tiny_ages, 2u, rare_sizes, 2u, tiny_dirs, 2u
};

/* Havuz tahmini: volume'e sigan ama FILL'de yalnizca volume bosken
 * sigan boyutlar (volume - hedef < boyut <= volume). Bu boyutlar ortalamaya
 * katilsaydi tahmin cok kucuk cikardi. */
static const fs_aging_size_bin_t tail1_sizes[] = { { 4096u, 0.99 }, { 16777216u, 0.01 } };
static const fs_aging_profile_t  tail1_profile = {
    "tail1", tiny_ages, 2u, tail1_sizes, 2u, tiny_dirs, 2u
};
static const fs_aging_size_bin_t tail2_sizes[] = { { 4096u, 0.9 }, { 8388608u, 0.1 } };
static const fs_aging_profile_t  tail2_profile = {
    "tail2", tiny_ages, 2u, tail2_sizes, 2u, tiny_dirs, 2u
};

/* ---------------- Testler ------------------------------------------- */

static void test_invalid_params(void)
{
    static fs_aging_ctx_t ctx;
    static fs_aging_file_t pool[64];
    static fs_aging_age_bin_t  ages [FS_AGING_MAX_BINS + 1u];
    static fs_aging_size_bin_t sizes[FS_AGING_MAX_BINS + 1u];
    static fs_aging_dir_bin_t  dirs [FS_AGING_MAX_BINS + 1u];
    const fs_aging_profile_t *ag = &fs_aging_profile_agrawal;
    fs_aging_profile_t  pr;
    fs_aging_config_t   cfg;
    fs_aging_platform_t plat;
    fake_fs_t fs;
    fs_aging_status_t s;
    uint32_t i;
    int c;

    test_begin();
    memset(&fs, 0, sizeof(fs));
    fs.mount     = "/fs0";
    fs.mount_len = 4u;

    /* 0: gecerli taban, 1..: tek bir bozulma */
    for (c = 0; c <= 40; c++) {
        const char *what = "gecerli taban";
        fs_aging_ctx_t           *pctx  = &ctx;
        const fs_aging_profile_t *ppr   = &pr;
        const fs_aging_config_t  *pcfg  = &cfg;
        const fs_aging_platform_t *ppl  = &plat;
        fs_aging_file_t          *ppool = pool;
        uint32_t                  cap   = 64u;
        fs_aging_status_t         want  = FS_AGING_ERR_PARAM;

        /* Tabanin kopyalari: her durumda yeniden kurulur */
        memset(ages, 0, sizeof(ages));
        memset(sizes, 0, sizeof(sizes));
        memset(dirs, 0, sizeof(dirs));
        for (i = 0u; i < FS_AGING_MAX_BINS + 1u; i++) {
            ages[i].age_cutoff  = (fs_aging_float_t)(i + 1u) * 10.0;
            ages[i].weight      = 1.0;
            sizes[i].size_bytes = 4096u;
            sizes[i].weight     = 1.0;
            dirs[i].depth       = 1u;
            dirs[i].weight      = 1.0;
        }
        for (i = 0u; i < ag->age_count; i++) {
            ages[i] = ag->ages[i];
        }
        for (i = 0u; i < ag->size_count; i++) {
            sizes[i] = ag->sizes[i];
        }
        for (i = 0u; i < ag->dir_count; i++) {
            dirs[i] = ag->dirs[i];
        }
        pr = *ag;
        pr.ages  = ages;
        pr.sizes = sizes;
        pr.dirs  = dirs;
        cfg = base_config(16ull << 20u, 1u);
        plat.create_file = fake_create;
        plat.delete_file = fake_delete;
        plat.make_dir    = fake_mkdir;
        plat.now_ms      = NULL;
        plat.log         = NULL;
        plat.user        = &fs;

        switch (c) {
        case 0:  want = FS_AGING_RUNNING; break;
        case 1:  what = "ctx NULL";       pctx  = NULL; break;
        case 2:  what = "profile NULL";   ppr   = NULL; break;
        case 3:  what = "config NULL";    pcfg  = NULL; break;
        case 4:  what = "platform NULL";  ppl   = NULL; break;
        case 5:  what = "pool NULL";      ppool = NULL; break;
        case 6:  what = "pool_capacity 0"; cap  = 0u; break;
        case 7:  what = "pool_capacity 0xFFFFFFFF"; cap = 0xFFFFFFFFu; break;
        case 8:  what = "create_file NULL"; plat.create_file = NULL; break;
        case 9:  what = "delete_file NULL"; plat.delete_file = NULL; break;
        case 10: what = "make_dir NULL";    plat.make_dir = NULL; break;
        case 11: what = "mount_point NULL"; cfg.mount_point = NULL; break;
        case 12: what = "volume 0";         cfg.volume_bytes = 0u; break;
        case 13: what = "fill_ratio 0";     cfg.fill_ratio = 0.0; break;
        case 14: what = "fill_ratio -0.5";  cfg.fill_ratio = -0.5; break;
        case 15: what = "fill_ratio 1.0001"; cfg.fill_ratio = 1.0001; break;
        case 16: what = "confidence -0.1";  cfg.confidence = -0.1; break;
        case 17: what = "confidence 1.0";   cfg.confidence = 1.0; break;
        case 18: what = "confidence 1.5";   cfg.confidence = 1.5; break;
        case 19: what = "esit yas esikleri"; ages[3].age_cutoff = ages[2].age_cutoff; break;
        case 20: what = "azalan yas esigi"; ages[5].age_cutoff = ages[4].age_cutoff - 1.0; break;
        case 21: what = "ilk yas esigi 0";  ages[0].age_cutoff = 0.0; break;
        case 22: what = "yas satiri 1";     pr.age_count = 1u; break;
        case 23: what = "yas satiri MAX+1";
            pr.age_count = FS_AGING_MAX_BINS + 1u;
            for (i = 0u; i < (FS_AGING_MAX_BINS + 1u); i++) {
                ages[i].age_cutoff = (fs_aging_float_t)(i + 1u);
            }
            break;
        case 24: what = "boyut satiri MAX+1"; pr.size_count = FS_AGING_MAX_BINS + 1u; break;
        case 25: what = "dizin satiri MAX+1"; pr.dir_count = FS_AGING_MAX_BINS + 1u; break;
        case 26: what = "boyut satiri 0";   pr.size_count = 0u; break;
        case 27: what = "dizin satiri 0";   pr.dir_count = 0u; break;
        case 28: what = "negatif yas agirligi";   ages[1].weight = -1.0; break;
        case 29: what = "negatif boyut agirligi"; sizes[2].weight = -1.0; break;
        case 30: what = "negatif dizin agirligi"; dirs[3].weight = -1.0; break;
        case 31: what = "tum yas agirliklari 0";
            for (i = 0u; i < pr.age_count; i++) {
                ages[i].weight = 0.0;
            }
            break;
        case 32: what = "ages NULL";  pr.ages = NULL; break;
        case 33: what = "sizes NULL"; pr.sizes = NULL; break;
        case 34: what = "dirs NULL";  pr.dirs = NULL; break;
        case 35: what = "sibling_dirs 65536"; dirs[0].sibling_dirs = 65536u; break;
        /* Sinirda gecerli olanlar */
        case 36: what = "fill_ratio 1.0 (gecerli)";
            cfg.fill_ratio = 1.0; want = FS_AGING_RUNNING; break;
        case 37: what = "confidence 0.999 (gecerli)";
            cfg.confidence = 0.999; want = FS_AGING_RUNNING; break;
        case 38: what = "yas satiri MAX (gecerli)";
            pr.age_count = FS_AGING_MAX_BINS;
            for (i = 0u; i < FS_AGING_MAX_BINS; i++) {
                ages[i].age_cutoff = (fs_aging_float_t)(i + 1u);
            }
            want = FS_AGING_RUNNING; break;
        case 39: what = "pool_capacity 1 (gecerli)"; cap = 1u; want = FS_AGING_RUNNING; break;
        default: what = "sibling_dirs 65535 (gecerli)";
            dirs[0].sibling_dirs = 65535u; want = FS_AGING_RUNNING; break;
        }

        fs.calls = 0u;
        s = fs_aging_init(pctx, ppr, pcfg, ppl, ppool, cap);
        check(s == want, "%s: init %s, beklenen %s", what, st_name(s), st_name(want));
        if (want == FS_AGING_ERR_PARAM) {
            /* Hatali ctx step'te de hata donmeli ve platforma dokunmamali */
            fs_aging_status_t s2 = fs_aging_step(pctx, 10u);
            check(s2 == FS_AGING_ERR_PARAM, "%s: step %s", what, st_name(s2));
        }
        check(fs.calls == 0u, "%s: platform cagrildi", what);
    }

    s = fs_aging_step(NULL, 1u);
    check(s == FS_AGING_ERR_PARAM, "step(NULL): %s", st_name(s));
    test_end("gecersiz parametreler -> FS_AGING_ERR_PARAM");
}

static void test_pool_full(void)
{
    run_opts_t o;
    run_result_t r;

    test_begin();
    memset(&o, 0, sizeof(o));
    o.profile  = &fs_aging_profile_agrawal;
    o.cfg      = base_config(16ull << 20u, 42u);
    o.max_ops  = 1000u;
    o.pool_cap = 10u;
    run_aging(&o, &r);
    print_run("havuz 10", &r);
    check(r.status == FS_AGING_ERR_POOL_FULL, "durum %s", st_name(r.status));
    check(r.st.live_files == 10u, "canli dosya %u, beklenen 10", r.st.live_files);
    check(r.st.phase == FS_AGING_PHASE_DONE, "asama %d", (int)r.st.phase);
    check_common(&r);
    test_end("havuz cok kucuk -> FS_AGING_ERR_POOL_FULL");
}

static void test_no_space_early(void)
{
    run_opts_t o;
    run_result_t r;
    uint64_t vol = 64ull << 20u;

    test_begin();
    memset(&o, 0, sizeof(o));
    o.max_ops = 1000u;

    /* a) Gercek kapasite hedefin (0.8) 512 KB ustunde, volume'un altinda:
     *    1 MB'lik dosyalar NO_SPACE alir, aging tam yakinsamaya ulasir. */
    o.profile     = &mix_profile;
    o.pool_cap    = 4096u;      /* burada havuz tahmini test edilmiyor */
    o.cfg         = base_config(16ull << 20u, 42u);
    o.fs_capacity = (uint64_t)((fs_aging_float_t)o.cfg.volume_bytes * 0.8) +
                    (512u << 10u);
    run_aging(&o, &r);
    print_run("mix, kapasite hedef+512K", &r);
    check(is_done(r.status), "durum %s", st_name(r.status));
    check(r.no_space > 0u, "hic NO_SPACE donmedi");
    check(r.fs_used <= o.fs_capacity, "kapasite asildi");
    check_common(&r);

    /* b) agrawal, gercek kapasite volume'un %81'i (hedef %80) */
    o.profile     = &fs_aging_profile_agrawal;
    o.pool_cap    = 0u;
    o.cfg         = base_config(vol, 42u);
    o.fs_capacity = (vol / 100u) * 81u;
    run_aging(&o, &r);
    print_run("agrawal, kapasite %81", &r);
    check(is_done(r.status), "durum %s", st_name(r.status));
    check(r.no_space > 0u, "hic NO_SPACE donmedi");
    check(r.fs_used <= o.fs_capacity, "kapasite asildi");
    check_common(&r);

    /* c) Gercek kapasite hedefin altinda: FILL hedefe ulasamaz, ancak
     *    yuk siniri ile hatasiz biter. */
    o.fs_capacity        = vol / 2u;
    o.cfg.overwrite_runs = 3u;
    run_aging(&o, &r);
    print_run("kapasite %50, runs 3", &r);
    check(r.status == FS_AGING_DONE_WORKLOAD_LIMIT, "durum %s", st_name(r.status));
    check(r.no_space > 0u, "hic NO_SPACE donmedi");
    check(r.fs_used <= o.fs_capacity, "kapasite asildi");
    check_common(&r);
    test_end("erken NO_SPACE -> aging hatasiz suruyor");
}

static void run_error_case(const char *name, uint64_t fc, uint64_t fd, uint64_t fm)
{
    run_opts_t o;
    run_result_t r;

    test_begin();
    memset(&o, 0, sizeof(o));
    o.profile        = &fs_aging_profile_agrawal;
    o.cfg            = base_config(16ull << 20u, 42u);
    o.max_ops        = 1000u;
    o.fail_create_at = fc;
    o.fail_delete_at = fd;
    o.fail_mkdir_at  = fm;
    run_aging(&o, &r);
    print_run("hata", &r);
    check(r.status == FS_AGING_ERR_PLATFORM, "durum %s", st_name(r.status));
    if (fc != 0u) {
        check(r.n_create == fc, "create cagrisi %llu, hata %llu. cagrida",
              (unsigned long long)r.n_create, (unsigned long long)fc);
    }
    if (fd != 0u) {
        check(r.n_delete == fd, "delete cagrisi %llu, hata %llu. cagrida",
              (unsigned long long)r.n_delete, (unsigned long long)fd);
        check(r.st.deletes == (fd - 1u), "istatistik silme %llu",
              (unsigned long long)r.st.deletes);
    }
    if (fm != 0u) {
        check(r.n_mkdir == fm, "make_dir cagrisi %llu, hata %llu. cagrida",
              (unsigned long long)r.n_mkdir, (unsigned long long)fm);
        check(r.n_create == 0u, "SETUP hatasindan sonra dosya olusturuldu");
    }
    check_common(&r);
    test_end(name);
}

static void test_error_create(void)
{
    run_error_case("create_file ERROR -> FS_AGING_ERR_PLATFORM", 5u, 0u, 0u);
}

static void test_error_delete(void)
{
    run_error_case("delete_file ERROR -> FS_AGING_ERR_PLATFORM", 0u, 3u, 0u);
}

static void test_error_mkdir(void)
{
    run_error_case("make_dir ERROR -> FS_AGING_ERR_PLATFORM", 0u, 0u, 7u);
}

/* Yol siniri: derinlik 0 profilde en uzun yol strlen(mount) + 23'tur.
 * Tam FS_AGING_PATH_MAX gecerli, bir fazlasi gecersiz olmali. */
static void test_path_boundary(void)
{
    static fs_aging_ctx_t ctx;
    static fs_aging_file_t pool[8];
    static char mount[FS_AGING_PATH_MAX + 1u];
    fs_aging_config_t cfg;
    fs_aging_platform_t plat;
    fake_fs_t fs;
    fs_aging_status_t s;
    uint32_t len = FS_AGING_PATH_MAX - 23u;

    test_begin();
    memset(&fs, 0, sizeof(fs));
    plat.create_file = fake_create;
    plat.delete_file = fake_delete;
    plat.make_dir    = fake_mkdir;
    plat.now_ms      = NULL;
    plat.log         = NULL;
    plat.user        = &fs;

    memset(mount, 'm', sizeof(mount));
    mount[0] = '/';
    mount[len] = '\0';
    cfg = base_config(1ull << 20u, 1u);
    cfg.mount_point = mount;
    s = fs_aging_init(&ctx, &flat_profile, &cfg, &plat, pool, 8u);
    check(s == FS_AGING_RUNNING, "yol %u byte: %s", FS_AGING_PATH_MAX, st_name(s));

    mount[len] = 'm';
    mount[len + 1u] = '\0';
    s = fs_aging_init(&ctx, &flat_profile, &cfg, &plat, pool, 8u);
    check(s == FS_AGING_ERR_PATH_TOO_LONG, "yol %u byte: %s",
          FS_AGING_PATH_MAX + 1u, st_name(s));
    check(fs_aging_step(&ctx, 10u) == FS_AGING_ERR_PATH_TOO_LONG, "step durumu");

    /* Dev derinlik: init sinirli surede PATH_TOO_LONG donmeli (takilmamali) */
    cfg.mount_point = "/fs0";
    s = fs_aging_init(&ctx, &huge_depth_profile, &cfg, &plat, pool, 8u);
    check(s == FS_AGING_ERR_PATH_TOO_LONG, "derinlik 0xFFFFFFFF: %s", st_name(s));
    check(fs.calls == 0u, "platform cagrildi");
    printf("    FS_AGING_PATH_MAX=%u, mount %u (gecerli) / %u (uzun) karakter\n",
           FS_AGING_PATH_MAX, len, len + 1u);
    test_end("yol siniri tam FS_AGING_PATH_MAX'ta");
}

/* Hazir profiller /fs0 ile: FS_AGING_PATH_MAX=64 iken derin profiller
 * init'te FS_AGING_ERR_PATH_TOO_LONG donmeli, wang_* (derinlik 0) gecmeli. */
static void test_path_too_long_profiles(void)
{
    static fs_aging_ctx_t ctx;
    static fs_aging_file_t pool[8];
    fs_aging_config_t cfg;
    fs_aging_platform_t plat;
    fake_fs_t fs;
    uint32_t i;

    if ((FS_AGING_PATH_MAX) != 64u) {
        test_skip("derin profil + FS_AGING_PATH_MAX=64 -> FS_AGING_ERR_PATH_TOO_LONG",
                  "edge_tests_path64 ile kosar");
        return;
    }
    test_begin();
    memset(&fs, 0, sizeof(fs));
    plat.create_file = fake_create;
    plat.delete_file = fake_delete;
    plat.make_dir    = fake_mkdir;
    plat.now_ms      = NULL;
    plat.log         = NULL;
    plat.user        = &fs;
    cfg = base_config(16ull << 20u, 1u);

    for (i = 0u; i < FS_AGING_PROFILE_COUNT; i++) {
        const fs_aging_profile_t *p = fs_aging_profiles[i];
        int flat = (p == &fs_aging_profile_wang_lanl) ||
                   (p == &fs_aging_profile_wang_os);
        fs_aging_status_t want = (flat != 0) ? FS_AGING_RUNNING
                                             : FS_AGING_ERR_PATH_TOO_LONG;
        fs_aging_status_t s = fs_aging_init(&ctx, p, &cfg, &plat, pool, 8u);
        printf("    %-10s init: %s\n", p->name, st_name(s));
        check(s == want, "%s: %s, beklenen %s", p->name, st_name(s), st_name(want));
    }
    check(fs.calls == 0u, "init platformu cagirdi");
    test_end("derin profil + FS_AGING_PATH_MAX=64 -> FS_AGING_ERR_PATH_TOO_LONG");
}

/* Kisa yollu profil derlenen yol siniriyla sonuna kadar kosabilmeli */
static void test_flat_run(void)
{
    run_opts_t o;
    run_result_t r;

    test_begin();
    memset(&o, 0, sizeof(o));
    o.profile = &fs_aging_profile_wang_os;
    o.cfg     = base_config(32ull << 20u, 42u);
    o.max_ops = 1000u;
    run_aging(&o, &r);
    print_run("wang_os 32 MB", &r);
    check(is_done(r.status), "durum %s", st_name(r.status));
    check_common(&r);
    test_end("wang_os derlenen FS_AGING_PATH_MAX ile tamamlaniyor");
}

static void test_determinism(void)
{
    static run_result_t a;
    static run_result_t b;
    static run_result_t c;
    const fs_aging_profile_t *profs[2];
    uint64_t vols[2];
    run_opts_t o;
    uint32_t i;

    profs[0] = &fs_aging_profile_agrawal;
    vols[0]  = 64ull << 20u;
    profs[1] = &fs_aging_profile_wang_os;
    vols[1]  = 32ull << 20u;

    test_begin();
    for (i = 0u; i < 2u; i++) {
        memset(&o, 0, sizeof(o));
        o.profile = profs[i];
        o.max_ops = 1000u;
        o.cfg     = base_config(vols[i], 42u);
        run_aging(&o, &a);
        run_aging(&o, &b);
        o.cfg.seed = 43u;
        run_aging(&o, &c);
        printf("    %s:\n", profs[i]->name);
        print_run("seed 42 (1)", &a);
        print_run("seed 42 (2)", &b);
        print_run("seed 43    ", &c);
        check(is_done(a.status) && is_done(c.status), "%s: kosu bitmedi",
              profs[i]->name);
        check(consistent(&a) && consistent(&b) && consistent(&c),
              "%s: tutarsizlik", profs[i]->name);
        check(same_results(&a, &b) != 0,
              "%s: ayni seed farkli sonuc (istatistik/dagilim/cagri dizisi)",
              profs[i]->name);
        check(a.call_hash != c.call_hash, "%s: farkli seed ayni cagri dizisi",
              profs[i]->name);
        check((a.st.ops != c.st.ops) || (a.st.live_bytes != c.st.live_bytes) ||
              (a.st.age_error != c.st.age_error),
              "%s: farkli seed ayni istatistik", profs[i]->name);
    }
    test_end("determinizm: ayni seed ayni, farkli seed farkli");
}

/* Adim bagimsizligi senaryolari: tam yakinsama, tolerans ile bitis
 * (confidence > 0) ve erken NO_SPACE (olusturma yerine silme yolu). */
typedef struct {
    const char               *label;
    const fs_aging_profile_t *profile;
    uint64_t                  volume;
    fs_aging_float_t          confidence;
    uint32_t                  capacity_pct;   /* 0: volume, aksi: volume'un %'si */
    fs_aging_status_t         want;           /* RUNNING: herhangi bir DONE_*    */
    int                       want_no_space;
} step_case_t;

static void test_step_independence(void)
{
    static run_result_t ref;
    static run_result_t r;
    static const uint32_t ops[3] = { 1u, 7u, 1000u };
    static const step_case_t cases[4] = {
        { "agrawal 64 MB",               &fs_aging_profile_agrawal, 64ull << 20u,
          0.0, 0u,  FS_AGING_RUNNING,        0 },
        { "tiny 16 MB",                  &tiny_profile,             16ull << 20u,
          0.0, 0u,  FS_AGING_RUNNING,        0 },
        { "agrawal 64 MB confidence 0.9", &fs_aging_profile_agrawal, 64ull << 20u,
          0.9, 0u,  FS_AGING_DONE_TOLERANCE, 0 },
        { "agrawal 64 MB kapasite %81",  &fs_aging_profile_agrawal, 64ull << 20u,
          0.0, 81u, FS_AGING_RUNNING,        1 }
    };
    run_opts_t o;
    uint32_t i;
    uint32_t k;

    test_begin();
    for (i = 0u; i < 4u; i++) {
        const step_case_t *sc = &cases[i];
        printf("    %s:\n", sc->label);
        for (k = 0u; k < 3u; k++) {
            char label[32];
            memset(&o, 0, sizeof(o));
            o.profile        = sc->profile;
            o.cfg            = base_config(sc->volume, 42u);
            o.cfg.confidence = sc->confidence;
            o.fs_capacity    = (sc->capacity_pct != 0u)
                               ? (sc->volume / 100u) * sc->capacity_pct : 0u;
            o.max_ops        = ops[k];
            run_aging(&o, (k == 0u) ? &ref : &r);
            (void)sprintf(label, "max_ops %4u", ops[k]);
            print_run(label, (k == 0u) ? &ref : &r);
            if (k == 0u) {
                check(is_done(ref.status), "%s: kosu bitmedi", sc->label);
                check((sc->want == FS_AGING_RUNNING) || (ref.status == sc->want),
                      "%s: durum %s, beklenen %s", sc->label,
                      st_name(ref.status), st_name(sc->want));
                check((sc->want_no_space == 0) || (ref.no_space > 0u),
                      "%s: hic NO_SPACE donmedi", sc->label);
                check_common(&ref);
            } else {
                check_common(&r);
                check(same_results(&ref, &r) != 0,
                      "%s: max_ops %u sonucu max_ops 1'den farkli",
                      sc->label, ops[k]);
            }
        }
    }
    test_end("adim bagimsizligi: max_ops 1 / 7 / 1000 ayni sonuc");
}

static void test_rebin_bounded(void)
{
    run_opts_t o;
    run_result_t r;
    uint32_t chunks;

    test_begin();

    /* a) tiny: NO_SPACE yok, SETUP islemleri tek seviye. max_ops=1 ile
     *    her adim en fazla 1 platform cagrisi yapmali. */
    memset(&o, 0, sizeof(o));
    o.profile = &tiny_profile;
    o.cfg     = base_config(16ull << 20u, 42u);
    o.max_ops = 1u;
    o.watch   = 1;
    run_aging(&o, &r);
    print_run("tiny", &r);
    chunks = (r.rebin_pool + FS_AGING_REBIN_CHUNK - 1u) / FS_AGING_REBIN_CHUNK;
    printf("    yerlestirme: havuz %u kayit, %llu adim (beklenen %u),"
           " adimda en fazla %u kayit; adimda en fazla cagri: SETUP %llu,"
           " FILL/STABLE %llu\n",
           r.rebin_pool, (unsigned long long)r.rebin_steps, chunks,
           r.max_rebin_advance, (unsigned long long)r.max_calls_setup,
           (unsigned long long)r.max_calls_work);
    check(is_done(r.status), "durum %s", st_name(r.status));
    check_common(&r);
    check(r.rebin_pool > FS_AGING_REBIN_CHUNK, "havuz tek parcaya sigdi (%u)",
          r.rebin_pool);
    check(r.rebin_steps == (uint64_t)chunks, "yerlestirme adimi %llu, beklenen %u",
          (unsigned long long)r.rebin_steps, chunks);
    check(r.max_rebin_advance <= FS_AGING_REBIN_CHUNK, "bir adimda %u kayit",
          r.max_rebin_advance);
    check(r.step_violation == 0, "adim siniri ihlali");
    check((r.max_calls_setup <= 1u) && (r.max_calls_work <= 1u),
          "bir adimda 1'den fazla platform cagrisi");
    check(r.no_space == 0u, "beklenmeyen NO_SPACE");
    check(r.rebin_checked != 0, "STABLE'a gecilmedi, yerlestirme sayimi kontrol edilemedi");
    printf("    yerlestirme sonrasi sum(count_age)=%llu canli_dosya=%u\n",
           (unsigned long long)r.rebin_sum, r.rebin_live);

    /* b) agrawal (derin dizinler, buyuk dosyalar): bir SETUP islemi
     *    yaprak dizinin tum ust seviyelerini olusturur (en fazla derinlik
     *    kadar make_dir), FILL/STABLE islemi NO_SPACE donerse yerine silme
     *    yapar (en fazla 2 cagri). Yerlestirme adimlari cagri yapmaz. */
    memset(&o, 0, sizeof(o));
    o.profile = &fs_aging_profile_agrawal;
    o.cfg     = base_config(64ull << 20u, 42u);
    o.max_ops = 1u;
    o.watch   = 1;
    run_aging(&o, &r);
    print_run("agrawal", &r);
    chunks = (r.rebin_pool + FS_AGING_REBIN_CHUNK - 1u) / FS_AGING_REBIN_CHUNK;
    printf("    yerlestirme: havuz %u kayit, %llu adim (beklenen %u);"
           " adimda en fazla cagri: SETUP %llu (derinlik %u), FILL/STABLE %llu,"
           " NO_SPACE+silme adimi %llu\n",
           r.rebin_pool, (unsigned long long)r.rebin_steps, chunks,
           (unsigned long long)r.max_calls_setup, r.max_setup_depth,
           (unsigned long long)r.max_calls_work,
           (unsigned long long)r.two_call_steps);
    check(is_done(r.status), "durum %s", st_name(r.status));
    check_common(&r);
    check(r.rebin_steps == (uint64_t)chunks, "yerlestirme adimi %llu, beklenen %u",
          (unsigned long long)r.rebin_steps, chunks);
    check(r.step_violation == 0, "adim siniri ihlali");
    check(r.max_calls_setup <= (uint64_t)r.max_setup_depth, "SETUP adiminda %llu cagri",
          (unsigned long long)r.max_calls_setup);
    check(r.max_calls_work <= 2u, "FILL/STABLE adiminda %llu cagri",
          (unsigned long long)r.max_calls_work);
    check(r.rebin_checked != 0, "STABLE'a gecilmedi, yerlestirme sayimi kontrol edilemedi");
    printf("    yerlestirme sonrasi sum(count_age)=%llu canli_dosya=%u\n",
           (unsigned long long)r.rebin_sum, r.rebin_live);
    test_end("yas kovasi yerlestirme sinirli: max_ops=1 ile adim basina cagri");
}

/* STABLE'da canli dosya 0'a inerse yazi-tura silme dese de olusturma
 * yapilir. agrawal 16 MB seed 42 eskiden 2655. islemde ERR_PLATFORM ile
 * duruyordu (volume bos, callback hatasi yok). */
static void test_stable_empty(void)
{
    run_opts_t o;
    run_result_t r;

    test_begin();
    memset(&o, 0, sizeof(o));
    o.profile = &fs_aging_profile_agrawal;
    o.cfg     = base_config(16ull << 20u, 42u);
    o.max_ops = 1u;
    run_aging(&o, &r);
    print_run("agrawal 16 MB", &r);
    printf("    STABLE'da canli dosya 0 olan adim: %llu\n",
           (unsigned long long)r.stable_empty_steps);
    check(is_done(r.status), "durum %s", st_name(r.status));
    check(r.stable_empty_steps > 0u, "STABLE'da canli dosya hic 0'a inmedi");
    check(r.st.ops > 2655u, "islem %llu", (unsigned long long)r.st.ops);
    check_common(&r);
    test_end("STABLE'da canli dosya yok -> silme yerine olusturma");
}

/* FILL'de bos volume'de NO_SPACE: silinecek dosya yok, islem tuketilir ve
 * sonraki islem baska boyut ceker. 64 MB'lik boyut 16 MB'a hic sigmaz;
 * en az bir seed'de ilk olusturma(lar) bos volume'de NO_SPACE almali. */
static void test_fill_empty_no_space(void)
{
    run_opts_t o;
    run_result_t r;
    uint32_t seed;
    uint32_t hit = 0u;
    uint32_t done = 0u;

    test_begin();
    for (seed = 1u; seed <= 20u; seed++) {
        memset(&o, 0, sizeof(o));
        o.profile = &bigmix_profile;
        o.cfg     = base_config(16ull << 20u, seed);
        o.max_ops = 1000u;
        run_aging(&o, &r);
        if (r.no_space_empty > 0u) {
            hit++;
        }
        if (is_done(r.status)) {
            done++;
        }
        check(is_done(r.status), "seed %u: durum %s", seed, st_name(r.status));
        check_common(&r);
    }
    printf("    bigmix 16 MB seed 1-20: %u kosu DONE_*, %u kosuda bos volume'de"
           " NO_SPACE\n", done, hit);
    check(hit > 0u, "hicbir seed'de bos volume'de NO_SPACE olusmadi");
    test_end("FILL'de bos volume'de NO_SPACE -> silmeden devam");
}

/* Gercek cikmaz: hicbir boyut olusturulamiyor ve silinecek dosya yok.
 * Calisma takilmadan FS_AGING_ERR_PLATFORM ile bitmeli. */
static void test_dead_end(void)
{
    run_opts_t o;
    run_result_t r;
    uint32_t est;

    test_begin();

    /* a) Her boyut volume'den buyuk: FILL'in ilk isleminde durur */
    est = fs_aging_estimate_files(&nofit_profile, 16ull << 20u, 0.8);
    check(est == 0u, "nofit tahmini %u, beklenen 0", est);
    memset(&o, 0, sizeof(o));
    o.profile  = &nofit_profile;
    o.cfg      = base_config(16ull << 20u, 42u);
    o.max_ops  = 1000u;
    o.pool_cap = 16u;
    run_aging(&o, &r);
    print_run("nofit 16 MB", &r);
    check(r.status == FS_AGING_ERR_PLATFORM, "durum %s", st_name(r.status));
    check((r.st.ops == 1u) && (r.n_create == 1u) && (r.n_delete == 0u),
          "islem %llu create %llu delete %llu", (unsigned long long)r.st.ops,
          (unsigned long long)r.n_create, (unsigned long long)r.n_delete);
    check(strcmp(r.err_msg, MSG_FILL_DEAD) == 0, "log '%s'", r.err_msg);
    check_common(&r);

    /* b) Platform bos volume'de bile NO_SPACE donuyor (kapasite 1 byte):
     *    once 64 MB (volume'den buyuk) cekilirse islem tuketilip devam
     *    edilir, en kucuk boyut (4 KB) NO_SPACE alinca durulur. En az bir
     *    seed'de 1'den fazla islem olmali (devam yolu da sinanir). */
    {
        uint32_t seed;
        uint64_t max_ops_seen = 0u;
        for (seed = 1u; seed <= 20u; seed++) {
            memset(&o, 0, sizeof(o));
            o.profile     = &bigmix_profile;
            o.cfg         = base_config(16ull << 20u, seed);
            o.max_ops     = 1000u;
            o.fs_capacity = 1u;
            run_aging(&o, &r);
            if (r.st.ops > max_ops_seen) {
                max_ops_seen = r.st.ops;
            }
            check(r.status == FS_AGING_ERR_PLATFORM, "seed %u: durum %s", seed,
                  st_name(r.status));
            check((r.n_delete == 0u) && (r.st.creates == 0u),
                  "seed %u: silme %llu olusturma %llu", seed,
                  (unsigned long long)r.n_delete, (unsigned long long)r.st.creates);
            check(r.no_space_empty == r.st.ops, "seed %u: NO_SPACE %llu, islem %llu",
                  seed, (unsigned long long)r.no_space_empty,
                  (unsigned long long)r.st.ops);
            check(strcmp(r.err_msg, MSG_FILL_DEAD) == 0, "seed %u: log '%s'", seed,
                  r.err_msg);
            check_common(&r);
        }
        printf("    bigmix, kapasite 1 byte, seed 1-20: hepsi ERR_PLATFORM, en uzun"
               " %llu islem\n", (unsigned long long)max_ops_seen);
        check(max_ops_seen > 1u, "hicbir seed'de once 64 MB cekilmedi");
    }

    /* c) STABLE cikmazi: tek boyut = volume. FILL tek dosyayla biter,
     *    STABLE'da dosya silinince sigan boyut kalmaz. */
    est = fs_aging_estimate_files(&whole_profile, 16ull << 20u, 0.8);
    memset(&o, 0, sizeof(o));
    o.profile = &whole_profile;
    o.cfg     = base_config(16ull << 20u, 42u);
    o.max_ops = 1u;
    run_aging(&o, &r);
    print_run("whole 16 MB", &r);
    printf("    whole tahmini %u kayit\n", est);
    check(est >= 1u, "whole tahmini %u", est);
    check(r.status == FS_AGING_ERR_PLATFORM, "durum %s", st_name(r.status));
    check(r.rebin_checked != 0, "STABLE'a gecilmedi");
    check((r.st.creates == 1u) && (r.st.deletes == 1u) && (r.st.live_files == 0u),
          "olusturma %llu silme %llu canli %u", (unsigned long long)r.st.creates,
          (unsigned long long)r.st.deletes, r.st.live_files);
    check(r.st.ops <= 3u, "islem %llu", (unsigned long long)r.st.ops);
    check(r.n_delete == 1u, "delete cagrisi %llu", (unsigned long long)r.n_delete);
    check(strcmp(r.err_msg, MSG_STABLE_NOFIT) == 0, "log '%s'", r.err_msg);
    check_common(&r);

    /* d) STABLE'da canli dosya yok ve olusturma NO_SPACE aldi: half
     *    (8 MB, fill 0.5) FILL'de tek dosya olusturur, STABLE'da o silinir,
     *    platform ikinci create'ten itibaren NO_SPACE doner. */
    memset(&o, 0, sizeof(o));
    o.profile      = &half_profile;
    o.cfg          = base_config(16ull << 20u, 42u);
    o.cfg.fill_ratio = 0.5;
    o.max_ops      = 1u;
    o.nospace_from = 2u;
    run_aging(&o, &r);
    print_run("half 16 MB", &r);
    check(r.status == FS_AGING_ERR_PLATFORM, "durum %s", st_name(r.status));
    check(r.rebin_checked != 0, "STABLE'a gecilmedi");
    check((r.st.creates == 1u) && (r.st.deletes == 1u) && (r.n_create == 2u) &&
          (r.n_delete == 1u) && (r.st.live_files == 0u),
          "olusturma %llu/%llu silme %llu/%llu canli %u",
          (unsigned long long)r.st.creates, (unsigned long long)r.n_create,
          (unsigned long long)r.st.deletes, (unsigned long long)r.n_delete,
          r.st.live_files);
    check(r.st.ops == 3u, "islem %llu", (unsigned long long)r.st.ops);
    check(strcmp(r.err_msg, MSG_STABLE_NOSPACE) == 0, "log '%s'", r.err_msg);
    check_common(&r);

    /* e) Bos volume'de volume'e sigan boyut NO_SPACE aldi (gercek kapasite
     *    volume'den kucuk): ayni islemde en kucuk boyut denenir, karar
     *    ornekleme beklemeden verilir. Kapasite 1 byte: ilk islemde cikmaz.
     *    Kapasite 6000 byte: 4 KB sigar, calisma yuk siniriyla biter. */
    {
        uint32_t seed;
        for (seed = 1u; seed <= 5u; seed++) {
            memset(&o, 0, sizeof(o));
            o.profile     = &rare_profile;
            o.cfg         = base_config(16ull << 20u, seed);
            o.max_ops     = 1u;
            o.watch       = 1;
            o.pool_cap    = 16u;
            o.fs_capacity = 1u;
            run_aging(&o, &r);
            check(r.status == FS_AGING_ERR_PLATFORM, "rare seed %u: durum %s", seed,
                  st_name(r.status));
            check((r.st.ops == 1u) && (r.n_create == 2u) && (r.n_delete == 0u),
                  "rare seed %u: islem %llu create %llu delete %llu", seed,
                  (unsigned long long)r.st.ops, (unsigned long long)r.n_create,
                  (unsigned long long)r.n_delete);
            check(strcmp(r.err_msg, MSG_FILL_DEAD) == 0, "rare seed %u: log '%s'",
                  seed, r.err_msg);
            check(r.step_violation == 0, "rare seed %u: adim siniri ihlali", seed);
            check_common(&r);
        }
        memset(&o, 0, sizeof(o));
        o.profile            = &rare_profile;
        o.cfg                = base_config(16ull << 20u, 1u);
        o.cfg.overwrite_runs = 1u;
        o.max_ops            = 1u;
        o.watch              = 1;
        o.pool_cap           = 16u;
        o.fs_capacity        = 6000u;
        run_aging(&o, &r);
        print_run("rare, kapasite 6000", &r);
        check(r.status == FS_AGING_DONE_WORKLOAD_LIMIT, "rare 6000: durum %s",
              st_name(r.status));
        check((r.no_space_empty > 0u) && (r.st.creates > 0u),
              "rare 6000: bos NO_SPACE %llu olusturma %llu",
              (unsigned long long)r.no_space_empty, (unsigned long long)r.st.creates);
        check(r.step_violation == 0, "rare 6000: adim siniri ihlali");
        check(r.max_calls_work <= 2u, "rare 6000: adimda %llu cagri",
              (unsigned long long)r.max_calls_work);
        check_common(&r);

        /* Adim bagimsizligi bu yolda da: max_ops 1000 ayni sonucu vermeli */
        {
            static run_result_t r2;
            o.max_ops = 1000u;
            o.watch   = 0;
            run_aging(&o, &r2);
            check(same_results(&r, &r2) != 0, "rare 6000: max_ops 1000 sonucu farkli");
            check_common(&r2);
        }
    }
    test_end("gercek cikmaz -> takilmadan FS_AGING_ERR_PLATFORM");
}

/* Havuz = fs_aging_estimate_files() (ek pay yok) ile yalnizca FILL kosulur:
 * 8 profil x 30 seed x 3 volume, hicbirinde FS_AGING_ERR_POOL_FULL olmamali.
 * Ayrica STABLE'da havuzun dolmasi zararsiz olmali (olusturma yerine silme). */
static void test_estimate_pool(void)
{
    static const uint64_t vols[3] = { 16ull << 20u, 64ull << 20u, 256ull << 20u };
    run_opts_t o;
    run_result_t r;
    uint32_t p;
    uint32_t v;
    uint32_t seed;

    test_begin();
    for (p = 0u; p < FS_AGING_PROFILE_COUNT; p++) {
        fs_aging_float_t worst = 0.0;
        uint32_t pool_full = 0u;
        for (v = 0u; v < 3u; v++) {
            for (seed = 1u; seed <= 30u; seed++) {
                memset(&o, 0, sizeof(o));
                o.profile        = fs_aging_profiles[p];
                o.cfg            = base_config(vols[v], seed);
                o.max_ops        = 1u;      /* sahte fs tepesi = FILL tepesi */
                o.stop_at_stable = 1;
                run_aging(&o, &r);
                if (r.status == FS_AGING_ERR_POOL_FULL) {
                    pool_full++;
                }
                if (((fs_aging_float_t)r.max_files / (fs_aging_float_t)r.pool_cap) > worst) {
                    worst = (fs_aging_float_t)r.max_files / (fs_aging_float_t)r.pool_cap;
                }
                check((r.status == FS_AGING_RUNNING) &&
                      (r.st.phase == FS_AGING_PHASE_STABLE),
                      "%s %llu MB seed %u: durum %s", o.profile->name,
                      (unsigned long long)(vols[v] >> 20u), seed, st_name(r.status));
                check_common(&r);
            }
        }
        printf("    %-10s 16/64/256 MB x seed 1-30: POOL_FULL %u, en yuksek"
               " FILL tepesi / tahmin %.3f\n", fs_aging_profiles[p]->name,
               pool_full, worst);
    }

    /* Dogrulama izgarasi disinda onceki taslaklarla POOL_FULL veren kosular ve
     * yalnizca volume bosken sigan boyutlu (volume - hedef < boyut <=
     * volume) sentetik profiller. */
    {
        typedef struct {
            const fs_aging_profile_t *pr;
            uint64_t vol;
            fs_aging_float_t fill;
            uint32_t seed_lo;
            uint32_t seed_hi;
        } est_case_t;
        static const est_case_t cases[6] = {
            { &fs_aging_profile_pramod,  16ull << 20u, 0.95, 849u, 849u },
            { &fs_aging_profile_wang_os,  4ull << 20u, 0.8,  152u, 152u },
            { &fs_aging_profile_wang_os,  8ull << 20u, 0.95, 1892u, 1892u },
            { &fs_aging_profile_wang_os, 16ull << 20u, 0.3,  1892u, 1892u },
            { &tail1_profile,            16ull << 20u, 0.8,  1u,   20u },
            { &tail2_profile,            16ull << 20u, 0.8,  1u,   20u }
        };
        uint32_t k;
        for (k = 0u; k < 6u; k++) {
            uint32_t pool_full = 0u;
            uint32_t peak = 0u;
            for (seed = cases[k].seed_lo; seed <= cases[k].seed_hi; seed++) {
                memset(&o, 0, sizeof(o));
                o.profile         = cases[k].pr;
                o.cfg             = base_config(cases[k].vol, seed);
                o.cfg.fill_ratio  = cases[k].fill;
                o.max_ops         = 1u;
                o.stop_at_stable  = 1;
                run_aging(&o, &r);
                if (r.status == FS_AGING_ERR_POOL_FULL) {
                    pool_full++;
                }
                if (r.max_files > peak) {
                    peak = r.max_files;
                }
                check((r.status == FS_AGING_RUNNING) &&
                      (r.st.phase == FS_AGING_PHASE_STABLE),
                      "%s %llu MB fill %.2f seed %u: durum %s", o.profile->name,
                      (unsigned long long)(cases[k].vol >> 20u), cases[k].fill, seed,
                      st_name(r.status));
                check_common(&r);
            }
            printf("    %-10s %llu MB fill %.2f seed %u-%u: tahmin %u, en yuksek FILL"
                   " tepesi %u, POOL_FULL %u\n", cases[k].pr->name,
                   (unsigned long long)(cases[k].vol >> 20u), cases[k].fill,
                   cases[k].seed_lo, cases[k].seed_hi, r.pool_cap, peak, pool_full);
        }
        /* Guvenli sinir yok: 0 byte'lik boyutu olan meyer, fill 1.0 */
        check(fs_aging_estimate_files(&fs_aging_profile_meyer, 16ull << 20u, 1.0) ==
              0xFFFFFFFFu, "meyer fill 1.0 tahmini 0xFFFFFFFF degil");
    }

    /* STABLE'da havuz dolu: tiny 16 MB'ta FILL tam 3277 dosya ister; havuz
     * tam bu kadar verilince STABLE'daki her olusturma denemesi silmeye
     * doner ve calisma hatasiz biter. */
    memset(&o, 0, sizeof(o));
    o.profile  = &tiny_profile;
    o.cfg      = base_config(16ull << 20u, 42u);
    o.max_ops  = 1u;
    o.pool_cap = 3277u;
    run_aging(&o, &r);
    print_run("tiny 16 MB havuz 3277", &r);
    printf("    STABLE'da havuz dolu adim: %llu, tiny tahmini %u\n",
           (unsigned long long)r.stable_pool_full,
           fs_aging_estimate_files(&tiny_profile, 16ull << 20u, 0.8));
    check(is_done(r.status), "durum %s", st_name(r.status));
    check(r.stable_pool_full > 0u, "STABLE'da havuz hic dolmadi");
    check(r.max_files <= 3277u, "havuzdan fazla dosya: %u", r.max_files);
    check(fs_aging_estimate_files(&tiny_profile, 16ull << 20u, 0.8) >= 3277u,
          "tiny tahmini FILL ihtiyacindan kucuk");
    check_common(&r);
    test_end("havuz = fs_aging_estimate_files(): FILL'de POOL_FULL yok");
}

/* ---------------- main ---------------------------------------------- */

static void run_or_skip(void (*fn)(void), const char *name)
{
    if (g_full_suite != 0) {
        fn();
    } else {
        test_skip(name, "FS_AGING_PATH_MAX != 256");
    }
}

int main(void)
{
    printf("edge_tests: FS_AGING_PATH_MAX=%u FS_AGING_REBIN_CHUNK=%u\n",
           FS_AGING_PATH_MAX, FS_AGING_REBIN_CHUNK);

    run_or_skip(test_invalid_params,    "gecersiz parametreler");
    run_or_skip(test_pool_full,         "havuz cok kucuk");
    run_or_skip(test_no_space_early,    "erken NO_SPACE");
    run_or_skip(test_error_create,      "create_file ERROR");
    run_or_skip(test_error_delete,      "delete_file ERROR");
    run_or_skip(test_error_mkdir,       "make_dir ERROR");
    test_path_boundary();
    test_path_too_long_profiles();
    test_flat_run();
    run_or_skip(test_determinism,       "determinizm");
    run_or_skip(test_step_independence, "adim bagimsizligi");
    run_or_skip(test_rebin_bounded,     "yas kovasi yerlestirme sinirli");
    run_or_skip(test_stable_empty,      "STABLE'da canli dosya yok");
    run_or_skip(test_fill_empty_no_space, "FILL'de bos volume'de NO_SPACE");
    run_or_skip(test_dead_end,          "gercek cikmaz");
    run_or_skip(test_estimate_pool,     "havuz = tahmin");

    printf("EDGE SUMMARY: passed=%d failed=%d skipped=%d\n",
           g_passed_total, g_failed_total, g_skipped_total);
    return (g_failed_total == 0) ? 0 : 1;
}
