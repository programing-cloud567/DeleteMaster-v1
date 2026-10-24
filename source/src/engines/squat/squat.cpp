#include "dm_api.h"
#include "dm_util.h"
#include <aclapi.h>

#pragma comment(lib, "Advapi32.lib")

static const wchar_t* kId = L"squat";
static const int kStage = 93;

static DWORD DenyRecreate(const std::wstring& ext) {
    PSID world = nullptr;
    SID_IDENTIFIER_AUTHORITY wa = SECURITY_WORLD_SID_AUTHORITY;
    if (!AllocateAndInitializeSid(&wa, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world))
        return GetLastError();
    DWORD sidLen = GetLengthSid(world);
    DWORD aceSize = static_cast<DWORD>(sizeof(ACCESS_DENIED_ACE) - sizeof(DWORD));
    DWORD aclSize = static_cast<DWORD>(sizeof(ACL)) + aceSize + sidLen;
    std::vector<BYTE> store(aclSize);
    PACL acl = reinterpret_cast<PACL>(store.data());
    DWORD err = ERROR_SUCCESS;
    if (!InitializeAcl(acl, aclSize, ACL_REVISION)) err = GetLastError();
    else if (!AddAccessDeniedAce(acl, ACL_REVISION, DELETE | FILE_WRITE_DATA | FILE_APPEND_DATA, world)) err = GetLastError();
    else err = SetNamedSecurityInfoW(const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
                                     DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                     nullptr, nullptr, acl, nullptr);
    FreeSid(world);
    return err;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_SQUAT, kId, L"DeleteEngine.Squat",
                 L"Захват имени после удаления: на месте объекта создаётся файл-заглушка с запретом записи и удаления, чтобы процессы не могли воссоздать папку.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions* opt, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (opt && opt->structSize == sizeof(DmOptions) && !(opt->flags & DM_FLAG_SQUAT)) return DM_STATUS_SKIP;
    if (dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 80;
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
    if (!(o.flags & DM_FLAG_SQUAT)) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Захват имени отключён флагом /squat");
        return DM_STATUS_SKIP;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: заглушка не создавалась");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    if (dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Объект ещё существует, захватывать имя нечего");
        return DM_STATUS_SKIP;
    }

    HANDLE h = CreateFileW(ext.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        dm::FillResult(result, DM_STATUS_SOFTFAIL, e, kId,
                       L"Не удалось создать заглушку: " + dm::ErrText(e));
        return DM_STATUS_SOFTFAIL;
    }
    const char text[] = "DeleteMaster: name held";
    DWORD written = 0;
    WriteFile(h, text, sizeof(text) - 1, &written, nullptr);
    CloseHandle(h);

    DWORD err = DenyRecreate(ext);
    SetFileAttributesW(ext.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_READONLY);

    std::wstring detail = L"Имя захвачено заглушкой: " + plain;
    if (err != ERROR_SUCCESS) detail += L" (ACL-запрет не встал: " + dm::ErrText(err) + L")";
    dm::FillResult(result, DM_STATUS_OK, 0, kId, detail);
    return DM_STATUS_OK;
}
