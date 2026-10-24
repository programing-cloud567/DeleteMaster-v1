#include "dm_api.h"
#include "dm_util.h"
#include "dm_locks.h"
#include "dm_nt.h"

static const wchar_t* kId = L"streams";
static const int kStage = 60;

struct StreamStats {
    DWORD removed;
    DWORD failed;
    unsigned __int64 bytes;
    StreamStats() : removed(0), failed(0), bytes(0) {}
};

static bool CleanStreams(const std::wstring& ext, StreamStats* st) {
    WIN32_FIND_STREAM_DATA data{};
    HANDLE h = FindFirstStreamW(ext.c_str(), FindStreamInfoStandard, &data, 0);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool any = false;
    do {
        std::wstring full = data.cStreamName;
        if (full.size() <= 1) continue;
        if (_wcsicmp(full.c_str(), L"::$DATA") == 0) continue;
        std::wstring sname = full.substr(1);
        size_t dollar = sname.find(L":$DATA");
        if (dollar != std::wstring::npos) sname = sname.substr(0, dollar);
        if (sname.empty()) continue;
        any = true;
        std::wstring tgt = ext + L":" + sname;
        dm::ClearAttributes(ext);
        BOOL ok = DeleteFileW(tgt.c_str());
        if (!ok) {
            LONG ns = 0;
            HANDLE sh = dm::NtOpenDelete(tgt, false, true, false, &ns);
            if (sh != INVALID_HANDLE_VALUE) {
                ok = dm::NtDeleteByHandle(sh) == ERROR_SUCCESS;
                CloseHandle(sh);
            }
        }
        if (ok) {
            st->removed++;
            st->bytes += static_cast<unsigned __int64>(data.StreamSize.QuadPart);
        } else {
            st->failed++;
        }
    } while (FindNextStreamW(h, &data));
    FindClose(h);
    return any;
}

static DWORD CleanHardlinks(const std::wstring& ext, const std::wstring& plain, DWORD* removed) {
    auto devmap = dm::DeviceMap();
    DWORD len = DM_MAX_PATH;
    std::vector<wchar_t> buf(DM_MAX_PATH);
    HANDLE h = FindFirstFileNameW(ext.c_str(), 0, &len, buf.data());
    if (h == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD err = ERROR_SUCCESS;
    do {
        std::wstring nt(buf.data(), len);
        std::wstring dos = dm::DeviceToDos(nt, devmap);
        if (_wcsicmp(dos.c_str(), plain.c_str()) != 0) {
            DWORD e = dm::NtDeletePath(dos, false, true, false);
            if (e == ERROR_SUCCESS) {
                (*removed)++;
            } else {
                err = e;
            }
        }
        len = DM_MAX_PATH;
    } while (FindNextFileNameW(h, &len, buf.data()));
    FindClose(h);
    return err;
}

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info) {
    if (!info || info->structSize != sizeof(DmEngineInfo)) return DM_STATUS_FAIL;
    dm::FillInfo(info, kStage, DM_CAP_STREAMS | DM_CAP_DELETE, kId, L"DeleteEngine.Streams",
                 L"Потоки и жёсткие ссылки NTFS: снимает альтернативные потоки данных (Zone.Identifier и прочие) и лишние имена файла, затем удаляет объект.");
    return DM_STATUS_OK;
}

DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions*, DWORD* score) {
    if (score) *score = 0;
    if (!target || !*target) return DM_STATUS_FAIL;
    std::wstring ext = dm::Extended(target);
    DWORD a = dm::AttrsOf(ext);
    if (a == INVALID_FILE_ATTRIBUTES) return DM_STATUS_SKIP;
    if (a & FILE_ATTRIBUTE_DIRECTORY) {
        if (score) *score = 5;
        return DM_STATUS_SKIP;
    }
    if (score) *score = 20;
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
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Пробный прогон: потоки не изменялись");
        return DM_STATUS_SKIP;
    }

    std::wstring plain = dm::FullPath(target);
    std::wstring ext = dm::Extended(plain);
    DWORD attr = dm::AttrsOf(ext);
    if (attr == INVALID_FILE_ATTRIBUTES) {
        dm::FillResult(result, DM_STATUS_OK, 0, kId, L"Объект уже отсутствует");
        return DM_STATUS_OK;
    }
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        dm::FillResult(result, DM_STATUS_SKIP, 0, kId, L"Каталог: альтернативные потоки не применимы");
        return DM_STATUS_SKIP;
    }

    std::wstring detail;
    StreamStats st;
    if (attr & FILE_ATTRIBUTE_REPARSE_POINT) {
        detail = L"Объект является reparse-точкой, потоки не сканировались; ";
    } else {
        bool any = CleanStreams(ext, &st);
        if (!any) detail = L"Альтернативных потоков не обнаружено; ";
        else detail = L"Потоков удалено: " + std::to_wstring(st.removed) +
                      (st.failed ? L" (не удалось: " + std::to_wstring(st.failed) + L")" : L"") + L"; ";

        DWORD links = 0;
        DWORD linkErr = CleanHardlinks(ext, plain, &links);
        if (linkErr == ERROR_SUCCESS && links == 0) detail += L"дополнительных имён нет; ";
        else if (linkErr == ERROR_SUCCESS) detail += L"снято жёстких ссылок: " + std::to_wstring(links) + L"; ";
        else detail += L"жёсткие ссылки: " + dm::ErrText(linkErr) + L"; ";
    }

    dm::Counters c;
    DWORD err = dm::DeleteTree(ext, o.retryCount, o.retryDelayMs, &c, 0);
    if (err == ERROR_SUCCESS || !dm::Exists(ext)) {
        detail += L"объект удалён";
        dm::FillResult(result, DM_STATUS_OK, 0, kId, detail);
        if (result) {
            result->filesRemoved = c.files + st.removed;
            result->dirsRemoved = c.dirs;
            result->bytesFreed = c.bytes + st.bytes;
        }
        return DM_STATUS_OK;
    }

    detail += L"удаление не прошло: " + dm::ErrText(err);
    dm::FillResult(result, DM_STATUS_SOFTFAIL, err, kId, detail);
    if (result) {
        result->filesRemoved = st.removed;
        result->bytesFreed = st.bytes;
    }
    return DM_STATUS_SOFTFAIL;
}
