# LIB_FS_AGING

Dosya sistemi yaşlandırma (aging) kütüphanesi. Saf C99 ile yazılmış, platformdan bağımsız bir çekirdek (FS_AGING_CORE) ve bu çekirdeği bir işletim sistemine bağlayan port katmanlarından oluşur.

Bu belge iki okur için yazıldı:

1. **ARINC 653 port'unu geliştiren ekip.** Çekirdeği partition içinde statik bellekle çalıştıracaklar.
2. **POSIX port'unu yazacak ekip.** Port'u yalnızca bu belgeye bakarak yazabilmeleri hedefleniyor. Bağlayıcı olan bölüm [POSIX port sözleşmesi](#7-posix-port-sözleşmesi)'dir.

## İçindekiler

1. [Amaç ve kaynak](#1-amaç-ve-kaynak)
2. [Klasör yapısı](#2-klasör-yapısı)
3. [Çalışma modeli](#3-çalışma-modeli)
4. [Kullanım örneği](#4-kullanım-örneği)
5. [Bellek boyutlandırma](#5-bellek-boyutlandırma)
6. [Orijinal Geriatrix'ten farklar](#6-orijinal-geriatrixten-farklar)
7. [POSIX port sözleşmesi](#7-posix-port-sözleşmesi)
8. [Profiller ve `gen_profiles.py`](#8-profiller-ve-gen_profilespy)
9. [Lisans](#9-lisans)

---

## 1. Amaç ve kaynak

Kütüphane, [Geriatrix](https://github.com/saurabhkadekodi/geriatrix) dosya sistemi yaşlandırma aracının **işlevsel karşılığıdır**. Geriatrix, bir profile göre kontrollü dosya oluşturma ve silme işlemleri yaparak dosya sisteminde gerçekçi dosya ve boş alan parçalanması üretir.

- Makale: Saurabh Kadekodi, Vaishnavh Nagarajan, Gregory R. Ganger, Garth A. Gibson, *"Geriatrix: Aging what you see and what you don't see. A file system aging approach for modern storage systems"*, USENIX ATC 2018. PDF: <http://cs.cmu.edu/~saukad/geriatrix.pdf>
- Orijinal kod C++11 ile yazılmıştır ve Boost'a bağımlıdır. Lisansı BSD-2-Clause'dur (bkz. [Lisans](#9-lisans)).

Amaç, şirket RTOS'unda hem **ARINC 653** hem **POSIX** profilinde dosya sistemi aging testleri yapabilmektir. Orijinalle bire bir aynı dosya dizisini üretmek hedeflenmez. Hedef, profildeki **yaş**, **boyut** ve **dizin derinliği** dağılımlarına yakınsayan bir aging yükü üretmektir.

Çekirdeğin temel özellikleri:

- Yalnızca `<stdint.h>` ve `<stddef.h>` kullanır. Hiçbir OS, APEX veya POSIX header'ını include etmez ve libc fonksiyonu çağırmaz.
- Bellek ayırmaz (`malloc` yok). Özyineleme ve VLA kullanmaz. Context ve dosya kayıt havuzunu çağıran taraf verir.
- Dosya sistemine yalnızca `fs_aging_platform_t` içindeki callback'ler üzerinden erişir.
- İş adım adım ilerler: `fs_aging_step(ctx, max_ops)` her çağrıda en fazla `max_ops` işlem yapıp döner. Böylece ARINC 653 partition penceresi taşmaz.
- Çekirdekte değiştirilebilir global durum yoktur. Tüm durum `fs_aging_ctx_t` içinde tutulur.

## 2. Klasör yapısı

Hedef konum (iç ağ): `RTOS/03_Verification/02_Testing/01_TestCases/UTILS_LIBS/COMMON/LIB_FS_AGING/`

```
LIB_FS_AGING/
├── AGENT_PROMPT.md              # geliştirme talimatı (kodlama ajanı için)
├── README.md                    # bu belge
├── LICENSE.geriatrix            # orijinal Geriatrix BSD-2-Clause lisans metni
├── FS_AGING_CORE/               # platformdan bağımsız çekirdek (saf C99)
│   ├── include/
│   │   ├── fs_aging.h           # dış API, platform arayüzü, tipler, derleme limitleri
│   │   └── fs_aging_profiles.h  # hazır 8 profilin bildirimleri
│   └── src/
│       ├── fs_aging.c           # tüm aging mantığı
│       └── fs_aging_profiles.c  # ÜRETİLİR (TOOLS/gen_profiles.py), elle düzenlenmez
├── ARINC653/                    # HENÜZ YAZILMADI (Görev 4, gerçek APEX header'ları bekleniyor)
└── TOOLS/
    └── gen_profiles.py          # Geriatrix profil .txt dosyaları -> C tabloları
```

- **ARINC653/** henüz yok. Planlanan içerik `ARINC653/include/fs_aging_arinc653.h`, `ARINC653/src/fs_aging_arinc653.c` (callback'ler) ve `ARINC653/src/fs_aging_arinc653_runner.c` (INIT modunda kurulum, periyodik process). Bu iş, gerçek APEX, `LIB_FILE_OPERATIONS`, `LIB_USER_PRINT` header'ları ve `float8_gt` tanımı gelene kadar bekliyor.
- **POSIX port'u** bu repoda yazılmaz. Ayrı ekip tarafından [bölüm 7](#7-posix-port-sözleşmesi)'deki sözleşmeye göre yazılır.
- Referans için klonlanan `geriatrix/` klasörü (varsa) salt okunurdur ve `.gitignore` ile repo dışında tutulur.

Bir projeye eklemek için gereken tek şey: include yolu `FS_AGING_CORE/include`, derlenecek dosyalar `FS_AGING_CORE/src/fs_aging.c` ve hazır profiller kullanılacaksa `FS_AGING_CORE/src/fs_aging_profiles.c`.

> **libc'siz hedefler (ARINC 653 / RTOS):** Kaynak kod hiçbir libc fonksiyonu çağırmaz. Ancak gcc, normal (hosted) derlemede `-O2`/`-O3` ile `fs_aging_init()` içindeki bayt sıfırlama döngüsünü bir `memset` çağrısına çevirir; nesne dosyasında tanımsız `memset` sembolü oluşur (`nm -u` ile görülür). Hedefte `memset` yoksa FS_AGING_CORE'u `-ffreestanding` (ya da `-fno-tree-loop-distribute-patterns`) ile derleyin; bu host'ta `-ffreestanding` ile `fs_aging.o` ve `fs_aging_profiles.o`'da tanımsız sembol kalmadığı doğrulandı. Diğer seçenek, hedefin `memset` sağlamasıdır.

### Derleme zamanı limitleri

`fs_aging.h` içindeki şu makrolar derleyicide `-D` ile değiştirilebilir:

| Makro | Varsayılan | Anlamı |
|---|---|---|
| `FS_AGING_PATH_MAX` | `256u` | Mount point dahil en uzun yol, sonlandırıcı `'\0'` dahil |
| `FS_AGING_MAX_BINS` | `48u` | Bir profildeki yaş, boyut ve dizin satırı sayısı üst sınırı |
| `FS_AGING_DELETE_SAMPLES` | `32u` | Silinecek dosya seçilirken bakılan rastgele aday sayısı |
| `FS_AGING_REBIN_CHUNK` | `1024u` | FILL sonundaki yaş kovası yerleştirmede işlem başına taranan kayıt sayısı (en az 1) |
| `FS_AGING_FILL_STALL_OPS` | `100000u` | FILL'de canlı veri yeni bir en yüksek değere çıkmadan art arda geçebilecek işlem sayısı; aşılırsa `FS_AGING_ERR_FILL_UNREACHABLE` |

> **Dikkat:** Bu makrolar `fs_aging_ctx_t` yapısının düzenini ve boyutunu değiştirir. `fs_aging.h`'yi include eden **tüm** derleme birimleri aynı değerlerle derlenmelidir. `FS_AGING_DELETE_SAMPLES` değiştirilirse aynı seed farklı bir işlem dizisi üretir.

## 3. Çalışma modeli

### API

| Fonksiyon | Görevi |
|---|---|
| `fs_aging_init(ctx, profile, config, platform, pool, pool_capacity)` | Context'i sıfırlar, profil ve parametreleri doğrular. **Dosya sistemine dokunmaz.** Başarıda `FS_AGING_RUNNING` döner. |
| `fs_aging_step(ctx, max_ops)` | En fazla `max_ops` işlem yapıp güncel durumu döner. |
| `fs_aging_get_stats(ctx, out)` | İstatistikleri `fs_aging_stats_t` içine kopyalar. |
| `fs_aging_bin_count(ctx, dist)` | Bir dağılımdaki (yaş/boyut/dizin) satır sayısı. |
| `fs_aging_get_bin(ctx, dist, index, &ideal, &actual)` | Bir satırın hedef ve gerçek oranı. Geçersiz index veya NULL çıkış pointer'ında `-1` döner. |
| `fs_aging_estimate_files(profile, volume_bytes, fill_ratio)` | FILL aşaması için güvenli kayıt havuzu kapasitesi. Doğrudan `pool_capacity` olarak kullanılır, ek pay gerekmez (bkz. [bölüm 5](#5-bellek-boyutlandırma)). `0` (ör. hiçbir boyut volume'e sığmıyor) ve `0xFFFFFFFF` (güvenli sınır yok ya da 32 bite sığmıyor) geçerli kapasite değildir. |
| `fs_aging_status_str(status)` | Durum kodunun okunabilir (ASCII) metni. |
| `fs_aging_profile_find(name)` | Hazır profili adıyla bulur (`fs_aging_profiles.h`). Tam ve büyük/küçük harf duyarlı eşleşme yapar; bulunamazsa `NULL` döner. |

### Aşamalar

Tüm aşamalar `fs_aging_step()` içinde, parça parça ilerler. Güncel aşama `stats.phase` alanında görülür.

1. **SETUP** — Profilin gerektirdiği dizin ağacı oluşturulur. Her işlemde bir yaprak dizin ve gerekiyorsa üst dizinleri oluşturulur.
2. **FILL** — Volume, `volume_bytes × fill_ratio` canlı veriye ulaşana kadar profil dağılımından rastgele örneklenen boyutlarla doldurulur. Oluşturma `NO_SPACE` alırsa yerine bir dosya silinir. Volume'de hiç canlı dosya yoksa silme yapılmaz: boyut volume'den büyükse işlem tüketilir ve sonraki işlem yeni bir boyut çeker; boyut volume'e sığdığı hâlde `NO_SPACE` alındıysa (gerçek kapasite `volume_bytes`'tan küçük) aynı işlemde ağırlığı pozitif en küçük boyut denenir. FILL hedefe ulaşana kadar sürer. Hedefe ulaşılamıyorsa (ör. `fill_ratio` 1.0'a çok yakın ya da gerçek boş alan `volume_bytes × fill_ratio`'dan küçük) çekirdek bunu kendisi fark eder: canlı veri art arda `FS_AGING_FILL_STALL_OPS` (100000) işlem boyunca yeni bir en yüksek değere çıkmazsa çalışma `FS_AGING_ERR_FILL_UNREACHABLE` ile biter. Bu kontrol rastgele sayı çekmez; tetiklenmediği çalışmaların sonucu değişmez.
   - **Yaş kovası yerleştirme (ara adım):** Doldurma hedefe ulaşınca, FILL'de oluşan dosyalar her işlemde en fazla `FS_AGING_REBIN_CHUNK` kayıt taranarak yaş kovalarına yerleştirilir. Bu sırada aşama hâlâ `FS_AGING_PHASE_FILL` olarak raporlanır. Dosya oluşturulmaz ya da silinmez; `stats.ops` (tick) ilerlemez ve rastgele sayı çekilmez.
3. **STABLE** — Her işlemde 50/50 yazı-tura ile oluşturma veya silme yapılır; yaş, boyut ve dizin dağılımları profile yakınsatılır. Canlı dosya yoksa yazı-tura silme dese de oluşturma yapılır (orijinalden bilinçli fark, bkz. [bölüm 6](#6-orijinal-geriatrixten-farklar)).
4. **DONE** — `FS_AGING_RUNNING` dışında bir durumda aşama `FS_AGING_PHASE_DONE` olur.

### `fs_aging_step()` ve `max_ops` anlamı

Bir "işlem" (op) aşamaya göre şudur:

| Aşama | Bir işlem | Bir işlemdeki platform çağrıları |
|---|---|---|
| SETUP | Bir yaprak dizin | Yaprak derinliği kadar `make_dir` (kök tarafından başlayarak her seviye için bir kez; `pramod`'da derinlik 36'ya kadar). Tüm dizinler bitince SETUP'tan FILL'e geçiş de bir işlem sayılır ve platform çağrısı yapmaz. |
| FILL | Bir dosya oluşturma | Bir `create_file`. `NO_SPACE` dönerse ve canlı dosya varsa aynı işlemde bir `delete_file`. Canlı dosya yoksa: boyut volume'den büyükse başka çağrı yapılmaz (işlem tüketilir); boyut volume'e sığıyorsa ve en küçük boyut değilse aynı işlemde en küçük boyutla ikinci bir `create_file` (en fazla 2 çağrı). |
| FILL (yaş kovası yerleştirme) | En fazla `FS_AGING_REBIN_CHUNK` havuz kaydının taranması | Yok |
| STABLE | Bir oluşturma veya silme | Bir `create_file` ya da bir `delete_file`. Canlı dosya yoksa yazı-tura sonucundan bağımsız olarak oluşturma denenir. Oluşturma yapılamazsa (volume'e sığan boyut yok, `NO_SPACE` döndü ya da kayıt havuzu dolu) aynı işlemde bir `delete_file`. Oluşturma yapılamıyor ve hiç canlı dosya da yoksa (gerçek çıkmaz) silme çağrısı yapılmaz ve çalışma `FS_AGING_ERR_PLATFORM` ile hemen biter (bkz. durum kodları). |

- Böylece tek bir `fs_aging_step()` çağrısının süresi `max_ops × (en pahalı tek işlem)` ile sınırlıdır ve havuz boyutundan bağımsızdır. Tek istisna, havuz çok seyrekken silinecek dosya arayan yedek taramadır. Bu durum pratikte görülmez.
- **Adım bağımsızlığı:** Sonuç `max_ops` değerinden bağımsızdır. Aynı seed ile `max_ops=1` ve `max_ops=1000` aynı işlem dizisini ve aynı istatistikleri üretir. Tek fark, süre sınırının yalnızca her `fs_aging_step()` çağrısının sonunda kontrol edilmesidir.
- `stats.ops`, FILL ve STABLE'daki **dosya işlemi** sayısıdır (tick). SETUP işlemleri ve yaş kovası yerleştirme işlemleri buna dahil değildir. Bu yüzden `max_ops` toplamı `stats.ops`'a eşit olmaz.
- `max_ops = 0` hiçbir işlem yapmaz; yalnızca geçen süreyi günceller ve süre sınırını kontrol eder.
- Durum `FS_AGING_RUNNING` değilse `fs_aging_step()` hiçbir callback çağırmadan aynı durumu döner. `ctx == NULL` ise `FS_AGING_ERR_PARAM` döner.
- Çalışma `fs_aging_step()` içinde bitince (başarı veya hata) `log` callback'ine `fs_aging_status_str()` metni bir kez gönderilir. `fs_aging_init()` hataları (`FS_AGING_ERR_PARAM`, `FS_AGING_ERR_PATH_TOO_LONG`) loglanmaz; yalnızca dönüş değeriyle bildirilir.

### Durum kodları (`fs_aging_status_t`)

| Kod | Ne zaman | Anlamı |
|---|---|---|
| `FS_AGING_RUNNING` | `init` başarılı, iş sürüyor | `fs_aging_step()`'i tekrar çağırın. |
| `FS_AGING_DONE_CONVERGED` | STABLE'da tick, hedef işlem sayısına (`stats.convergence_ops`, orijinaldeki `calculateT`) ulaştı | Tam yakınsama. |
| `FS_AGING_DONE_TOLERANCE` | `confidence > 0` ve yaş hatası `≤ 1 − confidence` | Hata toleransa indi. |
| `FS_AGING_DONE_WORKLOAD_LIMIT` | Yazılan toplam veri `overwrite_runs × volume_bytes`'a ulaştı (`overwrite_runs > 0` ise) | Yük sınırı. FILL'de de tetiklenebilir. |
| `FS_AGING_DONE_TIME_LIMIT` | `now_ms` verilmiş, `time_limit_s > 0` ve geçen süre bu sınıra ulaştı | Süre sınırı (step sonunda kontrol edilir). |
| `FS_AGING_ERR_PARAM` | `init` doğrulaması başarısız ya da `ctx == NULL` | Geçersiz profil, config veya platform (ayrıntı aşağıda). |
| `FS_AGING_ERR_POOL_FULL` | **FILL** sırasında canlı dosya sayısı havuz kapasitesine ulaştı | Havuz küçük. STABLE'da havuzun dolması hata değildir (bkz. [bölüm 5](#5-bellek-boyutlandırma)). |
| `FS_AGING_ERR_PATH_TOO_LONG` | `init`'te profilin en uzun yolu `FS_AGING_PATH_MAX`'ı aşıyor | `FS_AGING_PATH_MAX`'ı büyütün veya mount point'i kısaltın. |
| `FS_AGING_ERR_PLATFORM` | Bir callback hata döndü; ya da hiçbir boyut oluşturulamıyor ve silinecek dosya da yok (gerçek çıkmaz) | Aging durur. Canlı dosya yokken silme gerekmesi tek başına hata **değildir**: STABLE'da oluşturma yapılır, FILL'de `NO_SPACE` alan işlem silmeden tüketilir. Çıkmaz yalnızca şu durumlarda callback hatası olmadan oluşur ve çalışma takılmadan biter: (a) FILL'de volume boşken `NO_SPACE` alındı ve ağırlığı pozitif en küçük boyut bile volume'den büyük, ya da volume'e sığan bir boyut `NO_SPACE` aldı ve en küçük boyut da (aynı işlemde denenir) `NO_SPACE` aldı (log: `"fs_aging: no profile size fits on the empty volume"`). Bu karar `NO_SPACE`'in alındığı işlemde verilir; yalnızca volume'den büyük bir boyut çekildiyse işlem tüketilir ve karar volume'e sığan bir boyut çekilen ilk işlemde verilir; (b) STABLE'da canlı dosya yok ve volume'e sığan profil boyutu yok (log: `"fs_aging: no profile size fits and no file to delete"`) ya da oluşturma `NO_SPACE` aldı (log: `"fs_aging: create got NO_SPACE and no file to delete"`). Ağırlığı pozitif hiçbir boyutu volume'e sığmayan profil için `fs_aging_estimate_files()` `0` döner. |
| `FS_AGING_ERR_FILL_UNREACHABLE` | FILL'de canlı veri art arda `FS_AGING_FILL_STALL_OPS` işlem boyunca yeni bir en yüksek değere çıkmadı | Doluluk hedefine ulaşılamıyor (log: `"fs_aging: FILL makes no progress, target unreachable"`). Genelde `fill_ratio` çok yüksek ya da `volume_bytes` gerçek boş alandan büyük. `fill_ratio`'yu düşürün veya `volume_bytes`'ı gerçek boş alana göre verin. |

`fs_aging_init()` şu durumlarda `FS_AGING_ERR_PARAM` döner:

- `ctx`, `profile`, `config`, `platform` veya `pool` NULL ise, `pool_capacity` 0 ya da `0xFFFFFFFF` ise.
- `create_file`, `delete_file` veya `make_dir` NULL ise.
- `mount_point` NULL ise, `volume_bytes == 0` ise, `fill_ratio` (0, 1] aralığında değilse, `confidence` [0, 1) aralığında değilse.
- Profil geçersizse. Geçerli bir profilde yaş satırı 2–48, boyut ve dizin satırı 1–48 adet olmalıdır. Ağırlıklar negatif olamaz ve her dağılımın toplamı sıfırdan büyük olmalıdır. Yaş eşikleri pozitif ve kesin artan olmalıdır. `sibling_dirs` en fazla 65535 olabilir.

### İstatistikler

`fs_aging_get_stats()` şu alanları doldurur: `status`, `phase`, `ops`, `creates`, `deletes`, `live_bytes`, `workload_bytes`, `live_files`, `convergence_ops`, `age_error`, `elapsed_ms`.

- `age_error`: Yaş dağılımının hedefe toplam varyasyon mesafesidir, `0.5 × Σ|gerçek − hedef|` ile hesaplanır (0 mükemmel, 1 en kötü). STABLE başlamadan önce ve yaş kovası yerleştirme sürerken `1.0` raporlanır.
- `fs_aging_get_bin(..., FS_AGING_DIST_AGE, ...)`: FILL boyunca ve yaş kovası yerleştirme sürerken `actual` değeri `0.0` döner. Boyut ve dizin dağılımları her zaman günceldir.
- `elapsed_ms`: `now_ms` NULL ise 0'dır.
- Orijinaldeki `-x/-y/-z` çıktı dosyalarının karşılığı `fs_aging_bin_count()` ile `fs_aging_get_bin()`'dir. Raporu basmak port'un işidir.

## 4. Kullanım örneği

Aşağıdaki kod, katı bayraklarla (`-std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wshadow`) uyarısız derlendi ve sahte bir platformla çalıştırıldı. Callback gövdeleri port'a aittir ([bölüm 7](#7-posix-port-sözleşmesi)).

```c
#include <stdio.h>
#include "fs_aging.h"
#include "fs_aging_profiles.h"

/* Callbacks provided by the port (see section 7) */
fs_aging_plat_result_t my_create(void *user, const char *path, uint64_t size_bytes);
fs_aging_plat_result_t my_delete(void *user, const char *path);
fs_aging_plat_result_t my_mkdir(void *user, const char *path);
uint64_t my_now_ms(void *user);
void my_log(void *user, const char *msg);

#define VOLUME_BYTES (1024ull * 1024u * 1024u)   /* 1 GB */
#define FILL_RATIO   0.8
#define POOL_CAP     10500u     /* agrawal, 1 GB, 0.8: estimate 10494, rounded up */

static fs_aging_file_t pool[POOL_CAP];
static fs_aging_ctx_t  ctx;

int run_aging(void)
{
    const fs_aging_profile_t *pr = fs_aging_profile_find("agrawal");
    fs_aging_platform_t plat;
    fs_aging_config_t   cfg;
    fs_aging_stats_t    st;
    fs_aging_status_t   s;
    uint32_t need;
    uint32_t i;

    if (pr == NULL) {
        return -1;
    }

    /* Pool capacity: use the estimate directly.
     * 0 means no profile size fits on the volume. */
    need = fs_aging_estimate_files(pr, VOLUME_BYTES, FILL_RATIO);
    if ((need == 0u) || (need > POOL_CAP)) {
        return -1;
    }

    plat.create_file = my_create;
    plat.delete_file = my_delete;
    plat.make_dir    = my_mkdir;
    plat.now_ms      = my_now_ms;   /* optional, may be NULL */
    plat.log         = my_log;      /* optional, may be NULL */
    plat.user        = NULL;        /* passed to every callback */

    cfg.mount_point    = "/fs0";    /* no trailing '/'; must stay valid */
    cfg.volume_bytes   = VOLUME_BYTES;
    cfg.fill_ratio     = FILL_RATIO;
    cfg.seed           = 42u;
    cfg.overwrite_runs = 1000u;     /* 0 = no limit */
    cfg.time_limit_s   = 0u;        /* 0 = no limit */
    cfg.confidence     = 0.0;       /* 0 = full convergence */

    s = fs_aging_init(&ctx, pr, &cfg, &plat, pool, POOL_CAP);
    if (s != FS_AGING_RUNNING) {
        printf("init: %s\n", fs_aging_status_str(s));
        return -1;
    }

    /* On ARINC 653: a periodic process calls this once per period */
    do {
        s = fs_aging_step(&ctx, 1000u);
    } while (s == FS_AGING_RUNNING);

    fs_aging_get_stats(&ctx, &st);
    printf("%s: ops=%llu live=%llu B age_error=%.4f\n",
           fs_aging_status_str(st.status),
           (unsigned long long)st.ops,
           (unsigned long long)st.live_bytes, st.age_error);

    for (i = 0u; i < fs_aging_bin_count(&ctx, FS_AGING_DIST_SIZE); i++) {
        fs_aging_float_t ideal;
        fs_aging_float_t actual;
        if (fs_aging_get_bin(&ctx, FS_AGING_DIST_SIZE, i, &ideal, &actual) == 0) {
            printf("size %2u: target %.4f actual %.4f\n", i, ideal, actual);
        }
    }

    return ((st.status == FS_AGING_DONE_CONVERGED) ||
            (st.status == FS_AGING_DONE_TOLERANCE)) ? 0 : -1;
}
```

`fs_aging_config_t` alanları ve orijinaldeki karşılıkları:

| Alan | Orijinal | Açıklama |
|---|---|---|
| `mount_point` | `-m` | Sonunda `/` olmadan, ör. `"/fs0"`. Pointer saklanır, aging boyunca geçerli kalmalıdır. |
| `volume_bytes` | `-n` | Dosya sisteminin toplam boyutu (byte). |
| `fill_ratio` | `-u` | Hedef doluluk, (0, 1] aralığında, ör. `0.8`. Hedefe ulaşılamazsa (ör. 1.0'a çok yakın değerler) çalışma `FS_AGING_ERR_FILL_UNREACHABLE` ile biter. |
| `seed` | `-r` | Rastgelelik tohumu. |
| `overwrite_runs` | `-i` | Toplam yük sınırı `overwrite_runs × volume_bytes`. 0 = sınırsız. |
| `time_limit_s` | `-w` | Süre sınırı, **saniye** cinsinden (orijinalde dakika). 0 = sınırsız. `now_ms` gerekir. |
| `confidence` | `-c` | 0 = tam yakınsama. Ör. 0.9 değeri, yaş hatası 0.1'e indiğinde durur. [0, 1) aralığında olmalıdır. |

## 5. Bellek boyutlandırma

Çekirdek bellek ayırmaz; iki blok çağıran tarafta durur.

**Dosya kayıt havuzu (`fs_aging_file_t[]`).** Canlı her dosya için bir kayıt tutulur. Kayıt boyutu **24 byte**'tır: 20 byte alan (`uint64_t` + `uint32_t` + 4 × `uint16_t`) + 8 byte hizalama için 4 byte dolgu (host'ta `sizeof` ile doğrulandı; farklı hizalamalı ABI'lerde hedefte `sizeof` ile ölçün). Yol saklanmaz, ihtiyaç anında üretilir.

- Kapasite `fs_aging_estimate_files(profile, volume_bytes, fill_ratio)` ile bulunur. Dönen değer FILL aşaması için **doğrudan `pool_capacity` olarak kullanılır; ek %25 pay gerekmez.** Güvenlik iki parçalıdır: kesin bir üst sınır (aşağıda 4. adım) ve yenileme modelinde havuzun yetmeme olasılığını `10⁻⁶` ile sınırlayan bir olasılık sınırı (2. adım). Model, FILL'deki `NO_SPACE` + silme etkileşimini ve silinecek dosya seçimini kesin olarak kapsamaz (bunların dosya sayısını yalnızca azalttığı varsayılır); bu yüzden sonuç ayrıca aşağıdaki geniş ampirik doğrulamayla sınandı ve hiç aşılmadı.
- `0` döner (geçerli kapasite değildir): hiçbir boyut volume'e sığmıyorsa, `profile` NULL/boş ise, `size_count > FS_AGING_MAX_BINS` ise ya da `fill_ratio` (0, 1] dışında ise.
- `0xFFFFFFFF` döner (geçerli kapasite değildir; `fs_aging_init()` reddeder): sonuç 32 bite sığmıyorsa ya da güvenli sınır yoksa. İkincisi, volume'e sığan 0 byte'lık bir boyut varken FILL boyunca her zaman sığan pozitif boyut olmadığında olur (ör. `meyer` veya `wang_os` ile `fill_ratio` 1.0).
- ARINC 653'te havuz statik bir dizidir; boyutu partition bellek bütçesine göre derleme zamanında seçilir.

**Context (`fs_aging_ctx_t`).** Bu host'ta (x86_64, MSYS2 UCRT64 gcc 16.2, varsayılan limitler) `sizeof(fs_aging_ctx_t) = 3024` byte'tır. Bu sayı **host'a özeldir**: pointer boyutu, hizalama ve `FS_AGING_MAX_BINS` / `FS_AGING_PATH_MAX` değerleri değiştikçe değişir. Hedef platformda `sizeof` ile ölçün. Context'in alanları dahilidir ve değiştirilmemelidir.

### `fs_aging_estimate_files()` nasıl hesaplar

Havuz ihtiyacı, **STABLE'a geçmeden önce (FILL boyunca) ulaşılan en yüksek canlı dosya sayısıdır.** STABLE'da havuzun dolması hata değildir (aşağıya bakın). Tahmin libm kullanmaz (karekök en fazla 2048 adımlık bir Newton döngüsüyle alınır, pratikte birkaç on adım), özyineleme yoktur, maliyeti en fazla 48 × (48 + 2048) adımdır.

1. **FILL boyunca her zaman sığan boyutlar:** FILL'de her oluşturmadan önce canlı veri hedef `T = volume × fill_ratio`'nun altındadır; bu yüzden `size_bytes <= volume − T` olan boyutlar FILL boyunca her zaman sığar. Ağırlığı pozitif bu boyut satırları alınır, ağırlıklar bunlar üzerinde yeniden normalize edilir. Daha büyük boyutlar ya hiç sığmaz (ör. `pramod`'daki 32 GiB) ya da yalnızca volume henüz boşken sığar (`volume − T < size <= volume`); sığmadıklarında `NO_SPACE` alıp yerine silme yaptırırlar. İkisi de dosya sayısını yalnızca azaltır. Eski tahmin bunları ortalamaya katıyor ve dosya sayısını çok küçük buluyordu. (Önceki taslak `size <= volume` alıyordu; ör. `pramod` 16 MB 0.95 seed 849 ve `wang_os` 4 MB 0.8 seed 152 bu yüzden `POOL_FULL` veriyordu.)
2. **Yenileme, olasılık sınırıyla:** Hedef `T`'ye ulaşmak için gereken dosya sayısı `N`, ilk `n` dosyanın toplamı `S_n ≥ T` olan en küçük `n`'dir; `N > n` ancak `S_n < T` ise olur. Negatif olmayan bağımsız boyutlar için normal yaklaşım gerektirmeyen bir alt kuyruk sınırı vardır: `P(S_n ≤ n·μ − t) ≤ exp(−t² / (2·n·m₂))` (μ ortalama, `m₂ = E[size²]`; etkin boyut dağılımı üzerinden). Bu olasılığı `δ = 10⁻⁶` yapan en küçük `n`, `n·μ − a·√n = T` denkleminin çözümüdür (`a = sqrt(2·ln(1/δ)·m₂)`): `√n = (a + sqrt(a² + 4·μ·T)) / (2·μ)`. Tahmin `n + 17`'dir (16 sabit pay + yukarı yuvarlama için 1; sonuç tamsayıya aşağı yuvarlanır). Önceki taslaktaki `T/μ + k·sqrt(T·σ²/μ³)` doğrusal yaklaşımı, ağır kuyruklu profillerde küçük `T`'de `a²/(4μ²)` mertebesindeki terimi atlıyordu ve yetmiyordu (ör. `wang_os` 16 MB 0.3 seed 1892: gereken 311, tahmin 301).
3. **Kırpma (ağır kuyruklu profiller):** Boyutlar bir `c` eşiğinde kırpılırsa (`min(size, c)`) toplam yalnızca azalır, dolayısıyla gereken dosya sayısı ancak artar; her `c` için hesaplanan değer geçerli bir üst sınırdır (aynı `δ` ile). 1. adımdaki her boyut `c` olarak denenir ve en küçük sonuç alınır. Bu, birkaç çok büyük dosyanın `m₂`'yi şişirdiği `dabre`, `grundman`, `pramod` gibi profillerde havuzu belirgin biçimde küçültür.
4. **Kesin üst sınır:** Oluşturmadan önce canlı veri en fazla `T − 1` byte'tır ve her dosya en az `s_min` byte'tır (`s_min`: ağırlığı pozitif, volume'e sığan en küçük pozitif boyut). Bu yüzden FILL'de dosya sayısı `floor((T − 1) / s_min) + 1`'i geçemez. Volume'e sığan 0 byte'lık boyut yoksa sonuç bu sınırla kırpılır (ör. tek boyutlu profillerde tahmin tam bu değerdir). 1. adımda hiç pozitif boyut kalmazsa (ör. `fill_ratio` 1.0) yalnızca bu sınır kullanılır; 0 byte'lık boyut da varsa sınır yoktur ve `0xFFFFFFFF` döner.

**Doğrulama** (sahte platform, yalnızca FILL, havuz = tahmin, **ek pay yok**). Ölçülen değer "gereken en küçük havuz"dur: FILL'deki oluşturma denemelerinde `canlı dosya + 1`'in en büyüğü (havuz bundan küçükse `POOL_FULL` olur). Sahte platform dosya tablosu tutmaz, kapasiteyi çekirdeğin canlı veri sayacıyla karşılaştırır; tablo taşması olmadığından atlanan kombinasyon yoktur.

- Ana ızgara: 8 profil × seed 1–200 × volume {16 MB, 64 MB, 256 MB, 1 GB, 4 GB} × `fill_ratio` {0.5, 0.8, 0.95} = 24000 çalışma. Hiçbirinde `FS_AGING_ERR_POOL_FULL` oluşmadı.
- Izgara dışı: seed 1–3000 × volume {1, 2, 4, 8, 16} MB × `fill_ratio` {0.1, 0.3, 0.5, 0.7, 0.8, 0.9, 0.95, 0.99} (8 profil, 960000 çalışma) ve seed 201–1000 × {64, 256} MB × {0.5, 0.8, 0.95} (38400 çalışma). Hiçbirinde `POOL_FULL` oluşmadı; en yüksek "gereken / tahmin" oranı küçük volume'lerde 0,775 (`dabre` 16 MB 0.9 seed 444: 1563 / 2017), 64/256 MB'ta 0,868 (`wang_lanl` 256 MB 0.95 seed 288: 1213 / 1398). Önceki taslaklarda `POOL_FULL` veren `pramod` 16 MB 0.95 seed 849 (gereken 965, tahmin 1844), `wang_os` 4 MB 0.8 seed 152 (258 / 494) ve `wang_os` 16 MB 0.3 seed 1892 (311 / 536) artık geçer.
- `fill_ratio` 1.0 ızgarada yoktur: FILL bu oranda çoğu zaman hedefe ulaşamaz ve `FS_AGING_ERR_FILL_UNREACHABLE` ile biter; tahmin orada yalnızca 4. adımdaki kesin sınırdır.

Ana ızgarada profil başına en yüksek "gereken havuz / tahmin" oranı:

| Profil | En yüksek gereken / tahmin | En kötü durum (volume, fill, seed: gereken / tahmin) |
|---|---:|---|
| agrawal | 0,934 | 4 GB, 0.8, 177: 33168 / 35530 |
| dabre | 0,743 | 1 GB, 0.8, 39: 31119 / 41870 |
| douceur | 0,952 | 4 GB, 0.95, 18: 95781 / 100657 |
| grundman | 0,767 | 1 GB, 0.8, 174: 17381 / 22660 |
| meyer | 0,774 | 4 GB, 0.8, 118: 10028 / 12951 |
| pramod | 0,728 | 4 GB, 0.95, 118: 33092 / 45441 |
| wang_lanl | 0,961 | 4 GB, 0.95, 29: 17801 / 18517 |
| wang_os | 0,935 | 4 GB, 0.8, 81: 40474 / 43272 |

Tahminler (`fill_ratio` 0.8) ve 200 seed'de ölçülen en büyük gereken havuz. "Eski" önceki `volume × fill_ratio / ortalama_boyut + 1` tahminidir; bellek maliyeti kayıt başına 24 byte'tır:

| Profil | 256 MB eski | eski +%25 | **yeni** | yeni / eski | 200 seed gereken | 1 GB eski | eski +%25 | **yeni** | yeni / eski | 200 seed gereken |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| agrawal | 1879 | 2348 | **3636** | ×1,94 | 2498 | 7515 | 9393 | **10494** | ×1,40 | 8985 |
| dabre | 2354 | 2942 | **13990** | ×5,94 | 9808 | 9413 | 11766 | **41870** | ×4,45 | 31119 |
| douceur | 4766 | 5957 | **7305** | ×1,53 | 5706 | 19063 | 23828 | **24007** | ×1,26 | 21054 |
| grundman | 1838 | 2297 | **8190** | ×4,46 | 5545 | 7349 | 9186 | **22660** | ×3,08 | 17381 |
| meyer | 434 | 542 | **2155** | ×4,97 | 1140 | 1733 | 2166 | **4851** | ×2,80 | 3050 |
| pramod | 674 | 842 | **5810** | ×8,62 | 3143 | 2695 | 3368 | **14052** | ×5,21 | 8637 |
| wang_lanl | 918 | 1147 | **1205** | ×1,31 | 1006 | 3670 | 4587 | **4195** | ×1,14 | 3822 |
| wang_os | 2344 | 2930 | **4136** | ×1,76 | 3008 | 9376 | 11720 | **12480** | ×1,33 | 10764 |

- Büyük artış, eski tahminin gerçekten yetmediği profillerdedir. Bu profillerde FILL tepesi seed'e göre çok değişir (ör. `dabre` 256 MB: 200 seed'de 1303–9808); güvenli kapasite bu yayılımı kapsamak zorundadır.
- Hafif profillerde (`wang_lanl`, `douceur` 1 GB, `wang_os` 1 GB) yeni değer eski "+%25" değerine yakın ya da ondan küçüktür.
- Küçük volume'lerde oran daha büyüktür (sabit pay ve olasılık sınırındaki `a²/(4μ²)` terimi `T`'den bağımsızdır); ör. 16 MB, 0.8'de yeni tahmin eski tahminin 2,7 (`wang_lanl`: 58 → 156) ile 34 (`pramod`: 43 → 1474) katıdır; buna karşın en büyüğü 1815 kayıttır (`dabre`, ~43 KB). Bellek sorun olursa `fill_ratio` ve volume için `fs_aging_estimate_files()` değeri derleme öncesinde hesaplanıp havuz tam o boyutta seçilmelidir.

### STABLE'da havuzun dolması

STABLE'da canlı dosya sayısı FILL tepesinin üstüne çıkabilir (sapmasız rastgele yürüyüş, bkz. [bölüm 6](#6-orijinal-geriatrixten-farklar)). Havuz bu aşamada dolarsa **hata oluşmaz**: oluşturma yerine silme yapılır (canlı dosya sayısı havuz kapasitesine eşit olduğundan silinecek dosya her zaman vardır). Bu yalnızca canlı dosya sayısına bir üst sınır koyar. Ancak işlem dizisini değiştirir: havuz kapasitesi farklıysa aynı seed STABLE'da farklı sonuç verebilir. Ör. seed 42 ile `wang_os` 256 MB'ta eski havuz (2930) STABLE'da 148 adımda doluydu, yeni havuzla (4136) hiç dolmuyor (en yüksek canlı dosya 3024); `douceur` 64 MB'ta eski havuz (1490) 4517 adımda, yeni havuz (2432) 909 adımda dolu. STABLE'ın havuz sınırına hiç takılmaması isteniyorsa, aynı profil, volume ve seed ile önceden yapılan bir çalıştırmada tüm çalışmanın tepe değeri ölçülüp havuz onun üstünde seçilebilir.

### Eski tahminle görülen `ERR_POOL_FULL` örnekleri

Eski tahmin (ortalama boyuttan, önerilen +%25 payla) şu çalışmalarda FILL'de `FS_AGING_ERR_POOL_FULL` veriyordu: `agrawal` 256 MB seed 8; `meyer` 1 GB seed 3, 4, 8; `grundman` 1 GB seed 3; `douceur` 64 MB seed 4; `dabre` ve `pramod` birçok volume'de (ör. seed 42 ile 256 MB ve 1 GB'ta; volume'e sığmayan 2 GiB / 32 GiB'lık boyutlar ortalamayı şişiriyordu). Yeni tahmin bu örneklerin hepsini yukarıdaki 24000 çalışmalık doğrulamada kapsar (aynı seed'ler 1–200 aralığındadır).

## 6. Orijinal Geriatrix'ten farklar

Bilinçli farklar (çekirdekle ilgili olanların bir kısmı — PRNG, silinecek dosya seçimi, `confidence`, yaş kovası yerleştirme, silinecek dosya yokken davranış ve havuz tahmini — `fs_aging.c` başında da yazılıdır; kararlı aşamadaki sapma notu `stable_one()` üstündedir):

- **PRNG:** Dahili xorshift64* kullanılır (seed splitmix64 ile karıştırılır). Aynı seed her platformda aynı işlem dizisini üretir. Ancak orijinalle bire bir aynı dizi üretilmez.
- **Silinecek dosya seçimi:** Öncelik orijinaldeki gibidir: en çok fazla temsil edilen yaş kovası, sonra boyut kovası, sonra dizin kovası. Fark şudur: tüm kovalar taranmaz, `FS_AGING_DELETE_SAMPLES` (32) rastgele aday arasından en iyisi seçilir. Bu sayede işlem başına maliyet sabit kalır ve dosya başına 24 byte yeter.
- **Yaş kovası:** Dosyanın doğum tick'inden hesaplanır. Kararlı aşamada eşikler sabittir.
- **`confidence`:** Yaş dağılımının toplam varyasyon mesafesine göre yorumlanır (0.9 → hata ≤ 0.1). Ki-kare CDF'si ve libm kullanılmaz.
- **Yaş kovası yerleştirme:** FILL'den STABLE'a geçişte havuz tek seferde taranmaz. Her işlemde `FS_AGING_REBIN_CHUNK` kayıt taranır. Bu ara işlemler tick ve rastgele sayı tüketmez; bu yüzden işlem dizisi tek seferlik taramayla aynıdır.
- **Olmayan seçenekler:** Çok thread (`-t`) ve boşta bekleme (`-p`) yoktur. `-q` (bitişte sorma) ve `-x/-y/-z` (dağılım dosyaları) port'un işidir; çekirdek bunlar için `fs_aging_get_bin()` sunar. Sahte mod (`-f`) yoktur; gerekirse sahte bir `fs_aging_platform_t` yazılabilir. Backend seçimi (`-b`) yerine `fs_aging_platform_t` vardır.
- **Süre sınırı:** `time_limit_s` saniye cinsindendir (orijinal `-w` dakika).
- **Yer yetersizliği:** Orijinal, `fallocate` başarısız olunca bekleyip aynı dosyayı tekrar dener. Burada platform `FS_AGING_PLAT_NO_SPACE` dönerse çekirdek aynı işlemde bir dosya siler ve devam eder (canlı dosya yoksa silmez, bir sonraki maddeye bakın).
- **Silinecek dosya yokken ("dosya yoksa silmek yerine oluştur"):** Orijinal, silinecek dosya bulamazsa `"Cannot delete a single file of any size!"` basıp `exit(1)` ile çıkar (`geriatrix.cpp:759-762`). Burada çalışma durmaz: STABLE'da canlı dosya yokken yazı-tura silme dese de oluşturma yapılır; FILL'de volume boşken `NO_SPACE` alan oluşturmanın yerine silme denenmez: boyut volume'den büyükse işlem tüketilir ve sonraki işlem yeni bir boyut çeker; volume'e sığan bir boyutsa aynı işlemde en küçük boyut denenir. Yalnızca oluşturma yapılamıyor ve silinecek dosya da yoksa (gerçek çıkmaz) `FS_AGING_ERR_PLATFORM` ile durulur. Karar, volume'e sığan bir boyutun denendiği ilk işlemde verilir; çalışma takılmaz (bkz. [durum kodları](#durum-kodları-fs_aging_status_t)). Yazı-tura her işlemde yine çekilir; daha önce hata vermeyen yollarda rastgele sayı tüketimi ve işlem dizisi değişmedi (ör. Agrawal 256 MB referans sonucu aynıdır). Önceden hata veren örnekler artık tamamlanır: `agrawal` 16 MB seed 42 (eskiden 2655. işlemde `ERR_PLATFORM`) 4768 işlemde `DONE_CONVERGED`, `douceur` 64 MB seed 3 `DONE_CONVERGED` ile biter.
- **Kayıt havuzu sınırı:** STABLE'da havuz doluysa oluşturma yerine silme yapılır (orijinalde böyle bir sınır yoktur). Bu yalnızca yukarı yönde bir sınırdır.
- **Havuz tahmini:** Orijinal dosya kayıtlarını dinamik bellekte tutar. Burada havuz çağıran taraftan gelir; `fs_aging_estimate_files()` FILL için güvenli kapasiteyi döner (bkz. [bölüm 5](#5-bellek-boyutlandırma)).
- **Bellek ve bağımlılık:** Bellek çağıran taraftan gelir. Boost, C++, libc ve `malloc` kullanılmaz.
- **grundman profili:** `size_distribution.txt` ilk satırda 22 satır bildirir ama 23 veri satırı içerir. Orijinal yalnızca ilk 22 satırı okur. `gen_profiles.py` de aynısını yapar ve son satırı (`4294967296 5`) uyarıyla atlar.

### Kararlı aşamada canlı verinin hedeften sapması (orijinal davranış)

Kararlı aşamada canlı veri `volume × fill_ratio` hedefinden sapabilir. Örneğin Agrawal, 2 GB ve 0.8 hedefte, seed 42 ile kararlı aşama sonunda ~1,48 GB gözlendi (oran 0,9075). Bu **orijinal Geriatrix'in davranışıdır**, implementasyon hatası değildir. **Davranış değiştirilmedi.**

- Orijinal `performStableAging` her işlemde koşulsuz 50/50 yazı-tura ile oluşturma veya silme seçer (`geriatrix.cpp:901-905`, `tossCoin` `535-537`). Doluluk hedefine bakan bir kural yoktur.
- Hedef yalnızca FILL'in sonunu belirler (`874-892`, `1084-1086`). Kararlı aşamadaki `till_size` bir yük sınırıdır (`1095`).
- Oluşturma, yalnızca diske hiçbir boyut sığmazsa silmeye döner (`628`).
- Boyut dağılımı dosya **sayısına** göre tutulur. Bu yüzden canlı byte, canlı dosya sayısını izler.
- Dosya sayısı `T − K` işlem boyunca sapmasız bir rastgele yürüyüştür. Canlı/hedef oranının standart sapması yaklaşık `sqrt(T − K) / N*` olur. Agrawal için bu değer 256 MB'ta ~%20, 1 GB'ta ~%10, 2 GB'ta ~%6–7'dir.
- Bizim `stable_one` aynı yazı-turayı kullanır. 30 tohumla yapılan ölçümlerde bizim kod ile orijinalin karar kurallarının modeli aynı dağılımı verdi (2 GB: ortalama oran 0,98, sapma 0,06–0,075). 2 GB'ta gözlenen 0,9075 oranı yaklaşık −1,5 sigma'lık normal bir sapmadır.
- Ek farklarımız: kayıt havuzu dolunca oluşturmanın silmeye dönmesi (yalnızca yukarı yönde bir sınır) ve canlı dosya sayısı 0'a inince yazı-tura silme dese de oluşturma yapılması (yalnızca aşağı yönde, 0'da bir sınır; orijinal burada çıkar).

Aynı not kodda, `fs_aging.c` içindeki `stable_one()` fonksiyonunun üstünde de yer alır.

Bir seçenek olarak, yazı-tura korunup canlı veri hedefin ±%5 bandı dışına çıkınca işlem türünü zorlayan küçük bir değişiklik sapmayı banda sınırlar. Bu değişiklik orijinalden bilinçli bir sapma olur ve **kullanıcı onayı olmadan uygulanmayacaktır**.

## 7. POSIX port sözleşmesi

Bu bölüm, çekirdeği bir POSIX dosya sistemine bağlayacak ekip içindir. Port, `fs_aging_platform_t` yapısını doldurur. Çekirdek dosya sistemine başka hiçbir yoldan erişmez. `fs_aging.h` içindeki **public API** (fonksiyon imzaları, `fs_aging_platform_t`, profil ve config tipleri) iki ekip arasındaki sözleşmedir ve tek taraflı değiştirilmez.

```c
typedef struct {
    fs_aging_plat_result_t (*create_file)(void *user, const char *path, uint64_t size_bytes);
    fs_aging_plat_result_t (*delete_file)(void *user, const char *path);
    fs_aging_plat_result_t (*make_dir)(void *user, const char *path);
    uint64_t (*now_ms)(void *user);            /* opsiyonel */
    void (*log)(void *user, const char *msg);  /* opsiyonel */
    void *user;
} fs_aging_platform_t;
```

### Dönüş kodları

| Kod | `create_file` sonrası çekirdek | `delete_file` / `make_dir` sonrası çekirdek |
|---|---|---|
| `FS_AGING_PLAT_OK` | Dosyayı kaydeder, `size_bytes` kadar canlı veri sayar. | Devam eder. |
| `FS_AGING_PLAT_NO_SPACE` | Dosya **oluşmamış** kabul edilir. Aynı işlemde bir dosya silinir ve aging devam eder. Silinecek dosya yoksa silme yapılmaz ve aging devam eder (FILL'de volume'e sığan bir boyut boş volume'de `NO_SPACE` aldıysa aynı işlemde en küçük boyutla bir `create_file` daha yapılır); yalnızca oluşturma yapılamıyor ve silinecek dosya yoksa (gerçek çıkmaz, bkz. durum kodları) `FS_AGING_ERR_PLATFORM`. FILL'de `NO_SPACE` yüzünden hedefe ulaşılamıyorsa (gerçek boş alan hedeften küçük) `FS_AGING_ERR_FILL_UNREACHABLE`. | `FS_AGING_ERR_PLATFORM`, aging durur. |
| `FS_AGING_PLAT_ERROR` | `FS_AGING_ERR_PLATFORM`, aging durur. | `FS_AGING_ERR_PLATFORM`, aging durur. |

`NO_SPACE` yalnızca `create_file` için anlamlıdır. `delete_file` ve `make_dir`, `OK` dışındaki **her** değerde aging'i durdurur.

### `create_file(user, path, size_bytes)`

- `path` yolunda **tam olarak `size_bytes` byte** boyutunda, yeri gerçekten ayrılmış bir dosya oluşturmalıdır. Bunun için yer ayırma (`posix_fallocate`) ya da veri yazma kullanılabilir. Orijinal Geriatrix `open(O_RDWR|O_CREAT, 0600)` + `posix_fallocate` + `close` kullanır. Yalnızca `ftruncate` ile oluşturulan seyrek (sparse) dosya blok ayırmadığı için aging etkisi üretmez; yeterli değildir.
- `size_bytes == 0` geçerlidir (ör. `wang_os` profilinde 0 byte dosyalar vardır). Boş dosya oluşturulur.
- `size_bytes` 4 GiB'ı aşabilir (`pramod`'da 32 GiB'a kadar). 64 bit boyutu doğru işleyin.
- Yer yetmiyorsa (`ENOSPC`, `EFBIG` gibi) `FS_AGING_PLAT_NO_SPACE` dönün. Bu durumda **yarım kalan dosyayı silin**. Çekirdek bu dosyayı bilmez; geride kalırsa yer kaplayan yetim bir dosya olur.
- Çekirdek canlı veriyi `size_bytes` toplamı olarak sayar; dosya sisteminin blok yuvarlaması ve kendi yönetim verisi buna dahil değildir. `volume_bytes × fill_ratio` gerçek boş alana sığmıyorsa FILL hedefe ulaşamaz ve çalışma `FS_AGING_ERR_FILL_UNREACHABLE` ile biter. `volume_bytes` olarak gerçek kullanılabilir alanı (ör. `statvfs`: `f_bavail × f_frsize`) verin.
- Diğer tüm hatalarda `FS_AGING_PLAT_ERROR` dönün.
- Çekirdek bir çalışma içinde aynı yolu iki kez oluşturmaz (dosya adı tekil doğum tick'idir). Dosya zaten varsa önceki bir çalışmadan kalmıştır. Aging **boş (yeni biçimlendirilmiş) bir volume'de** başlatılmalıdır. Port, var olan dosyada `ERROR` dönebilir.
- Üst dizinin var olduğu varsayılabilir: dizinler SETUP aşamasında oluşturulmuştur.

### `delete_file(user, path)`

- Çekirdeğin daha önce `OK` ile oluşturduğu bir dosyayı siler (`unlink`). Başarıda `OK` döner.
- Dosya yoksa veya silinemiyorsa `FS_AGING_PLAT_ERROR` dönün; aging durur.
- Çekirdek dizin silmez. Aging bittiğinde dosyalar ve dizinler yerinde kalır (yaşlandırılmış dosya sistemi amaçtır).

### `make_dir(user, path)`

- **Tek seviye** dizin oluşturur (`mkdir`, ör. mod `0755`).
- Dizin zaten varsa **`FS_AGING_PLAT_OK` dönmelidir** (`EEXIST` ve yol bir dizin). Çekirdek aynı üst dizinleri birçok kez ister, çünkü farklı derinlikteki yapraklar aynı üst dizinleri paylaşır.
- Üst dizinleri çekirdek sırayla oluşturur: bir yaprak için önce `<mount>/d1`, sonra `<mount>/d1/d2`, ... şeklinde kök taraftan başlayarak çağırır. Port'un `mkdir -p` yapmasına gerek yoktur.
- Mount point'in kendisi için `make_dir` çağrılmaz. Mount point var olmalı ve yazılabilir olmalıdır.
- Hata durumunda `FS_AGING_PLAT_ERROR` dönün.

### `now_ms(user)` — opsiyonel

- **Monoton** zaman, milisaniye (ör. `clock_gettime(CLOCK_MONOTONIC)`). Duvar saati kullanmayın; geçen süre işaretsiz fark (`now − start`) ile hesaplanır.
- `fs_aging_init()` içinde bir kez (başlangıç), her `fs_aging_step()` sonunda bir kez çağrılır.
- NULL ise `elapsed_ms` 0 kalır ve `time_limit_s` etkisizdir.

### `log(user, msg)` — opsiyonel

- Aşama geçişleri, hata ve bitiş mesajları. Mesajlar İngilizce, ASCII ve satır sonu (`\n`) içermez (ör. `"fs_aging: FILL done, STABLE started"`).
- `msg` yalnızca çağrı süresince geçerli kabul edilmelidir; saklamak gerekirse kopyalayın.
- NULL ise mesaj basılmaz.

### `user`

- Çekirdek bu pointer'ı yorumlamaz; her callback'e ilk argüman olarak aynen iletir. Port'un durumunu (ör. mount bilgisi, istatistik, dosya tanımlayıcı tamponu) taşımak içindir.

### Yol tamponu ve ömürler

- `path` argümanı çekirdeğin `ctx->path_buf` tamponunu gösterir. **Yalnızca çağrı süresince geçerlidir**; sonraki çağrıda üzerine yazılır. Pointer'ı saklamayın, gerekiyorsa kopyalayın. `const`'tur, değiştirmeyin.
- `fs_aging_init()`, `config` ve `platform` yapılarını context'e **kopyalar**; bu yapılar `init`'ten sonra yok olabilir. Ancak pointer olarak saklananlar aging boyunca geçerli kalmalıdır: `config.mount_point` dizgesi, `profile` (ve tabloları), `pool` dizisi, `ctx` ve `user`'ın gösterdiği veri.
- Havuz ve context'e çağıran taraf aging süresince dokunmamalıdır.

### Eşzamanlılık ve yeniden girme

- Çekirdek thread kullanmaz ve kilit içermez. Bir context aynı anda tek bir thread'den kullanılmalıdır. `fs_aging_get_stats()` / `fs_aging_get_bin()` de, `fs_aging_step()` çalışırken başka bir thread'den çağrılmamalıdır.
- Callback'ler, `fs_aging_step()` (ve `now_ms`/`log` için `fs_aging_init()`) çağrısını yapan thread'de, senkron olarak çağrılır.
- Callback'ler aynı context üzerinde `fs_aging_*` fonksiyonlarını **çağırmamalıdır** (yeniden girme desteklenmez).
- Çekirdekte global değişken durum yoktur. Farklı context'ler (farklı havuzlar ve mount point'ler ile) bağımsız çalışabilir. Hazır profil tabloları `const`'tur ve paylaşılabilir.

### Yol formatı

```
<mount>/d1/d2/.../d(k-1)/<yaprak>/f<doğum_tick>
```

- `k`, profilin dizin satırındaki derinliktir. Ara seviyeler `d1 ... d(k-1)` olarak adlandırılır.
- Yaprak dizinin adı, satırda kardeş dizin sayısı `n > 0` ise `d1 ... dn`'den biri, `n = 0` ise `d<k>`'dir.
- Derinlik 0'da dosya doğrudan mount altına yazılır (`wang_lanl` ve `wang_os` profillerinde tüm dosyalar böyledir).
- Dosya adı `f` + doğum tick'inin ondalık yazımıdır (başında sıfır yoktur). Tick 1'den başlar ve en fazla 20 haneye ulaşabilir.

Örnekler (mount `/fs0`, `agrawal` profili):

| Dizin satırı | Yaprak dizinler (üstleri de oluşturulur) | Örnek dosya |
|---|---|---|
| derinlik 1, kardeş 1 | `/fs0/d1` | `/fs0/d1/f17` |
| derinlik 2, kardeş 5 | `/fs0/d1/d1` ... `/fs0/d1/d5` | `/fs0/d1/d3/f1042` |
| derinlik 3, kardeş 13 | `/fs0/d1/d2/d1` ... `/fs0/d1/d2/d13` | `/fs0/d1/d2/d7/f88123` |
| derinlik 0 (`wang_os`) | — | `/fs0/f5` |

### Yol uzunluğu

`fs_aging_init()`, profilin üretebileceği en uzun yolu (`'\0'` dahil, tick için 20 hane ayırarak) `FS_AGING_PATH_MAX` (varsayılan 256) ile karşılaştırır. Aşıyorsa `FS_AGING_ERR_PATH_TOO_LONG` döner. Mount `/fs0` için `gen_profiles.py` raporu:

| Profil | Yaş / boyut / dizin satırı | En uzun yol (byte, `'\0'` dahil) |
|---|---|---:|
| agrawal | 10 / 11 / 15 | 77 |
| dabre | 14 / 22 / 14 | 70 |
| douceur | 20 / 25 / 16 | 78 |
| grundman | 9 / 22 / 26 | 118 |
| meyer | 24 / 29 / 21 | 98 |
| pramod | 16 / 26 / 37 | **162** |
| wang_lanl | 12 / 11 / 1 | 27 |
| wang_os | 10 / 15 / 1 | 27 |

- Başka bir mount point için her değere `strlen(mount) − 4` ekleyin (ör. `/mnt/very/long/mountpoint` ile pramod 183 olur). `py -3 TOOLS/gen_profiles.py --mount <yol>` aynı raporu basar.
- Port'un dosya sistemi yol limiti bu değerlerden küçükse ilgili profil çalıştırılamaz. Dosya sisteminin dizin derinliği limiti de en az profilin en büyük derinliği kadar olmalıdır (`pramod` için 36 seviye + dosya).

### POSIX port'u için kontrol listesi

- [ ] `create_file` tam `size_bytes` kadar yer ayırıyor (sparse değil), 0 byte'ı ve > 4 GiB'ı işliyor.
- [ ] `create_file` yer yetmediğinde yarım dosyayı silip `NO_SPACE`, diğer hatalarda `ERROR` dönüyor.
- [ ] `make_dir` var olan dizinde `OK` dönüyor.
- [ ] `delete_file` başarıda `OK`, aksi halde `ERROR` dönüyor.
- [ ] `now_ms` monoton; `log` satır sonu eklemeden yazıyor.
- [ ] `path` pointer'ı saklanmıyor; `mount_point`, profil, havuz ve context aging boyunca yaşıyor.
- [ ] Aging boş bir volume'de başlıyor; `volume_bytes` gerçek kullanılabilir kapasiteyi yansıtıyor.
- [ ] `fs_aging_step()` döngüsü `FS_AGING_RUNNING` dışında bir değer gelene kadar sürüyor; bitişte `fs_aging_get_stats()` ve `fs_aging_get_bin()` ile rapor basılıyor.

## 8. Profiller ve `gen_profiles.py`

Hazır 8 profil `FS_AGING_CORE/include/fs_aging_profiles.h` içinde bildirilir:

- `fs_aging_profile_agrawal`, `_dabre`, `_douceur`, `_grundman`, `_meyer`, `_pramod`, `_wang_lanl`, `_wang_os`
- `fs_aging_profiles[FS_AGING_PROFILE_COUNT]` (alfabetik sıra, `FS_AGING_PROFILE_COUNT` = 8)
- `fs_aging_profile_find(name)`: Profil adı Geriatrix'teki klasör adıdır (ör. `"agrawal"`).

Tablolar `FS_AGING_CORE/src/fs_aging_profiles.c` içindedir. Bu dosya **otomatik üretilir, elle düzenlenmez.** Değişiklik gerekiyorsa script ya da kaynak profil dosyaları değiştirilip dosya yeniden üretilir.

```
py -3 TOOLS/gen_profiles.py                 # Windows
python3 TOOLS/gen_profiles.py               # Linux
```

| Seçenek | Varsayılan | Açıklama |
|---|---|---|
| `--geriatrix <klasör>` | `<repo>/../geriatrix`, yoksa `<repo>/geriatrix` | Geriatrix klonu (`profiles/<ad>/*.txt` okunur) |
| `--out <dosya>` | `FS_AGING_CORE/src/fs_aging_profiles.c` | Çıktı dosyası |
| `--mount <yol>` | `/fs0` | Yalnızca yol uzunluğu raporunu etkiler; üretilen dosyayı değiştirmez |

- Yalnızca Python standart kütüphanesini kullanır.
- Çıktı deterministiktir: aynı girdi aynı dosyayı üretir (zaman damgası ve mutlak yol yok, ASCII, `\n` satır sonu).
- `FS_AGING_MAX_BINS` ve `FS_AGING_PATH_MAX` değerlerini `fs_aging.h`'den okur.
- Profil dosyalarını katı biçimde ayrıştırır. Bozuk sayı, satır sayısı uyuşmazlığı, artmayan yaş eşikleri, negatif ya da tamamen sıfır ağırlıklar, `FS_AGING_MAX_BINS`'i aşan satır sayısı gibi durumlarda `gen_profiles: ERROR: <dosya>:<satır>: ...` mesajıyla çıkış kodu 1 döner. Dosya sonundaki "Format:" açıklama bloğu yok sayılır.
- Bilinen tek istisna `grundman/size_distribution.txt` dosyasıdır (bkz. [bölüm 6](#6-orijinal-geriatrixten-farklar)). Bu dosya için `WARNING` basılır.
- Standart çıktıda her profil için satır sayıları ve en uzun yol raporlanır.

Profil formatı (Geriatrix): ilk satır veri satırı sayısıdır. Ardından yaş dosyasında `<eşik> <ağırlık>`, boyut dosyasında `<byte> <ağırlık>`, dizin dosyasında `<derinlik> <ağırlık> <kardeş_dizin>` satırları gelir.

## 9. Lisans

Bu kütüphane Geriatrix'in türev çalışmasıdır. Algoritma ve `FS_AGING_CORE/src/fs_aging_profiles.c` içindeki profil verileri orijinal Geriatrix reposundan alınmıştır:

- Copyright (c) 2018, Carnegie Mellon University. All rights reserved.
- Lisans: BSD-2-Clause. Tam metin repo kökündeki [`LICENSE.geriatrix`](LICENSE.geriatrix) dosyasındadır ve korunmalıdır.

BSD-2-Clause gereği kaynak dağıtımlarında telif bildirimi, koşullar ve sorumluluk reddi korunmalıdır. İkili dağıtımlarda da belgelerde yer almalıdır. Kaynak dosyalardaki orijinal Geriatrix referansları (`https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)`) silinmemelidir.
