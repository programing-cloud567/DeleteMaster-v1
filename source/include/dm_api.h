#pragma once
#ifndef DM_API_H
#define DM_API_H

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define DM_API_VERSION 3
#define DM_HOST_NAME L"DeleteMaster"
#define DM_MAX_PATH 32768
#define DM_DETAIL_CCH 1024
#define DM_ID_CCH 64
#define DM_TITLE_CCH 128
#define DM_SUMMARY_CCH 512

#define DM_STDCALL __stdcall

#ifdef __cplusplus
#define DM_EXPORT extern "C" __declspec(dllexport)
#else
#define DM_EXPORT __declspec(dllexport)
#endif

#define DM_STATUS_OK 0
#define DM_STATUS_SKIP 1
#define DM_STATUS_SOFTFAIL 2
#define DM_STATUS_FAIL 3

#define DM_FLAG_RECURSIVE 0x00000001u
#define DM_FLAG_KILL_HOLDERS 0x00000002u
#define DM_FLAG_FORCE_ACL 0x00000004u
#define DM_FLAG_ALLOW_REBOOT 0x00000008u
#define DM_FLAG_FOLLOW_LINKS 0x00000010u
#define DM_FLAG_DRY_RUN 0x00000020u
#define DM_FLAG_QUIET 0x00000040u
#define DM_FLAG_STREAMS 0x00000080u
#define DM_FLAG_NO_GRACEFUL 0x00000100u
#define DM_FLAG_FORCE_REBOOT 0x00000200u
#define DM_FLAG_STAGING 0x00000400u
#define DM_FLAG_SQUAT 0x00000800u
#define DM_FLAG_DEFAULT (DM_FLAG_RECURSIVE | DM_FLAG_KILL_HOLDERS | DM_FLAG_FORCE_ACL | DM_FLAG_ALLOW_REBOOT | DM_FLAG_STREAMS | DM_FLAG_FORCE_REBOOT | DM_FLAG_STAGING)

#define DM_RES_REBOOT_PENDING 0x00001000u
#define DM_RES_VERIFIED 0x00002000u

#define DM_CAP_DELETE 0x00000001u
#define DM_CAP_SCAN 0x00000002u
#define DM_CAP_KILL 0x00000004u
#define DM_CAP_PRIVILEGE 0x00000008u
#define DM_CAP_ACL 0x00000010u
#define DM_CAP_NATIVE 0x00000020u
#define DM_CAP_STREAMS 0x00000040u
#define DM_CAP_REPARSE 0x00000080u
#define DM_CAP_REBOOT 0x00000100u
#define DM_CAP_VERIFY 0x00000200u
#define DM_CAP_STAGING 0x00000400u
#define DM_CAP_SERVICE 0x00000800u
#define DM_CAP_FREEZE 0x00001000u
#define DM_CAP_SQUAT 0x00002000u

typedef struct DmOptions {
    DWORD structSize;
    DWORD flags;
    DWORD killTimeoutMs;
    DWORD retryCount;
    DWORD retryDelayMs;
    DWORD reserved;
} DmOptions;

typedef struct DmResult {
    DWORD structSize;
    int status;
    DWORD win32Error;
    DWORD flagsOut;
    DWORD filesRemoved;
    DWORD dirsRemoved;
    DWORD processesKilled;
    unsigned __int64 bytesFreed;
    wchar_t engineId[DM_ID_CCH];
    wchar_t detail[DM_DETAIL_CCH];
} DmResult;

typedef struct DmEngineInfo {
    DWORD structSize;
    DWORD apiVersion;
    int stage;
    DWORD capabilities;
    wchar_t id[DM_ID_CCH];
    wchar_t title[DM_TITLE_CCH];
    wchar_t summary[DM_SUMMARY_CCH];
} DmEngineInfo;

typedef int(DM_STDCALL* PFN_DmGetInfo)(DmEngineInfo* info);
typedef int(DM_STDCALL* PFN_DmProbe)(const wchar_t* target, const DmOptions* opt, DWORD* score);
typedef int(DM_STDCALL* PFN_DmExecute)(const wchar_t* target, const DmOptions* opt, DmResult* result);

DM_EXPORT int DM_STDCALL DmGetInfo(DmEngineInfo* info);
DM_EXPORT int DM_STDCALL DmProbe(const wchar_t* target, const DmOptions* opt, DWORD* score);
DM_EXPORT int DM_STDCALL DmExecute(const wchar_t* target, const DmOptions* opt, DmResult* result);

#endif
