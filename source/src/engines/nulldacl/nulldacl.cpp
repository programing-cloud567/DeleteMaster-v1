#include "dm_api.h"
#include "dm_util.h"
#include <aclapi.h>

#pragma comment(lib, "Advapi32.lib")

static const wchar_t* kId = L"nulldacl";
static const int kStage = 41;

static DWORD TakeOwner(const std::wstring& ext) {
    dm::EnablePrivilege(L"SeTakeOwnershipPrivilege");
    dm::EnablePrivilege(L"SeRestorePrivilege");
    HANDLE tok = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return GetLastError();
    DWORD needed = 0;
    GetTokenInformation(tok, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> buf(needed ? needed : 256);
    if (!GetTokenInformation(tok, TokenUser, buf.data(), static_cast<DWORD>(buf.size()), &needed)) {
        DWORD e = GetLastError();
        CloseHandle(tok);
        return e;
    }
    CloseHandle(tok);
    PSID sid = reinterpret_cast<TOKEN_USER*>(buf.data())->User.Sid;
    return SetNamedSecurityInfoW(const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
                                 OWNER_SECURITY_INFORMATION, sid, nullptr, nullptr, nullptr);
}

static DWORD SetNullDacl(const std::wstring& ext) {
    return SetNamedSecurityInfoW(const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
                                 DACL_SECURITY_INFORMATION | UNPROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, nullptr, nullptr);
}

static DWORD LowerLabel(const std::wstring& ext) {
    PSID medium = nullptr;
    SID_IDENTIFIER_AUTHORITY ml = SECURITY_MANDATORY_LABEL_AUTHORITY;
    if (!AllocateAndInitializeSid(&ml, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &medium))
        return GetLastError();
    DWORD sidLen = GetLengthSid(medium);
    DWORD size = static_cast<DWORD>(sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + sidLen);
    std::vector<BYTE> store(size);
    PACL sacl = reinterpret_cast<PACL>(store.data());
    DWORD err = ERROR_SUCCESS;
    if (!InitializeAcl(sacl, size, ACL_REVISION)) err = GetLastError();
    else if (!AddMandatoryAce(sacl, ACL_REVISION, 0, 0, medium)) err = GetLastError();
    else err = SetNamedSecurityInfoW(const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
                                     LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl);
    FreeSid(medium);
    return err;
}

static void ApplyNull(const std::wstring& ext, DWORD* granted, DWORD* denied, int depth) {
    if (depth > 512) return;
    DWORD a = TakeOwner(ext);
    DWORD b = SetNullDacl(ext);
    if (a == ERROR_SUCCESS || b == ERROR_SUCCESS) (*granted)++;
    else (*denied)++;
    LowerLabel(ext);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return;
    std::wstring pattern = ext + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        ApplyNull(ext + L"\\" + fd.cFileName, granted, denied, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_ACL | DM_CAP_DELETE, kId, L"DeleteEngine.NullDacl",
                 L"Максимум прав: смена владельца, NULL DACL (доступ всем), снятие метки целостности и SACL по всему дереву, затем удаление.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 70;
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmExecute(const wchar_t* target, const DmOptions* opt, DmResult* result) {
    DmOptions o{};
    if (opt) o = *opt;
    if (o.structSize != sizeof(DmOptions)) {
        o.flags = DM_FLAG_DEFAULT;
        o.retryCount = 3;
        o.retryDelayMs = 40;
        o.killTimeoutMs = 2000;
    }
    if (!target || !*target) {
        dm::FillResult(result, DM_STATUS_FAIL, ERROR_INVALID_PARAMETER, kId, L"Пустой путь");
        return DM_STATUS_FAIL;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: права не менялись");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    if (!dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }

    dm::EnableStandardPrivileges();
    DWORD granted = 0, denied = 0;
    ApplyNull(ext, &granted, &denied, 0);

    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);
    if (result) {
        result->filesRemoved = c.files;
        result->dirsRemoved = c.dirs;
        result->bytesFreed = c.bytes;
    }

    std::wstring head = L"NULL DACL выставлен на " + std::to_wstring(granted) + L" объектах";
    if (denied) head += L", не удалось на " + std::to_wstring(denied);

    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, head + L". Объект удалён");
        return DM_STATUS_OK;
    }
    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId,
                   head + L". Удаление не прошло: " + dm::ErrText(err));
    return DM_STATUS_SOFTFAIL;
}
