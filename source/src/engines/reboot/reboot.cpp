#include "dm_api.h"
#include "dm_util.h"

static const wchar_t* kId = L"reboot";
static const int kStage = 80;

static const wchar_t* kSessionKey = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
static const wchar_t* kPendingValue = L"PendingFileRenameOperations";

static std::wstring NtPrefixed(const std::wstring& plain) {
    if (plain.size() >= 2 && plain[0] == L'\\' && plain[1] == L'\\')
        return L"\\??\\UNC\\" + plain.substr(2);
    return L"\\??\\" + plain;
}

static DWORD AddPendingRename(const std::wstring& plain) {
    HKEY key = nullptr;
    DWORD disp = 0;
    LONG rc = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kSessionKey, 0, nullptr, 0,
                              KEY_READ | KEY_WRITE, nullptr, &key, &disp);
    if (rc != ERROR_SUCCESS) return static_cast<DWORD>(rc);

    std::vector<wchar_t> existing;
    DWORD type = 0;
    DWORD cb = 0;
    rc = RegQueryValueExW(key, kPendingValue, nullptr, &type, nullptr, &cb);
    if (rc == ERROR_SUCCESS && (type == REG_MULTI_SZ || type == REG_SZ) && cb > 0) {
        existing.resize(cb / sizeof(wchar_t) + 2, 0);
        DWORD cb2 = static_cast<DWORD>(existing.size() * sizeof(wchar_t));
        if (RegQueryValueExW(key, kPendingValue, nullptr, &type,
                             reinterpret_cast<LPBYTE>(existing.data()), &cb2) != ERROR_SUCCESS) {
            existing.clear();
        } else {
            DWORD chars = cb2 / sizeof(wchar_t);
            while (chars > 0 && existing[chars - 1] == 0) chars--;
            existing.resize(chars);
        }
    }

    std::wstring entry = NtPrefixed(plain);
    std::vector<wchar_t> merged = existing;
    if (!merged.empty()) merged.push_back(0);
    merged.insert(merged.end(), entry.begin(), entry.end());
    merged.push_back(0);
    merged.push_back(0);
    merged.push_back(0);

    DWORD bytes = static_cast<DWORD>(merged.size() * sizeof(wchar_t));
    rc = RegSetValueExW(key, kPendingValue, 0, REG_MULTI_SZ,
                        reinterpret_cast<const BYTE*>(merged.data()), bytes);
    RegCloseKey(key);
    return static_cast<DWORD>(rc);
}

struct ScheduleStats {
    DWORD scheduled;
    DWORD failed;
    ScheduleStats() : scheduled(0), failed(0) {}
};

static void ScheduleTree(const std::wstring& ext, ScheduleStats* st, int depth) {
    if (depth > 512) {
        st->failed++;
        return;
    }
    DWORD attr = GetFileAttributesW(ext.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        st->failed++;
        return;
    }
    if ((attr & FILE_ATTRIBUTE_DIRECTORY) && !(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        std::wstring pattern = ext + L"\\*";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                ScheduleTree(ext + L"\\" + fd.cFileName, st, depth + 1);
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (MoveFileExW(ext.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        st->scheduled++;
        return;
    }
    if (AddPendingRename(dm::StripExtended(ext)) == ERROR_SUCCESS) st->scheduled++;
    else st->failed++;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_REBOOT, kId, L"DeleteEngine.Reboot",
                 L"Отложенное удаление: ставит в очередь MoveFileEx и PendingFileRenameOperations всю цель, включая содержимое каталогов. Хост после этого перезагружает машину.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions* opt, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (opt && opt->structSize == sizeof(DmOptions) && !(opt->flags & DM_FLAG_ALLOW_REBOOT)) return DM_STATUS_SKIP;
    if (score) *score = 15;
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
    if (!(o.flags & DM_FLAG_ALLOW_REBOOT)) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Отложенное удаление отключено флагом");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    if (!dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: очередь перезагрузки не изменялась");
        return DM_STATUS_SKIP;
    }

    dm::EnableStandardPrivileges();
    dm::EnablePrivilege(L"SeShutdownPrivilege");

    wchar_t vol[MAX_PATH] = {0};
    if (GetVolumePathNameW(plain.c_str(), vol, MAX_PATH)) {
        DWORD flags = 0;
        if (GetVolumeInformationW(vol, nullptr, 0, nullptr, nullptr, &flags, nullptr, 0) &&
            (flags & FILE_READ_ONLY_VOLUME)) {
            dm::FillResult(result, DM_STATUS_FAIL, ERROR_WRITE_PROTECT, kId,
                           L"Том только для чтения: удаление при загрузке невозможно, очередь не создавалась");
            return DM_STATUS_FAIL;
        }
    }
    ScheduleStats st;
    ScheduleTree(ext, &st, 0);

    if (st.scheduled == 0) {
        dm::FillResult(result, DM_STATUS_FAIL, ERROR_ACCESS_DENIED, kId,
                       L"Не удалось поставить объект в очередь удаления при загрузке (элементов: " +
                           std::to_wstring(st.failed) + L")");
        return DM_STATUS_FAIL;
    }

    std::wstring detail = L"В очередь на удаление при загрузке поставлено элементов: " +
                          std::to_wstring(st.scheduled);
    if (st.failed) detail += L", не удалось: " + std::to_wstring(st.failed);
    detail += L". Требуется перезагрузка";
    dm::FillResult(result, DM_STATUS_SOFTFAIL, 0, kId, detail);
    if (result) result->flagsOut = DM_RES_REBOOT_PENDING;
    return DM_STATUS_SOFTFAIL;
}
