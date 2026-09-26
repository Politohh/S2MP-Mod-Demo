///////////////////////////////////////////
//         Debug Patches
//	Patching some anti-tamper stuff
////////////////////////////////////////////
#include "pch.h"
#include "Arxan.hpp"
#include <winternl.h>
#include "Hook.hpp"

#pragma intrinsic(_ReturnAddress)

#define THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER 0x00000004
#define ThreadHideFromDebugger 0x11
#define LOG(...) Logger::write(__VA_ARGS__)
namespace Logger {
    inline std::ofstream logFile;
    inline std::mutex logMutex;

    inline void initUnlocked() {
        if (!logFile.is_open()) {
            std::filesystem::create_directories("main");
            logFile.open("main/startup.log", std::ios::out | std::ios::app);
        }
    }

    inline void init() {
        std::lock_guard<std::mutex> lock(logMutex);
        initUnlocked();
    }

    inline void write(const char* fmt, ...) {

        //disabling for now
        return;

        char buffer[2048];

        va_list args;
        va_start(args, fmt);
        vsnprintf(buffer, sizeof(buffer), fmt, args);
        va_end(args);

        std::lock_guard<std::mutex> lock(logMutex);
        initUnlocked();

        if (logFile.is_open()) {
            logFile << buffer << std::endl;
            logFile.flush();
        }
    }
}


std::string DebugPatches::conLabel = "DP";
uintptr_t DebugPatches::base = (uintptr_t)GetModuleHandle(NULL) + 0x1000;
///////////////////////////////////////////////////////////////////////////////////////////////////////////////

typedef NTSTATUS(WINAPI* NtUserBuildHwndList_t)(HDESK hdesk, HWND hwndParent, BOOL fChildren, BOOL fOwner, DWORD dwThreadId, UINT cHwndMax, HWND* phwnd, PUINT pcHwndNeeded);
NtUserBuildHwndList_t fpNtUserBuildHwndList = nullptr;

typedef BOOL(WINAPI* CheckRemoteDebuggerPresent_t)(HANDLE hProcess, PBOOL pbDebuggerPresent);
CheckRemoteDebuggerPresent_t fpCheckRemoteDebuggerPresent = nullptr;

typedef NTSTATUS(WINAPI* NtCreateThreadEx_t)(OUT PHANDLE ThreadHandle, IN ACCESS_MASK DesiredAccess, IN PVOID ObjectAttributes, IN HANDLE ProcessHandle, IN PVOID StartRoutine, IN PVOID Argument, IN ULONG CreateFlags, IN SIZE_T ZeroBits, IN SIZE_T StackSize, IN SIZE_T MaximumStackSize, IN PVOID AttributeList);
NtCreateThreadEx_t fpNtCreateThreadEx = nullptr;

typedef NTSTATUS(WINAPI* NtSetInformationThread_t)(HANDLE ThreadHandle, THREAD_INFORMATION_CLASS ThreadInformationClass, PVOID ThreadInformation, ULONG ThreadInformationLength);
NtSetInformationThread_t fpNtSetInformationThread = nullptr;

typedef NTSTATUS(WINAPI* NtQueryInformationThread_t)(HANDLE ThreadHandle, THREAD_INFORMATION_CLASS ThreadInformationClass, PVOID ThreadInformation, ULONG ThreadInformationLength, PULONG ReturnLength);
NtQueryInformationThread_t fpNtQueryInformationThread = nullptr;

NTSTATUS WINAPI HookedNtUserBuildHwndList(HDESK hdesk, HWND hwndParent, BOOL fChildren, BOOL fOwner, DWORD dwThreadId, UINT cHwndMax, HWND* phwnd, PUINT pcHwndNeeded) {
    NTSTATUS result = fpNtUserBuildHwndList(hdesk, hwndParent, fChildren, fOwner, dwThreadId, cHwndMax, phwnd, pcHwndNeeded);

    if (NT_SUCCESS(result) && pcHwndNeeded) {
        *pcHwndNeeded = 0;
    }

    return result;
}


void bypassHwndChecks() {
    HMODULE hUser32 = LoadLibraryA("win32u.dll");
    if (!hUser32) {
        LOG("Failed to load win32u.dll");

        return;
    }

    FARPROC pFunc = GetProcAddress(hUser32, "NtUserBuildHwndList");
    if (!pFunc) {
        LOG("Failed to get address of NtUserBuildHwndList");
        return;
    }

    if (MH_CreateHook(pFunc, &HookedNtUserBuildHwndList, reinterpret_cast<LPVOID*>(&fpNtUserBuildHwndList)) != MH_OK) {
        LOG("Failed to create hook");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook");
        return;
    }


}

using NtClose_t = NTSTATUS(NTAPI*)(HANDLE);
using NtQueryInformationProcess_t = NTSTATUS(NTAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG);
NtClose_t fpNtClose = nullptr;
NtQueryInformationProcess_t fpNtQueryInformationProcess = nullptr;

typedef NTSTATUS(NTAPI* PNTQUERYOBJECT)(HANDLE Handle, OBJECT_INFORMATION_CLASS ObjectInformationClass, PVOID ObjectInformation, ULONG ObjectInformationLength, PULONG ReturnLength);
PNTQUERYOBJECT pNtQueryObject = nullptr;


NTSTATUS NTAPI HookedNtClose(HANDLE handle) {
    if (pNtQueryObject) {
        char info[16] = { 0 };
        if (pNtQueryObject(handle, (OBJECT_INFORMATION_CLASS)4, &info, 2, nullptr) >= 0 && (uint64_t)handle != 0x12345) {
            return fpNtClose(handle);
        }
    }
    return STATUS_INVALID_HANDLE;
}
#ifndef ProcessDebugPort
#define ProcessDebugPort 7
#endif 

#ifndef ProcessDebugObjectHandle
#define ProcessDebugObjectHandle 30
#endif 

#ifndef ProcessDebugFlags
#define ProcessDebugFlags 31
#endif 
using fnNtQueryInformationProcess = NTSTATUS(WINAPI*)(HANDLE, PROCESSINFOCLASS, PVOID, ULONG, PULONG); 
fnNtQueryInformationProcess oNtQueryInformationProcess = nullptr;

NTSTATUS WINAPI HookedNtQueryInformationProcess(HANDLE handle, PROCESSINFOCLASS info_class, PVOID info, ULONG info_length, PULONG ret_length) {
    const NTSTATUS status = oNtQueryInformationProcess(handle, info_class, info, info_length, ret_length);

    if (NT_SUCCESS(status)) {
        if (info_class == ProcessBasicInformation) {
            static DWORD explorer_pid = 0;
            if (!explorer_pid) {
                HWND shell_window = GetShellWindow();
                GetWindowThreadProcessId(shell_window, &explorer_pid);
            }

            reinterpret_cast<PPROCESS_BASIC_INFORMATION>(info)->Reserved3 =
                reinterpret_cast<PVOID>(static_cast<ULONG_PTR>(explorer_pid));
        }
        else if (info_class == ProcessDebugObjectHandle) {
            *reinterpret_cast<HANDLE*>(info) = nullptr;
            return STATUS_PORT_NOT_SET; // 0xC0000353
        }
        else if (info_class == ProcessDebugPort) {
            *reinterpret_cast<HANDLE*>(info) = nullptr;
        }
        else if (info_class == 31 /* ProcessDebugFlags */) {
            *reinterpret_cast<ULONG*>(info) = 1;
        }
    }

    return status;
}


BOOL WINAPI HookedCheckRemoteDebuggerPresent(HANDLE hProcess, PBOOL pbDebuggerPresent) {
    BOOL result = fpCheckRemoteDebuggerPresent(hProcess, pbDebuggerPresent);
    if (pbDebuggerPresent != nullptr) {
        if (pbDebuggerPresent) {
            Console::labelPrint(DebugPatches::conLabel, "CheckRemoteDebuggerPresent called");
            LOG("CheckRemoteDebuggerPresent called");
        }
        *pbDebuggerPresent = FALSE;
    }

    return result;
}

NTSTATUS WINAPI HookedNtCreateThreadEx(PHANDLE ThreadHandle, ACCESS_MASK DesiredAccess, PVOID ObjectAttributes, HANDLE ProcessHandle, PVOID StartRoutine, PVOID Argument, ULONG CreateFlags,
    SIZE_T ZeroBits, SIZE_T StackSize, SIZE_T MaximumStackSize, PVOID AttributeList) {
    //remove HIDE_FROM_DEBUGGER flag
    if (CreateFlags & THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER) {
        CreateFlags &= ~THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER;
        Console::labelPrint(DebugPatches::conLabel, "Stripped THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER from thread creation");
        LOG("Stripped THREAD_CREATE_FLAGS_HIDE_FROM_DEBUGGER from thread creation");
    }

    return fpNtCreateThreadEx(ThreadHandle, DesiredAccess, ObjectAttributes, ProcessHandle, StartRoutine, Argument, CreateFlags, ZeroBits, StackSize, MaximumStackSize, AttributeList);
}

void bypassHiddenThreadCreation() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) {
        LOG("Failed to get handle for ntdll.dll");
        return;
    }

    FARPROC pFunc = GetProcAddress(hNtdll, "NtCreateThreadEx");
    if (!pFunc) {
        LOG("Failed to get address of NtCreateThreadEx");
        return;
    }

    if (MH_CreateHook(pFunc, &HookedNtCreateThreadEx, reinterpret_cast<LPVOID*>(&fpNtCreateThreadEx)) != MH_OK) {
        LOG("Failed to create hook for NtCreateThreadEx");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtCreateThreadEx");
        return;
    }
}
NTSTATUS WINAPI HookedNtSetInformationThread(HANDLE ThreadHandle, THREAD_INFORMATION_CLASS ThreadInformationClass, PVOID ThreadInformation, ULONG ThreadInformationLength) {
    if (ThreadInformationClass == (THREAD_INFORMATION_CLASS)ThreadHideFromDebugger) {
        return 0; //success
    }

    return fpNtSetInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength);
}

void bypassThreadHideFromDebugger() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) {
        LOG("Failed to get handle for ntdll.dll");
        return;
    }

    FARPROC pFunc = GetProcAddress(hNtdll, "NtSetInformationThread");
    if (!pFunc) {
        LOG("Failed to get address of NtSetInformationThread");
        return;
    }

    if (MH_CreateHook(pFunc, &HookedNtSetInformationThread, reinterpret_cast<LPVOID*>(&fpNtSetInformationThread)) != MH_OK) {
        LOG("Failed to create hook for NtSetInformationThread");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtSetInformationThread");
        return;
    }
}

void hookNtClose() {
    HMODULE hNtDll = GetModuleHandleA("ntdll.dll");
    if (hNtDll) {
        pNtQueryObject = (PNTQUERYOBJECT)GetProcAddress(hNtDll, "NtQueryObject");
        if (!pNtQueryObject) {
            LOG("Failed to resolve NtQueryObject");
            return;
        }
        MH_CreateHook(GetProcAddress(hNtDll, "NtClose"), HookedNtClose, reinterpret_cast<LPVOID*>(&fpNtClose));
    }
    MH_EnableHook(MH_ALL_HOOKS);
}

NTSTATUS WINAPI HookedNtQueryInformationThread(HANDLE ThreadHandle, THREAD_INFORMATION_CLASS ThreadInformationClass, PVOID ThreadInformation, ULONG ThreadInformationLength, PULONG ReturnLength) {
    if (ThreadInformationClass == (THREAD_INFORMATION_CLASS)ThreadHideFromDebugger) {
        LOG("Blocked NtQueryInformationThread(ThreadHideFromDebugger)");
        return 0xC00000BB; //STATUS_NOT_SUPPORTED
    }

    return fpNtQueryInformationThread(ThreadHandle, ThreadInformationClass, ThreadInformation, ThreadInformationLength, ReturnLength);
}

void bypassThreadQueryHideFromDebugger() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) {
        LOG("Failed to get handle for ntdll.dll");
        return;
    }

    FARPROC pFunc = GetProcAddress(hNtdll, "NtQueryInformationThread");
    if (!pFunc) {
        LOG("Failed to get address of NtQueryInformationThread");
        return;
    }

    if (MH_CreateHook(pFunc, &HookedNtQueryInformationThread, reinterpret_cast<LPVOID*>(&fpNtQueryInformationThread)) != MH_OK) {
        LOG("Failed to create hook for NtQueryInformationThread");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtQueryInformationThread");
        return;
    }
}

void hookNtQueryInformationProcess() {
    HMODULE hNtdll = GetModuleHandleA("ntdll.dll");
    if (!hNtdll) {
        LOG("Failed to get handle for ntdll.dll");
        return;
    }

    FARPROC pFunc = GetProcAddress(hNtdll, "NtQueryInformationProcess");
    if (!pFunc) {
        LOG("Failed to get address of NtQueryInformationProcess");
        return;
    }

    if (MH_CreateHook(pFunc, &HookedNtQueryInformationProcess, reinterpret_cast<LPVOID*>(&oNtQueryInformationProcess)) != MH_OK) {
        LOG("Failed to create hook for NtQueryInformationProcess");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtQueryInformationProcess");
        return;
    }
}

typedef NTSTATUS(NTAPI* NtQuerySystemInformation_t)(ULONG SystemInformationClass, PVOID SystemInformation, ULONG SystemInformationLength, PULONG ReturnLength);
NtQuerySystemInformation_t fpNtQuerySystemInformation = nullptr;
typedef NTSTATUS(NTAPI* NtQueryObject_t)(HANDLE Handle, ULONG ObjectInformationClass, PVOID ObjectInformation, ULONG ObjectInformationLength, PULONG ReturnLength);
#define SystemExtendedHandleInformation 64
#define ObjectNameInformation 1
#define ObjectTypeInformation 2

std::wstring toLower(std::wstring s) {
    for (auto& c : s) {
        c = (wchar_t)towlower(c);
    }
    return s;
}


bool containsSubstring(const std::wstring& text, const std::wstring& sub, bool caseInsensitive) {
    if (caseInsensitive) {
        return toLower(text).find(toLower(sub)) != std::wstring::npos;
    }

    return text.find(sub) != std::wstring::npos;
}


size_t closeCurrentProcessMutantsContaining(const std::wstring& substring, bool caseInsensitive = true, bool dryRun = false) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return 0;
    }
    
    NtQueryObject_t NtQueryObject = (NtQueryObject_t)GetProcAddress(ntdll, "NtQueryObject");

    if (!fpNtQuerySystemInformation || !NtQueryObject) {
        return 0;
    }

    DWORD currentPid = GetCurrentProcessId();
    ULONG handleInfoSize = 0x10000;
    std::vector<BYTE> handleInfoBuffer(handleInfoSize);

    NTSTATUS status;

    while (true) {
        status = fpNtQuerySystemInformation(SystemExtendedHandleInformation, handleInfoBuffer.data(), handleInfoSize, &handleInfoSize);

        if (status == STATUS_INFO_LENGTH_MISMATCH) {
            handleInfoSize *= 2;
            handleInfoBuffer.resize(handleInfoSize);
            continue;
        }

        break;
    }

    if (!NT_SUCCESS(status)) {
        return 0;
    }

    PSYSTEM_HANDLE_INFORMATION_EX handleInfo = reinterpret_cast<PSYSTEM_HANDLE_INFORMATION_EX>(handleInfoBuffer.data());

    size_t closedCount = 0;

    for (ULONG_PTR i = 0; i < handleInfo->NumberOfHandles; i++) {
        auto& entry = handleInfo->Handles[i];

        if ((DWORD)entry.UniqueProcessId != currentPid) {
            continue;
        }

        HANDLE h = reinterpret_cast<HANDLE>(entry.HandleValue);
        BYTE typeBuffer[0x1000] = {};
        ULONG returnLength = 0;

        status = NtQueryObject(h, ObjectTypeInformation, typeBuffer, sizeof(typeBuffer), &returnLength);

        if (!NT_SUCCESS(status)) {
            continue;
        }

        UNICODE_STRING* typeName = reinterpret_cast<UNICODE_STRING*>(typeBuffer);

        if (!typeName->Buffer || typeName->Length == 0) {
            continue;
        }

        std::wstring type(typeName->Buffer, typeName->Length / sizeof(wchar_t));

        if (type != L"Mutant") {
            continue;
        }

        ULONG nameBufferSize = 0x1000;
        std::vector<BYTE> nameBuffer(nameBufferSize);

        status = NtQueryObject(h, ObjectNameInformation, nameBuffer.data(), nameBufferSize, &returnLength);

        if (status == STATUS_INFO_LENGTH_MISMATCH || returnLength > nameBufferSize) {
            nameBufferSize = returnLength;
            nameBuffer.resize(nameBufferSize);

            status = NtQueryObject(h, ObjectNameInformation, nameBuffer.data(), nameBufferSize, &returnLength);
        }

        if (!NT_SUCCESS(status)) {
            continue;
        }

        UNICODE_STRING* objectName = reinterpret_cast<UNICODE_STRING*>(nameBuffer.data());

        if (!objectName->Buffer || objectName->Length == 0) {
            continue;
        }

        std::wstring name(objectName->Buffer, objectName->Length / sizeof(wchar_t));

        if (!containsSubstring(name, substring, caseInsensitive)) {
            continue;
        }


        if (!dryRun) {
            if (CloseHandle(h)) {
                closedCount++;
            }
        }
    }

    return closedCount;
}

void freeIdaMutants() {
    //game would hold the mutex for ida module handles, forcing ida to fail to load lol
    size_t closedIda = closeCurrentProcessMutantsContaining(L"IDA ", true, false); //very needed
    size_t closeMisc = closeCurrentProcessMutantsContaining(L"WilStaging_02", true, false); //idk but looked sus
    closeMisc += closeCurrentProcessMutantsContaining(L"WilError_03", true, false); //idk but looked sus
    LOG("Closed %zu IDA Mutants", closedIda);
    LOG("Closed %zu Misc Mutants", closeMisc);
}


typedef _CLIENT_ID* R_PCLIENT_ID; //ffs

typedef NTSTATUS(NTAPI* NtOpenProcess_t)(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, R_PCLIENT_ID ClientId);
NtOpenProcess_t fpNtOpenProcess = nullptr;

//prevent the steam must be running error from us blocking the processes
bool isSteamProcessId_Nt(DWORD pid) {
    if (!fpNtOpenProcess || pid == 0) {
        return false;
    }

    HANDLE hProc = nullptr;

    _CLIENT_ID cid{};
    cid.UniqueProcess = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid));
    cid.UniqueThread = nullptr;

    OBJECT_ATTRIBUTES oa{};
    InitializeObjectAttributes(&oa, nullptr, 0, nullptr, nullptr);

    NTSTATUS status = fpNtOpenProcess(&hProc, PROCESS_QUERY_LIMITED_INFORMATION, &oa, &cid);

    if (status < 0 || !hProc) {
        return false;
    }

    bool result = false;

    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;

    if (QueryFullProcessImageNameW(hProc, 0, path, &size)){
        const wchar_t* name = wcsrchr(path, L'\\');
        name = name ? name + 1 : path;

        if (_wcsicmp(name, L"steam.exe") == 0)
            result = true;
    }

    CloseHandle(hProc);
    return result;
}


NTSTATUS NtOpenProcess_hookfunc(PHANDLE ProcessHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes, R_PCLIENT_ID ClientId) {
    if (ProcessHandle) {
        *ProcessHandle = nullptr;
    }

    if (!fpNtOpenProcess || !ClientId || !ClientId->UniqueProcess) {
        return STATUS_ACCESS_DENIED;
    }

    DWORD pid = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(ClientId->UniqueProcess));

    if (!isSteamProcessId_Nt(pid)) {
        return STATUS_ACCESS_DENIED;
    }

    return fpNtOpenProcess(ProcessHandle, DesiredAccess, ObjectAttributes, ClientId);
}

void disableProgramScans() {
    //NtOpenProcess 
    HMODULE dll = GetModuleHandleA("ntdll.dll");
    if (!dll) {
        return;
    }
    FARPROC pFunc = GetProcAddress(dll, "NtOpenProcess");
    if (!pFunc) {
        LOG("Failed to get address of NtOpenProcess");
        return;
    }
    
    if (MH_CreateHook(pFunc, &NtOpenProcess_hookfunc, reinterpret_cast<LPVOID*>(&fpNtOpenProcess)) != MH_OK) {
        LOG("Failed to create hook for NtOpenProcess");
        return;
    }
    
    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtOpenProcess");
        return;
    }
}


typedef BOOLEAN(NTAPI* RtlEqualUnicodeString_t)(PUNICODE_STRING, PUNICODE_STRING, BOOLEAN);
RtlEqualUnicodeString_t fpRtlEqualUnicodeString = nullptr;

void myRtlInitUnicodeString(PUNICODE_STRING dst, PCWSTR src) {
    if (!src) {
        dst->Length = 0;
        dst->MaximumLength = 0;
        dst->Buffer = NULL;
        return;
    }
    size_t len = wcslen(src) * sizeof(WCHAR);
    dst->Length = (USHORT)len;
    dst->MaximumLength = (USHORT)(len + sizeof(WCHAR));
    dst->Buffer = (PWSTR)src;
}

__kernel_entry NTSTATUS NtQuerySystemInformation_hookfunc(
    ULONG SystemInformationClass,
    PVOID SystemInformation,
    ULONG SystemInformationLength,
    PULONG ReturnLength)
{
    NTSTATUS status = fpNtQuerySystemInformation(
        SystemInformationClass,
        SystemInformation,
        SystemInformationLength,
        ReturnLength);

    if (!NT_SUCCESS(status) ||
        SystemInformationClass != SystemProcessInformation ||
        SystemInformation == NULL)
    {
        return status;
    }

    // Tools the game must not see when it enumerates running processes.
    const wchar_t* blockedProcesses[] = {
        L"OLLYDBG.exe", L"x32dbg.exe", L"x64dbg.exe", L"x96dbg.exe",
        L"windbg.exe", L"dnSpy.exe", L"HxD.exe", L"ida.exe", L"ida64.exe",
        L"ImmunityDebugger.exe", L"Scylla_x64.exe", L"Scylla_x86.exe",
        L"OllyDumpEx_SA32.exe", L"OllyDumpEx_SA64.exe", L"apimonitor-x64.exe",
        L"apimonitor-x86.exe", L"cheatengine-x86_64.exe", L"Cheat Engine.exe",
        L"cheatengine-x86_64-SSE4-AVX2.exe", L"cheatengine-i386.exe",
        L"cheatengine-x86_64-AVX512.exe", L"DbgX.Shell.exe", L"EngHost.exe",
    };
    const size_t numBlocked = sizeof(blockedProcesses) / sizeof(blockedProcesses[0]);

    // SAFE in-place approach (replaces the old entry-removal surgery that crashed on
    // some machines -- it moved list memory around and mis-computed sizes). Here we
    // NEVER move memory or touch NextEntryOffset/ReturnLength: we only blank the
    // ImageName of a blocked entry in-place, so the game's name comparison can't match
    // it. Worst case it fails to hide (no crash), never corrupts the returned buffer.
    BYTE* const bufStart = (BYTE*)SystemInformation;
    BYTE* const bufEnd = bufStart + SystemInformationLength;

    PSYSTEM_PROCESS_INFORMATION spi = (PSYSTEM_PROCESS_INFORMATION)SystemInformation;

    for (unsigned guard = 0; guard < 100000; ++guard) {
        // Entry header must fit entirely inside the buffer before we read any field.
        if ((BYTE*)spi < bufStart || (BYTE*)spi + sizeof(SYSTEM_PROCESS_INFORMATION) > bufEnd)
            break;

        if (spi->ImageName.Buffer && spi->ImageName.Length >= sizeof(wchar_t)) {
            BYTE* nameStart = (BYTE*)spi->ImageName.Buffer;
            BYTE* nameEnd = nameStart + spi->ImageName.Length;

            // Only touch the name if it lives inside the buffer we own.
            if (nameStart >= bufStart && nameEnd <= bufEnd) {
                UNICODE_STRING name = spi->ImageName;
                for (size_t i = 0; i < numBlocked; i++) {
                    UNICODE_STRING blocked;
                    myRtlInitUnicodeString(&blocked, blockedProcesses[i]);

                    if (fpRtlEqualUnicodeString && fpRtlEqualUnicodeString(&name, &blocked, TRUE)) {
                        // Blank the name in place -- no structural change to the list.
                        RtlZeroMemory(spi->ImageName.Buffer, spi->ImageName.Length);
                        spi->ImageName.Length = 0;
                        break;
                    }
                }
            }
        }

        if (spi->NextEntryOffset == 0)
            break;

        BYTE* next = (BYTE*)spi + spi->NextEntryOffset;
        if (next <= (BYTE*)spi || next + sizeof(SYSTEM_PROCESS_INFORMATION) > bufEnd)
            break; // corrupt/last -- stop safely

        spi = (PSYSTEM_PROCESS_INFORMATION)next;
    }

    return status;
}


void patchProcessNameChecks() {
    //NtQuerySystemInformation 

    fpRtlEqualUnicodeString = (RtlEqualUnicodeString_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlEqualUnicodeString");
    HMODULE dll = GetModuleHandleA("ntdll.dll");
    if (!dll) {
        LOG("Failed to get address of ntdll.dll");
        return;
    }
    FARPROC pFunc = GetProcAddress(dll, "NtQuerySystemInformation");
    if (!pFunc) {
        LOG("Failed to get address of NtQuerySystemInformation");
        return;
    }

    if (MH_CreateHook(pFunc, &NtQuerySystemInformation_hookfunc, reinterpret_cast<LPVOID*>(&fpNtQuerySystemInformation)) != MH_OK) {
        LOG("Failed to create hook for NtQuerySystemInformation");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtQuerySystemInformation");
        return;
    }
}
typedef PVOID(NTAPI* AddVectoredExceptionHandler_t)(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler);
AddVectoredExceptionHandler_t fpAddVectoredExceptionHandler = nullptr;


//for (int offset = 0; offset >= -424; offset -= 8) {
//    uintptr_t addr = rsp + offset;
//    void* value = *(void**)addr;
//
//    LOG("[rsp%+d] @ %p = %p", offset, (void*)addr, value);
//}

const char* exceptionCodeToString(DWORD code) {
    switch (code) {
    case EXCEPTION_ACCESS_VIOLATION:         return "EXCEPTION_ACCESS_VIOLATION";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:    return "EXCEPTION_ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_BREAKPOINT:               return "EXCEPTION_BREAKPOINT";
    case EXCEPTION_DATATYPE_MISALIGNMENT:    return "EXCEPTION_DATATYPE_MISALIGNMENT";
    case EXCEPTION_FLT_DENORMAL_OPERAND:     return "EXCEPTION_FLT_DENORMAL_OPERAND";
    case EXCEPTION_FLT_DIVIDE_BY_ZERO:       return "EXCEPTION_FLT_DIVIDE_BY_ZERO";
    case EXCEPTION_FLT_INEXACT_RESULT:       return "EXCEPTION_FLT_INEXACT_RESULT";
    case EXCEPTION_FLT_INVALID_OPERATION:    return "EXCEPTION_FLT_INVALID_OPERATION";
    case EXCEPTION_FLT_OVERFLOW:             return "EXCEPTION_FLT_OVERFLOW";
    case EXCEPTION_FLT_STACK_CHECK:          return "EXCEPTION_FLT_STACK_CHECK";
    case EXCEPTION_FLT_UNDERFLOW:            return "EXCEPTION_FLT_UNDERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION:      return "EXCEPTION_ILLEGAL_INSTRUCTION";
    case EXCEPTION_IN_PAGE_ERROR:            return "EXCEPTION_IN_PAGE_ERROR";
    case EXCEPTION_INT_DIVIDE_BY_ZERO:       return "EXCEPTION_INT_DIVIDE_BY_ZERO";
    case EXCEPTION_INT_OVERFLOW:             return "EXCEPTION_INT_OVERFLOW";
    case EXCEPTION_INVALID_DISPOSITION:      return "EXCEPTION_INVALID_DISPOSITION";
    case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "EXCEPTION_NONCONTINUABLE_EXCEPTION";
    case EXCEPTION_PRIV_INSTRUCTION:         return "EXCEPTION_PRIV_INSTRUCTION";
    case EXCEPTION_SINGLE_STEP:              return "EXCEPTION_SINGLE_STEP";
    case EXCEPTION_STACK_OVERFLOW:           return "EXCEPTION_STACK_OVERFLOW";
    default:                                return "UNKNOWN_EXCEPTION";
    }
}

//hot damn
typedef LONG (CALLBACK* ARX_VEH_t)(PEXCEPTION_POINTERS info);
ARX_VEH_t ARX_VEH = nullptr;

LONG CALLBACK testVectoredHandler(PEXCEPTION_POINTERS info) {
    //LOG("VEH DEBUG CLEAR");
    CONTEXT* ctx = info->ContextRecord;
   // auto originalDr0 = ctx->Dr0;
   // auto originalDr1 = ctx->Dr1;
   // auto originalDr2 = ctx->Dr2;
   // auto originalDr3 = ctx->Dr3;
   // auto originalDr6 = ctx->Dr6;
   // auto originalDr7 = ctx->Dr7;
   // 
   // ctx->Dr0 = 0;
   // ctx->Dr1 = 0;
   // ctx->Dr2 = 0;
   // ctx->Dr3 = 0;
   // ctx->Dr6 = 0;
   // ctx->Dr7 = 0;

    DWORD code = info->ExceptionRecord->ExceptionCode;
   // LOG("ARXAN VEH TRIGGERED BY [%s]", exceptionCodeToString(code));
    LONG result = ARX_VEH(info);
    //LOG("Returned %lu", result);
    //ctx->Dr0 = originalDr0;
    //ctx->Dr1 = originalDr1;
    //ctx->Dr2 = originalDr2;
    //ctx->Dr3 = originalDr3;
    //ctx->Dr6 = originalDr6;
    //ctx->Dr7 = originalDr7;
    return result;
}

//fire that this actually works to wrap every veh easily.
//i need thread safety tho cuz this could EASILY fuck up control flow and die if two threads use the VEH and overwrite the func addr
PVOID AddVectoredExceptionHandler_hookfunc(ULONG First, PVECTORED_EXCEPTION_HANDLER Handler) {
    ARX_VEH = Handler;
    return fpAddVectoredExceptionHandler(First, testVectoredHandler);
}

#ifdef DEVELOPMENT_BUILD
void superTestVEH() {
    DEV_ONLY_FUNCTION();
    HMODULE dll = GetModuleHandleA("kernel32.dll");
    if (!dll) {
        return;
    }
    FARPROC pFunc = GetProcAddress(dll, "AddVectoredExceptionHandler");
    if (!pFunc) {
        LOG("Failed to get address of AddVectoredExceptionHandler");
        return;
    }

    if (MH_CreateHook(pFunc, &AddVectoredExceptionHandler_hookfunc, reinterpret_cast<LPVOID*>(&fpAddVectoredExceptionHandler)) != MH_OK) {
        LOG("Failed to create hook for AddVectoredExceptionHandler");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for AddVectoredExceptionHandler");
        return;
    }
}
#endif

typedef BOOL(NTAPI* VirtualProtect_t)(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect);
VirtualProtect_t fpVirtualProtect = nullptr;

bool containsAddress(LPCVOID lpAddress, SIZE_T dwSize, const void* target) {
    auto start = reinterpret_cast<uintptr_t>(lpAddress);
    auto addr = reinterpret_cast<uintptr_t>(target);

    // Empty range.
    if (dwSize == 0)
        return false;

    // Overflow-safe check: addr - start < size, but only after addr >= start.
    return addr >= start && (addr - start) < dwSize;
}
void* dbgBreakPoint = nullptr;
BOOL VirtualProtect_hookfunc(LPVOID lpAddress, SIZE_T dwSize, DWORD flNewProtect, PDWORD lpflOldProtect) {
   // if (containsAddress(lpAddress, dwSize, dbgBreakPoint)) {
   //     LOG("RANGE CONTAINS TARGET");
   //     return fpVirtualProtect(lpAddress, dwSize, PAGE_EXECUTE_READWRITE, lpflOldProtect);
   // }
    return fpVirtualProtect(lpAddress, dwSize, flNewProtect, lpflOldProtect);
}

#ifdef DEVELOPMENT_BUILD
void vpHook() {
    DEV_ONLY_FUNCTION();
    HMODULE ntdll = GetModuleHandleA("ntdll.dll");
    dbgBreakPoint = GetProcAddress(ntdll, "DbgBreakPoint");

    HMODULE dll = GetModuleHandleA("kernel32.dll");
    if (!dll) {
        return;
    }
    FARPROC pFunc = GetProcAddress(dll, "VirtualProtect");
    if (!pFunc) {
        LOG("Failed to get address of VirtualProtect");
        return;
    }

    if (MH_CreateHook(pFunc, &VirtualProtect_hookfunc, reinterpret_cast<LPVOID*>(&fpVirtualProtect)) != MH_OK) {
        LOG("Failed to create hook for VirtualProtect");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for VirtualProtect");
        return;
    }
    
}
#endif

typedef NTSTATUS(NTAPI* NtGetContextThread_t)(HANDLE ThreadHandle, PCONTEXT ThreadContext);
NtGetContextThread_t fpNtGetContextThread = nullptr;

NTSTATUS NtGetContextThread_hookfunc( HANDLE hThread, PCONTEXT lpContext) {
    NTSTATUS status = fpNtGetContextThread(hThread, lpContext);

    if (!lpContext) {
        return status;
    }

    DWORD flags = lpContext->ContextFlags;

    if ((flags & CONTEXT_DEBUG_REGISTERS) == CONTEXT_DEBUG_REGISTERS || (flags & CONTEXT_ALL) == CONTEXT_ALL) {
       // LOG("NTGCT: Clearing Debug Registers");
        lpContext->Dr0 = 0;
        lpContext->Dr1 = 0;
        lpContext->Dr2 = 0;
        lpContext->Dr3 = 0;
        lpContext->Dr6 = 0;
        lpContext->Dr7 = 0;
    }

    return status;
}

#ifdef DEVELOPMENT_BUILD
void ntgctHook() {
    DEV_ONLY_FUNCTION();
    HMODULE dll = GetModuleHandleA("ntdll.dll");
    if (!dll) {
        return;
    }
    FARPROC pFunc = GetProcAddress(dll, "NtGetContextThread");
    if (!pFunc) {
        LOG("Failed to get address of NtGetContextThread");
        return;
    }

    if (MH_CreateHook(pFunc, &NtGetContextThread_hookfunc, reinterpret_cast<LPVOID*>(&fpNtGetContextThread)) != MH_OK) {
        LOG("Failed to create hook for NtGetContextThread");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtGetContextThread");
        return;
    }
}
#endif

#ifdef DEVELOPMENT_BUILD
typedef __analysis_noreturn VOID(NTAPI* FatalExit_t)(int ExitCode);
FatalExit_t fpFatalExit = nullptr;

VOID FatalExit_hookfunc(int ExitCode) {
    LOG("ARXAN IS UPSET :(");
    LOG("FatalExit called with exit code %d", ExitCode);
    LOG("What now?");
    Sleep(99999999); //this wont work. Maybe force RIP to stick
}

//can be triggered by those nt functions that get patched with fatalexit
void fatalExitThing() {
    DEV_ONLY_FUNCTION(); //this also impacts some normal game exits so dont ship this. Just for testing just like with ceg
    HMODULE dll = GetModuleHandleA("kernel32.dll");
    if (!dll) {
        return;
    }
    FARPROC pFunc = GetProcAddress(dll, "FatalExit");
    if (!pFunc) {
        LOG("Failed to get address of FatalExit");
        return;
    }

    if (MH_CreateHook(pFunc, &FatalExit_hookfunc, reinterpret_cast<LPVOID*>(&fpFatalExit)) != MH_OK) {
        LOG("Failed to create hook for FatalExit");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for FatalExit");
        return;
    }
}
#endif

typedef NTSYSAPI VOID(NTAPI* RtlCaptureContext2_t)(PCONTEXT ContextRecord);
RtlCaptureContext2_t fpRtlCaptureContext2 = nullptr;

VOID RtlCaptureContext2_hookfunc(PCONTEXT ContextRecord) {
    //LOG("RCC2: Clearing Debug Registers");
    fpRtlCaptureContext2(ContextRecord);
    ContextRecord->Dr0 = 0;
    ContextRecord->Dr1 = 0;
    ContextRecord->Dr2 = 0;
    ContextRecord->Dr3 = 0;
    ContextRecord->Dr6 = 0;
    ContextRecord->Dr7 = 0;

}
#ifdef DEVELOPMENT_BUILD
void captureContextTest() {
    DEV_ONLY_FUNCTION();
    HMODULE dll = GetModuleHandleA("ntdll.dll");
    if (!dll) {
        return;
    }
    //arxan uses the 2 verison
    FARPROC pFunc = GetProcAddress(dll, "RtlCaptureContext2");
    if (!pFunc) {
        LOG("Failed to get address of RtlCaptureContext2");
        return;
    }

    if (MH_CreateHook(pFunc, &RtlCaptureContext2_hookfunc, reinterpret_cast<LPVOID*>(&fpRtlCaptureContext2)) != MH_OK) {
        LOG("Failed to create hook for RtlCaptureContext2");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for RtlCaptureContext2");
        return;
    }
}
#endif

typedef NTSYSAPI VOID(NTAPI* NtContinue_t)(PCONTEXT ThreadContext, BOOLEAN RaiseAlert);
NtContinue_t fpNtContinue = nullptr;

VOID NtContinue_hookfunc(PCONTEXT ThreadContext, BOOLEAN RaiseAlert) {
   // LOG("NtContinue %s", RaiseAlert ? "Alert" : "No Alert");

    fpNtContinue(ThreadContext, RaiseAlert);

}
#ifdef DEVELOPMENT_BUILD
void ntContinueTest() {
    DEV_ONLY_FUNCTION();
    HMODULE dll = GetModuleHandleA("ntdll.dll");
    if (!dll) {
        return;
    }
    //arxan uses the 2 verison
    FARPROC pFunc = GetProcAddress(dll, "NtContinue");
    if (!pFunc) {
        LOG("Failed to get address of NtContinue");
        return;
    }

    if (MH_CreateHook(pFunc, &NtContinue_hookfunc, reinterpret_cast<LPVOID*>(&fpNtContinue)) != MH_OK) {
        LOG("Failed to create hook for NtContinue");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for NtContinue");
        return;
    }
}
#endif

typedef DWORD(NTAPI* ResumeThread_t)(HANDLE hThread);
ResumeThread_t fpResumeThread = nullptr;
//looks like my theory was correct >:)
DWORD ResumeThread_hookfunc(HANDLE hThread) {
    PVOID startAddress = nullptr;
    NTSTATUS status = fpNtQueryInformationThread(hThread, static_cast<THREAD_INFORMATION_CLASS>(9), &startAddress, sizeof(startAddress), nullptr);
    if (reinterpret_cast<uintptr_t>(startAddress) == 0x1BF4E0_b) {//s2_mp64_ship.exe 2.20 specific offset btw so yeah
        //also like i dont need to suspend this myself the first time cuz it was already suspended by arxan? maybe on startup as a test or something while it patches the nt user stubs
        return 0; //dont resume
    }
    return fpResumeThread(hThread);
}

//there is one thread that is constantly monitoring the NtDbg functions since it places fatal exits on them.
//attempting to modify the code is protected by a thread which will kill the game if modification detected.
//suspending the thread results in it being resumed and the game killed. Theory is that if i dont allow it to resume the thread but also tell it 
//that the thread was never suspended then we might be bing chillin.
void resumeThreadHook() {
    HMODULE dll = GetModuleHandleA("kernel32.dll");
    if (!dll) {
        return;
    }
    FARPROC pFunc = GetProcAddress(dll, "ResumeThread");
    if (!pFunc) {
        LOG("Failed to get address of ResumeThread");
        return;
    }

    if (MH_CreateHook(pFunc, &ResumeThread_hookfunc, reinterpret_cast<LPVOID*>(&fpResumeThread)) != MH_OK) {
        LOG("Failed to create hook for ResumeThread");
        return;
    }

    if (MH_EnableHook(pFunc) != MH_OK) {
        LOG("Failed to enable hook for ResumeThread");
        return;
    }
}


struct SavedStub {
    const char* name;
    void* addr;
    std::vector<BYTE> bytes;
};

static std::vector<SavedStub> g_savedStubs;

constexpr SIZE_T STUB_SAVE_SIZE = 32;

void captureNtdllDebugStubs() {
    const char* names[] = {
        "DbgBreakPoint",
        "DbgUiContinue",
        "DbgUiConnectToDbg",
        "DbgPrintReturnControlC",
        "DbgPrompt",
        "DbgUiConvertStateChangeStructure",
        "DbgUiDebugActiveProcess",
        "DbgUiGetThreadDebugObject",
        "DbgUiIssueRemoteBreakin",
        "DbgUiRemoteBreakin",
        "DbgUiSetThreadDebugObject",
        "DbgUiStopDebugging",
        "DbgUiWaitStateChange",
        "DbgUserBreakPoint"
    };

    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) {
        return;
    }

    g_savedStubs.clear();

    for (auto name : names) {
        void* p = GetProcAddress(ntdll, name);
        if (!p) {
            continue;
        }

        SavedStub s;
        s.name = name;
        s.addr = p;
        s.bytes.assign((BYTE*)p, (BYTE*)p + STUB_SAVE_SIZE);

        g_savedStubs.push_back(std::move(s));
    }
}

void DebugPatches::repairNtUserStubs() {
    for (auto& s : g_savedStubs) {
        DWORD oldProtect;

        if (VirtualProtect(s.addr, s.bytes.size(), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            memcpy(s.addr, s.bytes.data(), s.bytes.size());
            FlushInstructionCache(GetCurrentProcess(), s.addr, s.bytes.size());

            DWORD ignored;
            VirtualProtect(s.addr, s.bytes.size(), oldProtect, &ignored);
        }
    }
}


//run from dll main to get pre unpacking hooks set
//crc checks are not patched at this point so syscalls only!
void DebugPatches::earlyInit() {
    MH_Initialize();
    captureNtdllDebugStubs(); //right away

    //this crashes on kpops machine & kamas 
    //patchProcessNameChecks();// run this first cuz of shared fpNtQuerySystemInformation usage

    freeIdaMutants(); //works, but how can we prevent it from ever happening
    disableProgramScans();
    bypassHwndChecks();
    hookNtQueryInformationProcess();
    bypassHiddenThreadCreation();
    bypassThreadHideFromDebugger();
    bypassThreadQueryHideFromDebugger();

   // superTestVEH();
    //vpHook();
    //ntgctHook();
    //fatalExitThing();
    
    //one of these causing zombies lua to die
    //captureContextTest();
    //ntContinueTest();

    resumeThreadHook(); //hell yeah
    
}



void DebugPatches::init() {
    DEV_INIT_PRINT();
    // Hide CheatEngine (and debuggers) from the game's process-list scans. Re-enabled here
    // in the LATE init -- once the game window exists and Arxan's early unpacking is done --
    // rather than in earlyInit, where the OLD entry-removal version crashed on some machines.
    // The rewritten hook only blanks names in place (no buffer surgery), so it's launch-safe.
    // Runs first so fpNtQuerySystemInformation is set for freeIdaMutants below.
    patchProcessNameChecks();
    freeIdaMutants(); //works, but how can we prevent it from ever happening
    hookNtClose();
    repairNtUserStubs();//lovely
}