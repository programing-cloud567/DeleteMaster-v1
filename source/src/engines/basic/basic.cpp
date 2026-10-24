#include "dm_api.h"
#include "dm_util.h"

static const wchar_t* kId = L"basic";
static const int kStage = 10;

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_DELETE, kId, L"DeleteEngine.Basic",
                 L"Базовое удаление: нормализация пути, снятие атрибутов, рекурсивный обход каталога, повторные попытки при занятости.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) {
        if (score) *score = 100;
        return DM_STATUS_SKIP;
    }
    if (score) *score = 10;
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

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    if (!dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует: " + plain);
        return DM_STATUS_OK;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон, удаление не выполнялось: " + plain);
        return DM_STATUS_SKIP;
    }

    DWORD attr = dm::AttrsOf(ext);
    bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool isLink = (attr & FILE_ATTRIBUTE_REPARSE_POINT) != 0;

    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);

    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        std::wstring d = isDir ? L"Каталог удалён" : L"Файл удалён";
        if (isLink) d += L" (как ссылка, содержимое цели не затрагивалось)";
        d += L". Файлов: " + std::to_wstring(c.files) + L", каталогов: " + std::to_wstring(c.dirs);
        dm::FillResult(result, DM_STATUS_OK, 0, kId, d);
        if (result) {
            result->filesRemoved = c.files;
            result->dirsRemoved = c.dirs;
            result->bytesFreed = c.bytes;
        }
        return DM_STATUS_OK;
    }

    std::wstring why;
    if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) why = L"объект занят другим процессом";
    else if (err == ERROR_ACCESS_DENIED) why = L"отказано в доступе";
    else if (err == ERROR_DIR_NOT_EMPTY) why = L"каталог не пуст, часть содержимого удалить не удалось";
    else if (err == ERROR_WRITE_PROTECT) why = L"носитель защищён от записи";
    else why = dm::ErrText(err);

    dm::FillResult(result, DM_STATUS_FAIL, err, kId, L"Не удалось удалить (" + why + L"): " + plain);
    if (result) {
        result->filesRemoved = c.files;
        result->dirsRemoved = c.dirs;
        result->bytesFreed = c.bytes;
        result->flagsOut = DM_CAP_DELETE;
    }
    return DM_STATUS_FAIL;
}
