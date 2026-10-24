#include "dm_api.h"
#include "dm_util.h"
#include "dm_nt.h"

static const wchar_t* kId = L"reparse";
static const int kStage = 70;

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_REPARSE | DM_CAP_DELETE, kId, L"DeleteEngine.Reparse",
                 L"Точки монтирования, символьные ссылки, облачные плейсхолдеры и дедупликация: удаляет саму ссылку, не заходя в её цель.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    std::wstring ext = dm::Extended(target);
    DWORD a = dm::AttrsOf(ext);
    if (a == INVALID_FILE_ATTRIBUTES) return DM_STATUS_SKIP;
    if (!(a & FILE_ATTRIBUTE_REPARSE_POINT)) return DM_STATUS_SKIP;
    if (score) *score = 90;
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
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }
    if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Объект не является reparse-точкой");
        return DM_STATUS_SKIP;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: ссылка не удалялась");
        return DM_STATUS_SKIP;
    }

    std::wstring kind = dm::DescribeReparse(ext);
    bool isDir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
    std::wstring detail = L"Тип: " + kind + L"; ";

    DWORD err = ERROR_SUCCESS;
    if (isDir) {
        dm::ClearAttributes(ext);
        if (RemoveDirectoryW(ext.c_str())) err = ERROR_SUCCESS;
        else err = GetLastError();
    } else {
        dm::ClearAttributes(ext);
        if (DeleteFileW(ext.c_str())) err = ERROR_SUCCESS;
        else err = GetLastError();
    }

    if (err != ERROR_SUCCESS) {
        err = dm::NtDeletePath(plain, isDir, false, true);
    }
    if (err != ERROR_SUCCESS) {
        err = dm::NtDeletePath(plain, isDir, true, true);
    }

    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        detail += isDir ? L"ссылка-каталог снята без обхода цели" : L"ссылка-файл снят без обхода цели";
        dm::FillResult(result, DM_STATUS_OK, 0, kId, detail);
        if (result) {
            result->filesRemoved = isDir ? 0 : 1;
            result->dirsRemoved = isDir ? 1 : 0;
        }
        return DM_STATUS_OK;
    }

    detail += L"снять точку не удалось: " + dm::ErrText(err);
    if (attr & (FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS | FILE_ATTRIBUTE_OFFLINE)) {
        detail += L"; объект принадлежит облачной синхронизации, попробуйте движок Reboot";
    }
    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId, detail);
    return DM_STATUS_SOFTFAIL;
}
