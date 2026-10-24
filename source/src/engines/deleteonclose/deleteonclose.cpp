#include "dm_api.h"
#include "dm_util.h"

static const wchar_t* kId = L"deleteonclose";
static const int kStage = 52;

struct TryResult {
    DWORD err;
    bool deleted;
};

static TryResult TryDeleteOnClose(const std::wstring& path, DWORD extraFlags) {
    TryResult r{ ERROR_SUCCESS, false };
    DWORD access = DELETE | SYNCHRONIZE | FILE_READ_ATTRIBUTES;
    DWORD flags = FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_BACKUP_SEMANTICS | extraFlags;
    HANDLE h = CreateFileW(path.c_str(), access,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.err = GetLastError();
        return r;
    }
    BOOL ok = CloseHandle(h);
    r.err = ok ? ERROR_SUCCESS : GetLastError();
    r.deleted = ok != FALSE;
    return r;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_DELETE, kId, L"DeleteEngine.DeleteOnClose",
                 L"Удаление через FILE_FLAG_DELETE_ON_CLOSE: объект помечается на удаление при закрытии последнего дескриптора. Проходит там, где DeleteFileW отказывает.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 55;
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
    bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool isLink = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    dm::EnableStandardPrivileges();
    dm::ClearAttributes(ext);

    const DWORD variants[] = { 0, FILE_FLAG_OPEN_REPARSE_POINT };
    DWORD lastErr = ERROR_SUCCESS;
    for (DWORD v : variants) {
        if (!isLink && v == FILE_FLAG_OPEN_REPARSE_POINT) continue;
        TryResult tr = TryDeleteOnClose(ext, v);
        lastErr = tr.err;
        if (tr.deleted || !dm::Exists(ext)) {
            dm::FillResult(result, DM_STATUS_OK, 0, kId,
                           L"Объект помечен на удаление при закрытии и снят");
            if (result) result->filesRemoved = isDir ? 0 : 1;
            return DM_STATUS_OK;
        }
    }

    if (isDir && lastErr == ERROR_ACCESS_DENIED && dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, lastErr, kId,
                       L"Каталог не пуст или открыт: delete-on-close применим только к пустым каталогам и файлам");
        return DM_STATUS_SOFTFAIL;
    }

    dm::FillResult(result, DM_STATUS_SOFTFAIL, lastErr, kId,
                   L"Delete-on-close не сработал: " + dm::ErrText(lastErr));
    return DM_STATUS_SOFTFAIL;
}
