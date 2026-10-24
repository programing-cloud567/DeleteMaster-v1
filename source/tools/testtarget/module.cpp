extern "C" __declspec(dllexport) int DmTestModule(int x) {
    return x * 2;
}

extern "C" __declspec(dllexport) const char* DmTestName() {
    return "DeleteMaster test module";
}

extern "C" __declspec(dllexport) int DmTestVersion() {
    return 1;
}
