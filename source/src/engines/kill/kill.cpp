#include "dm_api.h"
#include "dm_util.h"
#include "dm_locks.h"
#include <tlhelp32.h>
#include <vector>
#include <string>
#include <set>
#include <map>
#include <algorithm>

static const wchar_t* kId = L"kill";
static const int kStage = 20;

#define DM_PROCESS_SUSPEND_RESUME 0x0800UL
#define DM_MAX_TREE_DEPTH 64

typedef LONG(NTAPI* NtSuspendProcess_t)(HANDLE);
typedef LONG(NTAPI* NtResumeProcess_t)(HANDLE);

struct ProcNode {
    DWORD pid;
    DWORD ppid;
    std::wstring image;
};

enum LockState {
    LockFree = 0,
    LockBusy = 1,
    LockAcl = 2
};

struct KillStats {
    DWORD holders;
    DWORD roots;
    DWORD suspended;
    DWORD terminated;
    DWORD graceful;
    DWORD failed;
    DWORD skippedCritical;
    bool suspendedFirst;
    KillStats() : holders(0), roots(0), suspended(0), terminated(0), graceful(0),
                  failed(0), skippedCritical(0), suspendedFirst(false) {}
};

static std::wstring BaseLower(const std::wstring& path) {
    std::wstring b = dm::BaseName(path);
    for (auto& c : b) c = static_cast<wchar_t>(towlower(c));
    return b;
}

static bool Snapshot(std::vector<ProcNode>* out) {
    out->clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcNode n;
            n.pid = pe.th32ProcessID;
            n.ppid = pe.th32ParentProcessID;
            n.image = pe.szExeFile;
            out->push_back(n);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return !out->empty();
}

static const ProcNode* FindNode(const std::vector<ProcNode>& snap, DWORD pid) {
    for (const auto& n : snap) if (n.pid == pid) return &n;
    return nullptr;
}

static std::wstring ImageOf(const std::vector<ProcNode>& snap, DWORD pid) {
    const ProcNode* n = FindNode(snap, pid);
    return n ? n->image : std::wstring();
}

static DWORD ParentOf(const std::vector<ProcNode>& snap, DWORD pid) {
    const ProcNode* n = FindNode(snap, pid);
    return n ? n->ppid : 0;
}

static bool IsCriticalImage(const std::wstring& image) {
    static const wchar_t* critical[] = {
        L"system", L"registry", L"memory compression", L"secure system",
        L"csrss.exe", L"wininit.exe", L"winlogon.exe", L"services.exe",
        L"lsass.exe", L"smss.exe", L"svchost.exe", L"fontdrvhost.exe",
        L"dwm.exe", L"sihost.exe", L"taskhostw.exe", L"explorer.exe",
        L"msmpeng.exe", L"nissrv.exe", L"securityhealthservice.exe",
        L"audiodg.exe", L"wudfhost.exe", L"spoolsv.exe", L"searchindexer.exe"
    };
    std::wstring b = BaseLower(image);
    for (const wchar_t* c : critical) {
        if (b == c) return true;
    }
    return false;
}

static bool IsOurself(DWORD pid) {
    static std::set<DWORD> own;
    static bool init = false;
    if (!init) {
        init = true;
        own.insert(GetCurrentProcessId());
        std::vector<ProcNode> snap;
        if (Snapshot(&snap)) {
            DWORD parent = ParentOf(snap, GetCurrentProcessId());
            for (int i = 0; i < 4 && parent; ++i) {
                own.insert(parent);
                parent = ParentOf(snap, parent);
            }
        }
    }
    return own.count(pid) != 0;
}

static bool IsProtected(DWORD pid, const std::vector<ProcNode>& snap) {
    if (pid <= 4) return true;
    if (IsOurself(pid)) return true;
    return IsCriticalImage(ImageOf(snap, pid));
}

static DWORD RootAncestor(DWORD pid, const std::vector<ProcNode>& snap) {
    DWORD cur = pid;
    for (int depth = 0; depth < DM_MAX_TREE_DEPTH; ++depth) {
        DWORD parent = ParentOf(snap, cur);
        if (parent == 0 || parent == cur || parent == 4) break;
        if (IsOurself(parent)) break;
        if (IsCriticalImage(ImageOf(snap, parent))) break;
        cur = parent;
    }
    return cur;
}

static void CollectSubtree(DWORD root, const std::vector<ProcNode>& snap, std::vector<DWORD>* out, int depth) {
    if (depth > DM_MAX_TREE_DEPTH) return;
    out->push_back(root);
    for (const auto& n : snap) {
        if (n.ppid == root && n.pid != root) CollectSubtree(n.pid, snap, out, depth + 1);
    }
}

static HANDLE OpenForControl(DWORD pid) {
    DWORD access = PROCESS_TERMINATE | PROCESS_SUSPEND_RESUME | SYNCHRONIZE |
                   PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SET_QUOTA;
    HANDLE h = OpenProcess(access, FALSE, pid);
    if (h) return h;
    return OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
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

static bool SuspendPid(HANDLE h) {
    NtSuspendProcess_t fn = PfnSuspend();
    if (!fn || !h) return false;
    return fn(h) >= 0;
}

static bool ResumePid(HANDLE h) {
    NtResumeProcess_t fn = PfnResume();
    if (!fn || !h) return false;
    return fn(h) >= 0;
}

struct ClosePost {
    DWORD pid;
    DWORD posted;
    DWORD sent;
};

static BOOL CALLBACK CloseProc(HWND hwnd, LPARAM param) {
    ClosePost* cp = reinterpret_cast<ClosePost*>(param);
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner != cp->pid) return TRUE;
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(hwnd, WM_CLOSE, 0, 0, SMTO_ABORTIFHUNG | SMTO_NORMAL, 400, &res)) cp->sent++;
    else if (PostMessageW(hwnd, WM_CLOSE, 0, 0)) cp->posted++;
    return TRUE;
}

static DWORD GracefulClose(DWORD pid, DWORD timeoutMs) {
    ClosePost cp{pid, 0, 0};
    EnumWindows(CloseProc, reinterpret_cast<LPARAM>(&cp));
    DWORD total = cp.sent + cp.posted;
    if (total == 0) return 0;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return total;
    DWORD wait = WaitForSingleObject(h, timeoutMs);
    CloseHandle(h);
    return wait == WAIT_OBJECT_0 ? total : total;
}

static bool TerminatePid(HANDLE h, DWORD pid, DWORD timeoutMs) {
    if (!h) return false;
    if (!TerminateProcess(h, 0xC0000001)) {
        DWORD e = GetLastError();
        if (e == ERROR_ACCESS_DENIED) {
            dm::EnablePrivilege(L"SeDebugPrivilege");
            HANDLE h2 = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
            if (h2) {
                BOOL ok = TerminateProcess(h2, 0xC0000001);
                DWORD e2 = ok ? ERROR_SUCCESS : GetLastError();
                WaitForSingleObject(h2, timeoutMs);
                CloseHandle(h2);
                return ok != FALSE && e2 == ERROR_SUCCESS;
            }
        }
        return false;
    }
    WaitForSingleObject(h, timeoutMs);
    return true;
}

static bool VolumeReadOnly(const std::wstring& ext) {
    wchar_t vol[MAX_PATH] = {0};
    if (!GetVolumePathNameW(ext.c_str(), vol, MAX_PATH)) return false;
    DWORD flags = 0;
    if (!GetVolumeInformationW(vol, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0)) return false;
    return (flags & FILE_READ_ONLY_VOLUME) != 0;
}

static LockState ProbeLock(const std::wstring& ext, bool isDir) {
    DWORD flags = FILE_FLAG_BACKUP_SEMANTICS;
    if (isDir) flags |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(ext.c_str(), DELETE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, flags, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        CloseHandle(h);
        return LockFree;
    }
    DWORD e = GetLastError();
    if (e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION) return LockBusy;
    if (e == ERROR_ACCESS_DENIED) return LockAcl;
    if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return LockFree;
    return LockAcl;
}

static std::wstring LockStateText(LockState s) {
    if (s == LockFree) return L"объект свободен";
    if (s == LockBusy) return L"объект удерживается дескриптором";
    return L"доступ закрыт правами (не блокировка)";
}

static bool KillTree(DWORD root, const std::vector<ProcNode>& snap, DWORD timeoutMs,
                     bool graceful, KillStats* st, std::wstring* log) {
    std::vector<DWORD> tree;
    CollectSubtree(root, snap, &tree, 0);
    if (tree.empty()) return false;

    std::vector<DWORD> ordered(tree.begin(), tree.end());
    std::reverse(ordered.begin(), ordered.end());

    std::vector<std::pair<DWORD, HANDLE>> handles;
    for (DWORD pid : ordered) {
        if (IsProtected(pid, snap)) {
            st->skippedCritical++;
            continue;
        }
        HANDLE h = OpenForControl(pid);
        if (h) handles.push_back(std::make_pair(pid, h));
    }
    if (handles.empty()) return false;

    if (graceful) {
        DWORD closed = GracefulClose(root, timeoutMs / 3);
        if (closed) st->graceful++;
    }

    for (auto& ph : handles) {
        if (SuspendPid(ph.second)) st->suspended++;
    }
    if (!handles.empty()) st->suspendedFirst = true;

    bool anyTerminated = false;
    for (auto& ph : handles) {
        if (TerminatePid(ph.second, ph.first, timeoutMs)) {
            st->terminated++;
            anyTerminated = true;
        } else {
            st->failed++;
            ResumePid(ph.second);
        }
    }

    for (auto& ph : handles) CloseHandle(ph.second);

    if (log) {
        std::wstring names;
        for (DWORD pid : tree) {
            std::wstring img = BaseLower(ImageOf(snap, pid));
            if (img.empty()) continue;
            if (!names.empty()) names += L", ";
            names += img;
        }
        if (!names.empty()) {
            if (!log->empty()) *log += L"; ";
            *log += L"дерево " + std::to_wstring(root) + L" (" + names + L")";
        }
    }
    return anyTerminated;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_KILL | DM_CAP_DELETE, kId, L"DeleteEngine.Kill",
                 L"Снятие блокировки: находит держателей файла и папки, поднимается до некритичного корня дерева процессов, приостанавливает всё дерево и завершает его, затем проверяет освобождение дескрипторов и удаляет объект.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions* opt, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (opt && opt->structSize == sizeof(DmOptions) && !(opt->flags & DM_FLAG_KILL_HOLDERS)) return DM_STATUS_SKIP;
    if (score) *score = 40;
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmExecute(const wchar_t* target, const DmOptions* opt, DmResult* result) {
    DmOptions o{};
    if (opt) o = *opt;
    if (o.structSize != sizeof(DmOptions)) {
        o.flags = DM_FLAG_DEFAULT;
        o.retryCount = 3;
        o.retryDelayMs = 60;
        o.killTimeoutMs = 3000;
    }
    if (!target || !*target) {
        dm::FillResult(result, DM_STATUS_FAIL, ERROR_INVALID_PARAMETER, kId, L"Пустой путь");
        return DM_STATUS_FAIL;
    }
    if (!(o.flags & DM_FLAG_KILL_HOLDERS)) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Завершение процессов отключено флагом");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }
    bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: процессы не завершались");
        return DM_STATUS_SKIP;
    }

    if (VolumeReadOnly(ext)) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, ERROR_WRITE_PROTECT, kId,
                       L"Том только для чтения: блокировка дескрипторами ни при чём, завершение процессов не поможет");
        return DM_STATUS_SOFTFAIL;
    }

    dm::EnableStandardPrivileges();
    DWORD timeout = o.killTimeoutMs ? o.killTimeoutMs : 3000;
    bool graceful = (o.flags & DM_FLAG_NO_GRACEFUL) == 0;

    std::vector<dm::Holder> holders = dm::FindHolders(plain);
    LockState state = ProbeLock(ext, isDir);

    if (holders.empty()) {
        if (state == LockBusy) {
            dm::FillResult(result, DM_STATUS_SOFTFAIL, ERROR_SHARING_VIOLATION, kId,
                           L"Объект удерживается, но держатель не определяется: нужен более глубокий скан или перезагрузка");
            return DM_STATUS_SOFTFAIL;
        }
        if (state == LockAcl) {
            dm::FillResult(result, DM_STATUS_SKIP, ERROR_ACCESS_DENIED, kId,
                           L"Блокировки нет, доступ закрыт правами: этим занимается движок Acl");
            return DM_STATUS_SKIP;
        }
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Держателей не найдено, объект свободен");
        return DM_STATUS_SKIP;
    }

    std::vector<ProcNode> snap;
    Snapshot(&snap);

    std::set<DWORD> roots;
    std::set<DWORD> holderPids;
    KillStats st;
    std::wstring log;

    for (const auto& h : holders) {
        if (h.pid == 0 || h.pid == 4) continue;
        if (IsProtected(h.pid, snap)) {
            st.skippedCritical++;
            continue;
        }
        st.holders++;
        holderPids.insert(h.pid);
        DWORD root = RootAncestor(h.pid, snap);
        if (root != h.pid) {
            if (!log.empty()) log += L"; ";
            log += L"держатель " + BaseLower(ImageOf(snap, h.pid)) + L" поднят до корня " +
                   BaseLower(ImageOf(snap, root));
        }
        roots.insert(root);
    }
    st.roots = static_cast<DWORD>(roots.size());

    if (roots.empty()) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, ERROR_ACCESS_DENIED, kId,
                       L"Все держатели оказались защищёнными системными процессами: " +
                           dm::FormatHolders(holders));
        return DM_STATUS_SOFTFAIL;
    }

    for (DWORD root : roots) {
        if (IsProtected(root, snap)) continue;
        KillTree(root, snap, timeout, graceful, &st, &log);
    }

    bool released = false;
    DWORD waited = 0;
    DWORD rounds = 0;
    while (waited <= timeout * 2 && rounds < 20) {
        state = ProbeLock(ext, isDir);
        if (state != LockBusy) {
            released = true;
            break;
        }
        std::vector<ProcNode> again;
        Snapshot(&again);
        bool respawned = false;
        for (const auto& h : dm::FindHolders(plain)) {
            if (holderPids.count(h.pid) == 0 && !IsProtected(h.pid, again)) {
                DWORD root = RootAncestor(h.pid, again);
                if (roots.insert(root).second) {
                    KillTree(root, again, timeout, false, &st, &log);
                    respawned = true;
                }
            }
        }
        if (!respawned) Sleep(150);
        waited += 150;
        rounds++;
    }

    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);
    if (result) {
        result->filesRemoved = c.files;
        result->dirsRemoved = c.dirs;
        result->bytesFreed = c.bytes;
        result->processesKilled = st.terminated;
        result->flagsOut = released ? DM_CAP_KILL : 0;
    }

    std::wstring head = L"Держателей: " + std::to_wstring(st.holders) +
                        L", корней: " + std::to_wstring(st.roots) +
                        L", приостановлено: " + std::to_wstring(st.suspended) +
                        L", завершено: " + std::to_wstring(st.terminated);
    if (st.graceful) head += L", закрыто штатно: " + std::to_wstring(st.graceful);
    if (st.failed) head += L", не удалось: " + std::to_wstring(st.failed);
    if (st.skippedCritical) head += L", пропущено системных: " + std::to_wstring(st.skippedCritical);
    head += released ? L", дескрипторы освобождены" : L", дескрипторы всё ещё держатся";
    if (!log.empty()) head += L". " + log;

    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, head + L". Объект удалён");
        return DM_STATUS_OK;
    }

    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId,
                   head + L". Удаление не прошло: " + dm::ErrText(err));
    return DM_STATUS_SOFTFAIL;
}
