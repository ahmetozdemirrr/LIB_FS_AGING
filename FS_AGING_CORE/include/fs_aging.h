/*
 * fs_aging.h - File system aging library.
 *
 * Portable core: no OS headers, no libc calls, no dynamic memory.
 * Based on Geriatrix: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
 *
 * Usage:
 *   1. Fill an fs_aging_platform_t with your platform callbacks.
 *   2. Allocate a pool of file records, sized with fs_aging_estimate_files().
 *   3. Call fs_aging_init() once.
 *   4. Call fs_aging_step() repeatedly until it returns anything other
 *      than FS_AGING_RUNNING.
 *
 * Phases: SETUP creates the directory tree, FILL fills the volume up to
 * the fill target, STABLE ages the files with random creates and deletes.
 */

#ifndef FS_AGING_H
#define FS_AGING_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Compile-time limits (override with -D if needed)                    */
/* ------------------------------------------------------------------ */

/* Use the same values in every file that includes this header. */

/* Longest path, including the mount point and the terminating '\0'. */
#ifndef FS_AGING_PATH_MAX
#define FS_AGING_PATH_MAX        256u
#endif

/* Maximum number of rows in each profile distribution. */
#ifndef FS_AGING_MAX_BINS
#define FS_AGING_MAX_BINS        48u
#endif

/* Candidates checked when choosing a file to delete. */
#ifndef FS_AGING_DELETE_SAMPLES
#define FS_AGING_DELETE_SAMPLES  32u
#endif

/* Pool records scanned per op at the end of the FILL phase. */
#ifndef FS_AGING_REBIN_CHUNK
#define FS_AGING_REBIN_CHUNK     1024u
#endif

/* Floating point type used by the library. */
typedef double fs_aging_float_t;

/* ------------------------------------------------------------------ */
/* Status codes                                                        */
/* ------------------------------------------------------------------ */

typedef enum {
    FS_AGING_RUNNING = 0,          /* still running, call step again        */
    FS_AGING_DONE_CONVERGED,       /* age distribution matches the profile  */
    FS_AGING_DONE_TOLERANCE,       /* age error is within confidence        */
    FS_AGING_DONE_WORKLOAD_LIMIT,  /* overwrite_runs limit reached          */
    FS_AGING_DONE_TIME_LIMIT,      /* time_limit_s limit reached            */
    FS_AGING_ERR_PARAM,            /* invalid argument (profile, config,
                                      platform or pool)                     */
    FS_AGING_ERR_POOL_FULL,        /* pool too small during FILL            */
    FS_AGING_ERR_PATH_TOO_LONG,    /* a path exceeds FS_AGING_PATH_MAX      */
    FS_AGING_ERR_PLATFORM          /* a callback failed, or no progress
                                      is possible                           */
} fs_aging_status_t;

/* Return values of the platform callbacks. */
typedef enum {
    FS_AGING_PLAT_OK = 0,          /* success                               */
    FS_AGING_PLAT_NO_SPACE,        /* create_file only: volume full, the
                                      library deletes a file instead        */
    FS_AGING_PLAT_ERROR            /* any other error: aging stops          */
} fs_aging_plat_result_t;

/* ------------------------------------------------------------------ */
/* Platform callbacks                                                  */
/* ------------------------------------------------------------------ */

/* path is valid only during the call; copy it if needed. */

typedef struct {
    /* Create a file of size_bytes. */
    fs_aging_plat_result_t (*create_file)(void *user, const char *path,
                                          uint64_t size_bytes);

    /* Delete a file. */
    fs_aging_plat_result_t (*delete_file)(void *user, const char *path);

    /* Create one directory level. Must return FS_AGING_PLAT_OK if the
     * directory already exists. */
    fs_aging_plat_result_t (*make_dir)(void *user, const char *path);

    /* Optional: monotonic time in milliseconds. NULL disables time_limit_s. */
    uint64_t (*now_ms)(void *user);

    /* Optional: log output. NULL disables logging. */
    void (*log)(void *user, const char *msg);

    /* User data passed to every callback. */
    void *user;
} fs_aging_platform_t;

/* ------------------------------------------------------------------ */
/* Aging profile                                                       */
/* ------------------------------------------------------------------ */

/* File age row. Cutoffs must be increasing; at least 2 rows. */
typedef struct {
    fs_aging_float_t age_cutoff;
    fs_aging_float_t weight;
} fs_aging_age_bin_t;

/* File size row. */
typedef struct {
    uint64_t         size_bytes;
    fs_aging_float_t weight;
} fs_aging_size_bin_t;

/* Directory row: depth, weight and number of sibling directories. */
typedef struct {
    uint32_t         depth;
    fs_aging_float_t weight;
    uint32_t         sibling_dirs;
} fs_aging_dir_bin_t;

/* A profile: age, size and directory distributions. */
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
/* Run configuration                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char       *mount_point;    /* mount point without trailing '/',
                                         e.g. "/fs0"; must stay valid
                                         during aging                       */
    uint64_t          volume_bytes;   /* volume size in bytes               */
    fs_aging_float_t  fill_ratio;     /* fill target, 0..1, e.g. 0.8; if it
                                         may be unreachable, also set
                                         overwrite_runs or time_limit_s     */
    uint32_t          seed;           /* random seed                        */
    uint32_t          overwrite_runs; /* stop after runs * volume_bytes
                                         written; 0 = no limit              */
    uint32_t          time_limit_s;   /* time limit; 0 = no limit           */
    fs_aging_float_t  confidence;     /* 0 = wait for full convergence;
                                         e.g. 0.9 stops at 10% age error    */
} fs_aging_config_t;

/* ------------------------------------------------------------------ */
/* File record pool                                                    */
/* ------------------------------------------------------------------ */

/* One file record per live file. Owned by the library; the caller only
 * allocates the array. */
typedef struct {
    uint64_t birth_tick;
    uint32_t next_free;
    uint16_t size_bin;
    uint16_t dir_bin;
    uint16_t sibling;
    uint16_t reserved;
} fs_aging_file_t;

/* ------------------------------------------------------------------ */
/* Statistics                                                          */
/* ------------------------------------------------------------------ */

/* Current phase. */
typedef enum {
    FS_AGING_PHASE_SETUP = 0,
    FS_AGING_PHASE_FILL,
    FS_AGING_PHASE_STABLE,
    FS_AGING_PHASE_DONE
} fs_aging_phase_t;

typedef struct {
    fs_aging_status_t status;
    fs_aging_phase_t  phase;
    uint64_t          ops;              /* ops in FILL and STABLE; same unit
                                           as convergence_ops               */
    uint64_t          creates;
    uint64_t          deletes;
    uint64_t          live_bytes;       /* bytes currently on the volume    */
    uint64_t          workload_bytes;   /* total bytes written so far       */
    uint32_t          live_files;
    uint64_t          convergence_ops;  /* ops needed for full convergence  */
    fs_aging_float_t  age_error;        /* age distribution error, 0..1     */
    uint64_t          elapsed_ms;       /* 0 if now_ms is not provided      */
} fs_aging_stats_t;

/* Distribution selector for fs_aging_bin_count() / fs_aging_get_bin(). */
typedef enum {
    FS_AGING_DIST_AGE = 0,
    FS_AGING_DIST_SIZE,
    FS_AGING_DIST_DIR
} fs_aging_dist_t;

/* ------------------------------------------------------------------ */
/* Context - internal state, do not modify the fields                  */
/* ------------------------------------------------------------------ */

typedef struct {
    const fs_aging_profile_t *profile;   /* must stay valid during aging */
    fs_aging_config_t         config;
    fs_aging_platform_t       platform;

    fs_aging_file_t *pool;
    uint32_t         pool_capacity;
    uint32_t         pool_high_water;
    uint32_t         free_head;

    fs_aging_status_t status;
    fs_aging_phase_t  phase;
    uint32_t          setup_dir_bin;
    uint32_t          setup_sibling;
    uint32_t          rebin_active;
    uint32_t          rebin_cursor;

    uint64_t rng_state;
    uint64_t tick;
    uint64_t fill_ops;
    uint64_t convergence_tick;
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
    fs_aging_float_t age_ratio [FS_AGING_MAX_BINS];
    uint64_t         age_cutoff_tick[FS_AGING_MAX_BINS];

    char path_buf[FS_AGING_PATH_MAX];
} fs_aging_ctx_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/* Validate the inputs and prepare the context. Does not touch the file
 * system. profile and config->mount_point must stay valid during aging.
 * Returns FS_AGING_RUNNING on success, otherwise FS_AGING_ERR_PARAM or
 * FS_AGING_ERR_PATH_TOO_LONG. */
fs_aging_status_t fs_aging_init(fs_aging_ctx_t             *ctx,
                                const fs_aging_profile_t   *profile,
                                const fs_aging_config_t    *config,
                                const fs_aging_platform_t  *platform,
                                fs_aging_file_t            *pool,
                                uint32_t                    pool_capacity);

/* Run at most max_ops ops, then return. An op is usually one file create
 * or delete (one directory in SETUP). */
fs_aging_status_t fs_aging_step(fs_aging_ctx_t *ctx, uint32_t max_ops);

/* Copy the current statistics. */
void fs_aging_get_stats(const fs_aging_ctx_t *ctx, fs_aging_stats_t *out);

/* Number of rows in a distribution. */
uint32_t fs_aging_bin_count(const fs_aging_ctx_t *ctx, fs_aging_dist_t dist);

/* Target and current share of one row. Returns 0, or -1 for bad
 * arguments. */
int fs_aging_get_bin(const fs_aging_ctx_t *ctx, fs_aging_dist_t dist,
                     uint32_t index,
                     fs_aging_float_t *ideal, fs_aging_float_t *actual);

/* Pool capacity needed for this profile and volume. Pass the result
 * directly as pool_capacity. 0 and 0xFFFFFFFF are not valid capacities:
 * they mean the inputs cannot be aged. */
uint32_t fs_aging_estimate_files(const fs_aging_profile_t *profile,
                                 uint64_t volume_bytes,
                                 fs_aging_float_t fill_ratio);

/* Short text for a status code. */
const char *fs_aging_status_str(fs_aging_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* FS_AGING_H */
