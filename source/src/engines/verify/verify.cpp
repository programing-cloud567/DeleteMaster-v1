#include "dm_api.h"
#include "dm_util.h"
#include "dm_nt.h"

static const wchar_t* kId = L"verify";
static const int kStage = 90;

static bool PendingDelete(const std::wstring& plain) {
    LONG st = 0;
    HANDLE h = dm::NtOpenDelete(plain, false, false, false, &st);
    if (h == INVALID_HANDLE_VALUE) return st == DM_STATUS_DELETE_PENDING;
    CloseHandle(h);
    return false;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_VERIFY, kId, L"DeleteEngine.Verify",
                 L"Контроль результата: проверяет, что объекта больше нет, различает отложенное удаление и остаточную блокировку.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 1;
    if (!target || !*target) return DM_STATUS_FAIL;
    return DM_STATUS_OK;
}

static bool StageLeftovers(const std::wstring& plain, std::wstring* first, DWORD* count) {
    *count = 0;
    wchar_t vol[MAX_PATH] = {0};
    if (!GetVolumePathNameW(plain.c_str(), vol, MAX_PATH)) return false;
    std::wstring stage = std::wstring(vol) + L"$DeleteMaster.Stage";
    std::wstring pattern = stage + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (*count == 0) *first = stage + L"\\" + fd.cFileName;
        (*count)++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return *count > 0;
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

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        std::wstring left;
        DWORD leftCount = 0;
        if (StageLeftovers(plain, &left, &leftCount)) {
            dm::FillResult(result, DM_STATUS_SOFTFAIL, 0, kId,
                           L"Исходный путь свободен, но в стейджинге осталось объектов: " +
                               std::to_wstring(leftCount) + L" (" + left +
                               L"). Данные не стёрты, а только перенесены");
            return DM_STATUS_SOFTFAIL;
        }
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Подтверждено: объекта нет на диске");
        if (result) result->flagsOut = DM_RES_VERIFIED;
        return DM_STATUS_OK;
    }

    DWORD err = ERROR_ACCESS_DENIED;
    bool opened = false;
    if (((attr & FILE_ATTRIBUTE_DIRECTORY) ? true : false) == false) {
        HANDLE h = CreateFileW(ext.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            opened = true;
            CloseHandle(h);
        } else {
            err = GetLastError();
        }
    } else {
        HANDLE h = CreateFileW(ext.c_str(), FILE_READ_ATTRIBUTES,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            opened = true;
            CloseHandle(h);
        } else {
            err = GetLastError();
        }
    }

    if (PendingDelete(plain) || err == ERROR_ACCESS_DENIED) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId,
                       L"Объект ещё виден в пространстве имён, но помечен на удаление или удерживается дескриптором");
        return DM_STATUS_SOFTFAIL;
    }

    std::wstring d = opened ? L"Объект остался на диске" : L"Объект остался на диске и не открывается: " + dm::ErrText(err);
    if (attr & FILE_ATTRIBUTE_READONLY) d += L" (readonly)";
    if (attr & FILE_ATTRIBUTE_SYSTEM) d += L" (system)";
    dm::FillResult(result, DM_STATUS_FAIL, 0, kId, d);
    return DM_STATUS_FAIL;
}
