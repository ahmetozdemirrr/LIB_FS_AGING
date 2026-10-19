/*
 * fs_aging_profiles.h - Hazir Geriatrix aging profilleri
 *
 * Orijinal Geriatrix reposundaki 8 profilin (geriatrix/profiles/<ad>/)
 * C tablosu karsiliklari. Tablolar fs_aging_profiles.c icindedir; o dosya
 * TOOLS/gen_profiles.py ile uretilir, elle duzenlenmez.
 *
 * Kullanim:
 *   const fs_aging_profile_t *pr = fs_aging_profile_find("agrawal");
 *   if (pr != NULL) {
 *       st = fs_aging_init(&ctx, pr, &cfg, &plat, pool, cap);
 *   }
 *
 * Profil adi (profile->name) Geriatrix'teki klasor adidir.
 *
 * Orijinal: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
 */

#ifndef FS_AGING_PROFILES_H
#define FS_AGING_PROFILES_H

#include "fs_aging.h"

#ifdef __cplusplus
extern "C" {
#endif

/* fs_aging_profiles[] dizisindeki profil sayisi. */
#define FS_AGING_PROFILE_COUNT 8u

extern const fs_aging_profile_t fs_aging_profile_agrawal;
extern const fs_aging_profile_t fs_aging_profile_dabre;
extern const fs_aging_profile_t fs_aging_profile_douceur;
extern const fs_aging_profile_t fs_aging_profile_grundman;
extern const fs_aging_profile_t fs_aging_profile_meyer;
extern const fs_aging_profile_t fs_aging_profile_pramod;
extern const fs_aging_profile_t fs_aging_profile_wang_lanl;
extern const fs_aging_profile_t fs_aging_profile_wang_os;

/* Tum profiller, ada gore alfabetik sirada. */
extern const fs_aging_profile_t *const fs_aging_profiles[FS_AGING_PROFILE_COUNT];

/* Ada gore profil arar (buyuk/kucuk harf duyarli, tam eslesme).
 * name NULL ise veya profil yoksa NULL doner. libc kullanmaz. */
const fs_aging_profile_t *fs_aging_profile_find(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* FS_AGING_PROFILES_H */
