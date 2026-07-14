/* mzng_strm_win32.c -- Stream for filesystem access for windows
   part of the minizip-ng project

   Copyright (C) Nathan Moinvaziri
     https://github.com/zlib-ng/minizip-ng
   Copyright (C) 2009-2010 Mathias Svensson
     Modifications for Zip64 support
     http://result42.com
   Copyright (C) 1998-2010 Gilles Vollant
     https://www.winimage.com/zLibDll/minizip.html

   This program is distributed under the terms of the same license as zlib.
   See the accompanying LICENSE file for the full text of the license.
*/

#include "mz.h"
#include "mz_os.h"
#include "mz_strm.h"
#include "mz_strm_os.h"

#include <windows.h>

/***************************************************************************/

#ifndef INVALID_HANDLE_VALUE
#  define INVALID_HANDLE_VALUE 0xFFFFFFFF
#endif

#ifndef INVALID_SET_FILE_POINTER
#  define INVALID_SET_FILE_POINTER (DWORD)(-1)
#endif

#ifndef _WIN32_WINNT_WIN8
#  define _WIN32_WINNT_WIN8 0x0602
#endif

/***************************************************************************/

static mzng_stream_vtbl mzng_stream_os_vtbl = {mzng_stream_os_open,
                                           mzng_stream_os_is_open,
                                           mzng_stream_os_read,
                                           mzng_stream_os_write,
                                           mzng_stream_os_tell,
                                           mzng_stream_os_seek,
                                           mzng_stream_os_close,
                                           mzng_stream_os_error,
                                           mzng_stream_os_create,
                                           mzng_stream_os_delete,
                                           NULL,
                                           NULL};

/***************************************************************************/

typedef struct mzng_stream_win32_s {
    mzng_stream stream;
    HANDLE handle;
    int32_t error;
} mzng_stream_win32;

/***************************************************************************/

#if 0
#  define mzng_stream_os_print printf
#else
#  define mzng_stream_os_print(fmt, ...)
#endif

/***************************************************************************/

int32_t mzng_stream_os_open(void *stream, const char *path, int32_t mode) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;
    uint32_t desired_access = 0;
    uint32_t creation_disposition = 0;
    uint32_t share_mode = FILE_SHARE_READ;
    uint32_t flags_attribs = FILE_ATTRIBUTE_NORMAL;
    wchar_t *path_wide = NULL;

    if (!path)
        return MZ_PARAM_ERROR;

    /* Some use cases require write sharing as well */
    share_mode |= FILE_SHARE_WRITE;

    if ((mode & MZ_OPEN_MODE_READWRITE) == MZ_OPEN_MODE_READ) {
        desired_access = GENERIC_READ;
        creation_disposition = OPEN_EXISTING;
    } else if (mode & MZ_OPEN_MODE_APPEND) {
        desired_access = GENERIC_WRITE | GENERIC_READ;
        creation_disposition = OPEN_EXISTING;
    } else if (mode & MZ_OPEN_MODE_CREATE) {
        desired_access = GENERIC_WRITE | GENERIC_READ;
        creation_disposition = CREATE_ALWAYS;
    } else {
        return MZ_PARAM_ERROR;
    }

    mzng_stream_os_print("Win32 - Open - %s (mode %" PRId32 ")\n", path);

    path_wide = mzng_os_unicode_string_create(path, MZ_ENCODING_UTF8);
    if (!path_wide)
        return MZ_PARAM_ERROR;

#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
    win32->handle = CreateFile2(path_wide, desired_access, share_mode, creation_disposition, NULL);
#else
    win32->handle = CreateFileW(path_wide, desired_access, share_mode, NULL, creation_disposition, flags_attribs, NULL);
#endif

    mzng_os_unicode_string_delete(&path_wide);

    if (mzng_stream_os_is_open(stream) != MZ_OK) {
        win32->error = GetLastError();
        return MZ_OPEN_ERROR;
    }

    if (mode & MZ_OPEN_MODE_APPEND)
        return mzng_stream_os_seek(stream, 0, MZ_SEEK_END);

    return MZ_OK;
}

int32_t mzng_stream_os_is_open(void *stream) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;
    if (!win32->handle || win32->handle == INVALID_HANDLE_VALUE)
        return MZ_OPEN_ERROR;
    return MZ_OK;
}

int32_t mzng_stream_os_read(void *stream, void *buf, int32_t size) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;
    uint32_t read = 0;

    if (mzng_stream_os_is_open(stream) != MZ_OK)
        return MZ_OPEN_ERROR;

    if (!ReadFile(win32->handle, buf, size, (DWORD *)&read, NULL)) {
        win32->error = GetLastError();
        if (win32->error == ERROR_HANDLE_EOF)
            win32->error = 0;
    }

    mzng_stream_os_print("Win32 - Read - %" PRId32 "\n", read);

    return read;
}

int32_t mzng_stream_os_write(void *stream, const void *buf, int32_t size) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;
    int32_t written = 0;

    if (mzng_stream_os_is_open(stream) != MZ_OK)
        return MZ_OPEN_ERROR;

    if (!WriteFile(win32->handle, buf, size, (DWORD *)&written, NULL)) {
        win32->error = GetLastError();
        if (win32->error == ERROR_HANDLE_EOF)
            win32->error = 0;
    }

    mzng_stream_os_print("Win32 - Write - %" PRId32 "\n", written);

    return written;
}

static int32_t mzng_stream_os_seekinternal(HANDLE handle, LARGE_INTEGER large_pos, LARGE_INTEGER *new_pos,
                                         uint32_t move_method) {
#if _WIN32_WINNT >= _WIN32_WINNT_WINXP
    BOOL success = FALSE;
    success = SetFilePointerEx(handle, large_pos, new_pos, move_method);
    if ((success == FALSE) && (GetLastError() != NO_ERROR))
        return MZ_SEEK_ERROR;

    return MZ_OK;
#else
    LONG high_part = 0;
    uint32_t pos = 0;

    high_part = large_pos.HighPart;
    pos = SetFilePointer(handle, large_pos.LowPart, &high_part, move_method);

    if ((pos == INVALID_SET_FILE_POINTER) && (GetLastError() != NO_ERROR))
        return MZ_SEEK_ERROR;

    if (new_pos) {
        new_pos->LowPart = pos;
        new_pos->HighPart = high_part;
    }

    return MZ_OK;
#endif
}

int64_t mzng_stream_os_tell(void *stream) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;
    LARGE_INTEGER large_pos;

    if (mzng_stream_os_is_open(stream) != MZ_OK)
        return MZ_OPEN_ERROR;

    large_pos.QuadPart = 0;

    if (mzng_stream_os_seekinternal(win32->handle, large_pos, &large_pos, FILE_CURRENT) != MZ_OK)
        win32->error = GetLastError();

    mzng_stream_os_print("Win32 - Tell - %" PRId64 "\n", large_pos.QuadPart);

    return large_pos.QuadPart;
}

int32_t mzng_stream_os_seek(void *stream, int64_t offset, int32_t origin) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;
    uint32_t move_method = 0xFFFFFFFF;
    int32_t err = MZ_OK;
    LARGE_INTEGER large_pos;

    if (mzng_stream_os_is_open(stream) != MZ_OK)
        return MZ_OPEN_ERROR;

    switch (origin) {
    case MZ_SEEK_CUR:
        move_method = FILE_CURRENT;
        break;
    case MZ_SEEK_END:
        move_method = FILE_END;
        break;
    case MZ_SEEK_SET:
        move_method = FILE_BEGIN;
        break;
    default:
        return MZ_SEEK_ERROR;
    }

    mzng_stream_os_print("Win32 - Seek - %" PRId64 " (origin %" PRId32 ")\n", offset, origin);

    large_pos.QuadPart = offset;

    err = mzng_stream_os_seekinternal(win32->handle, large_pos, NULL, move_method);
    if (err != MZ_OK) {
        win32->error = GetLastError();
        return err;
    }

    return MZ_OK;
}

int32_t mzng_stream_os_close(void *stream) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;

    if (win32->handle)
        CloseHandle(win32->handle);
    mzng_stream_os_print("Win32 - Close\n");
    win32->handle = NULL;
    return MZ_OK;
}

int32_t mzng_stream_os_error(void *stream) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)stream;
    return win32->error;
}

void *mzng_stream_os_create(void) {
    mzng_stream_win32 *win32 = (mzng_stream_win32 *)calloc(1, sizeof(mzng_stream_win32));
    if (win32)
        win32->stream.vtbl = &mzng_stream_os_vtbl;
    return win32;
}

void mzng_stream_os_delete(void **stream) {
    mzng_stream_win32 *win32 = NULL;
    if (!stream)
        return;
    win32 = (mzng_stream_win32 *)*stream;
    if (win32)
        free(win32);
    *stream = NULL;
}

void *mzng_stream_os_get_interface(void) {
    return (void *)&mzng_stream_os_vtbl;
}
