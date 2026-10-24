#include "dm_api.h"
#include "dm_util.h"
#include <aclapi.h>
#include <sddl.h>

#pragma comment(lib, "Advapi32.lib")

static const wchar_t* kId = L"acl";
static const int kStage = 40;

static DWORD TakeOwnership(const std::wstring& ext) {
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

static DWORD ForceAllowDacl(const std::wstring& ext) {
    PSID world = nullptr;
    PSID admins = nullptr;
    SID_IDENTIFIER_AUTHORITY worldAuth = SECURITY_WORLD_SID_AUTHORITY;
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    DWORD err = ERROR_SUCCESS;
    do {
        if (!AllocateAndInitializeSid(&worldAuth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &world)) {
            err = GetLastError();
            break;
        }
        if (!AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
                                      0, 0, 0, 0, 0, 0, &admins)) {
            err = GetLastError();
            break;
        }
        DWORD worldLen = GetLengthSid(world);
        DWORD admLen = GetLengthSid(admins);
        DWORD aceSize = static_cast<DWORD>(sizeof(ACCESS_ALLOWED_ACE) - sizeof(DWORD));
        DWORD aclSize = static_cast<DWORD>(sizeof(ACL)) + (aceSize + worldLen) + (aceSize + admLen);
        std::vector<BYTE> store(aclSize);
        PACL acl = reinterpret_cast<PACL>(store.data());
        if (!InitializeAcl(acl, aclSize, ACL_REVISION)) {
            err = GetLastError();
            break;
        }
        if (!AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, world)) {
            err = GetLastError();
            break;
        }
        if (!AddAccessAllowedAce(acl, ACL_REVISION, FILE_ALL_ACCESS, admins)) {
            err = GetLastError();
            break;
        }
        err = SetNamedSecurityInfoW(const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
                                    DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                    nullptr, nullptr, acl, nullptr);
    } while (false);
    if (world) FreeSid(world);
    if (admins) FreeSid(admins);
    return err;
}

static DWORD DropIntegrityLabel(const std::wstring& ext) {
    PSID medium = nullptr;
    SID_IDENTIFIER_AUTHORITY mlAuth = SECURITY_MANDATORY_LABEL_AUTHORITY;
    if (!AllocateAndInitializeSid(&mlAuth, 1, SECURITY_MANDATORY_MEDIUM_RID, 0, 0, 0, 0, 0, 0, 0, &medium))
        return GetLastError();
    DWORD sidLen = GetLengthSid(medium);
    DWORD saclSize = static_cast<DWORD>(sizeof(ACL) + sizeof(SYSTEM_MANDATORY_LABEL_ACE) - sizeof(DWORD) + sidLen);
    std::vector<BYTE> store(saclSize);
    PACL sacl = reinterpret_cast<PACL>(store.data());
    DWORD err = ERROR_SUCCESS;
    if (!InitializeAcl(sacl, saclSize, ACL_REVISION)) err = GetLastError();
    else if (!AddMandatoryAce(sacl, ACL_REVISION, 0, 0, medium)) err = GetLastError();
    else err = SetNamedSecurityInfoW(const_cast<LPWSTR>(ext.c_str()), SE_FILE_OBJECT,
                                     LABEL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, sacl);
    FreeSid(medium);
    return err;
}

static void ApplyRecursive(const std::wstring& ext, DWORD* granted, DWORD* denied, int depth) {
    if (depth > 512) return;
    DWORD e1 = TakeOwnership(ext);
    DWORD e2 = ForceAllowDacl(ext);
    if (e1 == ERROR_SUCCESS || e2 == ERROR_SUCCESS) (*granted)++;
    else (*denied)++;
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    if (!(attr & FILE_ATTRIBUTE_DIRECTORY)) return;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return;
    DropIntegrityLabel(ext);
    std::wstring pattern = ext + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        ApplyRecursive(ext + L"\\" + fd.cFileName, granted, denied, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_ACL | DM_CAP_DELETE, kId, L"DeleteEngine.Acl",
                 L"Права доступа: смена владельца, перезапись DACL с полным доступом, снятие метки целостности. Работает при отказе в доступе.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    if (!dm::Exists(dm::Extended(target))) return DM_STATUS_SKIP;
    if (score) *score = 35;
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
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: ACL не изменялись");
        return DM_STATUS_SKIP;
    }
    if (!dm::IsElevated()) {
        dm::FillResult(result, DM_STATUS_SKIP, ERROR_ELEVATION_REQUIRED, kId, L"Нужны права администратора");
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
    ApplyRecursive(ext, &granted, &denied, 0);

    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);
    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        std::wstring d = L"Владелец сменён, DACL перезаписан";
        if (denied) d += L", без прав осталось элементов: " + std::to_wstring(denied);
        d += L". Удалено файлов: " + std::to_wstring(c.files) + L", каталогов: " + std::to_wstring(c.dirs);
        dm::FillResult(result, DM_STATUS_OK, 0, kId, d);
        if (result) {
            result->filesRemoved = c.files;
            result->dirsRemoved = c.dirs;
            result->bytesFreed = c.bytes;
        }
        return DM_STATUS_OK;
    }

    if (err == ERROR_INVALID_FUNCTION || err == ERROR_NOT_SUPPORTED) {
        dm::FillResult(result, DM_STATUS_SKIP, err, kId,
                       L"Файловая система не поддерживает ACL (FAT/exFAT), движок неприменим");
        return DM_STATUS_SKIP;
    }

    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId,
                   L"Права перезаписаны, но удаление не прошло: " + dm::ErrText(err));
    if (result) {
        result->filesRemoved = c.files;
        result->dirsRemoved = c.dirs;
    }
    return DM_STATUS_SOFTFAIL;
}
