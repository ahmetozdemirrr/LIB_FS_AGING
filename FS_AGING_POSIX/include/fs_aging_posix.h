/*
 * fs_aging_posix.h - POSIX port of the fs_aging library (to be written).
 *
 * This file is a placeholder for the POSIX team. Suggested interface,
 * mirroring the ARINC 653 port (FS_AGING_ARINC653/include/fs_aging_arinc653.h):
 *
 *   - a config struct: profile name, mount point, volume size, fill
 *     ratio, seed, limits, and how many operations to run per call;
 *   - fs_aging_posix_start(config): checks the setup and calls
 *     fs_aging_init() with a pool sized by fs_aging_estimate_files();
 *   - a loop (or thread) that calls fs_aging_step() until it returns a
 *     status other than FS_AGING_RUNNING, then prints the report;
 *   - fs_aging_posix_get_platform(): fills fs_aging_platform_t.
 *
 * The platform callback contract is in README.md, section "Kendi port'unuzu yazmak"
 * and in FS_AGING_CORE/include/fs_aging.h.
 */
