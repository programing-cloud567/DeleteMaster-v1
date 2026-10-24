#include "dm_api.h"
#include "dm_util.h"
#include "dm_locks.h"

static const wchar_t* kId = L"service";
static const int kStage = 25;

struct SvcMatch {
    std::wstring name;
    std::wstring display;
    std::wstring image;
    DWORD pid;
    bool deletable;
    bool stopped;
    bool deleted;
    DWORD err;
    SvcMatch() : pid(0), deletable(false), stopped(false), deleted(false), err(0) {}
};

static std::wstring Lowered(const std::wstring& s) {
    std::wstring r = s;
    for (auto& c : r) c = static_cast<wchar_t>(towlower(c));
    return r;
}

static std::wstring ExpandServicePath(const std::wstring& raw) {
    std::wstring s = dm::TrimQuotesAndSpace(raw);
    if (s.empty()) return s;
    if (s[0] == L'"') {
        size_t e = s.find(L'"', 1);
        if (e != std::wstring::npos) s = s.substr(1, e - 1);
        else s = s.substr(1);
    }
    if (s.compare(0, 12, L"\\SystemRoot\\") == 0) {
        wchar_t win[MAX_PATH] = {0};
        if (GetWindowsDirectoryW(win, MAX_PATH)) s = std::wstring(win) + s.substr(11);
    } else if (s.compare(0, 11, L"%SystemRoot%") == 0) {
        wchar_t win[MAX_PATH] = {0};
        if (GetWindowsDirectoryW(win, MAX_PATH)) s = std::wstring(win) + s.substr(12);
    } else if (s.compare(0, 4, L"\\??\\") == 0) {
        s = s.substr(4);
    }
    size_t sp = s.find(L' ');
    if (sp != std::wstring::npos) {
        std::wstring head = s.substr(0, sp);
        if (head.find(L".exe") != std::wstring::npos) s = head;
    }
    return s;
}

static bool PathInside(const std::wstring& candidate, const std::wstring& target, bool targetIsDir) {
    if (candidate.empty()) return false;
    std::wstring c = Lowered(dm::FullPath(candidate));
    std::wstring t = Lowered(target);
    if (c == t) return true;
    if (!targetIsDir) return false;
    if (c.size() > t.size() && c.compare(0, t.size(), t) == 0 && c[t.size()] == L'\\') return true;
    return false;
}

static DWORD WaitStopped(SC_HANDLE svc, DWORD timeoutMs) {
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    DWORD waited = 0;
    while (waited <= timeoutMs) {
        if (!QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&ssp),
                                  sizeof(ssp), &needed))
            return GetLastError();
        if (ssp.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
        if (ssp.dwCurrentState == SERVICE_STOP_PENDING) {
            DWORD waitHint = ssp.dwWaitHint ? ssp.dwWaitHint : 500;
            if (waitHint > timeoutMs) waitHint = timeoutMs;
            Sleep(150);
            waited += 150;
            continue;
        }
        Sleep(150);
        waited += 150;
    }
    return ERROR_SERVICE_REQUEST_TIMEOUT;
}

static void StopDependents(SC_HANDLE scm, SC_HANDLE svc, DWORD timeoutMs, int depth) {
    if (depth > 8) return;
    DWORD needed = 0;
    DWORD count = 0;
    EnumDependentServicesW(svc, SERVICE_ACTIVE, nullptr, 0, &needed, &count);
    if (needed == 0) return;
    std::vector<BYTE> buf(needed);
    if (!EnumDependentServicesW(svc, SERVICE_ACTIVE, reinterpret_cast<LPENUM_SERVICE_STATUSW>(buf.data()),
                                needed, &needed, &count))
        return;
    LPENUM_SERVICE_STATUSW arr = reinterpret_cast<LPENUM_SERVICE_STATUSW>(buf.data());
    for (DWORD i = 0; i < count; ++i) {
        SC_HANDLE dep = OpenServiceW(scm, arr[i].lpServiceName,
                                     SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
        if (!dep) continue;
        StopDependents(scm, dep, timeoutMs, depth + 1);
        SERVICE_STATUS st{};
        if (ControlService(dep, SERVICE_CONTROL_STOP, &st)) WaitStopped(dep, timeoutMs);
        CloseServiceHandle(dep);
    }
}

static void CollectMatches(SC_HANDLE scm, const std::wstring& targetPlain, bool targetIsDir,
                           const std::vector<DWORD>& holderPids, std::vector<SvcMatch>* out) {
    std::vector<BYTE> buf(256 * 1024);
    DWORD needed = 0;
    DWORD returned = 0;
    DWORD resume = 0;
    int guard = 0;
    while (guard++ < 8) {
        BOOL ok = EnumServicesStatusExW(scm, SC_ENUM_PROCESS_INFO,
                                        SERVICE_WIN32 | SERVICE_DRIVER, SERVICE_STATE_ALL,
                                        buf.data(), static_cast<DWORD>(buf.size()),
                                        &needed, &returned, &resume, nullptr);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_MORE_DATA && needed > buf.size()) {
                buf.resize(needed);
                resume = 0;
                continue;
            }
            break;
        }
        LPENUM_SERVICE_STATUS_PROCESSW arr = reinterpret_cast<LPENUM_SERVICE_STATUS_PROCESSW>(buf.data());
        for (DWORD i = 0; i < returned; ++i) {
            SC_HANDLE svc = OpenServiceW(scm, arr[i].lpServiceName, SERVICE_QUERY_CONFIG);
            if (!svc) continue;
            DWORD cfgSize = 0;
            QueryServiceConfigW(svc, nullptr, 0, &cfgSize);
            std::wstring image;
            if (cfgSize) {
                std::vector<BYTE> cfg(cfgSize);
                if (QueryServiceConfigW(svc, reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(cfg.data()), cfgSize, &cfgSize)) {
                    LPQUERY_SERVICE_CONFIGW qc = reinterpret_cast<LPQUERY_SERVICE_CONFIGW>(cfg.data());
                    if (qc->lpBinaryPathName) image = ExpandServicePath(qc->lpBinaryPathName);
                }
            }
            CloseServiceHandle(svc);

            bool byPath = PathInside(image, targetPlain, targetIsDir);
            bool byPid = false;
            if (!byPath && arr[i].ServiceStatusProcess.dwProcessId > 4) {
                for (DWORD pid : holderPids) {
                    if (pid == arr[i].ServiceStatusProcess.dwProcessId) {
                        byPid = true;
                        break;
                    }
                }
            }
            if (!byPath && !byPid) continue;

            for (const auto& existing : *out) {
                if (existing.name == arr[i].lpServiceName) goto next_service;
            }
            {
                SvcMatch m;
                m.name = arr[i].lpServiceName ? arr[i].lpServiceName : L"";
                m.display = arr[i].lpDisplayName ? arr[i].lpDisplayName : L"";
                m.image = image;
                m.pid = arr[i].ServiceStatusProcess.dwProcessId;
                m.deletable = byPath;
                out->push_back(m);
            }
        next_service:;
        }
        if (resume == 0) break;
    }
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_SERVICE | DM_CAP_DELETE, kId, L"DeleteEngine.Service",
                 L"Службы: находит службы, чьи файлы лежат в удаляемой папке (удаляет службу целиком) или которые держат её файлы (останавливает), снимает зависимые службы.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 25;
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
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: службы не изменялись");
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

    dm::EnableStandardPrivileges();
    dm::EnablePrivilege(L"SeLoadDriverPrivilege");

    std::vector<DWORD> holderPids;
    if (o.flags & DM_FLAG_KILL_HOLDERS) {
        for (const auto& h : dm::FindHolders(plain)) holderPids.push_back(h.pid);
    }

    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ENUMERATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) {
        DWORD e = GetLastError();
        dm::FillResult(result, DM_STATUS_SOFTFAIL, e, kId,
                       L"Нет доступа к диспетчеру служб: " + dm::ErrText(e));
        return DM_STATUS_SOFTFAIL;
    }

    std::vector<SvcMatch> matches;
    CollectMatches(scm, plain, isDir, holderPids, &matches);

    if (matches.empty()) {
        CloseServiceHandle(scm);
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Служб, связанных с объектом, не найдено");
        return DM_STATUS_SKIP;
    }

    DWORD timeout = o.killTimeoutMs ? o.killTimeoutMs : 3000;
    DWORD stoppedCount = 0;
    DWORD deletedCount = 0;
    std::wstring detail;

    for (auto& m : matches) {
        DWORD access = SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS | SERVICE_QUERY_CONFIG;
        if (m.deletable) access |= DELETE | SERVICE_CHANGE_CONFIG;
        SC_HANDLE svc = OpenServiceW(scm, m.name.c_str(), access);
        if (!svc) {
            m.err = GetLastError();
            if (!detail.empty()) detail += L"; ";
            detail += m.name + L": открыть не удалось (" + dm::ErrText(m.err) + L")";
            continue;
        }

        StopDependents(scm, svc, timeout, 0);
        SERVICE_STATUS st{};
        if (ControlService(svc, SERVICE_CONTROL_STOP, &st)) {
            DWORD waitErr = WaitStopped(svc, timeout);
            if (waitErr == ERROR_SUCCESS) {
                m.stopped = true;
                stoppedCount++;
            } else {
                m.err = waitErr;
            }
        } else {
            DWORD e = GetLastError();
            if (e == ERROR_SERVICE_NOT_ACTIVE) {
                m.stopped = true;
                stoppedCount++;
            } else {
                m.err = e;
            }
        }

        if (m.deletable) {
            if (DeleteService(svc)) {
                m.deleted = true;
                deletedCount++;
            } else {
                DWORD e = GetLastError();
                if (e == ERROR_SERVICE_MARKED_FOR_DELETE) {
                    m.deleted = true;
                    deletedCount++;
                } else {
                    m.err = e;
                    ChangeServiceConfigW(svc, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
                                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
                }
            }
        }
        CloseServiceHandle(svc);

        if (!detail.empty()) detail += L"; ";
        detail += m.name;
        if (m.deleted) detail += L" (удалена)";
        else if (m.stopped) detail += L" (остановлена)";
        else detail += L" (не удалось: " + dm::ErrText(m.err) + L")";
    }
    CloseServiceHandle(scm);

    Sleep(150);
    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);

    std::wstring head = L"Служб найдено: " + std::to_wstring(matches.size()) +
                        L", удалено: " + std::to_wstring(deletedCount) +
                        L", остановлено: " + std::to_wstring(stoppedCount) + L". " + detail;
    if (result) {
        result->filesRemoved = c.files;
        result->dirsRemoved = c.dirs;
        result->bytesFreed = c.bytes;
    }

    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, head + L". Объект удалён");
        return DM_STATUS_OK;
    }

    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId,
                   head + L". Объект пока на месте: " + dm::ErrText(err));
    return DM_STATUS_SOFTFAIL;
}
