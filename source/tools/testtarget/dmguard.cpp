#include <windows.h>
#include <string>
#include <vector>
#include <cstdlib>

struct Kid {
    PROCESS_INFORMATION pi;
};

static std::wstring DirOf(const std::wstring& full) {
    size_t s = full.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring(L".") : full.substr(0, s);
}

static bool FolderGone(const std::wstring& folder) {
    DWORD a = GetFileAttributesW(folder.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return false;
    DWORD e = GetLastError();
    return e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND;
}

static void MarkHidden(const std::wstring& folder) {
    SetFileAttributesW(folder.c_str(), FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_DIRECTORY);
}

static void DropCanary(const std::wstring& folder) {
    std::wstring canary = folder + L"\\~guard.dat";
    HANDLE f = CreateFileW(canary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(f, "guard", 5, &written, nullptr);
    CloseHandle(f);
}

static int RunWatcher(const std::wstring& folder) {
    int made = 0;
    DWORD deadline = GetTickCount() + 7200u * 1000u;
    while (GetTickCount() < deadline && made < 500) {
        if (FolderGone(folder)) {
            if (CreateDirectoryW(folder.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) {
                MarkHidden(folder);
                DropCanary(folder);
                made++;
            }
        }
        Sleep(20);
    }
    return 0;
}

static int RunPool(const std::wstring& folder, const std::wstring& mode, int count, int perChild) {
    wchar_t selfBuf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, selfBuf, MAX_PATH);
    std::wstring dir = DirOf(selfBuf);

    HANDLE folderHold = INVALID_HANDLE_VALUE;
    if (mode == L"lock") {
        folderHold = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                 FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    }

    std::vector<Kid> kids;

    auto spawnOne = [&]() {
        std::wstring exe = dir + (mode == L"lock" ? L"\\dmlocker.exe" : L"\\dmguard.exe");
        std::wstring cmd = L"\"" + exe + L"\" \"" + folder + L"\"";
        if (mode == L"lock") cmd += L" 7200";
        else cmd += L" lock " + std::to_wstring(perChild);
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            Kid k;
            k.pi = pi;
            kids.push_back(k);
        }
    };

    for (int i = 0; i < count; ++i) spawnOne();

    int respawns = 0;
    DWORD deadline = GetTickCount() + 7200u * 1000u;
    while (GetTickCount() < deadline && respawns < 2000) {
        if (FolderGone(folder)) break;
        if (kids.empty()) {
            spawnOne();
            continue;
        }
        std::vector<HANDLE> handles;
        handles.reserve(kids.size());
        for (auto& k : kids) handles.push_back(k.pi.hProcess);
        DWORD w = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, 20);
        if (w == WAIT_TIMEOUT) continue;
        if (w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + handles.size()) {
            size_t idx = w - WAIT_OBJECT_0;
            CloseHandle(kids[idx].pi.hProcess);
            CloseHandle(kids[idx].pi.hThread);
            kids.erase(kids.begin() + idx);
            spawnOne();
            respawns++;
        }
    }

    for (auto& k : kids) {
        CloseHandle(k.pi.hProcess);
        CloseHandle(k.pi.hThread);
    }
    if (folderHold != INVALID_HANDLE_VALUE) CloseHandle(folderHold);
    return 0;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) return 1;
    std::wstring folder = argv[1];
    std::wstring mode = argv[2];
    int count = _wtoi(argv[3]);
    if (count < 1) count = 1;
    if (count > 16) count = 16;

    if (mode == L"watch") return RunWatcher(folder);
    int perChild = 4;
    if (mode == L"guard") return RunPool(folder, mode, count, perChild);
    return RunPool(folder, mode, count, perChild);
}

