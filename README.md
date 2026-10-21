# LIB_FS_AGING

Dosya sistemlerini **yaşlandıran** bir araç kütüphanesi. Boş bir diski, yıllarca kullanılmış bir diske benzeyecek hâle getirir: dosyalar parçalanır, boş alan dağınık küçük parçalara bölünür. Böylece dosya sistemi testleri yeni ve tertemiz bir disk yerine gerçekçi bir disk üzerinde yapılabilir.

Kütüphane [Geriatrix](https://github.com/saurabhkadekodi/geriatrix) aracının (USENIX ATC 2018) saf C99 ile yazılmış karşılığıdır.

## İçindekiler

1. [Neden disk yaşlandırma?](#1-neden-disk-yaşlandırma)
2. [Temel kavramlar](#2-temel-kavramlar)
3. [Çekirdek nasıl çalışır?](#3-çekirdek-nasıl-çalışır)
4. [Klasör yapısı](#4-klasör-yapısı)
5. [Çekirdeği kullanmak](#5-çekirdeği-kullanmak)
6. [Kendi port'unuzu yazmak](#6-kendi-portunuzu-yazmak)
7. [ARINC 653 port'u](#7-arinc-653-portu)
8. [Hazır profiller](#8-hazır-profiller)
9. [Bellek ve süre](#9-bellek-ve-süre)
10. [Orijinal Geriatrix'ten farklar](#10-orijinal-geriatrixten-farklar)
11. [Lisans](#11-lisans)

---

## 1. Neden disk yaşlandırma?

Yeni biçimlendirilmiş bir diskte boş alan tek parçadır. Yazılan her dosya diskte art arda duran bloklara yerleşir.

Gerçek hayatta diskler böyle kalmaz. Aylar ve yıllar boyunca dosyalar oluşturulur, silinir, yenileri yazılır. Silinen dosyaların yerinde küçük boşluklar kalır; yeni dosyalar bu boşluklara parça parça yerleşir. Sonuçta:

- dosyalar diskte birçok parçaya bölünür,
- boş alan küçük, dağınık parçalardan oluşur,
- aynı işlem yeni diske göre daha yavaş ya da farklı davranır.

Bir dosya sistemini yalnızca yeni diskte test etmek bu yüzden yanıltıcı olabilir. Bu kütüphane, kontrollü bir dizi dosya oluşturma ve silme işlemiyle diski kısa sürede "yaşlı" hâle getirir.

Örnek: 512 MB'lık bir FAT32 imajında 64 MB'lık bir dosya, yalnızca doldurulmuş diskte **1** parça hâlinde yazıldı; bu kütüphaneyle yaşlandırılmış diskte ise aynı dosya **1.115** parçaya bölündü.

## 2. Temel kavramlar

### Profil

Kütüphane diski gelişigüzel değil, bir **profile** göre yaşlandırır. Profil, gerçek bir yaşlı diskten çıkarılmış üç istatistik tablosudur:

| Tablo | Sorduğu soru | Örnek (Agrawal profili) |
|---|---|---|
| **Boyut** | Dosyalar ne büyüklükte? | Dosyaların %21,7'si 2 KB, %20'si 8 KB, %0,4'ü 8 MB |
| **Dizin derinliği** | Dosyalar kaçıncı dizin seviyesinde? | Dosyaların %22,5'i 7. seviyede |
| **Yaş** | Dosyalar birbirine göre ne kadar yaşlı? | Dosyaların %31,5'i "orta yaşlı", %0,2'si "en yaşlı" grupta |

Aging bittiğinde diskteki dosyaların boyut, derinlik ve yaş dağılımı bu tablolara oturur. Kütüphanede 8 hazır profil vardır (bkz. [bölüm 8](#8-hazır-profiller)).

### Yaş göreli ölçülür

Kütüphane gerçek zamanı beklemez; 14 yıllık bir diski taklit etmek için 14 yıl beklemeyiz. Saat yerine bir işlem sayacı kullanılır: her dosya oluşturma veya silme bir "tık"tır ve bir dosyanın yaşı kaç işlem önce oluşturulduğudur. Kütüphane, aging sonunda dosyaların **birbirine göre** yaşları profildeki oranlara uyacak şekilde hangi dosyaları sileceğini seçer.

### Tekrarlanabilirlik

Aynı profil, aynı volume boyutu ve aynı **seed** ile çalıştırıldığında kütüphane her seferinde ve her platformda **birebir aynı** işlem dizisini üretir. Böylece belirli parametrelerle hep aynı şekilde yaşlanmış diskler oluşturulabilir.

### Çekirdek ve port

Kütüphane iki katmandan oluşur:

- **Çekirdek (`FS_AGING_CORE`)**: Hangi dosyanın ne zaman, hangi boyutta oluşturulacağına ya da silineceğine karar veren algoritma. Hiçbir işletim sistemini bilmez ve dosya sistemine kendisi dokunmaz.
- **Port**: Çekirdeğin "şu dosyayı oluştur", "şunu sil", "şu dizini aç" isteklerini gerçek işletim sistemi çağrılarına çeviren ince katman. Her platform için ayrıdır: ARINC 653 port'u hazırdır, POSIX port'u ayrı ekip tarafından yazılacaktır.

## 3. Çekirdek nasıl çalışır?

Aging üç aşamada ilerler:

```
SETUP  →  FILL   →  STABLE           →  bitti
dizin     diski     oluştur / sil ile
ağacı     doldur    yaşlandır
```

**1. SETUP — dizin ağacı.** Profilin gerektirdiği dizinler oluşturulur (`/d1/d2/...`). Dosya oluşturulmaz.

**2. FILL — doldurma.** Disk hedef doluluğa (ör. %80) kadar dosyalarla doldurulur. Dosya boyutları ve derinlikleri profile göre seçilir. Bu aşamada henüz parçalanma yoktur; disk sadece "dolu"dur.

**3. STABLE — yaşlandırma.** Asıl yaşlanma burada olur. Her işlemde yazı-tura atılır: ya yeni bir dosya oluşturulur ya da bir dosya silinir. Silinecek dosya, yaş, boyut ve dizin dağılımlarını profile en çok yaklaştıracak şekilde seçilir. Silinen dosyaların bıraktığı boşluklara yeni dosyalar yerleştikçe disk parçalanır.

Kütüphane başta, yaş dağılımının profile tam oturması için kaç işlem gerektiğini hesaplar (`convergence_ops`). Bu sayıya ulaşınca aging biter.

### Parça parça çalışma: `fs_aging_step()`

Çekirdek bütün işi tek seferde yapmaz. `fs_aging_step(ctx, N)` her çağrıldığında **en fazla N işlem** yapar ve döner; çağıran taraf onu tekrar tekrar çağırır. Bu, ARINC 653 gibi zaman bölmeli sistemlerde önemlidir: her periyotta bir `step` çağrılır ve iş periyotlara yayılır, böylece partition penceresi taşmaz.

Bir "işlem" (op) çoğunlukla bir dosya oluşturma ya da silmedir; SETUP'ta ise tek bir dizin oluşturmadır. Bir işlem en fazla iki dosya sistemi çağrısı yapar (ör. yer kalmamışsa oluşturma denemesi ve bir silme).

Sonuç N'den bağımsızdır: periyot başına 1, 7 ya da 1000 işlem yapılsa da aynı disk oluşur.

### Ne zaman durur?

Normal bitişler:

| Durum | Anlamı |
|---|---|
| `FS_AGING_DONE_CONVERGED` | Dağılımlar profile tam oturdu |
| `FS_AGING_DONE_TOLERANCE` | Yaş hatası `confidence` ile verilen sınıra indi |
| `FS_AGING_DONE_WORKLOAD_LIMIT` | `overwrite_runs` ile verilen yazma miktarına ulaşıldı |
| `FS_AGING_DONE_TIME_LIMIT` | `time_limit_s` ile verilen süre doldu |

Hatalar:

| Durum | Anlamı | Ne yapılmalı |
|---|---|---|
| `FS_AGING_ERR_PARAM` | Geçersiz parametre | Profili, ayarları ve callback'leri kontrol edin |
| `FS_AGING_ERR_POOL_FULL` | Dosya kayıt havuzu küçük | Havuzu `fs_aging_estimate_files()` ile boyutlandırın |
| `FS_AGING_ERR_PATH_TOO_LONG` | Bir yol `FS_AGING_PATH_MAX`'ı aşıyor | Mount noktasını kısaltın ya da sınırı büyütün |
| `FS_AGING_ERR_PLATFORM` | Bir callback hata döndü ya da ilerlemek imkânsız (hiçbir boyut diske sığmıyor) | Port'u ve diski kontrol edin |
| `FS_AGING_ERR_FILL_UNREACHABLE` | Disk hedef doluluğa ulaşamıyor | `fill_ratio`'yu düşürün ya da `volume_bytes` olarak gerçek boş alanı verin |

`fs_aging_status_str()` her kod için kısa bir İngilizce metin döner.

## 4. Klasör yapısı

```
LIB_FS_AGING/
├── README.md                    # bu belge
├── LICENSE.geriatrix            # orijinal Geriatrix lisansı (BSD-2-Clause)
├── FS_AGING_CORE/               # çekirdek (saf C99, platformdan bağımsız)
│   ├── include/
│   │   ├── fs_aging.h           # çekirdeğin dış arayüzü
│   │   └── fs_aging_profiles.h  # hazır profiller
│   └── src/
│       ├── fs_aging.c           # algoritma
│       └── fs_aging_profiles.c  # profil tabloları (üretilir, elle düzenlenmez)
├── FS_AGING_ARINC653/           # ARINC 653 port'u (bkz. bölüm 7)
│   ├── include/fs_aging_arinc653.h
│   └── src/
│       ├── fs_aging_arinc653.c         # dosya servisleri (callback'ler)
│       └── fs_aging_arinc653_runner.c  # kurulum ve periyodik aging process'i
├── FS_AGING_POSIX/              # POSIX port'u için yer tutucu (bkz. bölüm 6)
│   ├── include/fs_aging_posix.h
│   └── src/fs_aging_posix.c
└── TOOLS/
    └── gen_profiles.py          # profil tablolarını üreten bakım aracı
```

Çekirdeği bir projeye eklemek için include yolu `FS_AGING_CORE/include`, derlenecek dosyalar `FS_AGING_CORE/src/fs_aging.c` ve `FS_AGING_CORE/src/fs_aging_profiles.c`'dir.

Çekirdek hiçbir libc fonksiyonu çağırmaz ve bellek ayırmaz. Not: gcc `-O2` ile derlerken bir sıfırlama döngüsünü `memset` çağrısına çevirebilir; hedefte `memset` yoksa çekirdeği `-ffreestanding` ile derleyin.

## 5. Çekirdeği kullanmak

Dört adım:

1. **Callback'leri verin.** Çekirdeğin dosya oluşturma, silme ve dizin oluşturma isteklerini karşılayan üç fonksiyon; yani port.
2. **Havuzu ayırın.** Çekirdek her canlı dosya için 24 byte'lık bir kayıt tutar; bu diziyi siz ayırırsınız. Gereken boyutu `fs_aging_estimate_files()` söyler.
3. **`fs_aging_init()` çağırın.** Parametreleri doğrular, dosya sistemine dokunmaz.
4. **`fs_aging_step()` çağırın.** `FS_AGING_RUNNING` dışında bir değer dönene kadar tekrar tekrar.

```c
#include "fs_aging.h"
#include "fs_aging_profiles.h"

#define POOL_SIZE 8000u

static fs_aging_ctx_t  ctx;
static fs_aging_file_t pool[POOL_SIZE];

int run_aging(void)
{
    const fs_aging_profile_t *profile = fs_aging_profile_find("agrawal");
    fs_aging_platform_t plat;
    fs_aging_config_t   cfg;
    fs_aging_status_t   s;
    uint32_t            need;

    plat.create_file = my_create_file;      /* port fonksiyonlarınız */
    plat.delete_file = my_delete_file;
    plat.make_dir    = my_make_dir;
    plat.now_ms      = my_now_ms;           /* opsiyonel, NULL olabilir */
    plat.log         = my_log;              /* opsiyonel, NULL olabilir */
    plat.user        = NULL;

    cfg.mount_point    = "/mnt/fs1";        /* sonunda '/' yok */
    cfg.volume_bytes   = 256ull << 20;      /* diskin gerçek boş alanı */
    cfg.fill_ratio     = 0.8;               /* %80 doluluk */
    cfg.seed           = 42u;
    cfg.overwrite_runs = 0u;                /* 0 = sınır yok */
    cfg.time_limit_s   = 0u;                /* 0 = sınır yok */
    cfg.confidence     = 0.0;               /* 0 = tam yakınsama */

    need = fs_aging_estimate_files(profile, cfg.volume_bytes, cfg.fill_ratio);
    if ((need == 0u) || (need > POOL_SIZE)) {
        return -1;                          /* havuz yetmiyor */
    }

    s = fs_aging_init(&ctx, profile, &cfg, &plat, pool, POOL_SIZE);
    while (s == FS_AGING_RUNNING) {
        s = fs_aging_step(&ctx, 100u);      /* her çağrıda en fazla 100 işlem */
    }
    return (s == FS_AGING_DONE_CONVERGED) ? 0 : -1;
}
```

Sonuç istatistikleri `fs_aging_get_stats()` ile, dağılımların hedefe ne kadar oturduğu `fs_aging_bin_count()` ve `fs_aging_get_bin()` ile okunur (her satır için hedef ve gerçekleşen oran).

## 6. Kendi port'unuzu yazmak

Bir port, `fs_aging_platform_t` yapısındaki fonksiyonları doldurur. Çekirdek bu fonksiyonları çağırır ve dönüş değerlerine göre devam eder. `FS_AGING_POSIX/` altındaki dosyalar POSIX port'u için başlangıç noktasıdır; `FS_AGING_ARINC653/src/fs_aging_arinc653.c` tamamlanmış bir örnektir.

### Callback'ler

| Callback | Ne yapmalı | Dönüş |
|---|---|---|
| `create_file(user, path, size_bytes)` | `path` adında, **tam `size_bytes` boyutunda** yeni bir dosya oluşturur: yer ayırarak (ör. `posix_fallocate`) ya da veri yazarak. | `OK`; yer yoksa `NO_SPACE`; diğer hatalarda `ERROR` |
| `delete_file(user, path)` | Dosyayı siler. | `OK` ya da `ERROR` |
| `make_dir(user, path)` | **Tek seviye** dizin oluşturur; üst dizinler önceden oluşturulmuştur. | `OK`; **dizin zaten varsa da `OK`**; diğer hatalarda `ERROR` |
| `now_ms(user)` | Opsiyonel. Monoton zaman, milisaniye. `NULL` ise süre sınırı kullanılamaz. | milisaniye |
| `log(user, msg)` | Opsiyonel. Çekirdeğin kısa mesajlarını basar. | – |

`user`, callback'lere olduğu gibi iletilen serbest bir pointer'dır.

### Dönüş kodlarına çekirdeğin tepkisi

- `FS_AGING_PLAT_OK`: işlem başarılı, devam.
- `FS_AGING_PLAT_NO_SPACE` (yalnızca `create_file`): çekirdek dosyayı **oluşmamış** kabul eder, yerine bir dosya siler ve devam eder.
- `FS_AGING_PLAT_ERROR`: aging durur ve `FS_AGING_ERR_PLATFORM` döner.

### Uyulması gereken kurallar

1. **Yarım dosyayı silin.** `create_file` yer kalmadığı için başarısız olursa yarım oluşan dosyayı silip `NO_SPACE` dönün. Çekirdek bu dosyadan habersizdir; geride kalırsa boşa yer kaplar.
2. **Boş bir volume'de başlayın.** Dosya adları her çalışmada baştan başlar (`f1`, `f2`, ...); önceki bir çalışmadan kalan dosyalar çakışır.
3. **`volume_bytes` olarak gerçek boş alanı verin.** Çekirdek yalnızca dosya boyutlarını toplar; dosya sisteminin kendi yönetim verisi ve blok yuvarlaması buna dahil değildir. Nominal boyut verilirse hedefe ulaşılamayabilir (`FS_AGING_ERR_FILL_UNREACHABLE`). POSIX'te ör. `statvfs` ile `f_bavail × f_frsize`.
4. **`path` yalnızca çağrı süresince geçerlidir.** Saklamanız gerekiyorsa kopyalayın.
5. **Mount noktası ve profil aging boyunca geçerli kalmalıdır.** Çekirdek bu pointer'ları saklar.
6. **Tek iş parçacığı.** Bir context'e aynı anda tek bir yerden `fs_aging_step()` çağrılmalıdır.
7. **Büyük dosyalar.** `size_bytes` 4 GiB'ı aşabilir (`pramod` profilinde 32 GiB'a kadar). 64 bit boyutu doğru işleyin; dosya sistemi bu boyutu desteklemiyorsa `NO_SPACE` dönün.

### Yol formatı

Çekirdeğin ürettiği yollar şöyledir:

```
<mount>/d1/d2/d3/d4/d5/d6/d17/f84213
        └── üst dizinler ──┘ │    └ dosya adı: oluşturulduğu işlem numarası
                             └ en alt seviyedeki dizin
```

Profilin dizin derinliği 0 ise dosyalar doğrudan `<mount>/f<numara>` olarak oluşturulur. En uzun yollar `/fs0` mount noktasıyla `pramod` için 162, Agrawal için 77 byte'tır; mount noktası uzadıkça yollar da aynı miktarda uzar. Sınır `FS_AGING_PATH_MAX`'tır (varsayılan 256).

### Kontrol listesi

- [ ] Üç zorunlu callback dolduruldu; `make_dir` var olan dizinde `OK` dönüyor.
- [ ] `create_file` tam `size_bytes` boyutunda dosya oluşturuyor; yer yoksa yarım dosyayı silip `NO_SPACE` dönüyor.
- [ ] Havuz `fs_aging_estimate_files()` ile boyutlandırıldı.
- [ ] `volume_bytes` gerçek boş alandan alınıyor ve volume boş.
- [ ] `fs_aging_step()` bir döngüde, `FS_AGING_RUNNING` dışında bir değer dönene kadar çağrılıyor.

## 7. ARINC 653 port'u

`FS_AGING_ARINC653/`, çekirdeği bir ARINC 653 partition'ında çalıştırır. Belirli parametrelerle yaşlanmış diskler üreten bir araç olarak tasarlanmıştır.

### Akış

```
INIT kodu:      fs_aging_a653_start()  →  SET_PARTITION_MODE(NORMAL)
                        │
Aging process:  [boş alanı oku] → aging (her periyotta en fazla N işlem)
                → rapor → on_done → SET_PARTITION_MODE(IDLE)
```

- INIT kodu yalnızca `fs_aging_a653_start()` ve `SET_PARTITION_MODE(NORMAL)` çağırır. `fs_aging_a653_start()` ayarları ve havuzu kontrol eder ve `FS_AGING` adında periyodik bir process oluşturur; dosya sistemine dokunmaz.
- Bütün iş aging process'inde yapılır: process her periyotta bir `fs_aging_step()` çağırır ve `PERIODIC_WAIT` ile bekler.
- Aging bitince (başarıyla ya da hatayla) rapor basılır, varsa `on_done` çağrılır ve partition `IDLE`'a alınır; program biter.

### Kullanım

```c
#include "fs_aging_arinc653.h"

/* Partition INIT kodu */
void aging_partition_init(void)
{
    RETURN_CODE_TYPE return_code = -1;
    fs_aging_a653_config_t cfg = {
        .profile_name   = "agrawal",
        .mount_point    = "/mnt/fs1",
        .volume_bytes   = 0u,           /* 0: boş alan otomatik okunur */
        .fill_ratio     = 0.8,
        .seed           = 42u,
        .ops_per_period = 50u,          /* periyot başına işlem */
        .period         = AGING_PERIOD,
        .time_capacity  = INFINITE_TIME_VALUE,
        .stack_size     = AGING_STACK_SIZE,
        .priority       = AGING_PRIORITY,
        .core_id        = 0u,
        .progress_every = 1000u,        /* 1000 periyotta bir ilerleme bas */
        .on_done        = NULL,         /* opsiyonel bitiş fonksiyonu */
    };

    if (FS_AGING_RUNNING != fs_aging_a653_start(&cfg)) {
        SET_PARTITION_MODE(IDLE, &return_code);   /* hata basıldı; programı bitir */
        return;
    }
    SET_PARTITION_MODE(NORMAL, &return_code);
    if (NO_ERROR != return_code) {
        TEST_PRINT("SET_PARTITION_MODE failed (%d)\r\n", (int)return_code);
    }
}
```

### Davranış

- **Volume boyutu:** `volume_bytes = 0` verilirse aging process'i başlarken mount noktasında geçici bir dosya açar, `GET_VOLUME_STATUS` ile boş alanı okur, dosyayı siler ve aging'i bu boyutla başlatır.
- **Dosya oluşturma:** Varsayılan olarak `OPEN_NEW_FILE` + `RESIZE_FILE` ile, veri yazmadan yapılır. `FS_AGING_A653_USE_RESIZE 0` ile `MAXIMUM_ATOMICITY` parçaları hâlinde `WRITE_FILE` kullanılır.
- **Çıktı:** Tüm mesajlar `TEST_PRINT` ile basılır. Hatalar `fs_aging: error: ...` ile başlar; bitişte tek satırlık bir `fs_aging: summary: ...` basılır.
- **Hatalar:** INIT'teki hatalarda `fs_aging_a653_start()` hata kodunu döner; programı bitirmek INIT kodunun işidir (yukarıdaki örnekte olduğu gibi). Aging sırasındaki hatalarda mesaj basılır ve partition `IDLE`'a alınır.
- Partition başına tek aging çalışması desteklenir.

### Derleme ayarları

| Makro | Varsayılan | Anlamı |
|---|---|---|
| `FS_AGING_A653_POOL_CAPACITY` | `16384u` | Statik havuz boyutu (kayıt başına 24 byte, ~384 KB) |
| `FS_AGING_A653_USE_RESIZE` | `1` | `1`: `RESIZE_FILE`, `0`: veri yazarak |
| `FS_AGING_A653_MAX_FILE_SIZE` | `0xFFFFFFFF` | En büyük dosya (`RESIZE_FILE` sınırı); daha büyükleri `NO_SPACE` sayılır |
| `FS_AGING_A653_IDLE_AT_END` | `1` | Aging bitince partition'ı `IDLE`'a al |
| `FS_AGING_A653_PRINT` | `TEST_PRINT` | Çıktı fonksiyonu |

### Hedefte henüz doğrulanmayanlar

- **`RESIZE_FILE` gerçekten yer ayırıyor mu?** Yeni bir dosyayı `RESIZE_FILE` ile 1 MB yapıp `GET_VOLUME_STATUS`'taki `USED_BYTES` değerinin arttığını görmek yeterli. Artmıyorsa `FS_AGING_A653_USE_RESIZE 0` kullanılmalı; aksi hâlde dosyalar yer kaplamaz ve parçalanma oluşmaz.
- **Son satırlar:** `IDLE`'a geçerken son `TEST_PRINT` satırları ekrana ulaşıyor mu?
- **Bütçe ve zamanlama:** Havuz boyutu partition bellek bütçesine, `ops_per_period` pencere süresine göre seçilmeli.

## 8. Hazır profiller

Profiller Geriatrix makalesindeki yaşlı disk çalışmalarından alınmıştır. Hepsi aynı algoritmayla çalışır; yalnızca taklit edilen disk farklıdır.

| Profil | Nereden | En yaşlı dosya |
|---|---|---|
| `agrawal` | Microsoft, Windows masaüstleri (2004) | 14 yıl |
| `douceur` | Microsoft, 10.000'den fazla Windows PC (1998) | 4 yıl |
| `meyer` | Microsoft, 857 Windows masaüstü (2009) | 2 yıl |
| `wang_os` | CMU Parallel Data Lab, NetApp depolama (2011) | 22 yıl |
| `wang_lanl` | Los Alamos Ulusal Laboratuvarı, HPC depolama | 11 yıl |
| `dabre` | Bir Linux dizüstünün kök bölümü (Ext4, 2017) | 1 yıl |
| `pramod` | Başka bir Linux dizüstünün kök bölümü (Ext4, 2017) | 3,75 yıl |
| `grundman` | Bir Linux dizüstünün home bölümü (Ext4, 2018) | 8,75 yıl |

Seçerken:

- **`agrawal`**: dengeli, genel amaçlı ve hızlı yakınsar; varsayılan olarak önerilir.
- **`wang_os`, `wang_lanl`**: dizin ağacı yoktur, tüm dosyalar doğrudan mount noktasındadır.
- **`pramod`**: en derin dizin ağacı (36 seviye) ve 32 GiB'a kadar dev dosyalar.
- **`douceur`**: yaş dağılımı çok geniştir; aynı disk için diğerlerinden çok daha fazla işlem ister.

Profil tabloları `FS_AGING_CORE/src/fs_aging_profiles.c` içindedir ve **elle düzenlenmez**. Yeni bir profil eklemek için `TOOLS/gen_profiles.py`, Geriatrix formatındaki üç metin dosyasından (`age_distribution.txt`, `size_distribution.txt`, `dir_distribution.txt`) bu dosyayı yeniden üretir:

```
py -3 TOOLS/gen_profiles.py --geriatrix <geriatrix klasörü>
```

Script her profil için satır sayılarını ve en uzun yol uzunluğunu da raporlar.

## 9. Bellek ve süre

**Bellek.** Context (`fs_aging_ctx_t`) yaklaşık 3 KB, her dosya kaydı 24 byte'tır; değerler platforma göre biraz değişebilir, hedefte `sizeof` ile ölçün. Havuz kapasitesi `fs_aging_estimate_files()` ile hesaplanır, ek pay gerekmez. %80 doluluk için örnekler:

| Profil | 256 MB | 1 GB |
|---|---|---|
| `agrawal` | 3.636 kayıt (~87 KB) | 10.494 kayıt (~252 KB) |
| `dabre` | 13.990 kayıt (~336 KB) | 41.870 kayıt (~1 MB) |
| `pramod` | 5.810 kayıt (~139 KB) | 14.052 kayıt (~337 KB) |

**Süre.** Süreyi işlem sayısı belirler. Agrawal profili %80 dolulukta MB başına yaklaşık 600 işlem ister (256 MB ≈ 156 bin, 512 MB ≈ 307 bin işlem). Toplam süre ≈ işlem sayısı ÷ (periyot başına işlem × saniyedeki periyot sayısı). Örneğin periyot başına 50 işlem ve 100 ms periyotla 512 MB yaklaşık 10 dakika sürer.

`RESIZE_FILE` gibi veri yazmadan yer ayıran bir yöntemle diske yalnızca dosya sistemi yönetim verisi yazılır. Veri yazılarak oluşturulduğunda ise toplam yazma volume'ün onlarca katına çıkar (512 MB'ta yaklaşık 60 GB) ve flash bellekte gerçek aşınma oluşur.

## 10. Orijinal Geriatrix'ten farklar

Kütüphane Geriatrix'in algoritmasını izler; birebir aynı işlem dizisini değil, aynı dağılımları hedefler. Bilinçli farklar:

- **Kendi rastgele sayı üreteci:** Aynı seed her platformda aynı sonucu verir (Geriatrix'in çıktısıyla aynı değildir).
- **Silinecek dosya seçimi:** Tüm dosyalar taranmak yerine 32 rastgele aday arasından en uygunu seçilir; işlem başına maliyet sabit kalır.
- **`confidence`:** Ki-kare testi yerine yaş dağılımının basit bir hata ölçüsüyle yorumlanır: 0,9 değeri, hata %10'un altına inince dur demektir.
- **Parça parça çalışma:** Tüm iş `fs_aging_step()` çağrılarına bölünmüştür. Çoklu iş parçacığı ve boşta bekleme seçenekleri yoktur; süre sınırı dakika değil saniye cinsindendir.
- **Canlı dosya yokken:** Geriatrix silinecek dosya bulamazsa durur; bu kütüphane o işlemi oluşturmaya çevirir.
- **Ulaşılamayan doluluk hedefi:** Geriatrix sonsuza kadar döner; bu kütüphane bunu fark edip `FS_AGING_ERR_FILL_UNREACHABLE` ile durur.
- **Sabit bellek:** Dosya kayıtları, çağıranın verdiği sabit boyutlu bir dizide tutulur.

Orijinalle aynı kalan bir davranış: yaşlandırma sırasında canlı veri miktarı hedef doluluğun altında ya da üstünde dalgalanabilir; küçük disklerde bu sapma daha büyüktür.

## 11. Lisans

Bu kütüphane Geriatrix'in türev çalışmasıdır. Algoritma ve profil verileri orijinal Geriatrix reposundan alınmıştır:

- Copyright (c) 2018, Carnegie Mellon University. All rights reserved.
- Lisans: BSD-2-Clause. Tam metin [LICENSE.geriatrix](LICENSE.geriatrix) dosyasındadır ve korunmalıdır.
- Kaynak: https://github.com/saurabhkadekodi/geriatrix
- Makale: S. Kadekodi, V. Nagarajan, G. R. Ganger, G. A. Gibson, "Geriatrix: Aging what you see and what you don't see. A file system aging approach for modern storage systems", USENIX ATC 2018.

BSD-2-Clause gereği kaynak ve ikili dağıtımlarda telif bildirimi, koşullar ve sorumluluk reddi korunmalıdır. Kaynak dosyalardaki Geriatrix referansları silinmemelidir.
