#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <shlobj.h>

#include "vps_temp_file.h"

#include <stdio.h>
#include <string.h>

static uint64_t vps_temp_hash(const char *value, size_t length)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t index;
    for (index = 0U; index < length; ++index) {
        hash ^= (unsigned char)value[index];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int vps_temp_directory_ensure(const wchar_t *path)
{
    if (CreateDirectoryW(path, NULL)) return 1;
    return GetLastError() == ERROR_ALREADY_EXISTS &&
           (GetFileAttributesW(path) & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

VpsTempFileStatus vps_temp_file_create_private(
    const VpsAllocator *allocator,
    VpsLogger *logger,
    VpsTempFilePath *path)
{
    PWSTR local = NULL;
    wchar_t *path_storage = NULL;
    wchar_t *root = NULL;
    wchar_t *directory = NULL;
    wchar_t *file = NULL;
    PSECURITY_DESCRIPTOR descriptor = NULL;
    SECURITY_ATTRIBUTES attributes;
    HANDLE handle = INVALID_HANDLE_VALUE;
    GUID id;
    int utf8_size;
    size_t allocation_size = 0U;
    char *utf8_path = NULL;
    VpsTempFileStatus result = VPS_TEMP_FILE_SYSTEM_ERROR;
    (void)logger;
    if (allocator == NULL || !vps_allocator_is_valid(allocator) ||
        path == NULL || path->path != NULL)
        return VPS_TEMP_FILE_INVALID_ARGUMENT;
    if (FAILED(SHGetKnownFolderPath(&FOLDERID_LocalAppData,
                                    KF_FLAG_CREATE, NULL, &local)))
        return VPS_TEMP_FILE_SYSTEM_ERROR;
    path_storage = (wchar_t *)HeapAlloc(
        GetProcessHeap(), HEAP_ZERO_MEMORY,
        3U * VPS_TEMP_FILE_PATH_LIMIT * sizeof(*path_storage));
    if (path_storage == NULL) {
        CoTaskMemFree(local);
        return VPS_TEMP_FILE_OUT_OF_MEMORY;
    }
    root = path_storage;
    directory = root + VPS_TEMP_FILE_PATH_LIMIT;
    file = directory + VPS_TEMP_FILE_PATH_LIMIT;
    if (swprintf(root, VPS_TEMP_FILE_PATH_LIMIT, L"%ls\\VirtualPostgreSQL",
                 local) < 0 ||
        swprintf(directory, VPS_TEMP_FILE_PATH_LIMIT, L"%ls\\Temp", root) < 0 ||
        !vps_temp_directory_ensure(root) ||
        !vps_temp_directory_ensure(directory) || FAILED(CoCreateGuid(&id)) ||
        swprintf(file, VPS_TEMP_FILE_PATH_LIMIT,
                 L"%ls\\vps-%08lx-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x.db",
                 directory, (unsigned long)id.Data1, id.Data2, id.Data3,
                 id.Data4[0], id.Data4[1], id.Data4[2], id.Data4[3],
                 id.Data4[4], id.Data4[5], id.Data4[6], id.Data4[7]) < 0) {
        goto cleanup;
    }
    CoTaskMemFree(local);
    local = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;FA;;;OW)", SDDL_REVISION_1, &descriptor, NULL))
        goto cleanup;
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    handle = CreateFileW(file, GENERIC_READ | GENERIC_WRITE, 0, &attributes,
                         CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY |
                                         FILE_ATTRIBUTE_NOT_CONTENT_INDEXED,
                         NULL);
    LocalFree(descriptor);
    descriptor = NULL;
    if (handle == INVALID_HANDLE_VALUE) goto cleanup;
    (void)CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
    utf8_size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, file, -1,
                                    NULL, 0, NULL, NULL);
    if (utf8_size <= 0 ||
        vps_size_from_int64(utf8_size, &allocation_size) != VPS_MEMORY_OK ||
        vps_memory_allocate(allocator, allocation_size,
                            (void **)&utf8_path) != VPS_MEMORY_OK ||
        utf8_path == NULL) {
        (void)DeleteFileW(file);
        result = VPS_TEMP_FILE_OUT_OF_MEMORY;
        goto cleanup;
    }
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, file, -1,
                            utf8_path, utf8_size, NULL, NULL) != utf8_size) {
        (void)DeleteFileW(file);
        vps_memory_release(allocator, (void **)&utf8_path, allocation_size);
        goto cleanup;
    }
    path->path = utf8_path;
    utf8_path = NULL;
    path->allocator = *allocator;
    path->path_size = allocation_size;
    path->fingerprint = vps_temp_hash(path->path, allocation_size - 1U);
    result = VPS_TEMP_FILE_OK;

cleanup:
    if (handle != INVALID_HANDLE_VALUE) (void)CloseHandle(handle);
    if (descriptor != NULL) LocalFree(descriptor);
    if (local != NULL) CoTaskMemFree(local);
    (void)HeapFree(GetProcessHeap(), 0, path_storage);
    if (utf8_path != NULL)
        vps_memory_release(allocator, (void **)&utf8_path, allocation_size);
    return result;
}

VpsTempFileStatus vps_temp_file_delete(VpsTempFilePath *path)
{
    wchar_t *wide = NULL;
    int wide_size;
    int deleted = 1;
    if (path == NULL) return VPS_TEMP_FILE_INVALID_ARGUMENT;
    if (path->path == NULL) return VPS_TEMP_FILE_OK;
    wide_size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path->path,
                                    -1, NULL, 0);
    if (wide_size <= 0 ||
        (size_t)wide_size > SIZE_MAX / sizeof(*wide))
        deleted = 0;
    else {
        wide = (wchar_t *)HeapAlloc(GetProcessHeap(), 0,
                                    (size_t)wide_size * sizeof(*wide));
        if (wide == NULL ||
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path->path,
                                -1, wide, wide_size) != wide_size)
            deleted = 0;
        else if (!DeleteFileW(wide) && GetLastError() != ERROR_FILE_NOT_FOUND)
            deleted = 0;
    }
    if (wide != NULL) HeapFree(GetProcessHeap(), 0, wide);
    vps_memory_release(&path->allocator, (void **)&path->path,
                       path->path_size);
    (void)memset(path, 0, sizeof(*path));
    return deleted ? VPS_TEMP_FILE_OK : VPS_TEMP_FILE_SYSTEM_ERROR;
}
