#pragma once
#ifndef DM_NT_H
#define DM_NT_H

#include "dm_util.h"

namespace dm {

typedef struct DmIoStatus {
    LONG Status;
    ULONG_PTR Information;
} DmIoStatus;

typedef struct DmUnicodeString {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} DmUnicodeString;

typedef struct DmObjectAttributes {
    ULONG Length;
    HANDLE RootDirectory;
    DmUnicodeString* ObjectName;
    ULONG Attributes;
    PVOID SecurityDescriptor;
    PVOID SecurityQualityOfService;
} DmObjectAttributes;

typedef LONG(NTAPI* NtCreateFile_t)(PHANDLE, ACCESS_MASK, DmObjectAttributes*, DmIoStatus*, PLARGE_INTEGER,
                                    ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
typedef LONG(NTAPI* NtSetInformationFile_t)(HANDLE, DmIoStatus*, PVOID, ULONG, ULONG);
typedef LONG(NTAPI* NtQueryInformationFile_t)(HANDLE, DmIoStatus*, PVOID, ULONG, ULONG);
typedef LONG(NTAPI* NtClose_t)(HANDLE);

#define DM_OBJ_CASE_INSENSITIVE 0x00000040UL
#define DM_FILE_OPEN 1UL
#define DM_FILE_OPEN_FOR_BACKUP_INTENT 0x00004000UL
#define DM_FILE_SYNCHRONOUS_IO_NONALERT 0x00000020UL
#define DM_FILE_DIRECTORY_FILE 0x00000001UL
#define DM_FILE_NON_DIRECTORY_FILE 0x00000040UL
#define DM_FILE_OPEN_REPARSE_POINT 0x00200000UL
#define DM_FILE_OPEN_REQUIRING_OPLOCK 0x00010000UL
#define DM_FILE_DELETE_ON_CLOSE 0x00001000UL

#define DM_FILE_DISPOSITION_INFO 13UL
#define DM_FILE_DISPOSITION_INFO_EX 64UL
#define DM_FILE_STANDARD_INFORMATION 5UL

#define DM_DISP_DELETE 0x00000001UL
#define DM_DISP_POSIX_SEMANTICS 0x00000002UL
#define DM_DISP_FORCE_IMAGE_SECTION_CHECK 0x00000004UL
#define DM_DISP_IGNORE_READONLY 0x00000010UL

#define DM_STATUS_SUCCESS_ 0x00000000L
#define DM_STATUS_OBJECT_NAME_NOT_FOUND 0xC0000034L
#define DM_STATUS_OBJECT_PATH_NOT_FOUND 0xC000003AL
#define DM_STATUS_ACCESS_DENIED 0xC0000022L
#define DM_STATUS_SHARING_VIOLATION 0xC0000043L
#define DM_STATUS_DELETE_PENDING 0xC0000056L
#define DM_STATUS_CANNOT_DELETE 0xC0000121L
#define DM_STATUS_DIRECTORY_NOT_EMPTY 0xC0000101L
#define DM_STATUS_INVALID_HANDLE 0xC0000008L

typedef struct DmFileDispositionEx {
    ULONG Flags;
} DmFileDispositionEx;

typedef struct DmFileDisposition {
    BOOLEAN DeleteFile;
} DmFileDisposition;

inline HMODULE NtDll() {
    static HMODULE mod = GetModuleHandleW(L"ntdll.dll");
    return mod;
}

inline NtCreateFile_t PfnNtCreateFile() {
    static NtCreateFile_t fn = reinterpret_cast<NtCreateFile_t>(GetProcAddress(NtDll(), "NtCreateFile"));
    return fn;
}

inline NtSetInformationFile_t PfnNtSetInformationFile() {
    static NtSetInformationFile_t fn = reinterpret_cast<NtSetInformationFile_t>(
        GetProcAddress(NtDll(), "NtSetInformationFile"));
    return fn;
}

inline std::wstring NtPath(const std::wstring& plain) {
    if (plain.size() >= 2 && plain[0] == L'\\' && plain[1] == L'\\') return L"\\??\\UNC\\" + plain.substr(2);
    return L"\\??\\" + plain;
}

inline DWORD NtToWin32(LONG st) {
    switch (static_cast<ULONG>(st)) {
        case 0x00000000UL: return ERROR_SUCCESS;
        case 0xC0000034UL: return ERROR_FILE_NOT_FOUND;
        case 0xC000003AL: return ERROR_PATH_NOT_FOUND;
        case 0xC0000022UL: return ERROR_ACCESS_DENIED;
        case 0xC0000043UL: return ERROR_SHARING_VIOLATION;
        case 0xC0000056UL: return ERROR_ACCESS_DENIED;
        case 0xC0000121UL: return ERROR_ACCESS_DENIED;
        case 0xC0000101UL: return ERROR_DIR_NOT_EMPTY;
        case 0xC0000008UL: return ERROR_INVALID_HANDLE;
        case 0xC00000BBUL: return ERROR_NOT_SUPPORTED;
        case 0xC00000BAUL: return ERROR_INVALID_PARAMETER;
        default: break;
    }
    if (st >= 0) return ERROR_SUCCESS;
    return ERROR_GEN_FAILURE;
}

inline HANDLE NtOpenDelete(const std::wstring& plain, bool dir, bool backupIntent, bool noFollow, LONG* status) {
    NtCreateFile_t create = PfnNtCreateFile();
    if (!create) {
        if (status) *status = DM_STATUS_CANNOT_DELETE;
        return INVALID_HANDLE_VALUE;
    }
    std::wstring nt = NtPath(plain);
    DmUnicodeString us{};
    us.Buffer = const_cast<PWSTR>(nt.c_str());
    us.Length = static_cast<USHORT>(nt.size() * sizeof(wchar_t));
    us.MaximumLength = static_cast<USHORT>(us.Length + sizeof(wchar_t));
    DmObjectAttributes oa{};
    oa.Length = sizeof(DmObjectAttributes);
    oa.ObjectName = &us;
    oa.Attributes = DM_OBJ_CASE_INSENSITIVE;
    DmIoStatus ios{};
    ULONG opts = DM_FILE_SYNCHRONOUS_IO_NONALERT;
    opts |= dir ? DM_FILE_DIRECTORY_FILE : DM_FILE_NON_DIRECTORY_FILE;
    if (backupIntent) opts |= DM_FILE_OPEN_FOR_BACKUP_INTENT;
    if (noFollow) opts |= DM_FILE_OPEN_REPARSE_POINT;
    HANDLE h = INVALID_HANDLE_VALUE;
    LONG st = create(&h, DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES, &oa, &ios, nullptr,
                     0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE | FILE_SHARE_DELETE,
                     DM_FILE_OPEN, opts, nullptr, 0);
    if (status) *status = st;
    if (st < 0) return INVALID_HANDLE_VALUE;
    return h;
}

inline DWORD NtDeleteByHandle(HANDLE h) {
    NtSetInformationFile_t set = PfnNtSetInformationFile();
    if (!set) return ERROR_NOT_SUPPORTED;
    DmIoStatus ios{};
    DmFileDispositionEx ex{};
    ex.Flags = DM_DISP_DELETE | DM_DISP_POSIX_SEMANTICS | DM_DISP_FORCE_IMAGE_SECTION_CHECK | DM_DISP_IGNORE_READONLY;
    LONG st = set(h, &ios, &ex, sizeof(ex), DM_FILE_DISPOSITION_INFO_EX);
    if (st >= 0) return ERROR_SUCCESS;
    DmFileDisposition legacy{};
    legacy.DeleteFile = TRUE;
    ios = DmIoStatus{};
    st = set(h, &ios, &legacy, sizeof(legacy), DM_FILE_DISPOSITION_INFO);
    if (st >= 0) return ERROR_SUCCESS;
    return NtToWin32(st);
}

inline DWORD NtDeletePath(const std::wstring& plain, bool dir, bool backupIntent, bool noFollow) {
    LONG st = 0;
    HANDLE h = NtOpenDelete(plain, dir, backupIntent, noFollow, &st);
    if (h == INVALID_HANDLE_VALUE) return NtToWin32(st);
    DWORD err = NtDeleteByHandle(h);
    CloseHandle(h);
    return err;
}

inline bool NtQueryTimes(const std::wstring& plain, unsigned long long* size, unsigned long long* attrOut) {
    HANDLE h = CreateFileW(dm::Extended(plain).c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION bi{};
    bool ok = GetFileInformationByHandle(h, &bi) != FALSE;
    if (ok) {
        if (size) *size = (static_cast<unsigned long long>(bi.nFileSizeHigh) << 32) | bi.nFileSizeLow;
        if (attrOut) *attrOut = bi.dwFileAttributes;
    }
    CloseHandle(h);
    return ok;
}

}
#endif
