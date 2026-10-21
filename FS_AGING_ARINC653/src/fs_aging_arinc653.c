/*
 * fs_aging_arinc653.c - ARINC 653 platform callbacks for fs_aging.
 *
 * Maps the fs_aging platform interface to the ARINC 653 file system
 * services.
 *
 * Original: https://github.com/saurabhkadekodi/geriatrix (BSD-2-Clause)
 */

#include <string.h>
#include "gzisTypes.h"
#include "gzisuTime.h"
#include "arinc653/ARINC653.h"
#include "fs/fileSystem.h"
#include "testUtils/lib/user/testUtils.h"
#include "fs_aging_arinc653.h"

/* The library's float type must match the platform's 8 byte float. */
typedef char fs_aging_a653_float8_check[
    (sizeof(float8_gt) == sizeof(fs_aging_float_t)) ? 1 : -1];

/* Every path the library builds must fit in a file name. */
typedef char fs_aging_a653_path_check[
    (FS_AGING_PATH_MAX <= MAX_NAME_LENGTH) ? 1 : -1];

#if !FS_AGING_A653_USE_RESIZE
static char a653_write_buf[MAXIMUM_ATOMICITY];
#endif

static fs_aging_plat_result_t no_space_or_error(FILE_ERRNO_TYPE file_errno)
{
    return (ENOSPC == file_errno) ? FS_AGING_PLAT_NO_SPACE : FS_AGING_PLAT_ERROR;
}

/* Creates a file of size_bytes. On failure the partial file is removed,
 * because the library treats a failed create as "no file". */
static fs_aging_plat_result_t a653_create_file(void *user, const char *path,
                                               uint64_t size_bytes)
{
    FILE_ID_TYPE     file_id     = 0;
    RETURN_CODE_TYPE return_code = NO_ERROR;
    RETURN_CODE_TYPE close_code  = NO_ERROR;
    FILE_ERRNO_TYPE  file_errno  = 0;
    FILE_ERRNO_TYPE  close_errno = 0;
    (void)user;

    if (size_bytes > (uint64_t)FS_AGING_A653_MAX_FILE_SIZE) {
        return FS_AGING_PLAT_NO_SPACE;
    }

    OPEN_NEW_FILE((char *)path, &file_id, &return_code, &file_errno);
    if (NO_ERROR != return_code) {
        return no_space_or_error(file_errno);
    }

#if FS_AGING_A653_USE_RESIZE
    RESIZE_FILE(file_id, (FILE_SIZE_TYPE)size_bytes, &return_code, &file_errno);
#else
    {
        uint64_t left = size_bytes;
        while ((NO_ERROR == return_code) && (left > 0u)) {
            uint64_t n = (left > (uint64_t)MAXIMUM_ATOMICITY)
                         ? (uint64_t)MAXIMUM_ATOMICITY : left;
            WRITE_FILE(file_id, (MESSAGE_ADDR_TYPE)a653_write_buf,
                       (MESSAGE_SIZE_TYPE)n, &return_code, &file_errno);
            left -= n;
        }
    }
#endif

    CLOSE_FILE(file_id, &close_code, &close_errno);

    if (NO_ERROR != return_code) {
        REMOVE_FILE((char *)path, &close_code, &close_errno);
        return no_space_or_error(file_errno);
    }
    return (NO_ERROR == close_code) ? FS_AGING_PLAT_OK : FS_AGING_PLAT_ERROR;
}

static fs_aging_plat_result_t a653_delete_file(void *user, const char *path)
{
    RETURN_CODE_TYPE return_code = NO_ERROR;
    FILE_ERRNO_TYPE  file_errno  = 0;
    (void)user;

    REMOVE_FILE((char *)path, &return_code, &file_errno);
    return (NO_ERROR == return_code) ? FS_AGING_PLAT_OK : FS_AGING_PLAT_ERROR;
}

/* An existing directory counts as success. */
static fs_aging_plat_result_t a653_make_dir(void *user, const char *path)
{
    RETURN_CODE_TYPE return_code = NO_ERROR;
    FILE_ERRNO_TYPE  file_errno  = 0;
    (void)user;

    MAKE_DIRECTORY((char *)path, &return_code, &file_errno);
    if ((NO_ERROR == return_code) || (EEXIST == file_errno)) {
        return FS_AGING_PLAT_OK;
    }
    return FS_AGING_PLAT_ERROR;
}

/* gzisu_time_get() returns nanoseconds. */
static uint64_t a653_now_ms(void *user)
{
    (void)user;
    return (uint64_t)gzisu_time_get() / 1000000u;
}

static void a653_log(void *user, const char *msg)
{
    (void)user;
    FS_AGING_A653_PRINT("%s\r\n", msg);
}

void fs_aging_a653_get_platform(fs_aging_platform_t *out)
{
    if (NULL == out) {
        return;
    }
    out->create_file = a653_create_file;
    out->delete_file = a653_delete_file;
    out->make_dir    = a653_make_dir;
    out->now_ms      = a653_now_ms;
    out->log         = a653_log;
    out->user        = NULL;
}

/* GET_VOLUME_STATUS needs a file on the volume, so a temporary empty
 * file is created and removed again. */
uint64_t fs_aging_a653_free_bytes(const char *mount_point)
{
    static const char  probe[] = "/fs_aging_probe";
    FILE_NAME_TYPE     name;
    VOLUME_STATUS_TYPE volume_status;
    FILE_ID_TYPE       file_id     = 0;
    RETURN_CODE_TYPE   return_code = NO_ERROR;
    RETURN_CODE_TYPE   ignore_code = NO_ERROR;
    FILE_ERRNO_TYPE    file_errno  = 0;
    FILE_ERRNO_TYPE    ignore_errno = 0;
    uint64_t           free_bytes  = 0u;
    size_t             len;

    if (NULL == mount_point) {
        return 0u;
    }
    len = strlen(mount_point);
    if ((len + sizeof(probe)) > sizeof(name)) {
        return 0u;
    }
    memcpy(name, mount_point, len);
    memcpy(&name[len], probe, sizeof(probe));

    OPEN_NEW_FILE(name, &file_id, &return_code, &file_errno);
    if (NO_ERROR != return_code) {
        return 0u;
    }
    GET_VOLUME_STATUS(name, &volume_status, &return_code, &file_errno);
    if ((NO_ERROR == return_code) && (volume_status.FREE_BYTES > 0)) {
        free_bytes = (uint64_t)volume_status.FREE_BYTES;
    }
    CLOSE_FILE(file_id, &ignore_code, &ignore_errno);
    REMOVE_FILE(name, &ignore_code, &ignore_errno);
    return free_bytes;
}
