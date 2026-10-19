# LIB_FS_AGING — Ajan Talimatı

Bu dosya, bu klasördeki kütüphane üzerinde çalışacak kodlama ajanı içindir. Başlamadan önce tamamını oku. Belirsiz bir nokta varsa varsayım yapmadan önce sor.

## 1. Amaç

Geriatrix (https://github.com/saurabhkadekodi/geriatrix, USENIX ATC 2018, BSD-2-Clause) bir dosya sistemi yaşlandırma (aging) aracıdır. Profile dayalı, kontrollü dosya oluşturma ve silme işlemleriyle dosya sisteminde gerçekçi bir parçalanma üretir. Orijinal C++11 ile yazılmıştır ve Boost'a bağımlıdır.

Biz bu aracın **işlevsel karşılığını** saf C ile yazıyoruz. Amaç, şirketimizin RTOS'unda hem ARINC 653 hem POSIX profilinde dosya sistemi aging testleri yapabilmek. Orijinalle bire bir aynı dosya dizisini üretmek **gerekmiyor**. Gereken şey, profildeki yaş, boyut ve dizin derinliği dağılımlarına yakınsayan bir aging yükü.

Kullanıcı yalnızca **ARINC 653** tarafını geliştiriyor. POSIX port'unu başka bir ekip yazacak. O yüzden POSIX port kodu yazma; sadece onların uyacağı sözleşmeyi belgele.

## 2. Çalışma ortamı

- Windows, VS Code, Git, MSYS2 üzerinden gcc.
- Bu bilgisayar **internet bilgisayarı**. Şirketin RTOS reposu, APEX header'ları ve iç kütüphaneler buradan erişilebilir değil. Kod daha sonra şirket prosedürüyle iç ağa taşınacak.
- Orijinal Geriatrix referans olarak `../geriatrix` altında klonlu (yoksa kullanıcıya sor). Bu repo yalnızca okunur, değiştirme.
- Host'ta derleme ve test: `gcc -std=c99 -Wall -Wextra -Wpedantic -Wconversion -Wshadow`

## 3. Şirket reposundaki hedef konum

Kütüphane iç ağda şuraya yerleşecek:

```
RTOS/03_Verification/02_Testing/01_TestCases/UTILS_LIBS/COMMON/LIB_FS_AGING/
```

Aynı dizindeki diğer kütüphaneler `LIB_*` (kütüphane) ve `UTIL_*` (araç/script) adlandırmasını kullanıyor. Örnekler: `LIB_ARINC653_COMMON`, `LIB_POSIX_COMMON`, `LIB_FILE_OPERATIONS`, `LIB_USER_PRINT`, `LIB_TEST_CTX`, `LIB_STATIC_DATA_PARSER_SCRIPT`. Ortak tipler `UTILS_LIBS/BSP/LIB_CONFIGS_COMMON/PLATFORM_DEFINITIONS` altında. Test kayıtları üst dizindeki `directoryTestMapping.json` ve `swcTestSuiteMapping.json` dosyalarında tutuluyor. Bu dosyaların içeriğini göremiyoruz; o kısım iç ağda kullanıcı tarafından yapılacak.

## 4. Klasör yapısı

```
LIB_FS_AGING/
├── AGENT_PROMPT.md              # bu dosya
├── README.md                    # (yazılacak)
├── LICENSE.geriatrix            # (eklenecek) orijinal BSD-2 lisans metni
├── FS_AGING_CORE/
│   ├── include/
│   │   ├── fs_aging.h           # MEVCUT: dış API + platform arayüzü + tipler
│   │   └── fs_aging_profiles.h  # (yazılacak)
│   └── src/
│       ├── fs_aging.c           # MEVCUT: tüm aging mantığı
│       └── fs_aging_profiles.c  # (üretilecek)
├── ARINC653/
│   ├── include/                 # (yazılacak) fs_aging_arinc653.h
│   └── src/                     # (yazılacak) platform callback'leri + runner
├── TEST/
│   ├── src/host_test.c          # MEVCUT: sahte platformla host testi
│   └── Makefile                 # (yazılacak) host testleri için
└── TOOLS/
    └── gen_profiles.py          # (yazılacak) profil .txt → C tablosu
```

Her alt kütüphanede `include/` ve `src/` ayrımı korunmalı. Gereksiz dosya veya katman ekleme; kullanıcı yapının sade kalmasını istiyor.

## 5. Mevcut tasarım (değiştirmeden önce anla)

**Katmanlar.** FS_AGING_CORE saf C99'dur. Hiçbir OS, APEX veya POSIX header'ı include etmez, libc fonksiyonu çağırmaz (yalnızca `stdint.h` ve `stddef.h`), bellek ayırmaz. Dosya sistemine sadece `fs_aging_platform_t` içindeki callback'ler üzerinden erişir: `create_file`, `delete_file`, `make_dir` zorunlu; `now_ms` ve `log` opsiyonel. Port katmanları bu yapıyı doldurur.

**Bellek.** Dosya kayıt havuzunu (`fs_aging_file_t`, kayıt başına 24 byte) çağıran taraf verir. ARINC tarafında bu statik bir dizidir. Gerekli kapasite `fs_aging_estimate_files()` ile tahmin edilir; %25 pay önerilir. Context (`fs_aging_ctx_t`) de çağıran tarafta durur.

**Yürütme.** `fs_aging_init()` bir kez çağrılır ve dosya sistemine dokunmaz. `fs_aging_step(ctx, max_ops)` her çağrıda en fazla `max_ops` işlem yapıp döner. Bu tasarım ARINC partition penceresini taşırmamak için seçildi ve korunmalı. Asamalar şunlardır:
- `SETUP`: dizin ağacı, her işlemde bir yaprak dizin.
- `FILL`: volume × `fill_ratio` kadar profilden örneklenen boyutlarla doldurma.
- `STABLE`: 50/50 oluştur/sil; tam yakınsama hedef işlem sayısına (orijinaldeki `calculateT`) ulaşılınca biter.

**Orijinalden bilinçli farklar.** Bunlar `fs_aging.c` başında da yazılı:
- PRNG dahili xorshift64*. Aynı seed her platformda aynı diziyi üretir.
- Silinecek dosya `FS_AGING_DELETE_SAMPLES` (32) rastgele aday arasından seçilir. Öncelik önce yaş, sonra boyut, sonra dizin kovası fazlalığı.
- Yaş kovası dosyanın doğum tick'inden hesaplanır (kararlı aşamada eşikler sabit).
- `confidence` parametresi yaş dağılımının toplam varyasyon mesafesine göre yorumlanır (0.9 → hata ≤ 0.1). Ki-kare/libm kullanılmaz.
- Çok thread (`-t`) ve boşta bekleme (`-p`) yok. `-q` ve `-x/-y/-z` çıktıları port'un işi; core `fs_aging_get_bin()` sunar.

**Yol formatı.** `<mount>/d1/d2/.../d(k-1)/<yaprak>/f<doğum_tick>`. Yaprak, kardeş dizin varsa `d<1..n>`, yoksa `d<k>`. Derinlik 0'da dosya doğrudan mount altına yazılır. `fs_aging_init()` profilin üreteceği en uzun yolu `FS_AGING_PATH_MAX` (varsayılan 256) ile karşılaştırır.

**Float.** Core `typedef double fs_aging_float_t;` kullanır. ARINC tarafında `float8_gt` adında 8 byte'lık bir float primitive'i var. Core bunu bilmez; ARINC port'u iki tipin aynı boyutta olduğunu derleme zamanında doğrular.

**Son doğrulanmış test sonucu.** Agrawal profili ile 256 MB'ta tam yakınsama sağlandı: yaş hatası %0.23, tutarsızlık 0. 2 GB'ta da sorunsuz çalıştı.

## 6. Kesin kurallar

1. FS_AGING_CORE'a OS/APEX/POSIX header'ı, libc çağrısı, `malloc`, özyineleme veya VLA ekleme.
2. `fs_aging.h` içindeki **public API'yi** (fonksiyon imzaları, `fs_aging_platform_t`, profil ve config tipleri) kullanıcıya sormadan değiştirme. POSIX ekibiyle aradaki sözleşme bu. Context'in iç alanları değişebilir.
3. Kodlama stili mevcut dosyalarla aynı kalmalı: C99, `uint32_t`/`uint64_t` gibi sabit genişlikli tipler, işaretsiz sabitlerde `u` son eki, her `if`/`for` gövdesinde süslü parantez, statik yardımcı fonksiyonlar. Yorumlar Türkçe ve ASCII karakterle (ş/ğ/ı yerine s/g/i) yazılır.
4. APEX veya şirket API'si imzası **uydurma**. Gerçek header'lar yoksa kullanıcıdan iste. Beklerken çalışmak gerekiyorsa `TEST/stubs/` altında açıkça `/* STUB - gercek header ile degistirilecek */` diye işaretlenmiş minimal bir stub kullan ve her varsayımı listele.
5. Her değişiklikten sonra katı bayraklarla uyarısız derlenmeli ve host testleri geçmeli.
6. Her tamamlanan görevden sonra açıklayıcı bir mesajla commit at.

## 7. Kalan işler (sırayla)

### Görev 1 — Profil tabloları
- `TOOLS/gen_profiles.py`: `../geriatrix/profiles/*/` altındaki 8 profili (`agrawal`, `dabre`, `douceur`, `grundman`, `meyer`, `pramod`, `wang_lanl`, `wang_os`) okuyup `FS_AGING_CORE/src/fs_aging_profiles.c` üretir. Dosya formatı: ilk satır satır sayısı, sonra `age: <eşik> <ağırlık>`, `size: <byte> <ağırlık>`, `dir: <derinlik> <ağırlık> <kardeş_dizin>`. Dosya sonundaki "Format:" açıklama bloğunu yok say.
- `FS_AGING_CORE/include/fs_aging_profiles.h`: her profil için `extern const fs_aging_profile_t fs_aging_profile_<ad>;`, ayrıca tüm profilleri içeren bir dizi, bunun eleman sayısı ve `const fs_aging_profile_t *fs_aging_profile_find(const char *name);` fonksiyonu. Bu fonksiyon libc'siz yazılmalı.
- Üretilen dosyanın başında "otomatik üretildi, elle düzenleme" uyarısı bulunmalı. Çıktı deterministik olmalı: aynı girdi her zaman aynı dosyayı üretmeli.
- Kabul: her profil `FS_AGING_MAX_BINS` (48) sınırına uyuyor (en büyüğü `pramod`, 37 dizin satırı). Script her profil için `mount="/fs0"` ile en uzun yol uzunluğunu raporluyor. Bu sayı ARINC dosya sistemi yol limitiyle karşılaştırılacak.

### Görev 2 — Core iyileştirmeleri
- `enter_stable()` şu an bütün havuzu tek seferde tarıyor. Bunu step'ler arasına yayılan bir ara aşamaya çevir. Örneğin her işlemde `FS_AGING_REBIN_CHUNK` (varsayılan 1024) kayıt taransın. Amaç, tek bir `fs_aging_step()` çağrısının süresinin havuz boyutundan bağımsız ve sınırlı olması.
- Kararlı aşamada canlı verinin hedefin altına düşmesini incele (2 GB × 0.8 hedefe karşı ~1.48 GB gözlendi). Orijinal `performStableAging` ile karşılaştır. Fark orijinalin davranışıysa koda ve README'ye not düş. Davranışı değiştirmeden önce kullanıcıya sor.
- Kabul: Görev 3'teki tüm testler geçiyor ve Agrawal sonuçları önceki doğrulamayla tutarlı.

### Görev 3 — Host testleri
- `host_test.c`'yi profil adı, volume ve confidence'ı komut satırından alacak şekilde genişlet ve 8 profilin hepsinde çalıştır.
- Ek senaryolar (ayrı test fonksiyonları ya da ayrı bir `TEST/src/edge_tests.c`):
  - Havuz kapasitesi çok küçükken → `FS_AGING_ERR_POOL_FULL`.
  - Sahte platform erken `NO_SPACE` döndüğünde aging hatasız devam ediyor.
  - Platform `ERROR` döndüğünde → `FS_AGING_ERR_PLATFORM`.
  - `-DFS_AGING_PATH_MAX=64` ile derin bir profil → `FS_AGING_ERR_PATH_TOO_LONG`.
  - Geçersiz parametreler (NULL pointer, `fill_ratio` 0 veya >1, artmayan yaş eşikleri) → `FS_AGING_ERR_PARAM`.
  - Determinizm: aynı seed ile iki çalıştırmada istatistikler bire bir aynı, farklı seed ile farklı.
  - Adım bağımsızlığı: `max_ops=1` ile `max_ops=1000` aynı sonucu veriyor. Bu ARINC için kritik.
- `TEST/Makefile`: `make` ile derleme, `make test` ile tüm testleri çalıştırma (MSYS2 `make` veya `mingw32-make` ile çalışmalı).
- Kabul: tüm testler 0 ile çıkıyor ve test çıktısında her profilin yaş, boyut ve dizin hataları özetleniyor.

### Görev 4 — ARINC 653 port'u (kullanıcının header'ları gerekli)
Başlamadan önce kullanıcıdan şunları iste: kullanılan RTOS ve APEX header'larındaki ilgili imzalar (process oluşturma, periyodik bekleme, zaman, partition modu, dosya servisleri), `LIB_FILE_OPERATIONS` ve `LIB_USER_PRINT` arayüzleri, `float8_gt` tanımının bulunduğu header, partition bellek bütçesi, periyot ve pencere süresi, dosya sistemi yol uzunluğu ve derinlik limitleri, `posix_fallocate` karşılığı (ARINC 653 Part 2'de `RESIZE_FILE` olabilir; yoksa veri yazılmalı).

Yazılacaklar:
- `ARINC653/include/fs_aging_arinc653.h`: port'un dış arayüzü (ör. `fs_aging_a653_start(profil, config)`).
- `ARINC653/src/fs_aging_arinc653.c`: `fs_aging_platform_t` callback'leri (dosya oluştur, sil, dizin oluştur, zaman, log). `float8_gt` ile `fs_aging_float_t` boyut eşitliği için derleme zamanı kontrolü. Mümkünse `LIB_FILE_OPERATIONS` ve `LIB_USER_PRINT` yeniden kullanılmalı.
- `ARINC653/src/fs_aging_arinc653_runner.c`: INIT modunda statik havuz ve context ile `fs_aging_init`, periyodik process oluşturma ve normal moda geçiş. Periyodik process her periyotta `fs_aging_step(ctx, N)` çağırır; N konfigüre edilebilir olmalı. Bitişte istatistik ve dağılım raporu basılır, hata durumu health monitor'e bildirilir (kullanıcıya nasıl yapıldığını sor).
- Kabul: gerçek header'larla iç ağda derlenecek; burada stub'larla uyarısız derleniyor ve tüm varsayımlar listelenmiş.

### Görev 5 — Dokümantasyon ve lisans
- `README.md`: amaç, klasör yapısı, kullanım örneği, bellek boyutlandırma, orijinalden farklar, POSIX ekibi için port sözleşmesi (hangi callback'in hangi davranışı beklediği, dönüş kodları, `make_dir`'in var olan dizinde `OK` döndürmesi, yol formatı) ve testlerin nasıl çalıştırılacağı.
- `LICENSE.geriatrix`: orijinal reponun `LICENSE.txt` içeriği. Türev çalışma olduğu için BSD-2 telif bildirimi korunmalı. Kaynak dosyalarda orijinale referans zaten var, korunmalı.

## 8. Çalışma şekli

- Küçük adımlarla ilerle. Her görevin başında ne yapacağını kısaca özetle, sonunda neyi değiştirdiğini ve test sonuçlarını raporla.
- Orijinal davranışı anlamak için `../geriatrix/src/geriatrix.cpp` ve `*_bucket.h` dosyalarına bak. Ama Boost/C++ yapısını taşımaya çalışma; işlevsel eşdeğerlik yeterli.
- Public API'yi değiştirmen, bir davranışı orijinalden farklılaştırman ya da yeni bir dosya veya klasör eklemen gerekiyorsa önce kullanıcıya sor.
- Görev 4'e, kullanıcı header'ları sağlayana kadar stub dışında başlama. Görev 1–3 ve 5 bu bilgisayarda tamamlanabilir.