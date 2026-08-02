#include <windows.h>
#include <dbghelp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

struct DumpState {
    void* base{};
    MINIDUMP_MEMORY64_LIST* memory64{};
    MINIDUMP_MEMORY_LIST* memory{};
};

static DumpState const* g_dumpState = nullptr;

static bool ReadDumpMemory(DumpState const& dump, DWORD64 address, void* buffer, DWORD size) {
    auto* out = static_cast<std::byte*>(buffer);
    if (dump.memory64 != nullptr) {
        auto rva = dump.memory64->BaseRva;
        for (ULONG64 i = 0; i < dump.memory64->NumberOfMemoryRanges; ++i) {
            auto const& range = dump.memory64->MemoryRanges[i];
            if (address >= range.StartOfMemoryRange && address + size <= range.StartOfMemoryRange + range.DataSize) {
                auto const offset = rva + (address - range.StartOfMemoryRange);
                std::copy_n(static_cast<std::byte*>(dump.base) + offset, size, out);
                return true;
            }
            rva += range.DataSize;
        }
    }
    if (dump.memory != nullptr) {
        for (ULONG i = 0; i < dump.memory->NumberOfMemoryRanges; ++i) {
            auto const& range = dump.memory->MemoryRanges[i];
            auto const start = range.StartOfMemoryRange;
            auto const bytes = range.Memory.DataSize;
            if (address >= start && address + size <= start + bytes) {
                auto const offset = range.Memory.Rva + (address - start);
                std::copy_n(static_cast<std::byte*>(dump.base) + offset, size, out);
                return true;
            }
        }
    }
    return false;
}

static BOOL CALLBACK ReadMemoryRoutine(HANDLE, DWORD64 baseAddress, PVOID buffer, DWORD size, LPDWORD bytesRead) {
    if (g_dumpState != nullptr && ReadDumpMemory(*g_dumpState, baseAddress, buffer, size)) {
        *bytesRead = size;
        return TRUE;
    }
    *bytesRead = 0;
    return FALSE;
}

static std::wstring ReadMiniString(void* base, RVA rva) {
    if (rva == 0) {
        return {};
    }
    auto const* s = reinterpret_cast<MINIDUMP_STRING*>(static_cast<std::byte*>(base) + rva);
    return std::wstring{s->Buffer, s->Length / sizeof(wchar_t)};
}

static void PrintSymbol(DWORD64 address) {
    char storage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME]{};
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (SymFromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        std::printf("%s+0x%llx", symbol->Name, static_cast<unsigned long long>(displacement));
    } else {
        std::printf("0x%llx", static_cast<unsigned long long>(address));
    }

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(line);
    DWORD lineDisplacement = 0;
    if (SymGetLineFromAddr64(GetCurrentProcess(), address, &lineDisplacement, &line)) {
        std::printf(" [%s:%lu]", line.FileName, line.LineNumber);
    }
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        std::fwprintf(stderr, L"usage: minidump_stack <dump> <symbol-path>\n");
        return 2;
    }

    auto const dumpPath = argv[1];
    auto const symbolPath = argv[2];
    auto file =
        CreateFileW(dumpPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        std::fwprintf(stderr, L"CreateFile failed: %lu\n", GetLastError());
        return 1;
    }

    auto mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        std::fwprintf(stderr, L"CreateFileMapping failed: %lu\n", GetLastError());
        CloseHandle(file);
        return 1;
    }

    auto base = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (base == nullptr) {
        std::fwprintf(stderr, L"MapViewOfFile failed: %lu\n", GetLastError());
        CloseHandle(mapping);
        CloseHandle(file);
        return 1;
    }

    DumpState dump{base};
    ULONG streamSize = 0;
    void* stream = nullptr;
    MINIDUMP_DIRECTORY* directory = nullptr;
    if (MiniDumpReadDumpStream(base, Memory64ListStream, &directory, &stream, &streamSize)) {
        dump.memory64 = static_cast<MINIDUMP_MEMORY64_LIST*>(stream);
    }
    if (MiniDumpReadDumpStream(base, MemoryListStream, &directory, &stream, &streamSize)) {
        dump.memory = static_cast<MINIDUMP_MEMORY_LIST*>(stream);
    }
    g_dumpState = &dump;

    SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    if (!SymInitializeW(GetCurrentProcess(), symbolPath, FALSE)) {
        std::printf("SymInitialize failed: %lu\n", GetLastError());
    }

    if (MiniDumpReadDumpStream(base, ModuleListStream, &directory, &stream, &streamSize)) {
        auto* modules = static_cast<MINIDUMP_MODULE_LIST*>(stream);
        std::printf("Modules: %lu\n", modules->NumberOfModules);
        for (ULONG i = 0; i < modules->NumberOfModules; ++i) {
            auto const& module = modules->Modules[i];
            auto name = ReadMiniString(base, module.ModuleNameRva);
            auto loaded = SymLoadModuleExW(GetCurrentProcess(),
                                           nullptr,
                                           name.empty() ? nullptr : name.c_str(),
                                           nullptr,
                                           module.BaseOfImage,
                                           module.SizeOfImage,
                                           nullptr,
                                           0);
            std::wprintf(L"  0x%llx size=0x%lx ts=0x%lx %ls%s\n",
                         static_cast<unsigned long long>(module.BaseOfImage),
                         module.SizeOfImage,
                         module.TimeDateStamp,
                         name.c_str(),
                         loaded == 0 ? L" (symbols not loaded yet)" : L"");
        }
    }

    MINIDUMP_EXCEPTION_STREAM* exception = nullptr;
    if (!MiniDumpReadDumpStream(base, ExceptionStream, &directory, reinterpret_cast<void**>(&exception), &streamSize)) {
        std::printf("No exception stream\n");
        return 0;
    }

    auto const code = exception->ExceptionRecord.ExceptionCode;
    auto const address = exception->ExceptionRecord.ExceptionAddress;
    std::printf("\nException thread=%lu code=0x%08lx address=0x%llx\n",
                exception->ThreadId,
                code,
                static_cast<unsigned long long>(address));
    for (ULONG i = 0; i < exception->ExceptionRecord.NumberParameters; ++i) {
        std::printf("  param[%lu]=0x%llx\n",
                    i,
                    static_cast<unsigned long long>(exception->ExceptionRecord.ExceptionInformation[i]));
    }

    auto const* dumpContext =
        reinterpret_cast<CONTEXT const*>(static_cast<std::byte*>(base) + exception->ThreadContext.Rva);
    auto context = *dumpContext;
    std::printf("Context RIP=0x%llx RSP=0x%llx RBP=0x%llx\n\n",
                static_cast<unsigned long long>(context.Rip),
                static_cast<unsigned long long>(context.Rsp),
                static_cast<unsigned long long>(context.Rbp));

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    auto machine = static_cast<DWORD>(IMAGE_FILE_MACHINE_AMD64);
    for (int frameIndex = 0; frameIndex < 80; ++frameIndex) {
        if (!StackWalk64(machine,
                         GetCurrentProcess(),
                         nullptr,
                         &frame,
                         &context,
                         ReadMemoryRoutine,
                         SymFunctionTableAccess64,
                         SymGetModuleBase64,
                         nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }
        std::printf("#%02d 0x%llx ", frameIndex, static_cast<unsigned long long>(frame.AddrPC.Offset));
        PrintSymbol(frame.AddrPC.Offset);
        std::printf("\n");
    }

    SymCleanup(GetCurrentProcess());
    UnmapViewOfFile(base);
    CloseHandle(mapping);
    CloseHandle(file);
    return 0;
}
