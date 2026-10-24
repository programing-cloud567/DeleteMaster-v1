#include "dm_api.h"
#include "dm_util.h"
#include "dm_nt.h"

static const wchar_t* kId = L"native";
static const int kStage = 50;

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_NATIVE | DM_CAP_DELETE, kId, L"DeleteEngine.Native",
                 L"Нативный уровень: NtCreateFile + FileDispositionInformationEx с POSIX-семантикой, удаление при открытом дескрипторе.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 45;
    return DM_STATUS_OK;
}

static DWORD TryVariants(const std::wstring& plain, bool dir, bool link, std::wstring* used) {
    struct Variant {
        bool backup;
        bool noFollow;
        const wchar_t* name;
    };
    const Variant variants[] = {
        { false, link, L"обычный доступ" },
        { false, true, L"без перехода по reparse" },
        { true, link, L"backup intent" },
        { true, true, L"backup intent + без перехода" }
    };
    DWORD last = ERROR_ACCESS_DENIED;
    for (const Variant& v : variants) {
        DWORD err = dm::NtDeletePath(plain, dir, v.backup, v.noFollow);
        if (err == ERROR_SUCCESS) {
            if (used) *used = v.name;
            return ERROR_SUCCESS;
        }
        last = err;
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return ERROR_SUCCESS;
        if (err == ERROR_DIR_NOT_EMPTY) break;
    }
    return last;
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
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: нативные вызовы не выполнялись");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }

    dm::EnableStandardPrivileges();
    bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool isLink = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    std::wstring used;
    DWORD err = TryVariants(plain, isDir, isLink, &used);

    if (err == ERROR_SUCCESS) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId,
                       L"Posix-удаление на уровне ядра выполнено (" + used + L"): " + plain);
        if (result) result->filesRemoved = 1;
        return DM_STATUS_OK;
    }

    if (isDir && (err == ERROR_DIR_NOT_EMPTY || err == ERROR_ACCESS_DENIED)) {
        dm::Counters c;
        DWORD treeErr = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);
        if (treeErr == ERROR_SUCCESS || !dm::Exists(ext)) {
            dm::FillResult(result, DM_STATUS_OK, 0, kId,
                           L"Каталог очищен рекурсивно после нативного прохода. Файлов: " +
                               std::to_wstring(c.files));
            if (result) {
                result->filesRemoved = c.files;
                result->dirsRemoved = c.dirs;
                result->bytesFreed = c.bytes;
            }
            return DM_STATUS_OK;
        }
        err = treeErr;
    }

    std::wstring d;
    if (err == ERROR_ACCESS_DENIED) d = L"Ядро вернуло отказ в доступе: объект защищён дескриптором с монопольным доступом или ACL";
    else if (err == ERROR_SHARING_VIOLATION) d = L"Объект открыт без режима FILE_SHARE_DELETE, требуется снятие блокировки";
    else if (err == ERROR_DIR_NOT_EMPTY) d = L"Каталог не пуст и часть элементов не удаляется";
    else d = L"Нативное удаление не выполнено: " + dm::ErrText(err);

    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId, d);
    if (result) result->flagsOut = DM_CAP_NATIVE;
    return DM_STATUS_SOFTFAIL;
}
