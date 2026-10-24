#include "dm_api.h"
#include "dm_util.h"

#include <shellapi.h>
#include <taskschd.h>
#include <fcntl.h>
#include <io.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>

#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Advapi32.lib")
#pragma comment(lib, "User32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Taskschd.lib")

static const wchar_t* kHostVersion = L"1.4.0-SERVICE";

template <class T>
struct ComPtr {
    T* ptr;
    ComPtr() : ptr(nullptr) {}
    ~ComPtr() { if (ptr) ptr->Release(); }
    T** operator&() { return &ptr; }
    T* operator->() const { return ptr; }
    operator T*() const { return ptr; }
    T* Get() const { return ptr; }
};

static std::wstring HrText(HRESULT hr) {
    wchar_t* buf = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                 FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, static_cast<DWORD>(hr),
                             MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                             reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring out;
    if (n && buf) {
        out.assign(buf, n);
        while (!out.empty() && (out.back() == L'\r' || out.back() == L'\n' || out.back() == L' ')) out.pop_back();
    }
    if (buf) LocalFree(buf);
    wchar_t code[32];
    swprintf_s(code, L"0x%08lX", static_cast<unsigned long>(hr));
    if (out.empty()) out = L"ошибка";
    out += L" (";
    out += code;
    out += L")";
    return out;
}

static std::wstring RawParams() {
    const wchar_t* cl = GetCommandLineW();
    if (!cl) return std::wstring();
    if (*cl == L'"') {
        const wchar_t* end = wcschr(cl + 1, L'"');
        if (end) {
            const wchar_t* p = end + 1;
            while (*p == L' ') ++p;
            return std::wstring(p);
        }
    }
    const wchar_t* sp = wcschr(cl, L' ');
    if (sp) {
        while (*sp == L' ') ++sp;
        return std::wstring(sp);
    }
    return std::wstring();
}

struct Logger {
    FILE* file;
    bool quiet;
    bool console;
    std::wstring logPath;
    Logger() : file(nullptr), quiet(false), console(true) {}

    void Open(const std::wstring& dir) {
        CreateDirectoryW(dir.c_str(), nullptr);
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf_s(name, L"delete-%04d%02d%02d-%02d%02d%02d-%lu.log", st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, static_cast<unsigned long>(GetCurrentProcessId()));
        logPath = dir + L"\\" + name;
        if (_wfopen_s(&file, logPath.c_str(), L"wb") == 0 && file) {
            const unsigned char bom[3] = { 0xEF, 0xBB, 0xBF };
            fwrite(bom, 1, 3, file);
            fflush(file);
        }
    }

    void Close() {
        if (file) {
            fclose(file);
            file = nullptr;
        }
    }

    void Emit(const std::wstring& text) {
        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t ts[32];
        swprintf_s(ts, L"%02d:%02d:%02d", st.wHour, st.wMinute, st.wSecond);
        std::wstring line = std::wstring(L"[") + ts + L"] " + text;
        if (console && !quiet) wprintf_s(L"%ls\n", line.c_str());
        if (file) {
            int need = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
            if (need > 1) {
                std::vector<char> buf(need);
                WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, buf.data(), need, nullptr, nullptr);
                fwrite(buf.data(), 1, need - 1, file);
                fwrite("\r\n", 1, 2, file);
                fflush(file);
            }
        }
    }
};

struct LoadedEngine {
    std::wstring file;
    HMODULE module;
    DmEngineInfo info;
    PFN_DmExecute exec;
    PFN_DmProbe probe;
    LoadedEngine() : module(nullptr), exec(nullptr), probe(nullptr) { ZeroMemory(&info, sizeof(info)); }
};

static bool StageLess(const LoadedEngine& a, const LoadedEngine& b) {
    return a.info.stage < b.info.stage;
}

struct Host {
    std::vector<LoadedEngine> engines;
    std::wstring engineDir;

    bool Load(const std::wstring& dir, Logger& log) {
        engineDir = dir;
        std::wstring pattern = dir + L"\\*.dll";
        WIN32_FIND_DATAW fd{};
        HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) {
            log.Emit(L"Каталог движков пуст или недоступен: " + dir);
            return false;
        }
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring full = dir + L"\\" + fd.cFileName;
            HMODULE m = LoadLibraryW(full.c_str());
            if (!m) {
                log.Emit(L"Не удалось загрузить " + std::wstring(fd.cFileName) + L": " + dm::ErrText(GetLastError()));
                continue;
            }
            PFN_DmGetInfo getInfo = reinterpret_cast<PFN_DmGetInfo>(GetProcAddress(m, "DmGetInfo"));
            PFN_DmProbe probe = reinterpret_cast<PFN_DmProbe>(GetProcAddress(m, "DmProbe"));
            PFN_DmExecute exec = reinterpret_cast<PFN_DmExecute>(GetProcAddress(m, "DmExecute"));
            if (!getInfo || !probe || !exec) {
                log.Emit(L"Пропущен " + std::wstring(fd.cFileName) + L": нет экспортов DmGetInfo/DmProbe/DmExecute");
                FreeLibrary(m);
                continue;
            }
            LoadedEngine e;
            e.file = full;
            e.module = m;
            e.info.structSize = sizeof(DmEngineInfo);
            if (getInfo(&e.info) != DM_STATUS_OK || e.info.apiVersion != DM_API_VERSION ||
                e.info.structSize != sizeof(DmEngineInfo)) {
                log.Emit(L"Пропущен " + std::wstring(fd.cFileName) + L": несовместимый ABI движка");
                FreeLibrary(m);
                continue;
            }
            e.probe = probe;
            e.exec = exec;
            engines.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
        std::sort(engines.begin(), engines.end(), StageLess);
        return !engines.empty();
    }

    void Unload() {
        for (auto& e : engines) {
            if (e.module) FreeLibrary(e.module);
            e.module = nullptr;
        }
        engines.clear();
    }
};

struct Totals {
    DWORD files;
    DWORD dirs;
    DWORD killed;
    DWORD flags;
    unsigned __int64 bytes;
    Totals() : files(0), dirs(0), killed(0), flags(0), bytes(0) {}
};

struct Verdict {
    bool deleted;
    bool pending;
    std::wstring message;
    Verdict() : deleted(false), pending(false) {}
};

static Verdict RunPipeline(Host& host, const std::wstring& target, DmOptions& opt, Logger& log) {
    Verdict v;
    std::wstring plain = dm::FullPath(target);
    log.Emit(L"=== Цель: " + plain);
    if (plain.empty()) {
        v.message = L"пустой путь";
        return v;
    }
    if (!dm::Exists(dm::Extended(plain))) {
        log.Emit(L"Объект отсутствует на диске, удаление не требуется");
        v.deleted = true;
        v.message = L"объект отсутствует";
        return v;
    }

    Totals t;
    bool satisfied = false;
    int verifyResult = -1;
    std::wstring lastDetail;

    for (auto& e : host.engines) {
        bool isVerify = (e.info.capabilities & DM_CAP_VERIFY) != 0;
        if (satisfied && !isVerify) continue;
        DWORD score = 0;
        int pr = e.probe ? e.probe(plain.c_str(), &opt, &score) : DM_STATUS_OK;
        if (pr == DM_STATUS_SKIP && !isVerify) {
            log.Emit(std::wstring(L"  - ") + e.info.id + L": не применим");
            continue;
        }
        DmResult r{};
        r.structSize = sizeof(DmResult);
        int st = e.exec(plain.c_str(), &opt, &r);
        std::wstring line = std::wstring(L"  - ") + e.info.id + L" [" + std::to_wstring(e.info.stage) + L"]: ";
        if (st == DM_STATUS_OK) line += L"OK";
        else if (st == DM_STATUS_SKIP) line += L"пропуск";
        else if (st == DM_STATUS_SOFTFAIL) line += L"частично";
        else line += L"ошибка";
        if (r.win32Error) line += L", " + dm::ErrText(r.win32Error);
        if (r.detail[0]) line += L" :: " + std::wstring(r.detail);
        log.Emit(line);

        t.files += r.filesRemoved;
        t.dirs += r.dirsRemoved;
        t.bytes += r.bytesFreed;
        if (r.processesKilled > t.killed) t.killed = r.processesKilled;
        t.flags |= r.flagsOut;
        if (r.detail[0]) lastDetail = r.detail;

        if (isVerify) {
            verifyResult = st;
            if (st == DM_STATUS_OK) v.deleted = true;
            else if (r.win32Error == ERROR_ACCESS_DENIED) v.pending = true;
        } else if (st == DM_STATUS_OK) {
            satisfied = true;
        }
    }

    if (verifyResult < 0) v.deleted = !dm::Exists(dm::Extended(plain));
    if (v.deleted && !dm::Exists(dm::Extended(plain))) {
        v.deleted = true;
        v.pending = false;
    } else if (!v.deleted) {
        v.deleted = false;
        if (t.flags & DM_RES_REBOOT_PENDING) v.pending = true;
    }
    v.message = lastDetail.empty() ? (v.deleted ? L"объект удалён" : L"объект остался") : lastDetail;
    if (t.files || t.dirs || t.bytes) {
        wchar_t buf[192];
        swprintf_s(buf, L"Итог: файлов %lu, каталогов %lu, байт %llu, процессов %lu",
                   static_cast<unsigned long>(t.files), static_cast<unsigned long>(t.dirs),
                   static_cast<unsigned long long>(t.bytes), static_cast<unsigned long>(t.killed));
        log.Emit(buf);
    }
    return v;
}

static std::wstring TaskNameFor(const std::wstring& path) {
    unsigned __int64 h = 1469598103934665603ULL;
    for (wchar_t c : path) {
        h ^= static_cast<unsigned __int64>(c);
        h *= 1099511628211ULL;
    }
    wchar_t buf[80];
    swprintf_s(buf, L"DeleteMaster-Cleanup-%016llX", static_cast<unsigned long long>(h));
    return buf;
}

static bool CreateBootTask(const std::wstring& taskName, const std::wstring& exePath,
                           const std::wstring& target, std::wstring* err) {
    ComPtr<ITaskService> svc;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&svc));
    if (FAILED(hr)) {
        if (err) *err = HrText(hr);
        return false;
    }
    VARIANT none;
    VariantInit(&none);
    hr = svc->Connect(none, none, none, none);
    if (FAILED(hr)) {
        if (err) *err = HrText(hr);
        return false;
    }

    ComPtr<ITaskDefinition> def;
    hr = svc->NewTask(0, &def);
    if (FAILED(hr)) {
        if (err) *err = HrText(hr);
        return false;
    }

    ComPtr<IRegistrationInfo> reg;
    if (SUCCEEDED(def->get_RegistrationInfo(&reg))) {
        BSTR author = SysAllocString(L"DeleteMaster");
        reg->put_Author(author);
        SysFreeString(author);
        BSTR desc = SysAllocString(L"Удаление объекта, отложенного до перезагрузки");
        reg->put_Description(desc);
        SysFreeString(desc);
    }

    ComPtr<IPrincipal> princ;
    if (SUCCEEDED(def->get_Principal(&princ))) {
        BSTR user = SysAllocString(L"SYSTEM");
        princ->put_UserId(user);
        SysFreeString(user);
        princ->put_LogonType(TASK_LOGON_SERVICE_ACCOUNT);
        princ->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
    }

    ComPtr<ITaskSettings> set;
    if (SUCCEEDED(def->get_Settings(&set))) {
        set->put_StartWhenAvailable(VARIANT_TRUE);
        set->put_DisallowStartIfOnBatteries(VARIANT_FALSE);
        set->put_StopIfGoingOnBatteries(VARIANT_FALSE);
        set->put_AllowHardTerminate(VARIANT_TRUE);
        set->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW);
        BSTR limit = SysAllocString(L"PT20M");
        set->put_ExecutionTimeLimit(limit);
        SysFreeString(limit);
    }

    ComPtr<ITriggerCollection> triggers;
    if (SUCCEEDED(def->get_Triggers(&triggers))) {
        ComPtr<ITrigger> trigger;
        if (SUCCEEDED(triggers->Create(TASK_TRIGGER_BOOT, &trigger))) {
            trigger->put_Enabled(VARIANT_TRUE);
        }
    }

    ComPtr<IActionCollection> actions;
    if (SUCCEEDED(def->get_Actions(&actions))) {
        ComPtr<IAction> action;
        if (SUCCEEDED(actions->Create(TASK_ACTION_EXEC, &action))) {
            ComPtr<IExecAction> exec;
            if (SUCCEEDED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(&exec)))) {
                BSTR path = SysAllocString(exePath.c_str());
                exec->put_Path(path);
                SysFreeString(path);
                std::wstring args = L"/postreboot \"" + target + L"\" /quiet /task:\"" + taskName + L"\"";
                BSTR argb = SysAllocString(args.c_str());
                exec->put_Arguments(argb);
                SysFreeString(argb);
            }
        }
    }

    ComPtr<IRegisteredTask> task;
    ComPtr<ITaskFolder> root;
    BSTR slash = SysAllocString(L"\\");
    hr = svc->GetFolder(slash, &root);
    SysFreeString(slash);
    if (FAILED(hr)) {
        if (err) *err = HrText(hr);
        return false;
    }
    BSTR name = SysAllocString(taskName.c_str());
    hr = root->RegisterTaskDefinition(name, def, TASK_CREATE_OR_UPDATE, none, none,
                                      TASK_LOGON_SERVICE_ACCOUNT, none, &task);
    SysFreeString(name);
    if (FAILED(hr)) {
        if (err) *err = HrText(hr);
        return false;
    }
    return true;
}

static bool DeleteBootTask(const std::wstring& taskName) {
    ComPtr<ITaskService> svc;
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_ITaskService, reinterpret_cast<void**>(&svc));
    if (FAILED(hr)) return false;
    VARIANT none;
    VariantInit(&none);
    if (FAILED(svc->Connect(none, none, none, none))) return false;
    ComPtr<ITaskFolder> root;
    BSTR slash = SysAllocString(L"\\");
    hr = svc->GetFolder(slash, &root);
    SysFreeString(slash);
    if (FAILED(hr)) return false;
    BSTR name = SysAllocString(taskName.c_str());
    ComPtr<IRegisteredTask> existing;
    hr = root->GetTask(name, &existing);
    if (FAILED(hr)) {
        SysFreeString(name);
        return false;
    }
    hr = root->DeleteTask(name, 0);
    SysFreeString(name);
    return SUCCEEDED(hr);
}

static void PrintRebootBox(const std::wstring& path, DWORD delayMs) {
    wprintf_s(L"\n");
    wprintf_s(L"==================================================================\n");
    wprintf_s(L"  ПЕРЕЗАГРУЗКА НЕОБХОДИМА\n");
    wprintf_s(L"  Объект нельзя удалить на работающей системе:\n");
    wprintf_s(L"  %ls\n", path.c_str());
    wprintf_s(L"  Он поставлен в очередь на удаление при загрузке\n");
    wprintf_s(L"  и в задачу SYSTEM DeleteMaster-Cleanup.\n");
    wprintf_s(L"  ПЕРЕЗАГРУЖАЮ ЧЕРЕЗ %lu СЕК - ОТМЕНЫ НЕТ\n",
              static_cast<unsigned long>(delayMs / 1000));
    wprintf_s(L"  После загрузки этой папки/файла не будет.\n");
    wprintf_s(L"==================================================================\n\n");
}

static bool RebootNow(Logger& log, DWORD delayMs) {
    dm::EnablePrivilege(L"SeShutdownPrivilege");
    log.Emit(L"REBOOT: принудительная перезагрузка, задержка " + std::to_wstring(delayMs) + L" мс");
    if (delayMs) Sleep(delayMs);
    if (ExitWindowsEx(EWX_REBOOT | EWX_FORCE | EWX_FORCEIFHUNG, 0)) {
        log.Emit(L"REBOOT: перезагрузка инициирована через ExitWindowsEx");
        wprintf_s(L"ПЕРЕЗАГРУЖАЮ...\n");
        return true;
    }
    DWORD e1 = GetLastError();
    if (InitiateSystemShutdownExW(nullptr, nullptr, 0, TRUE, TRUE, 0x80040000u)) {
        log.Emit(L"REBOOT: перезагрузка инициирована через InitiateSystemShutdownEx");
        wprintf_s(L"ПЕРЕЗАГРУЖАЮ...\n");
        return true;
    }
    DWORD e2 = GetLastError();
    log.Emit(L"REBOOT: не удалось, ExitWindowsEx " + dm::ErrText(e1) +
             L", InitiateSystemShutdownEx " + dm::ErrText(e2));
    wprintf_s(L"ПЕРЕЗАГРУЗКА НЕ УДАЛАСЬ (код %lu / %lu). Удаление останется в очереди.\n",
              static_cast<unsigned long>(e1), static_cast<unsigned long>(e2));
    return false;
}

static bool RelaunchElevated() {
    wchar_t exe[MAX_PATH] = {0};
    if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) return false;
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    std::wstring params = RawParams();
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return false;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        CloseHandle(sei.hProcess);
    }
    return true;
}

static void PrintBanner(Host& host) {
    wprintf_s(L"\n==============================================================\n");
    wprintf_s(L" DeleteMaster %ls\n", kHostVersion);
    wprintf_s(L" Автоперезагрузка при блокировке: ВКЛЮЧЕНА\n");
    wprintf_s(L" Новая система: стейджинг-папка $DeleteMaster.Stage на томе,\n");
    wprintf_s(L" случайное base64-имя, обнуление содержимого и безвозвратное удаление.\n");
    wprintf_s(L" Если папку или файл нельзя удалить - будет перезагрузка,\n");
    wprintf_s(L" а объект удалится при старте системы.\n");
    wprintf_s(L"==============================================================\n");
    wprintf_s(L"Загружено движков: %zu из %ls\n", host.engines.size(), host.engineDir.c_str());
    for (const auto& e : host.engines) {
        wprintf_s(L"  %2d  %-24ls %ls\n", e.info.stage, e.info.id, e.info.title);
    }
    wprintf_s(L"\nВставьте путь (можно перетащить объект в окно). Пустая строка или exit - выход.\n\n");
}

static std::wstring TrimTarget(const std::wstring& s) {
    std::wstring t = s;
    while (!t.empty() && (t.back() == L'\r' || t.back() == L'\n' || t.back() == L' ' || t.back() == L'\t')) t.pop_back();
    size_t b = 0;
    while (b < t.size() && (t[b] == L' ' || t[b] == L'\t')) ++b;
    return t.substr(b);
}

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U16TEXT);
    _setmode(_fileno(stdin), _O_U16TEXT);
    SetConsoleTitleW(L"DeleteMaster 1.4.0-SERVICE  |  перезагрузка при блокировке");
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    bool postReboot = false;
    std::wstring postTaskName;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        std::wstring low = a;
        for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
        if (low == L"/postreboot" || low == L"-postreboot") {
            postReboot = true;
            continue;
        }
        if (low.compare(0, 6, L"/task:") == 0 || low.compare(0, 6, L"-task:") == 0) {
            postTaskName = a.substr(6);
        }
    }

    if (!postReboot && !dm::IsElevated()) {
        wprintf_s(L"Требуются права администратора. Запускаю копию с повышением...\n");
        if (RelaunchElevated()) {
            CoUninitialize();
            return 0;
        }
        wprintf_s(L"Повышение отклонено. Запустите файл через «Запуск от имени администратора».\n");
        CoUninitialize();
        return 5;
    }

    Logger log;
    std::wstring baseDir = dm::ModuleDir();
    log.Open(baseDir + L"\\logs");

    Host host;
    DmOptions opt{};
    opt.structSize = sizeof(DmOptions);
    opt.flags = DM_FLAG_DEFAULT;
    opt.killTimeoutMs = 3000;
    opt.retryCount = 3;
    opt.retryDelayMs = 60;

    std::vector<std::wstring> targets;
    bool interactive = true;
    bool pauseAtEnd = false;

    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a.empty()) continue;
        if (a[0] == L'/' || a[0] == L'-') {
            std::wstring k = a.substr(1);
            for (auto& ch : k) ch = static_cast<wchar_t>(towlower(ch));
            if (k == L"nokill") opt.flags &= ~DM_FLAG_KILL_HOLDERS;
            else if (k == L"noacl") opt.flags &= ~DM_FLAG_FORCE_ACL;
            else if (k == L"noreboot") opt.flags &= ~DM_FLAG_ALLOW_REBOOT;
            else if (k == L"norebootnow") opt.flags &= ~DM_FLAG_FORCE_REBOOT;
            else if (k == L"norecursive") opt.flags &= ~DM_FLAG_RECURSIVE;
            else if (k == L"nostreams") opt.flags &= ~DM_FLAG_STREAMS;
            else if (k == L"nostaging") opt.flags &= ~DM_FLAG_STAGING;
            else if (k == L"squat") opt.flags |= DM_FLAG_SQUAT;
            else if (k == L"dry" || k == L"dry-run") opt.flags |= DM_FLAG_DRY_RUN;
            else if (k == L"quiet" || k == L"q") log.quiet = true;
            else if (k == L"pause") pauseAtEnd = true;
            else if (k == L"noconsole") log.console = false;
            else if (k.compare(0, 8, L"timeout:") == 0) opt.killTimeoutMs = static_cast<DWORD>(_wtoi(k.c_str() + 8));
            else if (k.compare(0, 8, L"retries:") == 0) opt.retryCount = static_cast<DWORD>(_wtoi(k.c_str() + 8));
            else if (k == L"help" || k == L"?") {
                wprintf_s(L"DeleteMaster.exe <путь> [ключи]\n"
                          L"  /nokill       не завершать процессы-держатели\n"
                          L"  /noacl        не менять владельца и DACL\n"
                          L"  /noreboot     не планировать удаление при перезагрузке\n"
                          L"  /norebootnow  не перезагружать автоматически\n"
                          L"  /norecursive  не заходить в подкаталоги\n"
                          L"  /nostreams    не трогать альтернативные потоки\n"
                          L"  /nostaging    отключить стейджинг-удаление\n"
                          L"  /squat        захватить имя заглушкой после удаления\n"
                          L"  /dry          пробный прогон без изменений\n"
                          L"  /quiet        без вывода в консоль\n"
                          L"  /pause        ждать Enter перед закрытием окна\n"
                          L"  /timeout:N    таймаут завершения процесса, мс\n"
                          L"  /retries:N    число повторов удаления\n");
                log.Close();
                CoUninitialize();
                return 0;
            }
            continue;
        }
        targets.push_back(a);
        if (!(postReboot && targets.size() == 1)) interactive = false;
    }
    if (postReboot) interactive = false;

    std::wstring dir = baseDir + L"\\engines";
    if (!host.Load(dir, log)) {
        if (!postReboot) wprintf_s(L"Не найдено ни одного движка в %ls\nСоберите проект целиком (EXE + DLL).\n", dir.c_str());
        host.Unload();
        log.Close();
        CoUninitialize();
        return 4;
    }

    if (postReboot) {
        opt.flags &= ~(DM_FLAG_FORCE_REBOOT | DM_FLAG_ALLOW_REBOOT);
        opt.flags |= DM_FLAG_QUIET;
        log.quiet = true;
        log.console = false;
        std::wstring target = targets.empty() ? std::wstring() : targets[0];
        log.Emit(L"Режим после перезагрузки, цель: " + target);
        if (target.empty()) {
            host.Unload();
            log.Close();
            CoUninitialize();
            return 1;
        }
        bool ok = false;
        for (int attempt = 0; attempt < 5 && !ok; ++attempt) {
            Verdict v = RunPipeline(host, target, opt, log);
            ok = v.deleted;
            if (!ok) Sleep(10000);
        }
        log.Emit(ok ? L"После перезагрузки объект удалён" : L"После перезагрузки удалить не удалось");
        if (ok && !postTaskName.empty()) {
            DeleteBootTask(postTaskName);
            log.Emit(L"Задача очистки удалена: " + postTaskName);
        }
        host.Unload();
        log.Close();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    log.Emit(std::wstring(L"DeleteMaster ") + kHostVersion + L" запущен, движков: " +
             std::to_wstring(host.engines.size()));

    int exitCode = 0;
    bool needReboot = false;
    std::vector<std::wstring> armed;

    auto arm = [&](const std::wstring& raw) {
        std::wstring exePath = baseDir + L"\\DeleteMaster.exe";
        std::wstring name = TaskNameFor(raw);
        std::wstring terr;
        if (CreateBootTask(name, exePath, raw, &terr)) {
            armed.push_back(name);
            log.Emit(L"Задача очистки при загрузке создана: " + name);
        } else {
            log.Emit(L"Задача очистки не создана: " + terr);
            wprintf_s(L"Внимание: задачу очистки при загрузке создать не удалось (%ls).\n", terr.c_str());
        }
    };

    auto handleTarget = [&](const std::wstring& raw, bool rebootNow) {
        Verdict v = RunPipeline(host, raw, opt, log);
        if (v.deleted) {
            wprintf_s(L"УДАЛЕНО: %ls\n", raw.c_str());
            return;
        }
        if (v.pending) {
            needReboot = true;
            exitCode = exitCode == 0 ? 2 : exitCode;
            arm(raw);
            bool willReboot = rebootNow && (opt.flags & DM_FLAG_FORCE_REBOOT) && !(opt.flags & DM_FLAG_DRY_RUN);
            if (willReboot) PrintRebootBox(raw, 4000);
            else wprintf_s(L"В ОЧЕРЕДИ: %ls (удалится при загрузке, перезагрузка отключена)\n", raw.c_str());
            if (willReboot) RebootNow(log, 4000);
            return;
        }
        wprintf_s(L"НЕ УДАЛЕНО: %ls\n  %ls\n", raw.c_str(), v.message.c_str());
        exitCode = 1;
    };

    if (!interactive) {
        bool single = targets.size() == 1;
        for (const auto& t : targets) handleTarget(t, single);
    } else {
        PrintBanner(host);
        wchar_t inputBuf[DM_MAX_PATH];
        while (true) {
            wprintf_s(L"путь> ");
            if (!fgetws(inputBuf, DM_MAX_PATH, stdin)) break;
            std::wstring line = TrimTarget(inputBuf);
            if (line.empty()) continue;
            std::wstring low = line;
            for (auto& c : low) c = static_cast<wchar_t>(towlower(c));
            if (low == L"exit" || low == L"quit" || low == L"выход") break;
            handleTarget(line, true);
        }
    }

    if (needReboot && (opt.flags & DM_FLAG_FORCE_REBOOT) && !(opt.flags & DM_FLAG_DRY_RUN)) {
        RebootNow(log, 4000);
    } else if (needReboot) {
        log.Emit(L"Удаление отложено до перезагрузки, автоперезагрузка отключена ключом");
        wprintf_s(L"Удаление отложено до перезагрузки. Перезагрузите вручную.\n");
    }

    log.Emit(L"Завершение, код " + std::to_wstring(exitCode));
    host.Unload();
    if (!log.logPath.empty() && !log.quiet) wprintf_s(L"Журнал: %ls\n", log.logPath.c_str());
    log.Close();
    if (pauseAtEnd && !log.quiet) {
        wprintf_s(L"\nГотово. Нажмите Enter, чтобы закрыть окно...");
        wchar_t pauseBuf[16];
        fgetws(pauseBuf, 16, stdin);
    }
    CoUninitialize();
    return exitCode;
}


