#include "dm_api.h"
#include "dm_util.h"
#include "dm_nt.h"

static const wchar_t* kId = L"privilege";
static const int kStage = 30;

struct PrivState {
    bool debug;
    bool takeOwner;
    bool restore;
    bool backup;
};

static PrivState EnableAll() {
    PrivState s{};
    s.debug = dm::EnablePrivilege(L"SeDebugPrivilege");
    s.takeOwner = dm::EnablePrivilege(L"SeTakeOwnershipPrivilege");
    s.restore = dm::EnablePrivilege(L"SeRestorePrivilege");
    s.backup = dm::EnablePrivilege(L"SeBackupPrivilege");
    return s;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_PRIVILEGE | DM_CAP_DELETE, kId, L"DeleteEngine.Privilege",
                 L"Привилегии: включает SeDebug/SeRestore/SeBackup/SeTakeOwnership и удаляет через открытие с backup intent, обходя проверку DACL.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 30;
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
    if (!dm::IsElevated()) {
        dm::FillResult(result, DM_STATUS_SKIP, ERROR_ELEVATION_REQUIRED, kId,
                       L"Процесс не elevated: привилегии недоступны");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    if (!dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: привилегии не применялись");
        return DM_STATUS_SKIP;
    }

    PrivState priv = EnableAll();
    bool isDir = dm::IsDirectory(ext);
    bool isLink = dm::IsReparsePoint(ext);

    DWORD firstErr = ERROR_SUCCESS;
    if (!isLink) {
        dm::ClearAttributes(ext);
        firstErr = dm::NtDeletePath(plain, isDir, true, isDir);
        if (firstErr == ERROR_SUCCESS) {
            dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Удалено открытием с backup intent (обход DACL): " + plain);
            if (result) result->filesRemoved = 1;
            return DM_STATUS_OK;
        }
    }

    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);
    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        std::wstring d = L"Удалено с расширенными привилегиями";
        if (isLink) d += L" (как ссылка)";
        d += L". Файлов: " + std::to_wstring(c.files) + L", каталогов: " + std::to_wstring(c.dirs);
        dm::FillResult(result, DM_STATUS_OK, 0, kId, d);
        if (result) {
            result->filesRemoved = c.files;
            result->dirsRemoved = c.dirs;
            result->bytesFreed = c.bytes;
        }
        return DM_STATUS_OK;
    }

    std::wstring d = L"Привилегии включены (debug=";
    d += priv.debug ? L"да" : L"нет";
    d += L", restore=";
    d += priv.restore ? L"да" : L"нет";
    d += L", backup=";
    d += priv.backup ? L"да" : L"нет";
    d += L", takeOwnership=";
    d += priv.takeOwner ? L"да" : L"нет";
    d += L"), но удаление не прошло: " + dm::ErrText(err == ERROR_SUCCESS ? firstErr : err);
    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId, d);
    if (result) {
        result->filesRemoved = c.files;
        result->dirsRemoved = c.dirs;
    }
    return DM_STATUS_SOFTFAIL;
}
