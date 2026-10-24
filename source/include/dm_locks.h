#pragma once
#ifndef DM_LOCKS_H
#define DM_LOCKS_H

#include "dm_util.h"
#include <restartmanager.h>
#include <tlhelp32.h>
#include <map>
#include <set>
#include <utility>

#pragma comment(lib, "Rstrtmgr.lib")
#pragma comment(lib, "Advapi32.lib")

namespace dm {

struct Holder {
    DWORD pid;
    std::wstring image;
    std::wstring source;
};

typedef LONG(NTAPI* NtQuerySystemInformation_t)(ULONG, PVOID, ULONG, PULONG);
typedef LONG(NTAPI* NtQueryObject_t)(HANDLE, ULONG, PVOID, ULONG, PULONG);

struct SysHandleEntry {
    void* Object;
    ULONG_PTR ProcessId;
    ULONG_PTR HandleValue;
    ULONG GrantedAccess;
    USHORT CreatorBackTraceIndex;
    USHORT ObjectTypeIndex;
    ULONG HandleAttributes;
    ULONG Reserved;
};

struct SysHandleInfoEx {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SysHandleEntry Handles[1];
};

inline std::wstring ProcessImage(DWORD pid) {
    if (pid == 0 || pid == 4) return L"System";
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return L"";
    std::vector<wchar_t> buf(DM_MAX_PATH);
    DWORD n = static_cast<DWORD>(buf.size());
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, buf.data(), &n)) out.assign(buf.data(), n);
    CloseHandle(p);
    return out;
}

inline std::wstring BaseName(const std::wstring& p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

inline std::vector<Holder> RestartManagerHolders(const std::wstring& plainPath) {
    std::vector<Holder> out;
    DWORD session = 0;
    wchar_t key[CCH_RM_SESSION_KEY + 1] = {0};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return out;
    LPCWSTR files[1] = { plainPath.c_str() };
    DWORD rc = RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr);
    if (rc == ERROR_SUCCESS) {
        UINT needed = 0;
        UINT count = 0;
        DWORD reasons = 0;
        RmGetList(session, &needed, &count, nullptr, &reasons);
        if (needed == 0) needed = 8;
        std::vector<RM_PROCESS_INFO> info(needed);
        count = needed;
        if (RmGetList(session, &needed, &count, info.data(), &reasons) == ERROR_SUCCESS) {
            std::set<DWORD> seen;
            for (UINT i = 0; i < count; ++i) {
                DWORD pid = info[i].Process.dwProcessId;
                if (pid == 0 || pid == GetCurrentProcessId()) continue;
                if (!seen.insert(pid).second) continue;
                Holder h;
                h.pid = pid;
                h.image = ProcessImage(pid);
                h.source = L"RestartManager";
                out.push_back(h);
            }
        }
    }
    RmEndSession(session);
    return out;
}

inline std::vector<std::pair<std::wstring, std::wstring>> DeviceMap() {
    std::vector<std::pair<std::wstring, std::wstring>> map;
    wchar_t drives[1024] = {0};
    DWORD n = GetLogicalDriveStringsW(1023, drives);
    if (n == 0) return map;
    for (wchar_t* p = drives; p < drives + n && *p; p += wcslen(p) + 1) {
        std::wstring root(p, 2);
        wchar_t target[2048] = {0};
        if (QueryDosDeviceW(root.c_str(), target, 2047)) map.push_back(std::make_pair(std::wstring(target), root));
    }
    return map;
}

inline std::wstring DeviceToDos(const std::wstring& nt, const std::vector<std::pair<std::wstring, std::wstring>>& map) {
    for (const auto& kv : map) {
        if (nt.size() >= kv.first.size() && _wcsnicmp(nt.c_str(), kv.first.c_str(), kv.first.size()) == 0)
            return kv.second + nt.substr(kv.first.size());
    }
    return nt;
}

struct NameQueryCtx {
    HANDLE handle;
    NtQueryObject_t fn;
    std::wstring name;
};

inline DWORD WINAPI NameQueryProc(LPVOID param) {
    NameQueryCtx* ctx = static_cast<NameQueryCtx*>(param);
    const ULONG size = 64 * 1024;
    std::vector<BYTE> buf(size, 0);
    ULONG ret = 0;
    LONG st = ctx->fn(ctx->handle, 1, buf.data(), size, &ret);
    if (st == 0) {
        typedef struct {
            USHORT Length;
            USHORT MaximumLength;
            PWSTR Buffer;
        } USTR;
        const USTR* us = reinterpret_cast<const USTR*>(buf.data());
        if (us->Buffer && us->Length > 0)
            ctx->name.assign(us->Buffer, us->Length / sizeof(wchar_t));
    }
    return 0;
}

inline bool QueryHandleName(HANDLE dup, NtQueryObject_t fn, std::wstring& out, DWORD timeoutMs) {
    NameQueryCtx* ctx = new NameQueryCtx();
    ctx->handle = dup;
    ctx->fn = fn;
    HANDLE t = CreateThread(nullptr, 64 * 1024, NameQueryProc, ctx, 0, nullptr);
    if (!t) {
        delete ctx;
        return false;
    }
    DWORD w = WaitForSingleObject(t, timeoutMs);
    if (w == WAIT_OBJECT_0) {
        out = ctx->name;
        CloseHandle(t);
        delete ctx;
        return !out.empty();
    }
    CloseHandle(t);
    return false;
}

struct Probes {
    HANDLE fileHandle;
    HANDLE dirHandle;
    std::wstring dirPath;
    Probes() : fileHandle(INVALID_HANDLE_VALUE), dirHandle(INVALID_HANDLE_VALUE) {}
    ~Probes() {
        if (fileHandle != INVALID_HANDLE_VALUE) CloseHandle(fileHandle);
        if (dirHandle != INVALID_HANDLE_VALUE) CloseHandle(dirHandle);
        if (!dirPath.empty()) RemoveDirectoryW(dirPath.c_str());
    }
};

inline void CreateProbes(Probes* p) {
    wchar_t tempDir[MAX_PATH] = {0};
    wchar_t tempFile[MAX_PATH] = {0};
    if (!GetTempPathW(MAX_PATH, tempDir)) return;
    if (!GetTempFileNameW(tempDir, L"dmz", 0, tempFile)) return;
    p->fileHandle = CreateFileW(tempFile, GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, CREATE_ALWAYS,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    p->dirPath = std::wstring(tempDir) + L"dmzdir_probe";
    CreateDirectoryW(p->dirPath.c_str(), nullptr);
    p->dirHandle = CreateFileW(p->dirPath.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
}

inline void DetectTypeIndexes(const SysHandleInfoEx* info, const Probes& probes, USHORT* fileIdx, USHORT* dirIdx) {
    if (fileIdx) *fileIdx = 0;
    if (dirIdx) *dirIdx = 0;
    const ULONG_PTR me = static_cast<ULONG_PTR>(GetCurrentProcessId());
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const SysHandleEntry& e = info->Handles[i];
        if (e.ProcessId != me) continue;
        if (fileIdx && *fileIdx == 0 && probes.fileHandle != INVALID_HANDLE_VALUE &&
            e.HandleValue == reinterpret_cast<ULONG_PTR>(probes.fileHandle))
            *fileIdx = e.ObjectTypeIndex;
        if (dirIdx && *dirIdx == 0 && probes.dirHandle != INVALID_HANDLE_VALUE &&
            e.HandleValue == reinterpret_cast<ULONG_PTR>(probes.dirHandle))
            *dirIdx = e.ObjectTypeIndex;
        if ((fileIdx ? *fileIdx : 1) && (dirIdx ? *dirIdx : 1)) break;
    }
}

inline std::vector<Holder> HandleHolders(const std::wstring& plainPath) {
    std::vector<Holder> out;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return out;
    NtQuerySystemInformation_t query = reinterpret_cast<NtQuerySystemInformation_t>(
        GetProcAddress(ntdll, "NtQuerySystemInformation"));
    NtQueryObject_t queryObj = reinterpret_cast<NtQueryObject_t>(GetProcAddress(ntdll, "NtQueryObject"));
    if (!query || !queryObj) return out;

    Probes probes;
    CreateProbes(&probes);
    ULONG size = 1u << 22;
    std::vector<BYTE> buffer;
    bool got = false;
    for (int attempt = 0; attempt < 14; ++attempt) {
        buffer.assign(size, 0);
        ULONG ret = 0;
        LONG st = query(64, buffer.data(), size, &ret);
        if (st == 0) {
            got = true;
            break;
        }
        if (st != static_cast<LONG>(0xC0000004)) break;
        size = ret + (1u << 20);
    }
    if (!got) return out;

    const SysHandleInfoEx* info = reinterpret_cast<const SysHandleInfoEx*>(buffer.data());
    USHORT fileType = 0, dirType = 0;
    DetectTypeIndexes(info, probes, &fileType, &dirType);
    if (fileType == 0 && dirType == 0) return out;
    std::wstring target = plainPath;
    while (!target.empty() && target.back() == L'\\') target.pop_back();
    std::wstring targetLower = target;
    for (auto& ch : targetLower) ch = static_cast<wchar_t>(towlower(ch));
    std::wstring prefix = targetLower + L"\\";
    auto devmap = DeviceMap();
    const DWORD me = GetCurrentProcessId();

    std::map<DWORD, HANDLE> cache;
    std::set<DWORD> found;
    for (ULONG_PTR i = 0; i < info->NumberOfHandles; ++i) {
        const SysHandleEntry& e = info->Handles[i];
        if (e.ObjectTypeIndex != fileType && e.ObjectTypeIndex != dirType) continue;
        DWORD pid = static_cast<DWORD>(e.ProcessId);
        if (pid == me || pid == 0 || pid == 4) continue;
        if (found.count(pid)) continue;
        HANDLE ph = nullptr;
        auto it = cache.find(pid);
        if (it != cache.end()) ph = it->second;
        else {
            ph = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            cache[pid] = ph;
        }
        if (!ph) continue;
        HANDLE dup = nullptr;
        if (!DuplicateHandle(ph, reinterpret_cast<HANDLE>(e.HandleValue), GetCurrentProcess(), &dup,
                             0, FALSE, DUPLICATE_SAME_ACCESS))
            continue;
        std::wstring name;
        bool ok = QueryHandleName(dup, queryObj, name, 120);
        CloseHandle(dup);
        if (!ok) continue;
        std::wstring dos = DeviceToDos(name, devmap);
        for (auto& ch : dos) ch = static_cast<wchar_t>(towlower(ch));
        if (dos == targetLower || dos.compare(0, prefix.size(), prefix) == 0) {
            Holder h;
            h.pid = pid;
            h.image = ProcessImage(pid);
            h.source = L"HandleScan";
            out.push_back(h);
            found.insert(pid);
        }
    }
    for (auto& kv : cache) if (kv.second) CloseHandle(kv.second);
    return out;
}

inline std::vector<Holder> FindHolders(const std::wstring& plainPath) {
    std::vector<Holder> rm = RestartManagerHolders(plainPath);
    std::vector<Holder> hs = HandleHolders(plainPath);
    std::set<DWORD> seen;
    std::vector<Holder> merged;
    for (const auto& h : rm) {
        if (seen.insert(h.pid).second) merged.push_back(h);
    }
    for (const auto& h : hs) {
        if (seen.insert(h.pid).second) merged.push_back(h);
    }
    return merged;
}

inline bool IsCriticalProcess(DWORD pid, const std::wstring& image) {
    if (pid <= 4) return true;
    if (pid == GetCurrentProcessId()) return true;
    static const wchar_t* critical[] = {
        L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
        L"lsass.exe", L"smss.exe", L"svchost.exe", L"fontdrvhost.exe",
        L"system", L"registry", L"memory compression", L"secure system"
    };
    std::wstring base = BaseName(image);
    for (auto& ch : base) ch = static_cast<wchar_t>(towlower(ch));
    for (const wchar_t* c : critical) {
        if (base == c) return true;
    }
    return false;
}

struct CloseCtx {
    DWORD pid;
    DWORD posted;
};

inline BOOL CALLBACK CloseWindowProc(HWND hwnd, LPARAM param) {
    CloseCtx* ctx = reinterpret_cast<CloseCtx*>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner == ctx->pid) {
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        ctx->posted++;
    }
    return TRUE;
}

inline bool TerminateHolder(DWORD pid, DWORD timeoutMs, bool graceful, DWORD* errOut) {
    if (errOut) *errOut = ERROR_SUCCESS;
    if (pid == GetCurrentProcessId()) return false;
    if (graceful) {
        CloseCtx ctx{ pid, 0 };
        EnumWindows(CloseWindowProc, reinterpret_cast<LPARAM>(&ctx));
    }
    HANDLE p = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) {
        DWORD e = GetLastError();
        if (e == ERROR_INVALID_PARAMETER) return true;
        if (errOut) *errOut = e;
        return false;
    }
    DWORD wait = WaitForSingleObject(p, graceful ? (timeoutMs / 2) : 0);
    if (wait == WAIT_OBJECT_0) {
        CloseHandle(p);
        return true;
    }
    bool ok = TerminateProcess(p, 0xC0000001) != FALSE;
    DWORD e = ok ? ERROR_SUCCESS : GetLastError();
    WaitForSingleObject(p, timeoutMs);
    CloseHandle(p);
    if (errOut) *errOut = e;
    return ok;
}

inline std::wstring FormatHolders(const std::vector<Holder>& v) {
    std::wstring s;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += L", ";
        wchar_t pidbuf[32];
        swprintf_s(pidbuf, L"%lu", static_cast<unsigned long>(v[i].pid));
        s += BaseName(v[i].image);
        if (s.empty()) s = L"неизвестный процесс";
        s += L" (PID ";
        s += pidbuf;
        s += L", ";
        s += v[i].source;
        s += L")";
    }
    return s;
}

}
#endif
