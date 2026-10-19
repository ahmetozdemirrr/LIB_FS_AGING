# LIB_FS_AGING

Dosya sistemi yaşlandırma (aging) kütüphanesi. Saf C99 ile yazılmış, platformdan bağımsız bir çekirdek (CORE) ve bu çekirdeği bir işletim sistemine bağlayan port katmanlarından oluşur.

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
9. [Testler](#9-testler)
10. [Lisans](#10-lisans)

---

## 1. Amaç ve kaynak

Kütüphane, [Geriatrix](https://github.com/saurabhkadekodi/geriatrix) dosya sistemi yaşlandırma aracının **işlevsel karşılığıdır**. Geriatrix, bir profile göre kontrollü dosya oluşturma ve silme işlemleri yaparak dosya sisteminde gerçekçi dosya ve boş alan parçalanması üretir.

- Makale: Saurabh Kadekodi, Vaishnavh Nagarajan, Gregory R. Ganger, Garth A. Gibson, *"Geriatrix: Aging what you see and what you don't see. A file system aging approach for modern storage systems"*, USENIX ATC 2018. PDF: <http://cs.cmu.edu/~saukad/geriatrix.pdf>
- Orijinal kod C++11 ile yazılmıştır ve Boost'a bağımlıdır. Lisansı BSD-2-Clause'dur (bkz. [Lisans](#10-lisans)).

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
├── CORE/                        # platformdan bağımsız çekirdek (saf C99)
│   ├── include/
│   │   ├── fs_aging.h           # dış API, platform arayüzü, tipler, derleme limitleri
│   │   └── fs_aging_profiles.h  # hazır 8 profilin bildirimleri
│   └── src/
│       ├── fs_aging.c           # tüm aging mantığı
│       └── fs_aging_profiles.c  # ÜRETİLİR (TOOLS/gen_profiles.py), elle düzenlenmez
├── ARINC653/                    # HENÜZ YAZILMADI (Görev 4, gerçek APEX header'ları bekleniyor)
├── TEST/                        # host (PC) testleri
│   ├── Makefile                 # make / make test / make clean
│   ├── src/
│   │   ├── host_test.c          # sahte platformla tam aging çalıştırması
│   │   └── edge_tests.c         # hata ve sınır durumu senaryoları
│   └── build/                   # make çıktısı (git'e girmez)
└── TOOLS/
    └── gen_profiles.py          # Geriatrix profil .txt dosyaları -> C tabloları
```

- **ARINC653/** henüz yok. Planlanan içerik `ARINC653/include/fs_aging_arinc653.h`, `ARINC653/src/fs_aging_arinc653.c` (callback'ler) ve `ARINC653/src/fs_aging_arinc653_runner.c` (INIT modunda kurulum, periyodik process). Bu iş, gerçek APEX, `LIB_FILE_OPERATIONS`, `LIB_USER_PRINT` header'ları ve `float8_gt` tanımı gelene kadar bekliyor.
- **POSIX port'u** bu repoda yazılmaz. Ayrı ekip tarafından [bölüm 7](#7-posix-port-sözleşmesi)'deki sözleşmeye göre yazılır.
- Referans için klonlanan `geriatrix/` klasörü (varsa) salt okunurdur ve `.gitignore` ile repo dışında tutulur.

Bir projeye eklemek için gereken tek şey: include yolu `CORE/include`, derlenecek dosyalar `CORE/src/fs_aging.c` ve hazır profiller kullanılacaksa `CORE/src/fs_aging_profiles.c`.

> **libc'siz hedefler (ARINC 653 / RTOS):** Kaynak kod hiçbir libc fonksiyonu çağırmaz. Ancak gcc, normal (hosted) derlemede `-O2`/`-O3` ile `fs_aging_init()` içindeki bayt sıfırlama döngüsünü bir `memset` çağrısına çevirir; nesne dosyasında tanımsız `memset` sembolü oluşur (`nm -u` ile görülür). Hedefte `memset` yoksa CORE'u `-ffreestanding` (ya da `-fno-tree-loop-distribute-patterns`) ile derleyin; bu host'ta `-ffreestanding` ile `fs_aging.o` ve `fs_aging_profiles.o`'da tanımsız sembol kalmadığı doğrulandı. Diğer seçenek, hedefin `memset` sağlamasıdır.

### Derleme zamanı limitleri

`fs_aging.h` içindeki şu makrolar derleyicide `-D` ile değiştirilebilir:

| Makro | Varsayılan | Anlamı |
|---|---|---|
| `FS_AGING_PATH_MAX` | `256u` | Mount point dahil en uzun yol, sonlandırıcı `'\0'` dahil |
| `FS_AGING_MAX_BINS` | `48u` | Bir profildeki yaş, boyut ve dizin satırı sayısı üst sınırı |
| `FS_AGING_DELETE_SAMPLES` | `32u` | Silinecek dosya seçilirken bakılan rastgele aday sayısı |
| `FS_AGING_REBIN_CHUNK` | `1024u` | FILL sonundaki yaş kovası yerleştirmede işlem başına taranan kayıt sayısı (en az 1) |

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
| `fs_aging_estimate_files(profile, volume_bytes, fill_ratio)` | FILL sonunda beklenen yaklaşık dosya sayısı (havuz boyutlandırma için). |
| `fs_aging_status_str(status)` | Durum kodunun okunabilir (ASCII) metni. |
| `fs_aging_profile_find(name)` | Hazır profili adıyla bulur (`fs_aging_profiles.h`). Tam ve büyük/küçük harf duyarlı eşleşme yapar; bulunamazsa `NULL` döner. |

### Aşamalar

Tüm aşamalar `fs_aging_step()` içinde, parça parça ilerler. Güncel aşama `stats.phase` alanında görülür.

1. **SETUP** — Profilin gerektirdiği dizin ağacı oluşturulur. Her işlemde bir yaprak dizin ve gerekiyorsa üst dizinleri oluşturulur.
2. **FILL** — Volume, `volume_bytes × fill_ratio` canlı veriye ulaşana kadar profil dağılımından rastgele örneklenen boyutlarla doldurulur.
   - **Yaş kovası yerleştirme (ara adım):** Doldurma hedefe ulaşınca, FILL'de oluşan dosyalar her işlemde en fazla `FS_AGING_REBIN_CHUNK` kayıt taranarak yaş kovalarına yerleştirilir. Bu sırada aşama hâlâ `FS_AGING_PHASE_FILL` olarak raporlanır. Dosya oluşturulmaz ya da silinmez; `stats.ops` (tick) ilerlemez ve rastgele sayı çekilmez.
3. **STABLE** — Her işlemde 50/50 yazı-tura ile oluşturma veya silme yapılır; yaş, boyut ve dizin dağılımları profile yakınsatılır.
4. **DONE** — `FS_AGING_RUNNING` dışında bir durumda aşama `FS_AGING_PHASE_DONE` olur.

### `fs_aging_step()` ve `max_ops` anlamı

Bir "işlem" (op) aşamaya göre şudur:

| Aşama | Bir işlem | Bir işlemdeki platform çağrıları |
|---|---|---|
| SETUP | Bir yaprak dizin | Yaprak derinliği kadar `make_dir` (kök tarafından başlayarak her seviye için bir kez; `pramod`'da derinlik 36'ya kadar). Tüm dizinler bitince SETUP'tan FILL'e geçiş de bir işlem sayılır ve platform çağrısı yapmaz. |
| FILL | Bir dosya oluşturma | Bir `create_file`. `NO_SPACE` dönerse aynı işlemde bir `delete_file`. |
| FILL (yaş kovası yerleştirme) | En fazla `FS_AGING_REBIN_CHUNK` havuz kaydının taranması | Yok |
| STABLE | Bir oluşturma veya silme | Bir `create_file` ya da bir `delete_file`. Oluşturma yapılamazsa (volume'e sığan boyut yok, `NO_SPACE` döndü ya da kayıt havuzu dolu) aynı işlemde bir `delete_file`. Silme gerektiğinde hiç canlı dosya yoksa platform çağrısı yapılmaz ve çalışma `FS_AGING_ERR_PLATFORM` ile biter (bkz. durum kodları). |

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
| `FS_AGING_ERR_PLATFORM` | Bir callback hata döndü; ya da silme gerekiyor ama hiç canlı dosya yok | Aging durur. İkinci durum callback hatası olmadan da oluşur (log: `"fs_aging: volume dolu ve silinecek dosya yok"`): (a) FILL'in ilk oluşturması `NO_SPACE` alırsa, (b) STABLE'da canlı dosya sayısı rastgele yürüyüşle 0'a inip yazı-tura silme derse. (b) küçük volume'lerde ve uzun çalışmalarda olasıdır (ör. `host_test agrawal 16777216 0 42`: 2655 işlemde canlı dosya 0, volume boş). Bu davranışın değiştirilmesi (ör. canlı dosya yokken silme yerine oluşturma) kullanıcı kararı bekliyor. |

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

/* Port'un sagladigi callback'ler (bkz. "POSIX port sozlesmesi") */
fs_aging_plat_result_t my_create(void *user, const char *path, uint64_t size_bytes);
fs_aging_plat_result_t my_delete(void *user, const char *path);
fs_aging_plat_result_t my_mkdir(void *user, const char *path);
uint64_t my_now_ms(void *user);
void my_log(void *user, const char *msg);

#define VOLUME_BYTES (1024ull * 1024u * 1024u)   /* 1 GB */
#define FILL_RATIO   0.8
#define POOL_CAP     12000u     /* agrawal, 1 GB: tahmin 7515 + %25 = 9393, yuvarlandi */

static fs_aging_file_t pool[POOL_CAP];   /* 24 B/kayit */
static fs_aging_ctx_t  ctx;              /* boyut host'ta 3008 B */

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

    /* Havuz kapasitesi: tahmin + %25 pay */
    need = fs_aging_estimate_files(pr, VOLUME_BYTES, FILL_RATIO);
    need += need / 4u;
    if (need > POOL_CAP) {
        return -1;
    }

    plat.create_file = my_create;
    plat.delete_file = my_delete;
    plat.make_dir    = my_mkdir;
    plat.now_ms      = my_now_ms;   /* opsiyonel, NULL olabilir */
    plat.log         = my_log;      /* opsiyonel, NULL olabilir */
    plat.user        = NULL;        /* callback'lere aynen iletilir */

    cfg.mount_point    = "/fs0";    /* sonunda '/' yok; aging boyunca gecerli */
    cfg.volume_bytes   = VOLUME_BYTES;
    cfg.fill_ratio     = FILL_RATIO;
    cfg.seed           = 42u;
    cfg.overwrite_runs = 1000u;     /* 0 = sinirsiz */
    cfg.time_limit_s   = 0u;        /* 0 = sinirsiz */
    cfg.confidence     = 0.0;       /* 0 = tam yakinsama */

    s = fs_aging_init(&ctx, pr, &cfg, &plat, pool, POOL_CAP);
    if (s != FS_AGING_RUNNING) {
        printf("init: %s\n", fs_aging_status_str(s));
        return -1;
    }

    /* ARINC 653'te: periyodik process her periyotta bir kez cagirir */
    do {
        s = fs_aging_step(&ctx, 1000u);
    } while (s == FS_AGING_RUNNING);

    fs_aging_get_stats(&ctx, &st);
    printf("%s: islem=%llu canli=%llu B yas_hatasi=%.4f\n",
           fs_aging_status_str(st.status),
           (unsigned long long)st.ops,
           (unsigned long long)st.live_bytes, st.age_error);

    for (i = 0u; i < fs_aging_bin_count(&ctx, FS_AGING_DIST_SIZE); i++) {
        fs_aging_float_t ideal;
        fs_aging_float_t actual;
        if (fs_aging_get_bin(&ctx, FS_AGING_DIST_SIZE, i, &ideal, &actual) == 0) {
            printf("boyut %2u: hedef %.4f gercek %.4f\n", i, ideal, actual);
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
| `fill_ratio` | `-u` | Hedef doluluk, (0, 1] aralığında, ör. `0.8`. |
| `seed` | `-r` | Rastgelelik tohumu. |
| `overwrite_runs` | `-i` | Toplam yük sınırı `overwrite_runs × volume_bytes`. 0 = sınırsız. |
| `time_limit_s` | `-w` | Süre sınırı, **saniye** cinsinden (orijinalde dakika). 0 = sınırsız. `now_ms` gerekir. |
| `confidence` | `-c` | 0 = tam yakınsama. Ör. 0.9 değeri, yaş hatası 0.1'e indiğinde durur. [0, 1) aralığında olmalıdır. |

## 5. Bellek boyutlandırma

Çekirdek bellek ayırmaz; iki blok çağıran tarafta durur.

**Dosya kayıt havuzu (`fs_aging_file_t[]`).** Canlı her dosya için bir kayıt tutulur. Kayıt boyutu **24 byte**'tır: 20 byte alan (`uint64_t` + `uint32_t` + 4 × `uint16_t`) + 8 byte hizalama için 4 byte dolgu (host'ta `sizeof` ile doğrulandı; farklı hizalamalı ABI'lerde hedefte `sizeof` ile ölçün). Yol saklanmaz, ihtiyaç anında üretilir.

- Kapasite `fs_aging_estimate_files(profile, volume_bytes, fill_ratio)` ile tahmin edilir. Bu değer `volume × fill_ratio / ortalama_boyut + 1`'dir. Üzerine **%25 pay** eklenmesi önerilir: `cap = est + est / 4`.
- ARINC 653'te havuz statik bir dizidir; boyutu partition bellek bütçesine göre derleme zamanında seçilir.

**Context (`fs_aging_ctx_t`).** Bu host'ta (x86_64, MSYS2 UCRT64 gcc 16.2, varsayılan limitler) `sizeof(fs_aging_ctx_t) = 3008` byte'tır. Bu sayı **host'a özeldir**: pointer boyutu, hizalama ve `FS_AGING_MAX_BINS` / `FS_AGING_PATH_MAX` değerleri değiştikçe değişir. Hedef platformda `sizeof` ile ölçün. Context'in alanları dahilidir ve değiştirilmemelidir.

### Büyük dosya boyutlu profiller

Bazı profillerde volume'e hiç sığmayan çok büyük boyut satırları vardır: `dabre` ve `grundman` 2 GiB, `pramod` 32 GiB dosya içerir. `fs_aging_estimate_files()` bu satırları ortalamaya kattığı için ortalama boyutu büyük, dosya sayısını küçük tahmin eder. Gerçekte bu boyutlar FILL'de `NO_SPACE` alır ve canlı dosyalar küçük boyutlardan oluşur. Sonuçta gereken kayıt sayısı tahminin çok üstüne çıkabilir.

Aşağıdaki değerler **yalnızca seed 42** ve `fill_ratio` 0.8 ile, sahte bir platformda (yalnızca canlı byte takibi) ölçüldü. Ölçüm deterministiktir, ama başka seed'lerde tepe değerleri belirgin biçimde farklı olabilir (aşağıya bakın):

| Profil | 256 MB tahmin | +%25 | FILL tepe | Tüm çalışma tepe | 1 GB tahmin | +%25 | FILL tepe | Tüm çalışma tepe |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| agrawal | 1879 | 2348 | 2024 | 2050 | 7515 | 9393 | 7417 | 7550 |
| dabre | 2354 | 2942 | **5152** | 5166 | 9413 | 11766 | **15738** | 15790 |
| douceur | 4766 | 5957 | 5142 | **8009** | 19063 | 23828 | 18790 | 19860 |
| grundman | 1838 | 2297 | 966 | 1000 | 7349 | 9186 | 7137 | 7227 |
| meyer | 434 | 542 | 480 | 503 | 1733 | 2166 | 1336 | 1381 |
| pramod | 674 | 842 | **1049** | 1049 | 2695 | 3368 | **4420** | 4438 |
| wang_lanl | 918 | 1147 | 937 | 1031 | 3670 | 4587 | 3652 | 4404 |
| wang_os | 2344 | 2930 | 2627 | **3024** | 9376 | 11720 | 9720 | 10704 |

Bu tablodan çıkan sonuçlar:

- **FILL'de** havuz dolarsa sonuç `FS_AGING_ERR_POOL_FULL` olur. Seed 42'de `dabre` ve `pramod` için %25 pay **yetmez**. Başka seed'lerde büyük boyut satırı olan diğer profiller de aşar. `host_test` ile (havuz = tahmin + %25) ölçülen örnekler:
  - `agrawal` 256 MB seed 8: `ERR_POOL_FULL`, 2349 işlem.
  - `meyer` 1 GB, seed 1–8: seed 3, 4 ve 8'de `ERR_POOL_FULL` (2167 işlem), diğer 5 seed'de tam yakınsama.
  - `grundman` 1 GB seed 3: `ERR_POOL_FULL`, 9187 işlem.
  - `douceur` 64 MB seed 4: `ERR_POOL_FULL`, 1491 işlem.

  Bu yüzden %25 pay yalnızca küçük boyutlu profiller için güvenlidir. Büyük boyut satırı olan profillerde (`agrawal`, `dabre`, `douceur`, `grundman`, `meyer`, `pramod`) havuzu belirgin biçimde büyük seçin (ör. tahminin 2,5 katı) ya da kullanılacak seed'lerin hepsiyle tepe değerini ölçün. Tahmin fonksiyonunun kendisinin değiştirilmesi kullanıcı kararı bekliyor.
- **STABLE'da** havuz dolarsa hata oluşmaz: oluşturma yerine silme yapılır. Ancak bu, canlı dosya sayısına bir üst sınır koyar ve dağılımları etkileyebilir. Bu yüzden havuzu "tüm çalışma tepe" sütununun üstünde tutmak en iyisidir (ör. `douceur` 256 MB'ta 8009 > 5957).
- Hedef platform için kesin değer gerekiyorsa aynı profil, volume ve seed ile host testinde tepe değerini ölçün.

## 6. Orijinal Geriatrix'ten farklar

Bilinçli farklar (çekirdekle ilgili olanların bir kısmı — PRNG, silinecek dosya seçimi, `confidence` ve yaş kovası yerleştirme — `fs_aging.c` başında da yazılıdır; kararlı aşamadaki sapma notu `stable_one()` üstündedir):

- **PRNG:** Dahili xorshift64* kullanılır (seed splitmix64 ile karıştırılır). Aynı seed her platformda aynı işlem dizisini üretir. Ancak orijinalle bire bir aynı dizi üretilmez.
- **Silinecek dosya seçimi:** Öncelik orijinaldeki gibidir: en çok fazla temsil edilen yaş kovası, sonra boyut kovası, sonra dizin kovası. Fark şudur: tüm kovalar taranmaz, `FS_AGING_DELETE_SAMPLES` (32) rastgele aday arasından en iyisi seçilir. Bu sayede işlem başına maliyet sabit kalır ve dosya başına 24 byte yeter.
- **Yaş kovası:** Dosyanın doğum tick'inden hesaplanır. Kararlı aşamada eşikler sabittir.
- **`confidence`:** Yaş dağılımının toplam varyasyon mesafesine göre yorumlanır (0.9 → hata ≤ 0.1). Ki-kare CDF'si ve libm kullanılmaz.
- **Yaş kovası yerleştirme:** FILL'den STABLE'a geçişte havuz tek seferde taranmaz. Her işlemde `FS_AGING_REBIN_CHUNK` kayıt taranır. Bu ara işlemler tick ve rastgele sayı tüketmez; bu yüzden işlem dizisi tek seferlik taramayla aynıdır.
- **Olmayan seçenekler:** Çok thread (`-t`) ve boşta bekleme (`-p`) yoktur. `-q` (bitişte sorma) ve `-x/-y/-z` (dağılım dosyaları) port'un işidir; çekirdek bunlar için `fs_aging_get_bin()` sunar. Sahte mod (`-f`) yerine host testlerindeki sahte platform kullanılır. Backend seçimi (`-b`) yerine `fs_aging_platform_t` vardır.
- **Süre sınırı:** `time_limit_s` saniye cinsindendir (orijinal `-w` dakika).
- **Yer yetersizliği:** Orijinal, `fallocate` başarısız olunca bekleyip aynı dosyayı tekrar dener. Burada platform `FS_AGING_PLAT_NO_SPACE` dönerse çekirdek aynı işlemde bir dosya siler ve devam eder.
- **Kayıt havuzu sınırı:** STABLE'da havuz doluysa oluşturma yerine silme yapılır (orijinalde böyle bir sınır yoktur). Bu yalnızca yukarı yönde bir sınırdır.
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
- Tek ek farkımız, kayıt havuzu dolunca oluşturmanın silmeye dönmesidir. Bu yalnızca yukarı yönde bir sınırdır.

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
| `FS_AGING_PLAT_NO_SPACE` | Dosya **oluşmamış** kabul edilir. Aynı işlemde bir dosya silinir ve aging devam eder. Silinecek dosya yoksa `FS_AGING_ERR_PLATFORM`. | `FS_AGING_ERR_PLATFORM`, aging durur. |
| `FS_AGING_PLAT_ERROR` | `FS_AGING_ERR_PLATFORM`, aging durur. | `FS_AGING_ERR_PLATFORM`, aging durur. |

`NO_SPACE` yalnızca `create_file` için anlamlıdır. `delete_file` ve `make_dir`, `OK` dışındaki **her** değerde aging'i durdurur.

### `create_file(user, path, size_bytes)`

- `path` yolunda **tam olarak `size_bytes` byte** boyutunda, yeri gerçekten ayrılmış bir dosya oluşturmalıdır. Bunun için yer ayırma (`posix_fallocate`) ya da veri yazma kullanılabilir. Orijinal Geriatrix `open(O_RDWR|O_CREAT, 0600)` + `posix_fallocate` + `close` kullanır. Yalnızca `ftruncate` ile oluşturulan seyrek (sparse) dosya blok ayırmadığı için aging etkisi üretmez; yeterli değildir.
- `size_bytes == 0` geçerlidir (ör. `wang_os` profilinde 0 byte dosyalar vardır). Boş dosya oluşturulur.
- `size_bytes` 4 GiB'ı aşabilir (`pramod`'da 32 GiB'a kadar). 64 bit boyutu doğru işleyin.
- Yer yetmiyorsa (`ENOSPC`, `EFBIG` gibi) `FS_AGING_PLAT_NO_SPACE` dönün. Bu durumda **yarım kalan dosyayı silin**. Çekirdek bu dosyayı bilmez; geride kalırsa yer kaplayan yetim bir dosya olur.
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

- Aşama geçişleri, hata ve bitiş mesajları. Mesajlar Türkçe, ASCII ve satır sonu (`\n`) içermez (ör. `"fs_aging: doldurma bitti, kararli aging basladi"`).
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

Hazır 8 profil `CORE/include/fs_aging_profiles.h` içinde bildirilir:

- `fs_aging_profile_agrawal`, `_dabre`, `_douceur`, `_grundman`, `_meyer`, `_pramod`, `_wang_lanl`, `_wang_os`
- `fs_aging_profiles[FS_AGING_PROFILE_COUNT]` (alfabetik sıra, `FS_AGING_PROFILE_COUNT` = 8)
- `fs_aging_profile_find(name)`: Profil adı Geriatrix'teki klasör adıdır (ör. `"agrawal"`).

Tablolar `CORE/src/fs_aging_profiles.c` içindedir. Bu dosya **otomatik üretilir, elle düzenlenmez.** Değişiklik gerekiyorsa script ya da kaynak profil dosyaları değiştirilip dosya yeniden üretilir.

```
py -3 TOOLS/gen_profiles.py                 # Windows
python3 TOOLS/gen_profiles.py               # Linux
```

| Seçenek | Varsayılan | Açıklama |
|---|---|---|
| `--geriatrix <klasör>` | `<repo>/../geriatrix`, yoksa `<repo>/geriatrix` | Geriatrix klonu (`profiles/<ad>/*.txt` okunur) |
| `--out <dosya>` | `CORE/src/fs_aging_profiles.c` | Çıktı dosyası |
| `--mount <yol>` | `/fs0` | Yalnızca yol uzunluğu raporunu etkiler; üretilen dosyayı değiştirmez |

- Yalnızca Python standart kütüphanesini kullanır.
- Çıktı deterministiktir: aynı girdi aynı dosyayı üretir (zaman damgası ve mutlak yol yok, ASCII, `\n` satır sonu).
- `FS_AGING_MAX_BINS` ve `FS_AGING_PATH_MAX` değerlerini `fs_aging.h`'den okur.
- Profil dosyalarını katı biçimde ayrıştırır. Bozuk sayı, satır sayısı uyuşmazlığı, artmayan yaş eşikleri, negatif ya da tamamen sıfır ağırlıklar, `FS_AGING_MAX_BINS`'i aşan satır sayısı gibi durumlarda `gen_profiles: HATA: <dosya>:<satır>: ...` mesajıyla çıkış kodu 1 döner. Dosya sonundaki "Format:" açıklama bloğu yok sayılır.
- Bilinen tek istisna `grundman/size_distribution.txt` dosyasıdır (bkz. [bölüm 6](#6-orijinal-geriatrixten-farklar)). Bu dosya için `UYARI` basılır.
- Standart çıktıda her profil için satır sayıları ve en uzun yol raporlanır.

Profil formatı (Geriatrix): ilk satır veri satırı sayısıdır. Ardından yaş dosyasında `<eşik> <ağırlık>`, boyut dosyasında `<byte> <ağırlık>`, dizin dosyasında `<derinlik> <ağırlık> <kardeş_dizin>` satırları gelir.

## 9. Testler

Host testleri `TEST/` altındadır ve PC'de sahte bir platformla çalışır. Sahte platform gerçek dosya oluşturmaz. Yolları bir tabloda tutar; olmayan dosyanın silinmesini veya aynı dosyanın iki kez oluşturulmasını hata olarak yakalar ve volume doluluğunu takip eder.

Gereksinimler: gcc (C99) ve GNU make. Windows'ta MSYS2 kullanılabilir (MSYS2 `make` veya `mingw32-make`). Araçlar PATH'te değilse, ör. PowerShell'de:

```
$env:Path += ';C:\msys64\ucrt64\bin;C:\msys64\usr\bin'
```

Komutlar (`TEST/` içinde ya da repo kökünden `-C TEST` ile):

```
make -C TEST            # TEST/build/ altına derler
make -C TEST test       # kenar durum testleri + 8 profilin hepsinde host_test
make -C TEST clean      # TEST/build/ temizlenir
```

`mingw32-make` için aynı hedefler geçerlidir (`mingw32-make -C TEST test`). Testler katı bayraklarla derlenir: `-std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Derleme uyarısız olmalı ve `make test` 0 ile çıkmalıdır.

- **`host_test`** — Bir profille tam aging çalıştırır, sonuç istatistiklerini ve yaş/boyut/dizin dağılımlarını (hedef / gerçek) basar.

  ```
  host_test [-q] [--max-err X] [--expect-ops N] <profil> [volume_bytes] [confidence] [seed] [max_ops]
  ```

  Ör. `TEST/build/host_test agrawal 268435456`. Profil adı `fs_aging_profile_find()` ile aranır. Varsayılanlar: volume 256 MB, confidence 0, seed 42, `max_ops` 1000.
  - `-q`: Yalnızca tek satırlık `SUMMARY` çıktısı basılır (log ve dağılım tabloları basılmaz).
  - `--max-err X`: Yaş, boyut veya dizin hatası (toplam varyasyon mesafesi) X'i aşarsa sonuç `FAIL` olur.
  - `--expect-ops N`: İşlem sayısı (`stats.ops`) tam N değilse sonuç `FAIL` olur.
  - Çıkış kodu 0 ancak sahte platform tutarlıysa, durum `DONE_*` ise ve verilen kalite koşulları sağlanıyorsa olur.

  `make test` her profili `-q --max-err 0.15` ile çalıştırır. Agrawal 256 MB için `--expect-ops 155682 --max-err 0.0025` kullanılır; böylece aşağıdaki referans sonuç otomatik doğrulanır.
  - **Not:** `make test`'teki volume/seed seçimleri tohuma bağlıdır. Örneğin `douceur` 64 MB seed 42'de geçer, ama seed 3'te `ERR_PLATFORM` (STABLE'da canlı dosya 0'a iner), seed 4'te `ERR_POOL_FULL` ile biter (bkz. [bölüm 3](#durum-kodları-fs_aging_status_t) ve [bölüm 5](#5-bellek-boyutlandırma)). İşlem dizisini değiştiren zararsız bir çekirdek değişikliği de bu nedenle `douceur` kapısını kırabilir. Kalıcı çözüm (çekirdek davranışı ve havuz payı) kullanıcı kararı bekliyor.
- **`edge_tests`** — Hata ve sınır durumları:
  - küçük havuzda `FS_AGING_ERR_POOL_FULL`
  - platformun erken `NO_SPACE` döndüğü durumda hatasız devam
  - platform `ERROR` döndüğünde `FS_AGING_ERR_PLATFORM`
  - `-DFS_AGING_PATH_MAX=64` ile derin profilde `FS_AGING_ERR_PATH_TOO_LONG`; tam sınırda yol uzunluğu; hiçbir yola sığmayan dev derinlikte (`0xFFFFFFFF`) init'in takılmadan `FS_AGING_ERR_PATH_TOO_LONG` dönmesi
  - geçersiz parametrelerde `FS_AGING_ERR_PARAM`
  - determinizm (aynı seed → aynı sonuç, farklı seed → farklı sonuç)
  - adım bağımsızlığı (`max_ops` 1 / 7 / 1000 aynı sonuç): tam yakınsama, `confidence` 0.9 ile `DONE_TOLERANCE` ve erken `NO_SPACE` (oluşturma yerine silme) senaryolarında
  - yaş kovası yerleştirme: adım başına sınırlı tarama ve çağrı; STABLE'a geçişte her canlı kaydın tam bir kez sayılması (`sum(count_age) == live_files`, her koşuda kontrol edilir)

Referans sonuç (Agrawal, 256 MB, `fill_ratio` 0.8, seed 42, `max_ops` 1000, `confidence` 0): `FS_AGING_DONE_CONVERGED`; 155682 işlem (78765 oluşturma, 76917 silme), 1848 canlı dosya, ~200 MB canlı veri, yaş hatası 0,0023, sahte platform tutarsızlığı 0.

## 10. Lisans

Bu kütüphane Geriatrix'in türev çalışmasıdır. Algoritma ve `CORE/src/fs_aging_profiles.c` içindeki profil verileri orijinal Geriatrix reposundan alınmıştır:

- Copyright (c) 2018, Carnegie Mellon University. All rights reserved.
- Lisans: BSD-2-Clause. Tam metin repo kökündeki [`LICENSE.geriatrix`](LICENSE.geriatrix) dosyasındadır ve korunmalıdır.

BSD-2-Clause gereği kaynak dağıtımlarında telif bildirimi, koşullar ve sorumluluk reddi korunmalıdır. İkili dağıtımlarda da belgelerde yer almalıdır. Kaynak dosyalardaki orijinal Geriatrix referansları (`https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)`) silinmemelidir.
