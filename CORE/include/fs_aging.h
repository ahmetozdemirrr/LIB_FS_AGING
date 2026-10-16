/*
 * fs_aging.h - File system aging kutuphanesi (Geriatrix tabanli)
 *
 * Platformdan bagimsiz core. fs_aging.c hicbir OS / APEX / POSIX header'i
 * ve libc fonksiyonu kullanmaz, kendi icinde bellek ayirmaz.
 *
 * Kullanim:
 *   1. Port katmani fs_aging_platform_t'yi doldurur.
 *   2. Cagiran taraf dosya kayit havuzunu verir:
 *          static fs_aging_file_t pool[50000];
 *      Gerekli kapasite icin fs_aging_estimate_files() kullanilabilir.
 *   3. fs_aging_init() bir kez cagrilir.
 *   4. fs_aging_step() FS_AGING_RUNNING disinda bir deger donene kadar
 *      tekrar tekrar cagrilir (ARINC653'te periyodik process'ten).
 *
 * Calisma asamalari (hepsi fs_aging_step() icinde, parca parca yurur):
 *   SETUP  : profilin gerektirdigi dizin agaci olusturulur.
 *   FILL   : volume, fill_ratio oranina kadar profile gore doldurulur.
 *   STABLE : rastgele olustur/sil islemleriyle yas, boyut ve dizin
 *            dagilimlari profile yakinsatilir.
 *
 * Orijinal: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
 */

#ifndef FS_AGING_H
#define FS_AGING_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Derleme zamani limitleri (gerekirse derleyici -D ile degistirilir)  */
/* ------------------------------------------------------------------ */

/* Mount point dahil olusturulan en uzun yol (sonlandirici '\0' dahil).
 * fs_aging_init() profilin uretecegi en uzun yolu bu sinirla karsilastirir. */
#ifndef FS_AGING_PATH_MAX
#define FS_AGING_PATH_MAX        256u
#endif

/* Bir profildeki yas / boyut / dizin satiri sayisi ust siniri. */
#ifndef FS_AGING_MAX_BINS
#define FS_AGING_MAX_BINS        48u
#endif

/* Silinecek dosya secilirken incelenen rastgele aday sayisi. Buyudukce
 * dagilimlar daha hizli yakinsar, islem basina maliyet artar. */
#ifndef FS_AGING_DELETE_SAMPLES
#define FS_AGING_DELETE_SAMPLES  32u
#endif

/* Kayan noktali tip. ARINC653 port'u float8_gt ile ayni boyutta
 * oldugunu derleme zamaninda dogrular. */
typedef double fs_aging_float_t;

/* ------------------------------------------------------------------ */
/* Durum kodlari                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    FS_AGING_RUNNING = 0,          /* devam ediyor, step'i tekrar cagir      */
    FS_AGING_DONE_CONVERGED,       /* yas dagilimi profile tam yakinsadi     */
    FS_AGING_DONE_TOLERANCE,       /* yas hatasi confidence sinirina indi    */
    FS_AGING_DONE_WORKLOAD_LIMIT,  /* overwrite_runs sinirina ulasildi       */
    FS_AGING_DONE_TIME_LIMIT,      /* time_limit_s sinirina ulasildi         */
    FS_AGING_ERR_PARAM,            /* gecersiz profil / config / platform    */
    FS_AGING_ERR_POOL_FULL,        /* FILL asamasinda kayit havuzu yetmedi   */
    FS_AGING_ERR_PATH_TOO_LONG,    /* profil FS_AGING_PATH_MAX'i asiyor      */
    FS_AGING_ERR_PLATFORM          /* platform callback'i hata dondu         */
} fs_aging_status_t;

/* Platform callback'lerinin donus degerleri. */
typedef enum {
    FS_AGING_PLAT_OK = 0,
    FS_AGING_PLAT_NO_SPACE,        /* volume dolu: core bunun yerine silme yapar */
    FS_AGING_PLAT_ERROR            /* diger tum hatalar: aging durur             */
} fs_aging_plat_result_t;

/* ------------------------------------------------------------------ */
/* Platform arayuzu - port katmani doldurur                            */
/* ------------------------------------------------------------------ */

typedef struct {
    /* size_bytes boyutunda dosya olusturur (yer ayirma veya veri yazma). */
    fs_aging_plat_result_t (*create_file)(void *user, const char *path,
                                          uint64_t size_bytes);

    /* Dosyayi siler. */
    fs_aging_plat_result_t (*delete_file)(void *user, const char *path);

    /* Tek seviye dizin olusturur. Dizin zaten varsa FS_AGING_PLAT_OK
     * donmelidir. Ust dizinleri sirayla core olusturur. */
    fs_aging_plat_result_t (*make_dir)(void *user, const char *path);

    /* Opsiyonel: monoton zaman, milisaniye. NULL ise sure siniri yok. */
    uint64_t (*now_ms)(void *user);

    /* Opsiyonel: asama gecisi ve hata mesajlari. NULL ise basilmaz. */
    void (*log)(void *user, const char *msg);

    /* Callback'lere aynen iletilen kullanici verisi. */
    void *user;
} fs_aging_platform_t;

/* ------------------------------------------------------------------ */
/* Aging profili (Geriatrix profil dosyalarinin karsiligi)             */
/* ------------------------------------------------------------------ */

/* age_distribution.txt satiri: "<yas_esigi> <agirlik>"
 * Esikler artan sirada olmalidir. En az 2 satir gerekir. */
typedef struct {
    fs_aging_float_t age_cutoff;
    fs_aging_float_t weight;
} fs_aging_age_bin_t;

/* size_distribution.txt satiri: "<boyut_byte> <agirlik>" */
typedef struct {
    uint64_t         size_bytes;
    fs_aging_float_t weight;
} fs_aging_size_bin_t;

/* dir_distribution.txt satiri: "<derinlik> <agirlik> <kardes_dizin_sayisi>" */
typedef struct {
    uint32_t         depth;
    fs_aging_float_t weight;
    uint32_t         sibling_dirs;
} fs_aging_dir_bin_t;

typedef struct {
    const char                *name;
    const fs_aging_age_bin_t  *ages;
    uint32_t                   age_count;
    const fs_aging_size_bin_t *sizes;
    uint32_t                   size_count;
    const fs_aging_dir_bin_t  *dirs;
    uint32_t                   dir_count;
} fs_aging_profile_t;

/* ------------------------------------------------------------------ */
/* Calisma parametreleri (parantez icinde orijinal Geriatrix parametresi) */
/* ------------------------------------------------------------------ */

typedef struct {
    const char       *mount_point;    /* sonunda '/' olmadan, or. "/fs0"   (-m)
                                         aging boyunca gecerli kalmalidir      */
    uint64_t          volume_bytes;   /* dosya sistemi toplam boyutu       (-n) */
    fs_aging_float_t  fill_ratio;     /* hedef doluluk, 0-1 arasi, or. 0.8 (-u) */
    uint32_t          seed;           /* rastgelelik tohumu                (-r) */
    uint32_t          overwrite_runs; /* toplam yuk = runs * volume_bytes  (-i)
                                         0 = sinirsiz                          */
    uint32_t          time_limit_s;   /* 0 = sinirsiz                      (-w) */
    fs_aging_float_t  confidence;     /* 0 = tam yakinsama beklenir        (-c)
                                         or. 0.9: yas dagilimi hatasi %10'a
                                         indiginde durulur                     */
} fs_aging_config_t;

/* ------------------------------------------------------------------ */
/* Dosya kayit havuzu                                                  */
/* ------------------------------------------------------------------ */

/* Canli her dosya icin bir kayit (24 byte). Yol saklanmaz, ihtiyac
 * aninda uretilir. Alanlar core'a aittir; cagiran taraf sadece diziyi
 * ayirir. */
typedef struct {
    uint64_t birth_tick;   /* olusturuldugu islem numarasi, dosya adi da bu */
    uint32_t next_free;    /* bos kayitlar listesi                         */
    uint16_t size_bin;     /* profildeki boyut satiri                      */
    uint16_t dir_bin;      /* profildeki dizin satiri, bos kayitta 0xFFFF  */
    uint16_t sibling;      /* kardes dizin numarasi (1..n), yoksa 0        */
    uint16_t reserved;
} fs_aging_file_t;

/* ------------------------------------------------------------------ */
/* Istatistikler                                                       */
/* ------------------------------------------------------------------ */

typedef enum {
    FS_AGING_PHASE_SETUP = 0,
    FS_AGING_PHASE_FILL,
    FS_AGING_PHASE_STABLE,
    FS_AGING_PHASE_DONE
} fs_aging_phase_t;

typedef struct {
    fs_aging_status_t status;
    fs_aging_phase_t  phase;
    uint64_t          ops;              /* toplam dosya islemi                 */
    uint64_t          creates;
    uint64_t          deletes;
    uint64_t          live_bytes;       /* su an dosya sisteminde duran veri   */
    uint64_t          workload_bytes;   /* simdiye kadar yazilan toplam veri   */
    uint32_t          live_files;
    uint64_t          convergence_ops;  /* tam yakinsama icin hedef islem no.  */
    fs_aging_float_t  age_error;        /* yas dagilimi hatasi, 0-1 arasi      */
    uint64_t          elapsed_ms;       /* now_ms verilmediyse 0               */
} fs_aging_stats_t;

/* Dagilim sorgulari icin (orijinaldeki -x / -y / -z ciktilarinin karsiligi) */
typedef enum {
    FS_AGING_DIST_AGE = 0,
    FS_AGING_DIST_SIZE,
    FS_AGING_DIST_DIR
} fs_aging_dist_t;

/* ------------------------------------------------------------------ */
/* Context - alanlari dogrudan degistirmeyin                           */
/* ------------------------------------------------------------------ */

typedef struct {
    const fs_aging_profile_t *profile;   /* aging boyunca gecerli kalmali */
    fs_aging_config_t         config;
    fs_aging_platform_t       platform;

    fs_aging_file_t *pool;
    uint32_t         pool_capacity;
    uint32_t         pool_high_water;    /* simdiye kadar kullanilan en yuksek slot */
    uint32_t         free_head;

    fs_aging_status_t status;
    fs_aging_phase_t  phase;
    uint32_t          setup_dir_bin;     /* SETUP asamasi imleci */
    uint32_t          setup_sibling;

    uint64_t rng_state;
    uint64_t tick;
    uint64_t fill_ops;                   /* FILL asamasindaki islem sayisi (K) */
    uint64_t convergence_tick;           /* tam yakinsama hedefi (T)           */
    uint64_t target_live_bytes;
    uint64_t workload_limit_bytes;
    uint64_t start_ms;
    uint64_t elapsed_ms;

    uint64_t live_bytes;
    uint64_t workload_bytes;
    uint64_t creates;
    uint64_t deletes;
    uint32_t live_files;

    fs_aging_float_t ideal_age [FS_AGING_MAX_BINS];
    fs_aging_float_t ideal_size[FS_AGING_MAX_BINS];
    fs_aging_float_t ideal_dir [FS_AGING_MAX_BINS];
    uint32_t         count_age [FS_AGING_MAX_BINS];
    uint32_t         count_size[FS_AGING_MAX_BINS];
    uint32_t         count_dir [FS_AGING_MAX_BINS];
    fs_aging_float_t age_ratio [FS_AGING_MAX_BINS];  /* 1 - esik / en_buyuk_esik */
    uint64_t         age_cutoff_tick[FS_AGING_MAX_BINS];

    char path_buf[FS_AGING_PATH_MAX];
} fs_aging_ctx_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Context'i hazirlar, profil ve parametreleri dogrular. Dosya sistemine
 * dokunmaz. Basarida FS_AGING_RUNNING doner. */
fs_aging_status_t fs_aging_init(fs_aging_ctx_t             *ctx,
                                const fs_aging_profile_t   *profile,
                                const fs_aging_config_t    *config,
                                const fs_aging_platform_t  *platform,
                                fs_aging_file_t            *pool,
                                uint32_t                    pool_capacity);

/* En fazla max_ops islem yapar ve doner (SETUP'ta bir islem = bir yaprak
 * dizin, diger asamalarda bir dosya olusturma ya da silme). */
fs_aging_status_t fs_aging_step(fs_aging_ctx_t *ctx, uint32_t max_ops);

/* Guncel istatistikleri kopyalar. */
void fs_aging_get_stats(const fs_aging_ctx_t *ctx, fs_aging_stats_t *out);

/* Bir dagilimdaki satir sayisi. */
uint32_t fs_aging_bin_count(const fs_aging_ctx_t *ctx, fs_aging_dist_t dist);

/* Bir satirin hedef ve guncel oranlari. Gecersiz index'te -1 doner. */
int fs_aging_get_bin(const fs_aging_ctx_t *ctx, fs_aging_dist_t dist,
                     uint32_t index,
                     fs_aging_float_t *ideal, fs_aging_float_t *actual);

/* Profil ve volume icin FILL sonunda beklenen yaklasik dosya sayisi.
 * Havuzu bunun en az %25 fazlasi kadar boyutlandirmak onerilir. */
uint32_t fs_aging_estimate_files(const fs_aging_profile_t *profile,
                                 uint64_t volume_bytes,
                                 fs_aging_float_t fill_ratio);

/* Durum kodunun okunabilir karsiligi. */
const char *fs_aging_status_str(fs_aging_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* FS_AGING_H */