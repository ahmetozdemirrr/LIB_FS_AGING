/*
 * fs_aging_posix.c - POSIX platform callbacks for fs_aging (to be written).
 *
 * This file is a placeholder for the POSIX team. What the callbacks must
 * do (full contract in README.md, POSIX port section):
 *
 *   create_file(path, size)
 *       open(path, O_CREAT | O_EXCL | O_WRONLY) and make the file exactly
 *       size bytes: posix_fallocate() if the file system supports it,
 *       otherwise write the data. Return FS_AGING_PLAT_NO_SPACE on
 *       ENOSPC / EFBIG and remove the partial file; any other failure is
 *       FS_AGING_PLAT_ERROR.
 *
 *   delete_file(path)
 *       unlink(path). Any failure is FS_AGING_PLAT_ERROR.
 *
 *   make_dir(path)
 *       mkdir(path, 0755). An existing directory (EEXIST) must return
 *       FS_AGING_PLAT_OK. Parent directories are created by the library.
 *
 *   now_ms() (optional)
 *       clock_gettime(CLOCK_MONOTONIC) in milliseconds.
 *
 *   log(msg) (optional)
 *       print the message.
 *
 * Start aging on an empty volume and pass the real usable space as
 * volume_bytes (e.g. statvfs: f_bavail * f_frsize).
 *
 * See FS_AGING_ARINC653/src/ for a complete port to follow.
 */
