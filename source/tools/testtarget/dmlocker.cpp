#include <windows.h>
#include <string>
#include <vector>
#include <cstdlib>

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 1;
    std::wstring folder = argv[1];
    DWORD seconds = argc > 2 ? static_cast<DWORD>(_wtoi(argv[2])) : 7200;
    if (seconds == 0) seconds = 7200;

    std::vector<HANDLE> held;

    HANDLE dir = CreateFileW(folder.c_str(), FILE_READ_ATTRIBUTES,
                             FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (dir != INVALID_HANDLE_VALUE) held.push_back(dir);

    std::wstring pattern = folder + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring path = folder + L"\\" + fd.cFileName;
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ,
                                   0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) held.push_back(f);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    DWORD deadline = GetTickCount() + seconds * 1000;
    while (GetTickCount() < deadline) Sleep(500);

    for (HANDLE f : held) CloseHandle(f);
    return 0;
}




