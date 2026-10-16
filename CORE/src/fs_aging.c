/*
 * fs_aging.c - File system aging core
 *
 * Geriatrix'in (USENIX ATC 2018) islevsel karsiligi. Orijinal algoritmadan
 * bilincli olarak ayrilan noktalar:
 *
 *  - Rastgele sayilar icin dahili xorshift64* kullanilir; ayni seed her
 *    platformda ayni islem dizisini uretir (orijinalle bire bir eslesmez).
 *  - Silinecek dosya, orijinaldeki gibi en cok fazla temsil edilen yas,
 *    sonra boyut, sonra dizin kovasindan secilir; ancak tum kovalar
 *    taranmak yerine FS_AGING_DELETE_SAMPLES adet rastgele aday arasindan
 *    en iyisi alinir. Bu, islem basina sabit maliyet ve dosya basina
 *    24 byte bellek saglar.
 *  - confidence parametresi, yas dagiliminin toplam varyasyon mesafesine
 *    (0.5 * sum|gercek - hedef|) gore yorumlanir: 0.9 icin hata <= 0.1
 *    oldugunda durulur. Boylece ki-kare CDF'si ve libm gerekmez.
 *
 * Bu dosya libc kullanmaz.
 */

#include "fs_aging.h"

#define NONE_U32       0xFFFFFFFFu
#define FREE_DIR_BIN   0xFFFFu
#define MAX_SIBLINGS   0xFFFFu

/* ================================================================== */
/* Yardimcilar                                                         */
/* ================================================================== */

static fs_aging_float_t f_abs(fs_aging_float_t x)
{
    return (x < 0.0) ? -x : x;
}

static uint32_t str_len(const char *s)
{
    uint32_t n = 0u;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

static uint32_t u64_digits(uint64_t v)
{
    uint32_t n = 1u;
    while (v >= 10u) {
        v /= 10u;
        n++;
    }
    return n;
}

static void log_msg(const fs_aging_ctx_t *c, const char *msg)
{
    if (c->platform.log != NULL) {
        c->platform.log(c->platform.user, msg);
    }
}

/* ---------------- yol olusturma (snprintf yerine) ---------------- */

typedef struct {
    char    *buf;
    uint32_t len;
    uint32_t cap;
    int      overflow;
} path_t;

static void p_init(path_t *p, char *buf, uint32_t cap)
{
    p->buf = buf;
    p->len = 0u;
    p->cap = cap;
    p->overflow = 0;
    buf[0] = '\0';
}

static void p_char(path_t *p, char ch)
{
    if ((p->len + 1u) < p->cap) {
        p->buf[p->len] = ch;
        p->len++;
        p->buf[p->len] = '\0';
    } else {
        p->overflow = 1;
    }
}

static void p_str(path_t *p, const char *s)
{
    while (*s != '\0') {
        p_char(p, *s);
        s++;
    }
}

static void p_u64(path_t *p, uint64_t v)
{
    char     tmp[20];
    uint32_t n = 0u;
    do {
        tmp[n] = (char)('0' + (int)(v % 10u));
        n++;
        v /= 10u;
    } while (v != 0u);
    while (n > 0u) {
        n--;
        p_char(p, tmp[n]);
    }
}

/* Dizin yolu: <mount>/d1/d2/.../d(k-1)/<yaprak>
 * Yaprak, kardes dizin varsa d<sibling>, yoksa d<k>'dir.
 * levels < k ise sadece ilk 'levels' seviye yazilir (mkdir -p icin). */
static void build_dir_path(const fs_aging_ctx_t *c, path_t *p,
                           uint32_t dir_bin, uint32_t sibling, uint32_t levels)
{
    uint32_t depth = c->profile->dirs[dir_bin].depth;
    uint32_t i;

    p_str(p, c->config.mount_point);
    for (i = 1u; (i <= levels) && (i <= depth); i++) {
        uint64_t name = (i < depth) ? i : ((sibling != 0u) ? sibling : depth);
        p_char(p, '/');
        p_char(p, 'd');
        p_u64(p, name);
    }
}

static void build_file_path(fs_aging_ctx_t *c, path_t *p,
                            const fs_aging_file_t *f)
{
    p_init(p, c->path_buf, FS_AGING_PATH_MAX);
    build_dir_path(c, p, f->dir_bin, f->sibling,
                   c->profile->dirs[f->dir_bin].depth);
    p_char(p, '/');
    p_char(p, 'f');
    p_u64(p, f->birth_tick);
}

/* ---------------- PRNG: splitmix64 tohum + xorshift64* ------------- */

static void rng_seed(fs_aging_ctx_t *c, uint32_t seed)
{
    uint64_t z = (uint64_t)seed + 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    c->rng_state = (z != 0u) ? z : 0x2545F4914F6CDD1DULL;
}

static uint64_t rng_next(fs_aging_ctx_t *c)
{
    uint64_t x = c->rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    c->rng_state = x;
    return x * 0x2545F4914F6CDD1DULL;
}

/* [0, n) araliginda sapmasiz tamsayi, n > 0 */
static uint64_t rng_below(fs_aging_ctx_t *c, uint64_t n)
{
    uint64_t limit = UINT64_MAX - (UINT64_MAX % n);
    uint64_t r;
    do {
        r = rng_next(c);
    } while (r >= limit);
    return r % n;
}

/* [0, 1) araliginda kayan noktali sayi */
static fs_aging_float_t rng_unit(fs_aging_ctx_t *c)
{
    return (fs_aging_float_t)(rng_next(c) >> 11) * (1.0 / 9007199254740992.0);
}

/* ================================================================== */
/* Dagilim yardimcilari                                                */
/* ================================================================== */

static fs_aging_float_t fraction(const fs_aging_ctx_t *c, uint32_t count)
{
    if (c->live_files == 0u) {
        return 0.0;
    }
    return (fs_aging_float_t)count / (fs_aging_float_t)c->live_files;
}

static uint32_t age_bin_of(const fs_aging_ctx_t *c, uint64_t birth_tick)
{
    uint32_t i;
    uint32_t n = c->profile->age_count;
    for (i = 0u; i < n; i++) {
        if (birth_tick >= c->age_cutoff_tick[i]) {
            return i;
        }
    }
    return n - 1u;
}

/* Yas dagilimi hatasi: toplam varyasyon mesafesi, 0 (mukemmel) - 1 */
static fs_aging_float_t age_error(const fs_aging_ctx_t *c)
{
    fs_aging_float_t sum = 0.0;
    uint32_t i;
    if (c->live_files == 0u) {
        return 1.0;
    }
    for (i = 0u; i < c->profile->age_count; i++) {
        sum += f_abs(fraction(c, c->count_age[i]) - c->ideal_age[i]);
    }
    return 0.5 * sum;
}

/* En az temsil edilen dizin derinligi */
static uint32_t pick_dir_bin(const fs_aging_ctx_t *c)
{
    uint32_t i;
    uint32_t best = 0u;
    fs_aging_float_t best_gap = 0.0;
    for (i = 0u; i < c->profile->dir_count; i++) {
        fs_aging_float_t gap = c->ideal_dir[i] - fraction(c, c->count_dir[i]);
        if ((i == 0u) || (gap > best_gap)) {
            best = i;
            best_gap = gap;
        }
    }
    return best;
}

/* FILL: boyut profil dagilimindan rastgele cekilir */
static uint32_t pick_size_sampled(fs_aging_ctx_t *c)
{
    fs_aging_float_t u = rng_unit(c);
    fs_aging_float_t cum = 0.0;
    uint32_t i;
    uint32_t n = c->profile->size_count;
    for (i = 0u; i < n; i++) {
        cum += c->ideal_size[i];
        if (u < cum) {
            return i;
        }
    }
    return n - 1u;
}

/* STABLE: volume'e sigan, en az temsil edilen boyut. Yoksa NONE_U32. */
static uint32_t pick_size_stable(const fs_aging_ctx_t *c)
{
    uint32_t i;
    uint32_t best = NONE_U32;
    fs_aging_float_t best_gap = 0.0;
    for (i = 0u; i < c->profile->size_count; i++) {
        uint64_t size = c->profile->sizes[i].size_bytes;
        fs_aging_float_t gap;
        if ((c->live_bytes + size) >= c->config.volume_bytes) {
            continue;
        }
        gap = c->ideal_size[i] - fraction(c, c->count_size[i]);
        if ((best == NONE_U32) || (gap > best_gap)) {
            best = i;
            best_gap = gap;
        }
    }
    return best;
}

/* a kaydini silmek b'yi silmekten daha mi iyi? Once yas, sonra boyut,
 * sonra dizin kovasinin fazlaligina bakilir (orijinaldeki oncelik). */
static int is_better_victim(const fs_aging_ctx_t *c, uint32_t a, uint32_t b)
{
    const fs_aging_file_t *fa = &c->pool[a];
    const fs_aging_file_t *fb = &c->pool[b];
    fs_aging_float_t ea;
    fs_aging_float_t eb;

    if (c->phase == FS_AGING_PHASE_STABLE) {
        uint32_t ba = age_bin_of(c, fa->birth_tick);
        uint32_t bb = age_bin_of(c, fb->birth_tick);
        ea = fraction(c, c->count_age[ba]) - c->ideal_age[ba];
        eb = fraction(c, c->count_age[bb]) - c->ideal_age[bb];
        if (ea != eb) {
            return ea > eb;
        }
    }

    ea = fraction(c, c->count_size[fa->size_bin]) - c->ideal_size[fa->size_bin];
    eb = fraction(c, c->count_size[fb->size_bin]) - c->ideal_size[fb->size_bin];
    if (ea != eb) {
        return ea > eb;
    }

    ea = fraction(c, c->count_dir[fa->dir_bin]) - c->ideal_dir[fa->dir_bin];
    eb = fraction(c, c->count_dir[fb->dir_bin]) - c->ideal_dir[fb->dir_bin];
    return ea > eb;
}

static uint32_t pick_victim(fs_aging_ctx_t *c)
{
    uint32_t n;
    uint32_t best = NONE_U32;

    if (c->live_files == 0u) {
        return NONE_U32;
    }

    for (n = 0u; n < FS_AGING_DELETE_SAMPLES; n++) {
        uint32_t idx = (uint32_t)rng_below(c, c->pool_high_water);
        if (c->pool[idx].dir_bin == FREE_DIR_BIN) {
            continue;
        }
        if ((best == NONE_U32) || is_better_victim(c, idx, best)) {
            best = idx;
        }
    }

    if (best == NONE_U32) {
        /* Havuz cok seyrek: rastgele bir noktadan ilk canli kaydi bul */
        uint32_t start = (uint32_t)rng_below(c, c->pool_high_water);
        for (n = 0u; n < c->pool_high_water; n++) {
            uint32_t idx = (start + n) % c->pool_high_water;
            if (c->pool[idx].dir_bin != FREE_DIR_BIN) {
                best = idx;
                break;
            }
        }
    }
    return best;
}

/* ================================================================== */
/* Dosya islemleri                                                     */
/* ================================================================== */

typedef enum {
    CR_OK = 0,
    CR_NO_SPACE,
    CR_POOL_FULL,
    CR_FAIL          /* ctx->status hata koduna cekildi */
} create_result_t;

static void fail(fs_aging_ctx_t *c, fs_aging_status_t st, const char *msg)
{
    c->status = st;
    log_msg(c, msg);
}

static create_result_t do_create(fs_aging_ctx_t *c, uint32_t size_bin)
{
    fs_aging_file_t rec;
    fs_aging_plat_result_t r;
    path_t   p;
    uint32_t idx;
    uint64_t size = c->profile->sizes[size_bin].size_bytes;
    uint32_t sib_count;

    if (c->live_files >= c->pool_capacity) {
        return CR_POOL_FULL;
    }

    rec.birth_tick = c->tick;
    rec.next_free  = NONE_U32;
    rec.size_bin   = (uint16_t)size_bin;
    rec.dir_bin    = (uint16_t)pick_dir_bin(c);
    rec.reserved   = 0u;
    sib_count      = c->profile->dirs[rec.dir_bin].sibling_dirs;
    rec.sibling    = (sib_count != 0u)
                     ? (uint16_t)(1u + (uint32_t)rng_below(c, sib_count))
                     : (uint16_t)0u;

    build_file_path(c, &p, &rec);
    if (p.overflow != 0) {
        fail(c, FS_AGING_ERR_PATH_TOO_LONG, "fs_aging: dosya yolu cok uzun");
        return CR_FAIL;
    }

    r = c->platform.create_file(c->platform.user, p.buf, size);
    if (r == FS_AGING_PLAT_NO_SPACE) {
        return CR_NO_SPACE;
    }
    if (r != FS_AGING_PLAT_OK) {
        fail(c, FS_AGING_ERR_PLATFORM, "fs_aging: create_file basarisiz");
        return CR_FAIL;
    }

    if (c->free_head != NONE_U32) {
        idx = c->free_head;
        c->free_head = c->pool[idx].next_free;
    } else {
        idx = c->pool_high_water;
        c->pool_high_water++;
    }
    c->pool[idx] = rec;

    c->count_size[rec.size_bin]++;
    c->count_dir[rec.dir_bin]++;
    if (c->phase == FS_AGING_PHASE_STABLE) {
        c->count_age[age_bin_of(c, rec.birth_tick)]++;
    }
    c->live_files++;
    c->live_bytes     += size;
    c->workload_bytes += size;
    c->creates++;
    return CR_OK;
}

static void do_delete(fs_aging_ctx_t *c)
{
    fs_aging_file_t *f;
    fs_aging_plat_result_t r;
    path_t   p;
    uint32_t idx = pick_victim(c);

    if (idx == NONE_U32) {
        fail(c, FS_AGING_ERR_PLATFORM,
             "fs_aging: volume dolu ve silinecek dosya yok");
        return;
    }
    f = &c->pool[idx];

    build_file_path(c, &p, f);
    r = c->platform.delete_file(c->platform.user, p.buf);
    if (r != FS_AGING_PLAT_OK) {
        fail(c, FS_AGING_ERR_PLATFORM, "fs_aging: delete_file basarisiz");
        return;
    }

    c->count_size[f->size_bin]--;
    c->count_dir[f->dir_bin]--;
    if (c->phase == FS_AGING_PHASE_STABLE) {
        c->count_age[age_bin_of(c, f->birth_tick)]--;
    }
    c->live_files--;
    c->live_bytes -= c->profile->sizes[f->size_bin].size_bytes;
    c->deletes++;

    f->dir_bin   = FREE_DIR_BIN;
    f->next_free = c->free_head;
    c->free_head = idx;
}

/* ================================================================== */
/* Asamalar                                                            */
/* ================================================================== */

/* SETUP: bir islemde bir yaprak dizin (ve gerekiyorsa ustleri) olusur */
static void setup_one(fs_aging_ctx_t *c)
{
    const fs_aging_profile_t *pr = c->profile;

    while (c->setup_dir_bin < pr->dir_count) {
        const fs_aging_dir_bin_t *d = &pr->dirs[c->setup_dir_bin];
        uint32_t lvl;

        if (d->depth == 0u) {
            c->setup_dir_bin++;
            continue;
        }
        if ((d->sibling_dirs != 0u) && (c->setup_sibling == 0u)) {
            c->setup_sibling = 1u;
        }

        for (lvl = 1u; lvl <= d->depth; lvl++) {
            path_t p;
            p_init(&p, c->path_buf, FS_AGING_PATH_MAX);
            build_dir_path(c, &p, c->setup_dir_bin, c->setup_sibling, lvl);
            if (p.overflow != 0) {
                fail(c, FS_AGING_ERR_PATH_TOO_LONG, "fs_aging: dizin yolu cok uzun");
                return;
            }
            if (c->platform.make_dir(c->platform.user, p.buf) != FS_AGING_PLAT_OK) {
                fail(c, FS_AGING_ERR_PLATFORM, "fs_aging: make_dir basarisiz");
                return;
            }
        }

        if ((d->sibling_dirs == 0u) || (c->setup_sibling >= d->sibling_dirs)) {
            c->setup_dir_bin++;
            c->setup_sibling = 0u;
        } else {
            c->setup_sibling++;
        }
        return;
    }

    c->phase = FS_AGING_PHASE_FILL;
    log_msg(c, "fs_aging: dizin agaci hazir, doldurma basladi");
}

/* Orijinaldeki calculateT(): yas dagiliminin tam oturmasi icin gereken
 * toplam islem sayisi. K = doldurma asamasindaki islem sayisi. */
static uint64_t calc_convergence_tick(const fs_aging_ctx_t *c)
{
    uint32_t n = c->profile->age_count;
    fs_aging_float_t K = (fs_aging_float_t)c->fill_ops;
    fs_aging_float_t T = 0.0;
    fs_aging_float_t s;
    fs_aging_float_t t;
    uint32_t i;

    for (i = 0u; i < (n - 1u); i++) {
        s = (i == 0u) ? (1.0 - c->age_ratio[0])
                      : (c->age_ratio[i - 1u] - c->age_ratio[i]);
        t = 2.0 * K * (c->ideal_age[i] / s);
        if (t > T) {
            T = t;
        }
    }

    s = c->age_ratio[n - 2u] - c->age_ratio[n - 1u];
    t = (2.0 * K * (c->ideal_age[n - 1u] - 1.0) + K) / s;
    if (t > T) {
        T = t;
    }
    if ((s * T) <= K) {
        T = K / s;
    }
    return (T > 0.0) ? (uint64_t)T : 0u;
}

static void enter_stable(fs_aging_ctx_t *c)
{
    uint32_t i;

    c->fill_ops = c->tick;
    c->convergence_tick = calc_convergence_tick(c);
    for (i = 0u; i < c->profile->age_count; i++) {
        c->age_cutoff_tick[i] =
            (uint64_t)(c->age_ratio[i] * (fs_aging_float_t)c->convergence_tick);
        c->count_age[i] = 0u;
    }

    /* Doldurma sirasinda olusan dosyalari yas kovalarina yerlestir.
     * Havuz boyutunda tek seferlik bir tarama. */
    for (i = 0u; i < c->pool_high_water; i++) {
        if (c->pool[i].dir_bin != FREE_DIR_BIN) {
            c->count_age[age_bin_of(c, c->pool[i].birth_tick)]++;
        }
    }

    c->phase = FS_AGING_PHASE_STABLE;
    log_msg(c, "fs_aging: doldurma bitti, kararli aging basladi");
}

static void check_workload(fs_aging_ctx_t *c)
{
    if ((c->workload_limit_bytes != 0u) &&
        (c->workload_bytes >= c->workload_limit_bytes)) {
        c->status = FS_AGING_DONE_WORKLOAD_LIMIT;
    }
}

static void fill_one(fs_aging_ctx_t *c)
{
    create_result_t r;

    if (c->live_bytes >= c->target_live_bytes) {
        enter_stable(c);
        return;
    }

    c->tick++;
    r = do_create(c, pick_size_sampled(c));
    if (r == CR_NO_SPACE) {
        do_delete(c);
    } else if (r == CR_POOL_FULL) {
        fail(c, FS_AGING_ERR_POOL_FULL,
             "fs_aging: kayit havuzu doldurma icin yetersiz");
    } else {
        /* CR_OK veya CR_FAIL: yapacak baska bir sey yok */
    }

    if (c->status == FS_AGING_RUNNING) {
        check_workload(c);
    }
}

static void stable_one(fs_aging_ctx_t *c)
{
    int need_delete = 1;

    c->tick++;
    if ((rng_next(c) >> 63) != 0u) {
        uint32_t size_bin = pick_size_stable(c);
        if (size_bin != NONE_U32) {
            create_result_t r = do_create(c, size_bin);
            if ((r == CR_OK) || (r == CR_FAIL)) {
                need_delete = 0;
            }
            /* CR_NO_SPACE / CR_POOL_FULL: yerine silme yapilir */
        }
    }
    if (need_delete != 0) {
        do_delete(c);
    }
    if (c->status != FS_AGING_RUNNING) {
        return;
    }

    if (c->tick >= c->convergence_tick) {
        c->status = FS_AGING_DONE_CONVERGED;
    } else if ((c->workload_limit_bytes != 0u) &&
               (c->workload_bytes >= c->workload_limit_bytes)) {
        c->status = FS_AGING_DONE_WORKLOAD_LIMIT;
    } else if ((c->config.confidence > 0.0) &&
               (age_error(c) <= (1.0 - c->config.confidence))) {
        c->status = FS_AGING_DONE_TOLERANCE;
    } else {
        /* devam */
    }
}

/* ================================================================== */
/* Dogrulama                                                           */
/* ================================================================== */

static int validate_profile(const fs_aging_profile_t *pr)
{
    uint32_t i;
    fs_aging_float_t total;

    if ((pr->ages == NULL) || (pr->sizes == NULL) || (pr->dirs == NULL)) {
        return -1;
    }
    if ((pr->age_count < 2u) || (pr->age_count > FS_AGING_MAX_BINS) ||
        (pr->size_count < 1u) || (pr->size_count > FS_AGING_MAX_BINS) ||
        (pr->dir_count < 1u) || (pr->dir_count > FS_AGING_MAX_BINS)) {
        return -1;
    }

    total = 0.0;
    for (i = 0u; i < pr->age_count; i++) {
        if (pr->ages[i].weight < 0.0) {
            return -1;
        }
        if ((i == 0u) ? (pr->ages[0].age_cutoff <= 0.0)
                      : (pr->ages[i].age_cutoff <= pr->ages[i - 1u].age_cutoff)) {
            return -1;   /* esikler pozitif ve kesin artan olmali */
        }
        total += pr->ages[i].weight;
    }
    if (total <= 0.0) {
        return -1;
    }

    total = 0.0;
    for (i = 0u; i < pr->size_count; i++) {
        if (pr->sizes[i].weight < 0.0) {
            return -1;
        }
        total += pr->sizes[i].weight;
    }
    if (total <= 0.0) {
        return -1;
    }

    total = 0.0;
    for (i = 0u; i < pr->dir_count; i++) {
        if ((pr->dirs[i].weight < 0.0) ||
            (pr->dirs[i].sibling_dirs > MAX_SIBLINGS)) {
            return -1;
        }
        total += pr->dirs[i].weight;
    }
    if (total <= 0.0) {
        return -1;
    }
    return 0;
}

/* Profilin uretebilecegi en uzun dosya yolu ('\0' dahil) */
static uint32_t max_path_len(const fs_aging_profile_t *pr, const char *mount)
{
    uint32_t best = 0u;
    uint32_t i;
    uint32_t lvl;

    for (i = 0u; i < pr->dir_count; i++) {
        uint32_t depth = pr->dirs[i].depth;
        uint32_t len = str_len(mount);
        if (depth > 0u) {
            uint32_t leaf = (pr->dirs[i].sibling_dirs != 0u)
                            ? pr->dirs[i].sibling_dirs : depth;
            for (lvl = 1u; lvl < depth; lvl++) {
                len += 2u + u64_digits(lvl);          /* "/d<lvl>"   */
            }
            len += 2u + u64_digits(leaf);             /* "/d<yaprak>" */
        }
        len += 2u + 20u + 1u;                         /* "/f<tick>\0" */
        if (len > best) {
            best = len;
        }
    }
    return best;
}

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

fs_aging_status_t fs_aging_init(fs_aging_ctx_t             *ctx,
                                const fs_aging_profile_t   *profile,
                                const fs_aging_config_t    *config,
                                const fs_aging_platform_t  *platform,
                                fs_aging_file_t            *pool,
                                uint32_t                    pool_capacity)
{
    unsigned char *raw;
    size_t   b;
    uint32_t i;
    fs_aging_float_t total;
    fs_aging_float_t max_cutoff;

    if (ctx == NULL) {
        return FS_AGING_ERR_PARAM;
    }
    raw = (unsigned char *)ctx;
    for (b = 0u; b < sizeof(*ctx); b++) {
        raw[b] = 0u;
    }
    ctx->status = FS_AGING_ERR_PARAM;
    ctx->phase  = FS_AGING_PHASE_DONE;

    if ((profile == NULL) || (config == NULL) || (platform == NULL) ||
        (pool == NULL) || (pool_capacity == 0u) ||
        (pool_capacity == NONE_U32)) {
        return FS_AGING_ERR_PARAM;
    }
    if ((platform->create_file == NULL) || (platform->delete_file == NULL) ||
        (platform->make_dir == NULL)) {
        return FS_AGING_ERR_PARAM;
    }
    if ((config->mount_point == NULL) || (config->volume_bytes == 0u) ||
        !(config->fill_ratio > 0.0) || (config->fill_ratio > 1.0) ||
        (config->confidence < 0.0) || !(config->confidence < 1.0)) {
        return FS_AGING_ERR_PARAM;
    }
    if (validate_profile(profile) != 0) {
        return FS_AGING_ERR_PARAM;
    }
    if (max_path_len(profile, config->mount_point) > FS_AGING_PATH_MAX) {
        ctx->status = FS_AGING_ERR_PATH_TOO_LONG;
        return FS_AGING_ERR_PATH_TOO_LONG;
    }

    ctx->profile       = profile;
    ctx->config        = *config;
    ctx->platform      = *platform;
    ctx->pool          = pool;
    ctx->pool_capacity = pool_capacity;
    ctx->free_head     = NONE_U32;

    total = 0.0;
    for (i = 0u; i < profile->age_count; i++) {
        total += profile->ages[i].weight;
    }
    max_cutoff = profile->ages[profile->age_count - 1u].age_cutoff;
    for (i = 0u; i < profile->age_count; i++) {
        ctx->ideal_age[i] = profile->ages[i].weight / total;
        ctx->age_ratio[i] = 1.0 - (profile->ages[i].age_cutoff / max_cutoff);
    }

    total = 0.0;
    for (i = 0u; i < profile->size_count; i++) {
        total += profile->sizes[i].weight;
    }
    for (i = 0u; i < profile->size_count; i++) {
        ctx->ideal_size[i] = profile->sizes[i].weight / total;
    }

    total = 0.0;
    for (i = 0u; i < profile->dir_count; i++) {
        total += profile->dirs[i].weight;
    }
    for (i = 0u; i < profile->dir_count; i++) {
        ctx->ideal_dir[i] = profile->dirs[i].weight / total;
    }

    ctx->target_live_bytes = (uint64_t)((fs_aging_float_t)config->volume_bytes *
                                        config->fill_ratio);
    ctx->workload_limit_bytes = config->volume_bytes *
                                (uint64_t)config->overwrite_runs;

    rng_seed(ctx, config->seed);

    if (ctx->platform.now_ms != NULL) {
        ctx->start_ms = ctx->platform.now_ms(ctx->platform.user);
    }

    ctx->phase  = FS_AGING_PHASE_SETUP;
    ctx->status = FS_AGING_RUNNING;
    log_msg(ctx, "fs_aging: init tamam");
    return FS_AGING_RUNNING;
}

fs_aging_status_t fs_aging_step(fs_aging_ctx_t *ctx, uint32_t max_ops)
{
    uint32_t n;

    if (ctx == NULL) {
        return FS_AGING_ERR_PARAM;
    }
    if (ctx->status != FS_AGING_RUNNING) {
        return ctx->status;
    }

    for (n = 0u; (n < max_ops) && (ctx->status == FS_AGING_RUNNING); n++) {
        switch (ctx->phase) {
        case FS_AGING_PHASE_SETUP:
            setup_one(ctx);
            break;
        case FS_AGING_PHASE_FILL:
            fill_one(ctx);
            break;
        case FS_AGING_PHASE_STABLE:
            stable_one(ctx);
            break;
        default:
            ctx->status = FS_AGING_ERR_PARAM;
            break;
        }
    }

    if (ctx->platform.now_ms != NULL) {
        ctx->elapsed_ms = ctx->platform.now_ms(ctx->platform.user) - ctx->start_ms;
        if ((ctx->status == FS_AGING_RUNNING) && (ctx->config.time_limit_s != 0u) &&
            (ctx->elapsed_ms >= ((uint64_t)ctx->config.time_limit_s * 1000u))) {
            ctx->status = FS_AGING_DONE_TIME_LIMIT;
        }
    }

    if (ctx->status != FS_AGING_RUNNING) {
        ctx->phase = FS_AGING_PHASE_DONE;
        log_msg(ctx, fs_aging_status_str(ctx->status));
    }
    return ctx->status;
}

void fs_aging_get_stats(const fs_aging_ctx_t *ctx, fs_aging_stats_t *out)
{
    if ((ctx == NULL) || (out == NULL)) {
        return;
    }
    out->status          = ctx->status;
    out->phase           = ctx->phase;
    out->ops             = ctx->tick;
    out->creates         = ctx->creates;
    out->deletes         = ctx->deletes;
    out->live_bytes      = ctx->live_bytes;
    out->workload_bytes  = ctx->workload_bytes;
    out->live_files      = ctx->live_files;
    out->convergence_ops = ctx->convergence_tick;
    out->age_error       = (ctx->convergence_tick != 0u) ? age_error(ctx) : 1.0;
    out->elapsed_ms      = ctx->elapsed_ms;
}

uint32_t fs_aging_bin_count(const fs_aging_ctx_t *ctx, fs_aging_dist_t dist)
{
    if ((ctx == NULL) || (ctx->profile == NULL)) {
        return 0u;
    }
    switch (dist) {
    case FS_AGING_DIST_AGE:  return ctx->profile->age_count;
    case FS_AGING_DIST_SIZE: return ctx->profile->size_count;
    case FS_AGING_DIST_DIR:  return ctx->profile->dir_count;
    default:                 return 0u;
    }
}

int fs_aging_get_bin(const fs_aging_ctx_t *ctx, fs_aging_dist_t dist,
                     uint32_t index,
                     fs_aging_float_t *ideal, fs_aging_float_t *actual)
{
    const fs_aging_float_t *ideals;
    const uint32_t         *counts;

    if ((ideal == NULL) || (actual == NULL) ||
        (index >= fs_aging_bin_count(ctx, dist))) {
        return -1;
    }
    switch (dist) {
    case FS_AGING_DIST_AGE:
        ideals = ctx->ideal_age;
        counts = ctx->count_age;
        break;
    case FS_AGING_DIST_SIZE:
        ideals = ctx->ideal_size;
        counts = ctx->count_size;
        break;
    default:
        ideals = ctx->ideal_dir;
        counts = ctx->count_dir;
        break;
    }
    *ideal  = ideals[index];
    *actual = fraction(ctx, counts[index]);
    return 0;
}

uint32_t fs_aging_estimate_files(const fs_aging_profile_t *profile,
                                 uint64_t volume_bytes,
                                 fs_aging_float_t fill_ratio)
{
    fs_aging_float_t total = 0.0;
    fs_aging_float_t mean  = 0.0;
    fs_aging_float_t files;
    uint32_t i;

    if ((profile == NULL) || (profile->sizes == NULL) ||
        (profile->size_count == 0u)) {
        return 0u;
    }
    for (i = 0u; i < profile->size_count; i++) {
        total += profile->sizes[i].weight;
        mean  += profile->sizes[i].weight *
                 (fs_aging_float_t)profile->sizes[i].size_bytes;
    }
    if ((total <= 0.0) || (mean <= 0.0)) {
        return 0u;
    }
    mean /= total;
    files = ((fs_aging_float_t)volume_bytes * fill_ratio) / mean + 1.0;
    if (files >= 4294967295.0) {
        return 0xFFFFFFFFu;
    }
    return (uint32_t)files;
}

const char *fs_aging_status_str(fs_aging_status_t status)
{
    switch (status) {
    case FS_AGING_RUNNING:             return "fs_aging: calisiyor";
    case FS_AGING_DONE_CONVERGED:      return "fs_aging: bitti, tam yakinsama";
    case FS_AGING_DONE_TOLERANCE:      return "fs_aging: bitti, hata toleransa indi";
    case FS_AGING_DONE_WORKLOAD_LIMIT: return "fs_aging: bitti, yuk sinirina ulasildi";
    case FS_AGING_DONE_TIME_LIMIT:     return "fs_aging: bitti, sure sinirina ulasildi";
    case FS_AGING_ERR_PARAM:           return "fs_aging: hata, gecersiz parametre";
    case FS_AGING_ERR_POOL_FULL:       return "fs_aging: hata, kayit havuzu dolu";
    case FS_AGING_ERR_PATH_TOO_LONG:   return "fs_aging: hata, yol cok uzun";
    case FS_AGING_ERR_PLATFORM:        return "fs_aging: hata, platform islemi basarisiz";
    default:                           return "fs_aging: bilinmeyen durum";
    }
}