#define _CRT_RAND_S
#include <cstdlib>
#include "dm_api.h"
#include "dm_util.h"

static const wchar_t* kId = L"staging";
static const int kStage = 65;
static const wchar_t* kStageDirName = L"$DeleteMaster.Stage";
static const unsigned long long kOverwriteCap = 1024ull * 1024ull;

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static unsigned int Rand32() {
    unsigned int v = 0;
    rand_s(&v);
    return v;
}

static std::wstring Base64Url(const std::vector<unsigned char>& in) {
    std::wstring out;
    size_t i = 0;
    while (i + 3 <= in.size()) {
        unsigned int n = (static_cast<unsigned int>(in[i]) << 16) |
                         (static_cast<unsigned int>(in[i + 1]) << 8) |
                         static_cast<unsigned int>(in[i + 2]);
        out.push_back(static_cast<wchar_t>(kB64[(n >> 18) & 63]));
        out.push_back(static_cast<wchar_t>(kB64[(n >> 12) & 63]));
        out.push_back(static_cast<wchar_t>(kB64[(n >> 6) & 63]));
        out.push_back(static_cast<wchar_t>(kB64[n & 63]));
        i += 3;
    }
    size_t rem = in.size() - i;
    if (rem == 1) {
        unsigned int n = static_cast<unsigned int>(in[i]) << 16;
        out.push_back(static_cast<wchar_t>(kB64[(n >> 18) & 63]));
        out.push_back(static_cast<wchar_t>(kB64[(n >> 12) & 63]));
    } else if (rem == 2) {
        unsigned int n = (static_cast<unsigned int>(in[i]) << 16) |
                         (static_cast<unsigned int>(in[i + 1]) << 8);
        out.push_back(static_cast<wchar_t>(kB64[(n >> 18) & 63]));
        out.push_back(static_cast<wchar_t>(kB64[(n >> 12) & 63]));
        out.push_back(static_cast<wchar_t>(kB64[(n >> 6) & 63]));
    }
    return out;
}

static std::vector<unsigned char> RandomBytes(size_t count) {
    std::vector<unsigned char> v(count);
    for (size_t i = 0; i < count; ++i) v[i] = static_cast<unsigned char>(Rand32() & 0xFF);
    return v;
}

static std::wstring RandomName() {
    std::wstring token = Base64Url(RandomBytes(18));
    std::wstring ext = Base64Url(RandomBytes(3));
    while (ext.size() > 3) ext.pop_back();
    while (ext.size() < 3) ext.push_back(static_cast<wchar_t>(kB64[Rand32() % 64]));
    return token + L"." + ext;
}

static std::wstring VolumeRootOf(const std::wstring& plain) {
    wchar_t buf[MAX_PATH] = {0};
    if (!GetVolumePathNameW(plain.c_str(), buf, MAX_PATH)) return std::wstring();
    return std::wstring(buf);
}

static bool EnsureStageDir(const std::wstring& root, std::wstring* stageDir) {
    std::wstring dir = root + kStageDirName;
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) return false;
    }
    SetFileAttributesW(dir.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY);
    if (stageDir) *stageDir = dir;
    return true;
}

static bool ResolveStageDir(const std::wstring& plain, std::wstring* stageDir, DWORD* errOut) {
    wchar_t env[DM_MAX_PATH] = {0};
    DWORD n = GetEnvironmentVariableW(L"DELETEMASTER_STAGE", env, DM_MAX_PATH);
    if (n > 0 && n < DM_MAX_PATH) {
        if (!CreateDirectoryW(env, nullptr)) {
            DWORD e = GetLastError();
            if (e != ERROR_ALREADY_EXISTS) {
                if (errOut) *errOut = e;
                return false;
            }
        }
        *stageDir = env;
        return true;
    }
    std::wstring root = VolumeRootOf(plain);
    if (root.empty()) {
        if (errOut) *errOut = ERROR_PATH_NOT_FOUND;
        return false;
    }
    if (!EnsureStageDir(root, stageDir)) {
        if (errOut) *errOut = GetLastError();
        return false;
    }
    return true;
}

static void PurgeStageDir(const std::wstring& stageDir) {
    std::wstring pattern = stageDir + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        SetFileAttributesW((stageDir + L"\\" + fd.cFileName).c_str(), FILE_ATTRIBUTE_NORMAL);
        dm::DeleteTree(stageDir + L"\\" + fd.cFileName, 2, 20, nullptr, 0);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

static bool ShrinkFile(const std::wstring& ext, unsigned __int64* freed) {
    dm::ClearAttributes(ext);
    unsigned __int64 size = dm::FileSizeOf(ext);
    HANDLE h = CreateFileW(ext.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    const DWORD chunk = 64 * 1024;
    std::vector<BYTE> rnd(chunk);
    for (DWORD i = 0; i < chunk; ++i) rnd[i] = static_cast<BYTE>(Rand32() & 0xFF);

    unsigned __int64 left = size < kOverwriteCap ? size : kOverwriteCap;
    LARGE_INTEGER zero{};
    SetFilePointerEx(h, zero, nullptr, FILE_BEGIN);
    while (left > 0) {
        DWORD want = static_cast<DWORD>(left < chunk ? left : chunk);
        DWORD wrote = 0;
        if (!WriteFile(h, rnd.data(), want, &wrote, nullptr) || wrote == 0) break;
        left -= wrote;
    }

    FILE_END_OF_FILE_INFO eof{};
    eof.EndOfFile.QuadPart = 0;
    BOOL ok = SetFileInformationByHandle(h, FileEndOfFileInfo, &eof, sizeof(eof));
    FlushFileBuffers(h);
    CloseHandle(h);
    if (ok && freed) *freed += size;
    return ok != FALSE;
}

static void ShrinkTree(const std::wstring& ext, DWORD* files, unsigned __int64* freed, int depth) {
    if (depth > 512) return;
    DWORD attr = GetFileAttributesW(ext.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return;
    dm::ClearAttributes(ext);
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        if (!(attr & FILE_ATTRIBUTE_REPARSE_POINT)) {
            std::wstring pattern = ext + L"\\*";
            WIN32_FIND_DATAW fd{};
            HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                    ShrinkTree(ext + L"\\" + fd.cFileName, files, freed, depth + 1);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        return;
    }
    if (ShrinkFile(ext, freed) && files) (*files)++;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_STAGING | DM_CAP_DELETE, kId, L"DeleteEngine.Staging",
                 L"Безвозвратное удаление через стейджинг: объект переносится в скрытую папку тома под случайным base64-именем, содержимое обнуляется и ужимается до 0 байт, затем стирается.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions* opt, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    std::wstring ext = dm::Extended(target);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) return DM_STATUS_SKIP;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) return DM_STATUS_SKIP;
    if (opt && opt->structSize == sizeof(DmOptions) && !(opt->flags & DM_FLAG_STAGING)) return DM_STATUS_SKIP;
    if (score) *score = 60;
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
    if (!(o.flags & DM_FLAG_STAGING)) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Стейджинг отключён флагом");
        return DM_STATUS_SKIP;
    }
    if (o.flags & DM_FLAG_DRY_RUN) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: стейджинг не выполнялся");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Reparse-точка не переносится: обрабатывается движком Reparse");
        return DM_STATUS_SKIP;
    }

    std::wstring root = VolumeRootOf(plain);
    if (root.empty()) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, ERROR_PATH_NOT_FOUND, kId,
                       L"Не удалось определить том для " + plain);
        return DM_STATUS_SOFTFAIL;
    }
    if (plain.size() + wcslen(kStageDirName) > 250) {
        dm::FillResult(result, DM_STATUS_SKIP, ERROR_FILENAME_EXCED_RANGE, kId,
                       L"Слишком длинный путь для стейджинга");
        return DM_STATUS_SKIP;
    }

    dm::EnableStandardPrivileges();
    std::wstring stageDir;
    DWORD dirErr = 0;
    if (!ResolveStageDir(plain, &stageDir, &dirErr)) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, dirErr, kId,
                       L"Не удалось создать папку стейджинга: " + dm::ErrText(dirErr));
        return DM_STATUS_SOFTFAIL;
    }
    PurgeStageDir(stageDir);

    std::wstring stageName;
    std::wstring dest;
    bool moved = false;
    for (int attempt = 0; attempt < 5 && !moved; ++attempt) {
        stageName = RandomName();
        dest = stageDir + L"\\" + stageName;
        if (MoveFileExW(ext.c_str(), dest.c_str(), 0)) {
            moved = true;
            break;
        }
        DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS && e != ERROR_FILE_EXISTS) {
            dm::FillResult(result, DM_STATUS_SOFTFAIL, e, kId,
                           L"Перенос в стейджинг не удался: " + dm::ErrText(e));
            return DM_STATUS_SOFTFAIL;
        }
    }
    if (!moved) {
        dm::FillResult(result, DM_STATUS_SOFTFAIL, ERROR_ALREADY_EXISTS, kId,
                       L"Не удалось подобрать свободное случайное имя в стейджинге");
        return DM_STATUS_SOFTFAIL;
    }

    DWORD files = 0;
    unsigned __int64 freed = 0;
    ShrinkTree(dest, &files, &freed, 0);

    dm::Counters c;
    DWORD err = dm::DeleteTree(dest, o.retryCount, o.retryDelayMs, &c, 0);
    RemoveDirectoryW(stageDir.c_str());

    std::wstring detail = L"Перенесено в " + stageDir + L"\\" + stageName +
                          L", обнулено файлов: " + std::to_wstring(files) +
                          L", освобождено: " + std::to_wstring(freed) + L" байт";
    if (err == ERROR_SUCCESS && !dm::Exists(dest)) {
        detail += L", стёрто безвозвратно";
        dm::FillResult(result, DM_STATUS_OK, 0, kId, detail);
        if (result) {
            result->filesRemoved = files ? files : 1;
            result->bytesFreed = freed;
            result->flagsOut = DM_CAP_STAGING;
        }
        return DM_STATUS_OK;
    }

    detail += L", но финальное удаление не прошло: " + dm::ErrText(err);
    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId, detail);
    if (result) {
        result->filesRemoved = files;
        result->bytesFreed = freed;
    }
    return DM_STATUS_SOFTFAIL;
}
