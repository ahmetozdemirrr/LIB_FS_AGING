/*
 * fs_aging_arinc653_runner.c - Runs fs_aging in an ARINC 653 partition.
 *
 * fs_aging_a653_start() checks the setup in INIT mode and creates one
 * periodic process. In NORMAL mode that process runs up to
 * ops_per_period operations per period until aging ends, then prints a
 * summary, calls on_done and switches the partition to IDLE. Errors are
 * printed and end the run the same way.
 *
 * The same process can first remove everything under the mount point
 * (cleanup), one directory entry per operation.
 *
 * Only one run per partition: the pool and context are static.
 *
 * Original: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
 */

#include <string.h>
#include "gzisTypes.h"
#include "arinc653/ARINC653.h"
#include "fs/fileSystem.h"
#include "testUtils/lib/user/testUtils.h"
#include "fs_aging_arinc653.h"

/* What the process does */
#define A653_DO_CLEAN  1u
#define A653_DO_AGE    2u

static fs_aging_ctx_t           a653_ctx;
static fs_aging_file_t          a653_pool[FS_AGING_A653_POOL_CAPACITY];
static fs_aging_a653_config_t   a653_cfg;
static const fs_aging_profile_t *a653_profile;
static uint32_t                 a653_tasks;

/* Cleanup state: current directory and counters */
static FILE_NAME_TYPE           a653_clean_path;
static size_t                   a653_clean_root_len;
static unsigned long            a653_clean_files;
static unsigned long            a653_clean_dirs;

/* Integer per mille, so output does not depend on %f support. */
static unsigned int per_mille(fs_aging_float_t x)
{
    if (!(x > 0.0)) {
        return 0u;
    }
    return (unsigned int)(x * 1000.0 + 0.5);
}

static int is_done_ok(fs_aging_status_t status)
{
    return (status == FS_AGING_DONE_CONVERGED) ||
           (status == FS_AGING_DONE_TOLERANCE) ||
           (status == FS_AGING_DONE_WORKLOAD_LIMIT) ||
           (status == FS_AGING_DONE_TIME_LIMIT);
}

/* Number of non-empty components of a path, e.g. "/mnt/fs1" -> 2. */
static uint32_t path_components(const char *path)
{
    uint32_t n = 0u;
    size_t   i;

    for (i = 0u; path[i] != '\0'; i++) {
        if ((path[i] != '/') && ((i == 0u) || (path[i - 1u] == '/'))) {
            n++;
        }
    }
    return n;
}

static uint32_t profile_max_depth(const fs_aging_profile_t *profile)
{
    uint32_t i;
    uint32_t depth = 0u;

    for (i = 0u; i < profile->dir_count; i++) {
        if (profile->dirs[i].depth > depth) {
            depth = profile->dirs[i].depth;
        }
    }
    return depth;
}

/* ------------------------------------------------------------------ */
/* Cleanup                                                             */
/* ------------------------------------------------------------------ */

static int is_dot_entry(const char *name)
{
    return (name[0] == '.') &&
           ((name[1] == '\0') || ((name[1] == '.') && (name[2] == '\0')));
}

/* One cleanup op on the current directory: remove its first file,
 * step into its first subdirectory, or remove it when empty and go back
 * to the parent. Returns 1 when the mount point is empty, 0 to continue,
 * -1 on error. */
static int cleanup_one(void)
{
    DIRECTORY_ID_TYPE    dir_id      = 0;
    DIRECTORY_ENTRY_TYPE entry;
    ENTRY_KIND_TYPE      kind        = END_OF_DIRECTORY;
    RETURN_CODE_TYPE     return_code = NO_ERROR;
    RETURN_CODE_TYPE     ignore_code = NO_ERROR;
    FILE_ERRNO_TYPE      file_errno  = 0;
    FILE_ERRNO_TYPE      ignore_errno = 0;
    size_t               len = strlen(a653_clean_path);
    size_t               name_len;
    uint32_t             reads = 0u;

    OPEN_DIRECTORY(a653_clean_path, &dir_id, &return_code, &file_errno);
    if (NO_ERROR != return_code) {
        FS_AGING_A653_PRINT("fs_aging: error: OPEN_DIRECTORY %s failed (%d, errno %d)\r\n",
                            a653_clean_path, (int)return_code, (int)file_errno);
        return -1;
    }
    entry[0] = '\0';
    do {
        READ_DIRECTORY(dir_id, &entry, &kind, &return_code, &file_errno);
        reads++;
    } while ((NO_ERROR == return_code) && (END_OF_DIRECTORY != kind) &&
             is_dot_entry(entry) && (reads < 3u));
    CLOSE_DIRECTORY(dir_id, &ignore_code, &ignore_errno);
    if (NO_ERROR != return_code) {
        FS_AGING_A653_PRINT("fs_aging: error: READ_DIRECTORY %s failed (%d, errno %d)\r\n",
                            a653_clean_path, (int)return_code, (int)file_errno);
        return -1;
    }

    /* Empty directory: done at the mount point, otherwise remove it */
    if ((END_OF_DIRECTORY == kind) || is_dot_entry(entry)) {
        if (len <= a653_clean_root_len) {
            return 1;
        }
        REMOVE_DIRECTORY(a653_clean_path, &return_code, &file_errno);
        if (NO_ERROR != return_code) {
            FS_AGING_A653_PRINT("fs_aging: error: REMOVE_DIRECTORY %s failed (%d, errno %d)\r\n",
                                a653_clean_path, (int)return_code, (int)file_errno);
            return -1;
        }
        a653_clean_dirs++;
        while ((len > a653_clean_root_len) && (a653_clean_path[len - 1u] != '/')) {
            len--;
        }
        a653_clean_path[len - 1u] = '\0';
        return 0;
    }

    name_len = strlen(entry);
    if ((len + 1u + name_len + 1u) > sizeof(a653_clean_path)) {
        FS_AGING_A653_PRINT("fs_aging: error: path too long below %s\r\n", a653_clean_path);
        return -1;
    }
    a653_clean_path[len] = '/';
    memcpy(&a653_clean_path[len + 1u], entry, name_len + 1u);

    if (DIRECTORY_ENTRY == kind) {
        return 0;                       /* continue inside it */
    }

    REMOVE_FILE(a653_clean_path, &return_code, &file_errno);
    if (NO_ERROR != return_code) {
        FS_AGING_A653_PRINT("fs_aging: error: REMOVE_FILE %s failed (%d, errno %d)\r\n",
                            a653_clean_path, (int)return_code, (int)file_errno);
        return -1;
    }
    a653_clean_path[len] = '\0';
    a653_clean_files++;
    return 0;
}

/* Removes everything under the mount point, ops_per_period entries per
 * period. Returns FS_AGING_DONE_CONVERGED when the volume is empty. */
static fs_aging_status_t run_cleanup(void)
{
    RETURN_CODE_TYPE return_code = NO_ERROR;
    uint32_t         period_count = 0u;
    uint32_t         n;
    int              result = 0;

    a653_clean_root_len = strlen(a653_cfg.mount_point);
    if ((a653_clean_root_len + 1u) > sizeof(a653_clean_path)) {
        FS_AGING_A653_PRINT("fs_aging: error: mount point too long\r\n");
        return FS_AGING_ERR_PARAM;
    }
    memcpy(a653_clean_path, a653_cfg.mount_point, a653_clean_root_len + 1u);
    a653_clean_files = 0u;
    a653_clean_dirs  = 0u;
    FS_AGING_A653_PRINT("fs_aging: cleanup of %s started\r\n", a653_cfg.mount_point);

    for (;;) {
        for (n = 0u; (n < a653_cfg.ops_per_period) && (0 == result); n++) {
            result = cleanup_one();
        }
        period_count++;
        if ((a653_cfg.progress_every != 0u) &&
            ((period_count % a653_cfg.progress_every) == 0u)) {
            FS_AGING_A653_PRINT("fs_aging: cleanup period %u, %lu files and %lu directories removed\r\n",
                                (unsigned int)period_count, a653_clean_files, a653_clean_dirs);
        }
        if (0 != result) {
            break;
        }
        PERIODIC_WAIT(&return_code);
        if (NO_ERROR != return_code) {
            FS_AGING_A653_PRINT("fs_aging: error: PERIODIC_WAIT failed (%d)\r\n", (int)return_code);
            result = -1;
            break;
        }
    }

    FS_AGING_A653_PRINT("fs_aging: cleanup: %lu files and %lu directories removed\r\n",
                        a653_clean_files, a653_clean_dirs);
    return (result > 0) ? FS_AGING_DONE_CONVERGED : FS_AGING_ERR_PLATFORM;
}

/* ------------------------------------------------------------------ */
/* Aging                                                               */
/* ------------------------------------------------------------------ */

/* Sizes the pool and initializes the core for the given volume size. */
static fs_aging_status_t prepare(uint64_t volume_bytes)
{
    fs_aging_platform_t platform;
    fs_aging_config_t   cfg;
    fs_aging_status_t   status;
    uint32_t            needed;

    needed = fs_aging_estimate_files(a653_profile, volume_bytes, a653_cfg.fill_ratio);
    if ((0u == needed) || (needed > FS_AGING_A653_POOL_CAPACITY)) {
        FS_AGING_A653_PRINT("fs_aging: error: pool needs %u records, capacity is %u\r\n",
                            (unsigned int)needed,
                            (unsigned int)FS_AGING_A653_POOL_CAPACITY);
        return FS_AGING_ERR_POOL_FULL;
    }

    cfg.mount_point    = a653_cfg.mount_point;
    cfg.volume_bytes   = volume_bytes;
    cfg.fill_ratio     = a653_cfg.fill_ratio;
    cfg.seed           = a653_cfg.seed;
    cfg.overwrite_runs = a653_cfg.overwrite_runs;
    cfg.time_limit_s   = a653_cfg.time_limit_s;
    cfg.confidence     = a653_cfg.confidence;

    fs_aging_a653_get_platform(&platform);

    status = fs_aging_init(&a653_ctx, a653_profile, &cfg, &platform,
                           a653_pool, FS_AGING_A653_POOL_CAPACITY);
    if (status != FS_AGING_RUNNING) {
        FS_AGING_A653_PRINT("fs_aging: error: init failed: %s\r\n", fs_aging_status_str(status));
    }
    return status;
}

static void print_progress(uint32_t period_count)
{
    fs_aging_stats_t st;

    fs_aging_get_stats(&a653_ctx, &st);
    FS_AGING_A653_PRINT("fs_aging: period %u phase %d ops %llu/%llu live %llu MB\r\n",
                        (unsigned int)period_count, (int)st.phase,
                        (unsigned long long)st.ops,
                        (unsigned long long)st.convergence_ops,
                        (unsigned long long)(st.live_bytes >> 20));
}

static void finish(fs_aging_status_t status)
{
    fs_aging_stats_t st;

    if (a653_ctx.profile != NULL) {
        fs_aging_a653_print_report();
        fs_aging_get_stats(&a653_ctx, &st);
        FS_AGING_A653_PRINT("fs_aging: summary: %s, profile %s, ops %llu, live %llu MB, "
                            "age error %u per mille\r\n",
                            fs_aging_status_str(status), a653_profile->name,
                            (unsigned long long)st.ops,
                            (unsigned long long)(st.live_bytes >> 20),
                            per_mille(st.age_error));
        if (!is_done_ok(status)) {
            FS_AGING_A653_PRINT("fs_aging: error: aging failed: %s\r\n", fs_aging_status_str(status));
        }
    }
    if (NULL != a653_cfg.on_done) {
        a653_cfg.on_done(status);
    }

#if FS_AGING_A653_IDLE_AT_END
    {
        RETURN_CODE_TYPE return_code = NO_ERROR;
        FS_AGING_A653_PRINT("fs_aging: done, switching partition to IDLE\r\n");
        SET_PARTITION_MODE(IDLE, &return_code);
        if (NO_ERROR != return_code) {
            FS_AGING_A653_PRINT("fs_aging: error: SET_PARTITION_MODE(IDLE) failed (%d)\r\n",
                                (int)return_code);
        }
    }
#endif
}

/* Entry point of the periodic process: cleanup and/or aging. */
static void a653_aging_process(void)
{
    RETURN_CODE_TYPE  return_code = NO_ERROR;
    fs_aging_status_t status      = FS_AGING_RUNNING;
    uint32_t          period_count = 0u;

    if (0u != (a653_tasks & A653_DO_CLEAN)) {
        status = run_cleanup();
        if ((status != FS_AGING_DONE_CONVERGED) || (0u == (a653_tasks & A653_DO_AGE))) {
            finish(status);
            return;
        }
        /* Start aging in a fresh period */
        PERIODIC_WAIT(&return_code);
        if (NO_ERROR != return_code) {
            FS_AGING_A653_PRINT("fs_aging: error: PERIODIC_WAIT failed (%d)\r\n", (int)return_code);
            finish(FS_AGING_ERR_PLATFORM);
            return;
        }
        status = FS_AGING_RUNNING;
    }

    if (0u == a653_cfg.volume_bytes) {
        uint64_t free_bytes = fs_aging_a653_free_bytes(a653_cfg.mount_point);
        if (0u == free_bytes) {
            FS_AGING_A653_PRINT("fs_aging: error: cannot read free space of %s\r\n",
                                a653_cfg.mount_point);
            finish(FS_AGING_ERR_PLATFORM);
            return;
        }
        FS_AGING_A653_PRINT("fs_aging: volume free space %llu bytes\r\n",
                            (unsigned long long)free_bytes);
        status = prepare(free_bytes);
        if (status != FS_AGING_RUNNING) {
            finish(status);
            return;
        }
    }

    do {
        status = fs_aging_step(&a653_ctx, a653_cfg.ops_per_period);
        period_count++;

        if ((a653_cfg.progress_every != 0u) &&
            ((period_count % a653_cfg.progress_every) == 0u)) {
            print_progress(period_count);
        }
        if (status == FS_AGING_RUNNING) {
            PERIODIC_WAIT(&return_code);
            if (NO_ERROR != return_code) {
                FS_AGING_A653_PRINT("fs_aging: error: PERIODIC_WAIT failed (%d)\r\n", (int)return_code);
                status = FS_AGING_ERR_PLATFORM;
            }
        }
    } while (status == FS_AGING_RUNNING);

    finish(status);
}

/* Creates the aging process, pins it to its core and starts it. */
static fs_aging_status_t create_aging_process(void)
{
    static const char      name[] = "FS_AGING";
    PROCESS_ATTRIBUTE_TYPE attr;
    PROCESS_ID_TYPE        process_id;
    RETURN_CODE_TYPE       return_code = NO_ERROR;

    memset(&attr, 0, sizeof(attr));
    attr.PERIOD        = a653_cfg.period;
    attr.TIME_CAPACITY = a653_cfg.time_capacity;
    attr.ENTRY_POINT   = &a653_aging_process;
    attr.STACK_SIZE    = a653_cfg.stack_size;
    attr.BASE_PRIORITY = a653_cfg.priority;
    attr.DEADLINE      = SOFT;
    memcpy(attr.NAME, name, sizeof(name));

    CREATE_PROCESS(&attr, &process_id, &return_code);
    if (NO_ERROR == return_code) {
        INITIALIZE_PROCESS_CORE_AFFINITY(process_id, a653_cfg.core_id, &return_code);
    }
    if (NO_ERROR == return_code) {
        START(process_id, &return_code);
    }
    if (NO_ERROR != return_code) {
        FS_AGING_A653_PRINT("fs_aging: error: aging process setup failed (%d)\r\n", (int)return_code);
        return FS_AGING_ERR_PLATFORM;
    }
    return FS_AGING_RUNNING;
}

fs_aging_status_t fs_aging_a653_start(const fs_aging_a653_config_t *config)
{
    fs_aging_status_t status;

    memset(&a653_ctx, 0, sizeof(a653_ctx));
    if ((NULL == config) || (NULL == config->profile_name) ||
        (NULL == config->mount_point) || (0u == config->ops_per_period)) {
        FS_AGING_A653_PRINT("fs_aging: error: invalid configuration\r\n");
        return FS_AGING_ERR_PARAM;
    }

    a653_profile = fs_aging_profile_find(config->profile_name);
    if (NULL == a653_profile) {
        FS_AGING_A653_PRINT("fs_aging: error: unknown profile %s\r\n", config->profile_name);
        return FS_AGING_ERR_PARAM;
    }

    /* The file system limits how deep directories can be nested */
    {
        uint32_t needed = path_components(config->mount_point) + profile_max_depth(a653_profile);
        if (needed > FS_AGING_A653_MAX_DIR_DEPTH) {
            FS_AGING_A653_PRINT("fs_aging: error: profile %s needs %u directory levels, "
                                "file system allows %u\r\n",
                                a653_profile->name, (unsigned int)needed,
                                (unsigned int)FS_AGING_A653_MAX_DIR_DEPTH);
            return FS_AGING_ERR_PATH_TOO_LONG;
        }
    }
    a653_cfg   = *config;
    a653_tasks = A653_DO_AGE | ((0u != config->clean_first) ? A653_DO_CLEAN : 0u);

    /* With a known volume size everything is checked now; otherwise the
     * aging process reads the free space first. */
    if (0u != config->volume_bytes) {
        status = prepare(config->volume_bytes);
        if (status != FS_AGING_RUNNING) {
            return status;
        }
    }
    return create_aging_process();
}

fs_aging_status_t fs_aging_a653_start_cleanup(const fs_aging_a653_config_t *config)
{
    memset(&a653_ctx, 0, sizeof(a653_ctx));
    if ((NULL == config) || (NULL == config->mount_point) ||
        ('\0' == config->mount_point[0]) || (0u == config->ops_per_period)) {
        FS_AGING_A653_PRINT("fs_aging: error: invalid configuration\r\n");
        return FS_AGING_ERR_PARAM;
    }
    a653_profile = NULL;
    a653_cfg     = *config;
    a653_tasks   = A653_DO_CLEAN;
    return create_aging_process();
}

void fs_aging_a653_get_stats(fs_aging_stats_t *out)
{
    fs_aging_get_stats(&a653_ctx, out);
}

static void print_dist(fs_aging_dist_t dist, const char *name)
{
    uint32_t i;
    uint32_t n = fs_aging_bin_count(&a653_ctx, dist);

    FS_AGING_A653_PRINT("fs_aging: %s distribution (target / actual, per mille)\r\n", name);
    for (i = 0u; i < n; i++) {
        fs_aging_float_t ideal  = 0.0;
        fs_aging_float_t actual = 0.0;
        (void)fs_aging_get_bin(&a653_ctx, dist, i, &ideal, &actual);
        FS_AGING_A653_PRINT("  %2u: %4u / %4u\r\n", (unsigned int)i,
                            per_mille(ideal), per_mille(actual));
    }
}

void fs_aging_a653_print_report(void)
{
    fs_aging_stats_t st;

    fs_aging_get_stats(&a653_ctx, &st);
    FS_AGING_A653_PRINT("fs_aging: result: %s\r\n", fs_aging_status_str(st.status));
    FS_AGING_A653_PRINT("fs_aging: ops %llu (target %llu) creates %llu deletes %llu\r\n",
                        (unsigned long long)st.ops,
                        (unsigned long long)st.convergence_ops,
                        (unsigned long long)st.creates,
                        (unsigned long long)st.deletes);
    FS_AGING_A653_PRINT("fs_aging: live %u files, %llu MB; written %llu MB; "
                        "age error %u per mille; %llu ms\r\n",
                        (unsigned int)st.live_files,
                        (unsigned long long)(st.live_bytes >> 20),
                        (unsigned long long)(st.workload_bytes >> 20),
                        per_mille(st.age_error),
                        (unsigned long long)st.elapsed_ms);
    print_dist(FS_AGING_DIST_AGE,  "age");
    print_dist(FS_AGING_DIST_SIZE, "size");
    print_dist(FS_AGING_DIST_DIR,  "dir");
}
