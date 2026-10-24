#include "dm_api.h"
#include "dm_util.h"
#include "dm_locks.h"
#include <tlhelp32.h>
#include <vector>
#include <set>
#include <map>
#include <string>

static const wchar_t* kId = L"freeze";
static const int kStage = 19;

typedef LONG(NTAPI* NtSuspendProcess_t)(HANDLE);
typedef LONG(NTAPI* NtResumeProcess_t)(HANDLE);

struct ProcNode {
    DWORD pid;
    DWORD ppid;
    std::wstring image;
};

static bool Snap(std::vector<ProcNode>* out) {
    out->clear();
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(s, &pe)) {
        do {
            ProcNode n{ pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile };
            out->push_back(n);
        } while (Process32NextW(s, &pe));
    }
    CloseHandle(s);
    return !out->empty();
}

static bool CriticalImage(const std::wstring& image) {
    static const wchar_t* critical[] = {
        L"system", L"registry", L"memory compression", L"secure system",
        L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
        L"lsass.exe", L"smss.exe", L"svchost.exe", L"fontdrvhost.exe",
        L"dwm.exe", L"sihost.exe", L"taskhostw.exe", L"explorer.exe",
        L"msmpeng.exe", L"nissrv.exe", L"securityhealthservice.exe"
    };
    std::wstring b = dm::BaseName(image);
    for (auto& c : b) c = static_cast<wchar_t>(towlower(c));
    for (const wchar_t* c : critical) if (b == c) return true;
    return false;
}

static std::wstring ImageOf(const std::vector<ProcNode>& snap, DWORD pid) {
    for (const auto& n : snap) if (n.pid == pid) return n.image;
    return std::wstring();
}

static bool OwnAncestor(DWORD pid, const std::vector<ProcNode>& snap) {
    static std::set<DWORD> own;
    static bool init = false;
    if (!init) {
        init = true;
        own.insert(GetCurrentProcessId());
        DWORD parent = 0;
        for (const auto& n : snap) if (n.pid == GetCurrentProcessId()) { parent = n.ppid; break; }
        for (int i = 0; i < 6 && parent; ++i) {
            own.insert(parent);
            DWORD next = 0;
            for (const auto& n : snap) if (n.pid == parent) { next = n.ppid; break; }
            parent = next;
        }
    }
    return own.count(pid) != 0;
}

static DWORD RootOf(DWORD pid, const std::vector<ProcNode>& snap) {
    DWORD cur = pid;
    for (int i = 0; i < 64; ++i) {
        DWORD parent = 0;
        for (const auto& n : snap) if (n.pid == cur) { parent = n.ppid; break; }
        if (parent == 0 || parent == cur || parent == 4) break;
        if (OwnAncestor(parent, snap)) break;
        if (CriticalImage(ImageOf(snap, parent))) break;
        cur = parent;
    }
    return cur;
}

static void CollectTree(DWORD root, const std::vector<ProcNode>& snap, std::set<DWORD>* out, int depth) {
    if (depth > 32) return;
    if (!out->insert(root).second) return;
    for (const auto& n : snap) {
        if (n.ppid == root && n.pid != root) CollectTree(n.pid, snap, out, depth + 1);
    }
}

static NtSuspendProcess_t PfnSuspend() {
    static NtSuspendProcess_t fn = reinterpret_cast<NtSuspendProcess_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtSuspendProcess"));
    return fn;
}

static NtResumeProcess_t PfnResume() {
    static NtResumeProcess_t fn = reinterpret_cast<NtResumeProcess_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtResumeProcess"));
    return fn;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_FREEZE | DM_CAP_DELETE, kId, L"DeleteEngine.Freeze",
                 L"Заморозка: приостанавливает всё дерево процессов, держащих объект, удаляет его, пока они стоят, затем возвращает живые процессы к работе.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions* opt, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (opt && opt->structSize == sizeof(DmOptions) && !(opt->flags & DM_FLAG_KILL_HOLDERS)) return DM_STATUS_SKIP;
    if (score) *score = 50;
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmExecute(const wchar_t* target, const DmOptions* opt, DmResult* result) {
    DmOptions o{};
    if (opt) o = *opt;
    if (o.structSize != sizeof(DmOptions)) {
        o.flags = DM_FLAG_DEFAULT;
        o.retryCount = 2;
        o.retryDelayMs = 40;
        o.killTimeoutMs = 2000;
    }
    if (!target || !*target) {
        dm::FillResult(result, DM_STATUS_FAIL, ERROR_INVALID_PARAMETER, kId, L"Пустой путь");
        return DM_STATUS_FAIL;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: процессы не замораживались");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    if (!dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }

    dm::EnableStandardPrivileges();
    std::vector<dm::Holder> holders = dm::FindHolders(plain);
    if (holders.empty()) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Держателей нет, замораживать некого");
        return DM_STATUS_SKIP;
    }

    std::vector<ProcNode> snap;
    Snap(&snap);

    std::set<DWORD> targets;
    for (const auto& h : holders) {
        if (h.pid <= 4 || h.pid == GetCurrentProcessId()) continue;
        if (CriticalImage(h.image.empty() ? ImageOf(snap, h.pid) : h.image)) continue;
        CollectTree(RootOf(h.pid, snap), snap, &targets, 0);
    }
    if (targets.empty()) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, ERROR_ACCESS_DENIED, kId,
                       L"Все держатели — системные процессы, заморозка отменена");
        return DM_STATUS_SOFTFAIL;
    }

    std::vector<std::pair<DWORD, HANDLE>> opened;
    DWORD frozen = 0;
    for (DWORD pid : targets) {
        if (pid == GetCurrentProcessId()) continue;
        if (CriticalImage(ImageOf(snap, pid))) continue;
        HANDLE h = OpenProcess(PROCESS_SUSPEND_RESUME | PROCESS_TERMINATE | SYNCHRONIZE |
                                   PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
        if (!h) continue;
        NtSuspendProcess_t susp = PfnSuspend();
        if (susp && susp(h) >= 0) {
            frozen++;
            opened.push_back(std::make_pair(pid, h));
        } else {
            CloseHandle(h);
        }
    }

    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);

    DWORD killed = 0;
    if (err != ERROR_SUCCESS && dm::Exists(ext)) {
        for (auto& ph : opened) {
            if (TerminateProcess(ph.second, 0xC0000001)) killed++;
        }
        if (killed) {
            Sleep(120);
            err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);
        }
    }

    NtResumeProcess_t resume = PfnResume();
    DWORD resumed = 0;
    for (auto& ph : opened) {
        DWORD code = 0;
        if (GetExitCodeProcess(ph.second, &code) && code == STILL_ACTIVE) {
            if (resume && resume(ph.second) >= 0) resumed++;
        }
        CloseHandle(ph.second);
    }

    if (result) {
        result->filesRemoved = c.files;
        result->dirsRemoved = c.dirs;
        result->bytesFreed = c.bytes;
    }

    std::wstring head = L"Заморожено процессов: " + std::to_wstring(frozen) +
                        L", завершено: " + std::to_wstring(killed) +
                        L", возвращено: " + std::to_wstring(resumed);
    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, head + L". Объект удалён под заморозкой");
        return DM_STATUS_OK;
    }
    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId,
                   head + L". Удаление не прошло: " + dm::ErrText(err));
    return DM_STATUS_SOFTFAIL;
}
