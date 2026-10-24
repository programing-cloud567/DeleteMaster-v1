#include "dm_api.h"
#include "dm_util.h"
#include "dm_locks.h"

static const wchar_t* kId = L"scan";
static const int kStage = 15;

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_SCAN, kId, L"DeleteEngine.Scan",
                 L"Разведка: Restart Manager и сканирование таблицы дескрипторов ядра. Показывает, кто держит объект, ничего не удаляет.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    std::wstring ext = dm::Extended(target);
    if (!dm::Exists(ext)) return DM_STATUS_SKIP;
    if (score) *score = 5;
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmExecute(const wchar_t* target, const DmOptions* opt, DmResult* result) {
    DmOptions o{};
    if (opt) o = *opt;
    if (!target || !*target) {
        dm::FillResult(result, DM_STATUS_FAIL, ERROR_INVALID_PARAMETER, kId, L"Пустой путь");
        return DM_STATUS_FAIL;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Объект отсутствует, разведка не требуется");
        return DM_STATUS_SKIP;
    }

    dm::EnableStandardPrivileges();
    uint64_t size = dm::FileSizeOf(ext);
    std::vector<dm::Holder> holders = dm::FindHolders(plain);

    std::wstring d = L"Атрибуты: 0x";
    wchar_t hex[32];
    swprintf_s(hex, L"%08lX", static_cast<unsigned long>(attr));
    d += hex;
    if (attr & FILE_ATTRIBUTE_READONLY) d += L" readonly";
    if (attr & FILE_ATTRIBUTE_HIDDEN) d += L" hidden";
    if (attr & FILE_ATTRIBUTE_SYSTEM) d += L" system";
    if (attr & FILE_ATTRIBUTE_DIRECTORY) d += L" dir";
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) d += L" reparse";
    if (attr & FILE_ATTRIBUTE_OFFLINE) d += L" offline";
    if (attr & FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS) d += L" cloud";
    d += L"; размер: " + std::to_wstring(size) + L" байт";

    if (holders.empty()) {
        d += L"; дескрипторов на объект не найдено";
    } else {
        d += L"; держат: ";
        d += dm::FormatHolders(holders);
        DWORD critical = 0;
        for (const auto& h : holders) if (dm::IsCriticalProcess(h.pid, h.image)) critical++;
        if (critical) d += L"; из них системных: " + std::to_wstring(critical);
    }

    if (dm::IsReparsePoint(ext)) {
        d += L"; reparse: ";
        d += dm::DescribeReparse(ext);
    }

    dm::FillResult(result, DM_STATUS_SKIP, 0, kId, d);
    if (result) {
        result->bytesFreed = size;
        result->flagsOut = holders.empty() ? 0 : 1;
    }
    return DM_STATUS_SKIP;
}
