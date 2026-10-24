#pragma once
#ifndef DM_UTIL_H
#define DM_UTIL_H

#include "dm_api.h"
#include <winioctl.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <cwctype>

namespace dm {

struct Counters {
    DWORD files;
    DWORD dirs;
    unsigned __int64 bytes;
    Counters() : files(0), dirs(0), bytes(0) {}
};

inline std::wstring ErrText(DWORD code) {
    if (code == 0) return L"нет ошибки";
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, code, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring out;
    if (n && buf) {
        out.assign(buf, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ' || out.back() == L'\t')) out.pop_back();
    }
    if (buf) LocalFree(buf);
    wchar_t num[40];
    swprintf_s(num, L"%lu", static_cast<unsigned long>(code));
    if (out.empty()) out = L"неизвестная ошибка";
    out += L" (код ";
    out += num;
    out += L")";
    return out;
}

inline bool IsExtended(const std::wstring& p) {
    return p.size() >= 4 && p[0] == L'\\' && p[1] == L'\\' && p[2] == L'?' && p[3] == L'\\';
}

inline std::wstring StripExtended(const std::wstring& p) {
    if (p.size() >= 8 && _wcsnicmp(p.c_str(), L"\\\\?\\UNC\\", 8) == 0) return L"\\\\" + p.substr(8);
    if (IsExtended(p)) return p.substr(4);
    return p;
}

inline std::wstring TrimQuotesAndSpace(const std::wstring& in) {
    size_t b = 0, e = in.size();
    while (b < e && (in[b] == L' ' || in[b] == L'\t' || in[b] == L'"')) ++b;
    while (e > b && (in[e - 1] == L' ' || in[e - 1] == L'\t' || in[e - 1] == L'"' || in[e - 1] == L'\r' || in[e - 1] == L'\n')) --e;
    return in.substr(b, e - b);
}

inline std::wstring FullPath(const std::wstring& in) {
    std::wstring plain = StripExtended(TrimQuotesAndSpace(in));
    if (plain.empty()) return std::wstring();
    std::vector<wchar_t> buf(DM_MAX_PATH);
    DWORD n = GetFullPathNameW(plain.c_str(), static_cast<DWORD>(buf.size()), buf.data(), nullptr);
    std::wstring abs;
    if (n > 0 && n < buf.size()) abs.assign(buf.data(), n);
    else abs = plain;
    while (abs.size() > 3 && (abs.back() == L'\\' || abs.back() == L'/')) abs.pop_back();
    if (abs.size() == 2 && abs[1] == L':') abs += L'\\';
    return abs;
}

inline std::wstring Extended(const std::wstring& in) {
    std::wstring abs = FullPath(in);
    if (abs.empty()) return abs;
    if (IsExtended(abs)) return abs;
    if (abs.size() >= 2 && abs[0] == L'\\' && abs[1] == L'\\') return L"\\\\?\\UNC\\" + abs.substr(2);
    return L"\\\\?\\" + abs;
}

inline bool Exists(const std::wstring& ext) {
    return GetFileAttributesW(ext.c_str()) != INVALID_FILE_ATTRIBUTES;
}

inline DWORD AttrsOf(const std::wstring& ext) {
    return GetFileAttributesW(ext.c_str());
}

inline bool IsDirectory(const std::wstring& ext) {
    DWORD a = GetFileAttributesW(ext.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool IsReparsePoint(const std::wstring& ext) {
    DWORD a = GetFileAttributesW(ext.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

inline bool IsCloudPlaceholder(const std::wstring& ext) {
    DWORD a = GetFileAttributesW(ext.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return false;
    if (a & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_RECALL_ON_OPEN)) return true;
    if (a & FILE_ATTRIBUTE_OFFLINE) return true;
    return false;
}

inline DWORD ClearAttributes(const std::wstring& ext) {
    DWORD a = GetFileAttributesW(ext.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) return GetLastError();
    DWORD keep = FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DEVICE;
    DWORD want = a & keep;
    if (a != want) {
        if (!SetFileAttributesW(ext.c_str(), want)) return GetLastError();
    }
    return 0;
}

inline DWORD FileSizeOf(const std::wstring& ext) {
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (!GetFileAttributesExW(ext.c_str(), GetFileExInfoStandard, &fad)) return 0;
    return (static_cast<unsigned __int64>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

inline bool EnablePrivilege(const wchar_t* name) {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return false;
    LUID luid{};
    BOOL ok = FALSE;
    if (LookupPrivilegeValueW(nullptr, name, &luid)) {
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        SetLastError(ERROR_SUCCESS);
        ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        if (ok && GetLastError() == ERROR_NOT_ALL_ASSIGNED) ok = FALSE;
    }
    CloseHandle(tok);
    return ok != FALSE;
}

inline void EnableStandardPrivileges() {
    static const wchar_t* list[] = {
        L"SeDebugPrivilege",
        L"SeTakeOwnershipPrivilege",
        L"SeRestorePrivilege",
        L"SeBackupPrivilege",
        L"SeSecurityPrivilege",
        L"SeManageVolumePrivilege",
        L"SeIncreaseQuotaPrivilege"
    };
    for (const wchar_t* p : list) EnablePrivilege(p);
}

inline bool IsElevated() {
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    TOKEN_ELEVATION te{};
    DWORD cb = sizeof(te);
    BOOL ok = GetTokenInformation(tok, TokenElevation, &te, sizeof(te), &cb);
    CloseHandle(tok);
    return ok && te.TokenIsElevated != 0;
}

inline std::wstring ModuleDir() {
    std::vector<wchar_t> buf(DM_MAX_PATH);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    std::wstring p(buf.data(), n);
    size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return p.substr(0, slash);
}

inline DWORD DeleteFileRetry(const std::wstring& ext, DWORD retries, DWORD delayMs, Counters* c) {
    DWORD lastErr = ERROR_SUCCESS;
    unsigned __int64 sz = FileSizeOf(ext);
    for (DWORD i = 0; i <= retries; ++i) {
        if (DeleteFileW(ext.c_str())) {
            if (c) {
                c->files++;
                c->bytes += sz;
            }
            return ERROR_SUCCESS;
        }
        lastErr = GetLastError();
        if (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
        ClearAttributes(ext);
        if (i < retries) Sleep(delayMs * (1u << (i > 5 ? 5 : i)));
    }
    return lastErr;
}

inline DWORD RemoveDirRetry(const std::wstring& ext, DWORD retries, DWORD delayMs, Counters* c) {
    DWORD lastErr = ERROR_SUCCESS;
    for (DWORD i = 0; i <= retries; ++i) {
        if (RemoveDirectoryW(ext.c_str())) {
            if (c) c->dirs++;
            return ERROR_SUCCESS;
        }
        lastErr = GetLastError();
        if (lastErr == ERROR_FILE_NOT_FOUND || lastErr == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
        ClearAttributes(ext);
        if (i < retries) Sleep(delayMs * (1u << (i > 5 ? 5 : i)));
    }
    return lastErr;
}

inline DWORD DeleteTree(const std::wstring& ext, DWORD retries, DWORD delayMs, Counters* c, int depth) {
    if (depth > 1024) return ERROR_CANT_RESOLVE_FILENAME;
    DWORD a = GetFileAttributesW(ext.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
        return e;
    }
    ClearAttributes(ext);
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::wstring pattern = ext + L"\\*";
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                DWORD childErr = ERROR_SUCCESS;
                do {
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                    childErr = DeleteTree(ext + L"\\" + fd.cFileName, retries, delayMs, c, depth + 1);
                    if (childErr == ERROR_ACCESS_DENIED) continue;
                } while (FindNextFileW(h, &fd) != FALSE);
                FindClose(h);
            }
        }
        return RemoveDirRetry(ext, retries, delayMs, c);
    }
    return DeleteFileRetry(ext, retries, delayMs, c);
}

inline void FillResult(DmResult* r, int status, DWORD err, const wchar_t* engineId, const std::wstring& detail) {
    if (!r) return;
    if (r->structSize != sizeof(DmResult)) ZeroMemory(r, sizeof(DmResult));
    else ZeroMemory(reinterpret_cast<BYTE*>(r) + sizeof(DWORD), sizeof(DmResult) - sizeof(DWORD));
    r->structSize = sizeof(DmResult);
    r->status = status;
    r->win32Error = err;
    if (engineId) wcsncpy_s(r->engineId, DM_ID_CCH, engineId, _TRUNCATE);
    wcsncpy_s(r->detail, DM_DETAIL_CCH, detail.c_str(), _TRUNCATE);
}

inline void FillInfo(DmEngineInfo* info, int stage, DWORD caps, const wchar_t* id, const wchar_t* title, const wchar_t* summary) {
    ZeroMemory(info, sizeof(DmEngineInfo));
    info->structSize = sizeof(DmEngineInfo);
    info->apiVersion = DM_API_VERSION;
    info->stage = stage;
    info->capabilities = caps;
    wcsncpy_s(info->id, DM_ID_CCH, id, _TRUNCATE);
    wcsncpy_s(info->title, DM_TITLE_CCH, title, _TRUNCATE);
    wcsncpy_s(info->summary, DM_SUMMARY_CCH, summary, _TRUNCATE);
}

inline const wchar_t* DescribeReparse(const std::wstring& ext) {
    HANDLE h = CreateFileW(ext.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return L"нет доступа";
    std::vector<BYTE> buf(16 * 1024, 0);
    DWORD ret = 0;
    const wchar_t* kind = L"неизвестная точка монтирования";
    if (DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(),
                        static_cast<DWORD>(buf.size()), &ret, nullptr) && ret >= sizeof(ULONG)) {
        ULONG tag = *reinterpret_cast<const ULONG*>(buf.data());
        switch (tag) {
            case IO_REPARSE_TAG_MOUNT_POINT: kind = L"junction"; break;
            case IO_REPARSE_TAG_SYMLINK: kind = L"символическая ссылка"; break;
            case IO_REPARSE_TAG_APPEXECLINK: kind = L"ссылка AppExec"; break;
            case IO_REPARSE_TAG_WCI: kind = L"контейнер WCI"; break;
            case IO_REPARSE_TAG_CLOUD:
            case IO_REPARSE_TAG_CLOUD_1:
            case IO_REPARSE_TAG_CLOUD_2:
            case IO_REPARSE_TAG_CLOUD_3:
            case IO_REPARSE_TAG_CLOUD_4:
            case IO_REPARSE_TAG_CLOUD_5:
            case IO_REPARSE_TAG_CLOUD_6:
            case IO_REPARSE_TAG_CLOUD_7:
            case IO_REPARSE_TAG_CLOUD_8:
            case IO_REPARSE_TAG_CLOUD_9:
            case IO_REPARSE_TAG_CLOUD_A:
            case IO_REPARSE_TAG_CLOUD_B:
            case IO_REPARSE_TAG_CLOUD_C:
            case IO_REPARSE_TAG_CLOUD_D:
            case IO_REPARSE_TAG_CLOUD_E:
            case IO_REPARSE_TAG_CLOUD_F: kind = L"облачный плейсхолдер"; break;
            case IO_REPARSE_TAG_DEDUP: kind = L"дедупликация"; break;
            default: kind = L"reparse-тег без имени"; break;
        }
    }
    CloseHandle(h);
    return kind;
}

}
#endif
