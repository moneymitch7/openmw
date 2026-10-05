#include "windowscrashcatcher.hpp"

#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <sstream>
#include <thread>

#include <SDL_messagebox.h>

#include <components/misc/strings/conversion.hpp>
#include <components/version/version.hpp>

#include "windowscrashdumppathhelpers.hpp"
#include "windowscrashmonitor.hpp"
#include "windowscrashshm.hpp"

namespace Crash
{
    namespace
    {
        template <class T, std::size_t N>
        void writePathToShm(T (&buffer)[N], const std::filesystem::path& path)
        {
            memset(buffer, 0, sizeof(buffer));
            const auto str = path.u8string();
            size_t length = str.length();
            if (length >= sizeof(buffer))
                length = sizeof(buffer) - 1;
            strncpy_s(buffer, sizeof(buffer), Misc::StringUtils::u8StringToString(str).c_str(), length);
        }

        // Crash report (openmw-crash.txt next to the dump), written by the crashing process itself before the monitor
        // is asked for a dump: the dump does not always appear (the process can end before the monitor gets to it),
        // this still says where the crash was. Only Win32 calls and static buffers (the heap may be what broke, and
        // a stack overflow leaves little stack).
        char sReportText[16384];
        int sReportLength = 0;
        wchar_t sReportPath[MAX_LONG_PATH + MAX_FILENAME + 8];
        char sModuleName[MAX_PATH];

        void appendReport(const char* format, ...)
        {
            if (sReportLength >= static_cast<int>(sizeof(sReportText)) - 1)
                return;
            va_list args;
            va_start(args, format);
            const int written
                = std::vsnprintf(sReportText + sReportLength, sizeof(sReportText) - sReportLength, format, args);
            va_end(args);
            if (written > 0)
                sReportLength = std::min(sReportLength + written, static_cast<int>(sizeof(sReportText)) - 1);
        }

        // module+offset, which the build's .pdb turns into a function and line
        void appendAddress(DWORD_PTR address)
        {
            HMODULE module = nullptr;
            if (GetModuleHandleExA(
                    GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                    reinterpret_cast<LPCSTR>(address), &module)
                && module != nullptr)
            {
                sModuleName[0] = '\0';
                const DWORD length = GetModuleFileNameA(module, sModuleName, sizeof(sModuleName));
                const char* name = length > 0 ? sModuleName : "?";
                for (DWORD i = 0; i < length && i < sizeof(sModuleName); ++i)
                    if (sModuleName[i] == '\\' || sModuleName[i] == '/')
                        name = sModuleName + i + 1;
                appendReport(
                    "%s+0x%llx", name, static_cast<unsigned long long>(address - reinterpret_cast<DWORD_PTR>(module)));
            }
            else
                appendReport("0x%llx", static_cast<unsigned long long>(address));
        }

        // An address in a loaded module's code (not JIT code, which nothing can symbolize anyway).
        bool isModuleCode(DWORD_PTR address)
        {
            if (address < 0x10000)
                return false;
            MEMORY_BASIC_INFORMATION info;
            if (VirtualQuery(reinterpret_cast<LPCVOID>(address), &info, sizeof(info)) == 0)
                return false;
            if (info.State != MEM_COMMIT || info.Type != MEM_IMAGE)
                return false;
            return (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))
                != 0;
        }

#if defined(_M_X64)
        CONTEXT sUnwindContext;

        void appendUnwoundStack(const CONTEXT& crashed)
        {
            sUnwindContext = crashed;
            CONTEXT& context = sUnwindContext;
            for (int frame = 0; frame < 64 && context.Rip != 0; ++frame)
            {
                appendReport("  %2d  ", frame);
                appendAddress(context.Rip);
                appendReport("\n");
                DWORD64 imageBase = 0;
                PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
                if (function == nullptr)
                {
                    // leaf function, or code without unwind data: the return address is on top of the stack
                    context.Rip = *reinterpret_cast<const DWORD64*>(context.Rsp);
                    context.Rsp += 8;
                }
                else
                {
                    PVOID handlerData = nullptr;
                    DWORD64 establisherFrame = 0;
                    RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, function, &context, &handlerData,
                        &establisherFrame, nullptr);
                }
            }
        }

        // Every value on the crashed thread's stack that points into module code: catches the callers the unwinder
        // can't step over (JIT-compiled Lua, corrupted frames).
        void appendStackScan(DWORD64 stackPointer)
        {
            const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
            const DWORD64 stackTop = reinterpret_cast<DWORD64>(tib->StackBase);
            int found = 0;
            for (DWORD64 p = stackPointer & ~DWORD64(7); p + 8 <= stackTop && found < 96; p += 8)
            {
                const DWORD64 value = *reinterpret_cast<const DWORD64*>(p);
                if (!isModuleCode(value))
                    continue;
                appendReport("  rsp+0x%04llx  ", static_cast<unsigned long long>(p - stackPointer));
                appendAddress(value);
                appendReport("\n");
                ++found;
            }
        }

        void appendStacks(const CONTEXT& crashed)
        {
            appendReport("\nStack:\n");
            __try
            {
                appendUnwoundStack(crashed);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                appendReport("  (stack walk stopped by an invalid frame)\n");
            }
            appendReport("\nModule code addresses on the stack:\n");
            __try
            {
                appendStackScan(crashed.Rsp);
            }
            __except (EXCEPTION_EXECUTE_HANDLER)
            {
                appendReport("  (stack scan stopped)\n");
            }
        }
#endif

        DWORD executableTimestamp()
        {
            const auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
            if (base == nullptr)
                return 0;
            const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
            const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
            return nt->FileHeader.TimeDateStamp;
        }

        // The report goes next to the dump: <dump directory>\<dump name without .dmp>.txt
        bool makeReportPath(const CrashSHM& shm)
        {
            const int directoryLength
                = MultiByteToWideChar(CP_UTF8, 0, shm.mStartup.mDumpDirectoryPath, -1, sReportPath, MAX_LONG_PATH);
            if (directoryLength <= 0)
                return false;
            int pos = directoryLength - 1; // without the terminator
            if (pos > 0 && sReportPath[pos - 1] != L'\\' && sReportPath[pos - 1] != L'/')
                sReportPath[pos++] = L'\\';
            const int nameLength
                = MultiByteToWideChar(CP_UTF8, 0, shm.mStartup.mCrashDumpFileName, -1, sReportPath + pos, MAX_FILENAME);
            if (nameLength <= 1)
                return false;
            pos += nameLength - 1;
            if (pos >= 4 && sReportPath[pos - 4] == L'.')
                pos -= 4;
            wmemcpy(sReportPath + pos, L".txt", 5);
            return true;
        }

        bool writeCrashReport(const CrashSHM& shm, PEXCEPTION_POINTERS info)
        {
            sReportLength = 0;
            const EXCEPTION_RECORD& record = *info->ExceptionRecord;

            SYSTEMTIME time;
            GetLocalTime(&time);
            const std::string_view version = Version::getVersion();
            const std::string_view commit = Version::getCommitHash();
            appendReport("OpenMW crash report %04u-%02u-%02u %02u:%02u:%02u\n", time.wYear, time.wMonth, time.wDay,
                time.wHour, time.wMinute, time.wSecond);
            appendReport("Version %.*s, revision %.*s, openmw.exe timestamp 0x%08lx\n",
                static_cast<int>(version.size()), version.data(), static_cast<int>(commit.size()), commit.data(),
                executableTimestamp());

            appendReport("\nException 0x%08lx at ", record.ExceptionCode);
            appendAddress(reinterpret_cast<DWORD_PTR>(record.ExceptionAddress));
            appendReport("\n");
            if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2)
            {
                const ULONG_PTR kind = record.ExceptionInformation[0];
                appendReport("  %s address 0x%llx\n", kind == 0 ? "reading" : (kind == 8 ? "executing" : "writing"),
                    static_cast<unsigned long long>(record.ExceptionInformation[1]));
            }
            const DWORD threadId = GetCurrentThreadId();
            appendReport("Thread %lu%s\n", threadId, threadId == shm.mStartup.mAppMainThreadId ? " (main thread)" : "");

#if defined(_M_X64)
            appendStacks(*info->ContextRecord);
#endif

            if (!makeReportPath(shm))
                return false;
            HANDLE file = CreateFileW(
                sReportPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file == INVALID_HANDLE_VALUE)
                return false;
            DWORD written = 0;
            const BOOL ok = WriteFile(file, sReportText, static_cast<DWORD>(sReportLength), &written, nullptr);
            FlushFileBuffers(file);
            CloseHandle(file);
            return ok != FALSE;
        }
    }

    HANDLE duplicateHandle(HANDLE handle)
    {
        HANDLE duplicate;
        if (!DuplicateHandle(
                GetCurrentProcess(), handle, GetCurrentProcess(), &duplicate, 0, TRUE, DUPLICATE_SAME_ACCESS))
        {
            throw std::runtime_error("Crash monitor could not duplicate handle");
        }
        return duplicate;
    }

    CrashCatcher* CrashCatcher::sInstance = nullptr;

    CrashCatcher::CrashCatcher(int argc, char** argv, const std::filesystem::path& dumpPath,
        const std::filesystem::path& crashDumpName, const std::filesystem::path& freezeDumpName)
    {
        assert(sInstance == nullptr); // don't allow two instances

        sInstance = this;

        HANDLE shmHandle = nullptr;
        for (int i = 0; i < argc; ++i)
        {
            if (strcmp(argv[i], "--crash-monitor"))
                continue;

            if (i >= argc - 1)
                throw std::runtime_error("Crash monitor is missing the SHM handle argument");

            sscanf(argv[i + 1], "%p", &shmHandle);
            break;
        }

        if (!shmHandle)
        {
            setupIpc();
            startMonitorProcess(dumpPath, crashDumpName, freezeDumpName);
            installHandler();
        }
        else
        {
            CrashMonitor(shmHandle).run();
            exit(0);
        }
    }

    CrashCatcher::~CrashCatcher()
    {
        sInstance = nullptr;

        if (mShm && mSignalMonitorEvent)
        {
            shmLock();
            mShm->mEvent = CrashSHM::Event::Shutdown;
            shmUnlock();

            SetEvent(mSignalMonitorEvent);
        }

        if (mShmHandle)
            CloseHandle(mShmHandle);
    }

    void CrashCatcher::updateDumpPath(const std::filesystem::path& dumpPath)
    {
        shmLock();

        writePathToShm(mShm->mStartup.mDumpDirectoryPath, dumpPath);

        shmUnlock();
    }

    void CrashCatcher::updateDumpNames(
        const std::filesystem::path& crashDumpName, const std::filesystem::path& freezeDumpName)
    {
        shmLock();

        writePathToShm(mShm->mStartup.mCrashDumpFileName, crashDumpName);
        writePathToShm(mShm->mStartup.mFreezeDumpFileName, freezeDumpName);

        shmUnlock();
    }

    void CrashCatcher::setupIpc()
    {
        SECURITY_ATTRIBUTES attributes;
        ZeroMemory(&attributes, sizeof(attributes));
        attributes.bInheritHandle = TRUE;

        mSignalAppEvent = CreateEventW(&attributes, FALSE, FALSE, NULL);
        mSignalMonitorEvent = CreateEventW(&attributes, FALSE, FALSE, NULL);

        mShmHandle = CreateFileMappingW(INVALID_HANDLE_VALUE, &attributes, PAGE_READWRITE, HIWORD(sizeof(CrashSHM)),
            LOWORD(sizeof(CrashSHM)), NULL);
        if (mShmHandle == nullptr)
            throw std::runtime_error("Failed to allocate crash catcher shared memory");

        mShm = reinterpret_cast<CrashSHM*>(MapViewOfFile(mShmHandle, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(CrashSHM)));
        if (mShm == nullptr)
            throw std::runtime_error("Failed to map crash catcher shared memory");

        mShm->mMonitorStatus = CrashSHM::Status::Uninitialised;

        mShmMutex = CreateMutexW(&attributes, FALSE, NULL);
        if (mShmMutex == nullptr)
            throw std::runtime_error("Failed to create crash catcher shared memory mutex");
    }

    void CrashCatcher::shmLock()
    {
        if (WaitForSingleObject(mShmMutex, CrashCatcherTimeout) != WAIT_OBJECT_0)
            throw std::runtime_error("SHM lock timed out");
    }

    void CrashCatcher::shmUnlock()
    {
        ReleaseMutex(mShmMutex);
    }

    void CrashCatcher::waitMonitor()
    {
        if (!waitMonitorNoThrow())
            throw std::runtime_error("Waiting for monitor failed");
    }

    bool CrashCatcher::waitMonitorNoThrow()
    {
        return WaitForSingleObject(mSignalAppEvent, CrashCatcherTimeout) == WAIT_OBJECT_0;
    }

    void CrashCatcher::signalMonitor()
    {
        SetEvent(mSignalMonitorEvent);
    }

    void CrashCatcher::installHandler()
    {
        SetUnhandledExceptionFilter(vectoredExceptionHandler);
    }

    void CrashCatcher::startMonitorProcess(const std::filesystem::path& dumpPath,
        const std::filesystem::path& crashDumpName, const std::filesystem::path& freezeDumpName)
    {
        std::wstring executablePath;
        DWORD copied = 0;
        do
        {
            executablePath.resize(executablePath.size() + MAX_PATH);
            copied = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
        } while (GetLastError() == ERROR_INSUFFICIENT_BUFFER);
        executablePath.resize(copied);

        writePathToShm(mShm->mStartup.mDumpDirectoryPath, dumpPath);
        writePathToShm(mShm->mStartup.mCrashDumpFileName, crashDumpName);
        writePathToShm(mShm->mStartup.mFreezeDumpFileName, freezeDumpName);

        // note that we don't need to lock the SHM here, the other process has not started yet
        mShm->mEvent = CrashSHM::Event::Startup;
        mShm->mStartup.mShmMutex = duplicateHandle(mShmMutex);
        mShm->mStartup.mAppProcessHandle = duplicateHandle(GetCurrentProcess());
        mShm->mStartup.mAppMainThreadId = GetThreadId(GetCurrentThread());
        mShm->mStartup.mSignalApp = duplicateHandle(mSignalAppEvent);
        mShm->mStartup.mSignalMonitor = duplicateHandle(mSignalMonitorEvent);

        std::wstringstream ss;
        ss << "--crash-monitor " << std::hex << duplicateHandle(mShmHandle);
        std::wstring arguments(ss.str());

        STARTUPINFOW si;
        ZeroMemory(&si, sizeof(si));

        PROCESS_INFORMATION pi;
        ZeroMemory(&pi, sizeof(pi));

        if (!CreateProcessW(executablePath.data(), arguments.data(), NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
            throw std::runtime_error("Could not start crash monitor process");

        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);

        waitMonitor();
    }

    LONG CrashCatcher::vectoredExceptionHandler(PEXCEPTION_POINTERS info)
    {
        switch (info->ExceptionRecord->ExceptionCode)
        {
            case EXCEPTION_SINGLE_STEP:
            case EXCEPTION_BREAKPOINT:
            case DBG_PRINTEXCEPTION_C:
                return EXCEPTION_EXECUTE_HANDLER;
        }
        if (!sInstance)
            return EXCEPTION_EXECUTE_HANDLER;

        sInstance->handleVectoredException(info);

        _Exit(1);
    }

    void CrashCatcher::handleVectoredException(PEXCEPTION_POINTERS info)
    {
        // Written first, by this process: whatever happens to the dump below, the report says where the crash was.
        const bool reportWritten = writeCrashReport(*mShm, info);

        // Nothing below may throw: this runs inside the unhandled exception filter, and an exception escaping it ends
        // the process on the spot (no dump, no message).
        if (WaitForSingleObject(mShmMutex, CrashCatcherTimeout) == WAIT_OBJECT_0)
        {
            mShm->mEvent = CrashSHM::Event::Crashed;
            mShm->mCrashed.mThreadId = GetCurrentThreadId();
            mShm->mCrashed.mContext = *info->ContextRecord;
            mShm->mCrashed.mExceptionRecord = *info->ExceptionRecord;

            shmUnlock();

            signalMonitor();
        }

        // Wait for the dump. The monitor holds the SHM lock while it writes it and signals when it's done; a big dump
        // takes longer than one wait, so keep waiting while it's dumping (up to about a minute). As we're suspended,
        // a wait might time out even if it's successful, so mMonitorStatus is the source of truth.
        CrashSHM::Status monitorStatus = CrashSHM::Status::Monitoring;
        for (int attempt = 0; attempt < 12; ++attempt)
        {
            waitMonitorNoThrow();
            if (WaitForSingleObject(mShmMutex, CrashCatcherTimeout) != WAIT_OBJECT_0)
            {
                monitorStatus = CrashSHM::Status::Dumping; // still writing it
                continue;
            }
            monitorStatus = mShm->mMonitorStatus;
            shmUnlock();
            if (monitorStatus != CrashSHM::Status::Dumping)
                break; // dumped, failed, or the monitor isn't responding at all
        }

        try
        {
            const std::string report = reportWritten ? "\nCrash report saved to '"
                    + Misc::StringUtils::u8StringToString(std::filesystem::path(sReportPath).u8string()) + "'."
                                                     : std::string();
            if (monitorStatus == CrashSHM::Status::DumpedSuccessfully)
            {
                std::string message = "OpenMW has encountered a fatal error.\nCrash dump saved to '"
                    + Misc::StringUtils::u8StringToString(getCrashDumpPath(*mShm).u8string()) + "'." + report
                    + "\nPlease report this to https://gitlab.com/OpenMW/openmw/issues !";
                SDL_ShowSimpleMessageBox(0, "Fatal Error", message.c_str(), nullptr);
            }
            else
            {
                std::string message = std::string("OpenMW has encountered a fatal error.\n")
                    + (monitorStatus == CrashSHM::Status::Dumping ? "Timed out while creating crash dump."
                                                                  : "No crash dump could be written.")
                    + report;
                SDL_ShowSimpleMessageBox(0, "Fatal Error", message.c_str(), nullptr);
            }
        }
        catch (...)
        {
        }
    }

} // namespace Crash
