#include "dm_api.h"
#include "dm_util.h"

static const wchar_t* kId = L"globalroot";
static const int kStage = 54;

struct DmDispositionEx {
    ULONG Flags;
};

static bool ToDevicePath(const std::wstring& plain, std::wstring* out) {
    wchar_t vol[MAX_PATH] = {0};
    if (!GetVolumePathNameW(plain.c_str(), vol, MAX_PATH)) return false;
    std::wstring root(vol);
    if (root.size() < 3) return false;
    std::wstring letter = root.substr(0, 2);
    wchar_t dev[2048] = {0};
    if (!QueryDosDeviceW(letter.c_str(), dev, 2048)) return false;
    std::wstring rest = plain.substr(root.size());
    if (rest.empty()) return false;
    *out = std::wstring(L"\\\\?\\GLOBALROOT") + dev + L"\\" + rest;
    return true;
}

static DWORD DeleteViaDevice(const std::wstring& devPath) {
    HANDLE h = CreateFileW(devPath.c_str(), DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return GetLastError();

    DmDispositionEx ex{};
    ex.Flags = 0x1 | 0x2 | 0x4 | 0x10;
    BOOL ok = SetFileInformationByHandle(h, static_cast<FILE_INFO_BY_HANDLE_CLASS>(21), &ex, sizeof(ex));
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    if (!ok) {
        FILE_DISPOSITION_INFO legacy{};
        legacy.DeleteFile = TRUE;
        ok = SetFileInformationByHandle(h, FileDispositionInfo, &legacy, sizeof(legacy));
        err = ok ? ERROR_SUCCESS : GetLastError();
    }
    CloseHandle(h);
    return err;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_DELETE | DM_CAP_NATIVE, kId, L"DeleteEngine.GlobalRoot",
                 L"Удаление через \\\\?\\GLOBALROOT\\Device\\HarddiskVolumeN: обход Win32-слоя путей и части проверок на длину и разбор имени.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 58;
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
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: удаление не выполнялось");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }

    std::wstring devPath;
    if (!ToDevicePath(plain, &devPath)) {
        dm::FillResult(result, DM_STATUS_SKIP, ERROR_PATH_NOT_FOUND, kId,
                       L"Не удалось построить device-путь для " + plain);
        return DM_STATUS_SKIP;
    }

    dm::EnableStandardPrivileges();
    dm::ClearAttributes(ext);
    DWORD err = DeleteViaDevice(devPath);

    if (err == ERROR_SUCCESS && !dm::Exists(ext)) {
        bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
        dm::FillResult(result, DM_STATUS_OK, 0, kId,
                       L"Удалено через device-путь: " + devPath);
        if (result) {
            if (isDir) result->dirsRemoved = 1;
            else result->filesRemoved = 1;
        }
        return DM_STATUS_OK;
    }

    if (err == ERROR_SUCCESS) err = ERROR_ACCESS_DENIED;
    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId,
                   L"Device-путь не помог: " + dm::ErrText(err) + L" (" + devPath + L")");
    return DM_STATUS_SOFTFAIL;
}
