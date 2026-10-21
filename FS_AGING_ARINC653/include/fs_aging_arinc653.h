/*
 * fs_aging_arinc653.h - ARINC 653 port of the fs_aging library.
 *
 * Ages a file system from an ARINC 653 partition. A periodic process
 * calls fs_aging_step() once per period, so one period never runs more
 * than ops_per_period file operations.
 *
 * Usage, in the partition's INIT code:
 *   1. Fill an fs_aging_a653_config_t.
 *   2. Call fs_aging_a653_start(). It checks the setup and creates the
 *      periodic aging process. It does not touch the file system.
 *   3. Switch the partition to NORMAL mode.
 * The aged disk is the result: nothing is removed after aging. To reuse a
 * disk, clean it first (clean_first or fs_aging_a653_start_cleanup()).
 *
 * All aging work runs inside the aging process. When aging ends, with
 * success or error, a summary is printed, on_done is called and the
 * partition is switched to IDLE (see FS_AGING_A653_IDLE_AT_END).
 *
 * All output, including errors, goes through FS_AGING_A653_PRINT. Error
 * lines start with "fs_aging: error:".
 *
 * Original: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
 */

#ifndef FS_AGING_ARINC653_H
#define FS_AGING_ARINC653_H

#include "gzisTypes.h"
#include "arinc653/ARINC653.h"
#include "fs_aging.h"
#include "fs_aging_profiles.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Number of file records in the static pool (24 bytes each). Aging is
 * not started if the chosen profile and volume need more. */
#ifndef FS_AGING_A653_POOL_CAPACITY
#define FS_AGING_A653_POOL_CAPACITY  16384u
#endif

/* 1: size new files with RESIZE_FILE (fast). 0: write data in
 * MAXIMUM_ATOMICITY chunks. Use 0 if RESIZE_FILE does not allocate
 * blocks on the target file system. */
#ifndef FS_AGING_A653_USE_RESIZE
#define FS_AGING_A653_USE_RESIZE     1
#endif

/* Largest file the file system accepts (RESIZE_FILE limit: 4 GiB - 1).
 * Larger profile sizes are reported to the core as "no space". */
#ifndef FS_AGING_A653_MAX_FILE_SIZE
#define FS_AGING_A653_MAX_FILE_SIZE  0xFFFFFFFFull
#endif

/* Deepest directory the file system allows, counting every path component
 * including the mount point (FS_MAXIMUM_DIRECTORY_HIERARCHY_DEPTH). A
 * profile that needs deeper directories is rejected at start. */
#ifndef FS_AGING_A653_MAX_DIR_DEPTH
#define FS_AGING_A653_MAX_DIR_DEPTH  32u
#endif

/* 1: switch the partition to IDLE when aging ends, so the program stops.
 * 0: only the aging process ends; the partition keeps running. */
#ifndef FS_AGING_A653_IDLE_AT_END
#define FS_AGING_A653_IDLE_AT_END    1
#endif

/* printf-like output function used for all messages. */
#ifndef FS_AGING_A653_PRINT
#define FS_AGING_A653_PRINT          TEST_PRINT
#endif

typedef struct {
    /* Aging parameters */
    const char       *profile_name;    /* built-in profile, e.g. "agrawal"  */
    const char       *mount_point;     /* e.g. "/mnt/fs1", no trailing '/';
                                          must stay valid during aging      */
    uint64_t          volume_bytes;    /* usable space of the empty volume;
                                          0 = read the free space with
                                          GET_VOLUME_STATUS at start        */
    fs_aging_float_t  fill_ratio;      /* fill target, e.g. 0.8             */
    uint32_t          seed;            /* random seed                       */
    uint32_t          overwrite_runs;  /* 0 = no workload limit             */
    uint32_t          time_limit_s;    /* 0 = no time limit                 */
    fs_aging_float_t  confidence;      /* 0 = full convergence              */
    uint32_t          clean_first;     /* 1 = remove everything under
                                          mount_point before aging          */

    /* Aging process */
    uint32_t          ops_per_period;  /* file operations per period        */
    SYSTEM_TIME_TYPE  period;          /* process period                    */
    SYSTEM_TIME_TYPE  time_capacity;   /* process time capacity             */
    STACK_SIZE_TYPE   stack_size;      /* process stack size                */
    PRIORITY_TYPE     priority;        /* process base priority             */
    uint32_t          core_id;         /* processor core of the process     */
    uint32_t          progress_every;  /* print progress every N periods;
                                          0 = off                           */

    /* Optional: called once from the aging process when aging ends,
     * after success or error, before the partition goes IDLE. */
    void (*on_done)(fs_aging_status_t status);
} fs_aging_a653_config_t;

/* Checks the configuration and creates and starts the periodic aging
 * process. Call in INIT mode. Returns FS_AGING_RUNNING on success;
 * otherwise prints the error and returns its status. */
fs_aging_status_t fs_aging_a653_start(const fs_aging_a653_config_t *config);

/* Cleanup only: creates the same periodic process, which removes every
 * file and directory under mount_point (not the mount point itself), up
 * to ops_per_period entries per period, then calls on_done and goes IDLE.
 * Uses mount_point and the process fields of config. on_done gets
 * FS_AGING_DONE_CONVERGED when the volume is empty, otherwise an error.
 * Call in INIT mode. Never runs automatically after aging. */
fs_aging_status_t fs_aging_a653_start_cleanup(const fs_aging_a653_config_t *config);

/* Current statistics of the running or finished aging. */
void fs_aging_a653_get_stats(fs_aging_stats_t *out);

/* Prints the statistics and the age, size and directory distributions. */
void fs_aging_a653_print_report(void);

/* Fills the platform callbacks for the ARINC 653 file system services.
 * Only needed when driving fs_aging_step() from your own process. */
void fs_aging_a653_get_platform(fs_aging_platform_t *out);

/* Free bytes of the volume that holds mount_point (uses a temporary
 * file). Returns 0 on failure. Call in NORMAL mode. */
uint64_t fs_aging_a653_free_bytes(const char *mount_point);

#ifdef __cplusplus
}
#endif

#endif /* FS_AGING_ARINC653_H */
