// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/alignment.h"
#include "common/arch.h"
#include "common/assert.h"
#include "common/elf_info.h"
#include "common/logging/formatter.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/string_util.h"
#include "common/thread.h"
#include "core/aerolib/aerolib.h"
#include "core/aerolib/stubs.h"
#include "core/devtools/widget/module_list.h"
#include "core/emulator_settings.h"
#include "core/file_sys/backends/host_fs.h"
#include "core/file_sys/fs.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/kernel/memory.h"
#include "core/libraries/kernel/threads.h"
#include "core/libraries/libc_internal/libc_internal.h"
#include "core/libraries/sysmodule/sysmodule.h"
#include "core/libraries/sysmodule/sysmodule_internal.h"
#include "core/linker.h"
#include "core/memory.h"
#include "core/tls.h"
#include "ipc/ipc.h"

#ifndef _WIN32
#include <signal.h>
#endif

namespace Core {

static PS4_SYSV_ABI void ProgramExitFunc() {
    LOG_ERROR(Core_Linker, "Exit function called");
}

static PS4_SYSV_ABI void* RunMainEntry [[noreturn]] (EntryParams* params) {
#ifdef ARCH_X86_64
    // Start shared library modules
    asm volatile("andq $-16, %%rsp\n" // Align to 16 bytes
                 "subq $8, %%rsp\n"   // videoout_basic expects the stack to be misaligned

                 // Kernel also pushes some more things here during process init
                 // at least: environment, auxv, possibly other things

                 "pushq 8(%1)\n" // copy EntryParams to top of stack like the kernel does
                 "pushq 0(%1)\n" // OpenOrbis expects to find it there

                 "movq %1, %%rdi\n" // also pass params and exit func
                 "movq %2, %%rsi\n" // as before

                 "jmp *%0\n" // can't use call here, as that would mangle the prepared stack.
                             // there's no coming back
                 :
                 : "r"(params->entry_addr), "r"(params), "r"(ProgramExitFunc)
                 : "rax", "rsi", "rdi");
    UNREACHABLE();
#else
    UNREACHABLE_MSG("RunMainEntry unimplemented for current architecture.");
#endif
}

Linker::Linker() : memory{Memory::Instance()} {}
Linker::Linker(MemoryManager& manager) : memory(&manager) {}

Linker::~Linker() = default;

void Linker::Execute(const std::vector<std::string>& args) {
    if (memory->IsGuestBackend())
        throw std::logic_error("guest execution must be owned by the production Session runtime");
    if (EmulatorSettings.IsDebugDump()) {
        DebugDump();
    }

    // Calculate static TLS size.
    Module* module = m_modules[0].get();
    static_tls_size = module->tls.offset = module->tls.image_size;

    // Map libSceLibcInternal
    const auto& libc_internal_path =
        EmulatorSettings.GetSysModulesDir() / "libSceLibcInternal.sprx";
    bool has_libcinternal = false;
    if (std::filesystem::exists(libc_internal_path)) {
        LoadModule(libc_internal_path);
        has_libcinternal = true;
    } else {
        // Need to load HLE, LLE isn't present
        LOG_INFO(Core_Linker, "Can't Load libSceLibcInternal.sprx switching to HLE");
        Libraries::LibcInternal::RegisterLib(&GetHLESymbols());
    }

    // Relocate all modules
    RelocateAllImports();

    // libkernel entry is responsible for initializing malloc-related elements of libSceLibcInternal
    // this is done through calling _malloc_init, and sceLibcInternalMemoryMutexEnable.
    static PS4_SYSV_ABI s32 (*malloc_init)() = nullptr;
    static PS4_SYSV_ABI void (*sceLibcInternalMemoryMutexEnable)() = nullptr;

    if (has_libcinternal) {
        for (const auto& m : m_modules) {
            const auto& mod = m.get();
            if (mod->name.contains("libSceLibcInternal.sprx")) {
                malloc_init =
                    reinterpret_cast<PS4_SYSV_ABI s32 (*)()>(mod->FindByName("_malloc_init"));
                sceLibcInternalMemoryMutexEnable = reinterpret_cast<PS4_SYSV_ABI void (*)()>(
                    mod->FindByName("sceLibcInternalMemoryMutexEnable"));
                break;
            }
        }
    }

    // Configure the direct and flexible memory regions.
    u64 fmem_size = ORBIS_KERNEL_FLEXIBLE_MEMORY_SIZE;
    bool use_extended_mem1 = true, use_extended_mem2 = true;

    const auto* proc_param = GetProcParam();
    ASSERT(proc_param);

    Core::OrbisKernelMemParam mem_param{};
    if (proc_param->size >= offsetof(OrbisProcParam, mem_param) + sizeof(OrbisKernelMemParam*)) {
        if (proc_param->mem_param) {
            mem_param = *proc_param->mem_param;
            if (mem_param.size >=
                offsetof(OrbisKernelMemParam, flexible_memory_size) + sizeof(u64*)) {
                if (const auto* flexible_size = mem_param.flexible_memory_size) {
                    fmem_size = *flexible_size + ORBIS_KERNEL_FLEXIBLE_MEMORY_BASE;
                }
            }
        }
    }

    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_1) + sizeof(u64*)) {
        mem_param.extended_memory_1 = nullptr;
    }
    if (mem_param.size < offsetof(OrbisKernelMemParam, extended_memory_2) + sizeof(u64*)) {
        mem_param.extended_memory_2 = nullptr;
    }

    const u64 sdk_ver = proc_param->sdk_version;
    if (sdk_ver < Common::ElfInfo::FW_500) {
        use_extended_mem1 = mem_param.extended_memory_1 ? *mem_param.extended_memory_1 : false;
        use_extended_mem2 = mem_param.extended_memory_2 ? *mem_param.extended_memory_2 : false;
    }

    memory->SetupMemoryRegions(fmem_size, use_extended_mem1, use_extended_mem2);

    main_thread.Run([this, module, &args, has_libcinternal](std::stop_token) {
        Common::SetCurrentThreadName("Game:Main");
        std::set_terminate(Common::Log::Terminate);

#ifndef _WIN32 // Clear any existing signal mask for game threads.
        sigset_t emptyset;
        sigemptyset(&emptyset);
        pthread_sigmask(SIG_SETMASK, &emptyset, nullptr);
#endif
        if (auto& ipc = IPC::Instance()) {
            ipc.WaitForStart();
        }

        // Load libSceLibcInternal, run malloc_init.
        if (has_libcinternal) {
            LoadLibcInternal();

            if (malloc_init && sceLibcInternalMemoryMutexEnable) {
                // Call _malloc_init
                s32 ret = malloc_init();
                ASSERT_MSG(ret == 0, "malloc_init failed");
                sceLibcInternalMemoryMutexEnable();
            }
        }

        // Have libSceSysmodule preload our libraries.
        Libraries::SysModule::sceSysmodulePreloadModuleForLibkernel();

        // Load and start custom modules from the user directory.
        std::string_view id = Common::ElfInfo::Instance().GameSerial();
        const auto& custom_mod_directory =
            Common::FS::GetUserPath(Common::FS::PathType::CustomModulesDir) / id;
        if (!std::filesystem::exists(custom_mod_directory)) {
            std::filesystem::create_directory(custom_mod_directory);
        }
        for (const auto& entry : std::filesystem::directory_iterator(custom_mod_directory)) {
            if (entry.is_regular_file()) {
                LOG_INFO(Core_Linker, "Loading custom module: {}",
                         fmt::UTF(entry.path().u8string()));
                if (LoadAndStartModule(entry.path(), 0, nullptr, nullptr) == -1) {
                    LOG_ERROR(Core_Linker, "Failed to load custom module: {}",
                              fmt::UTF(entry.path().u8string()));
                }
            }
        }

        // Simulate libSceGnmDriver initialization, which maps a chunk of direct memory.
        // Some games fail without accurately emulating this behavior.
        s64 phys_addr{};
        s32 result = Libraries::Kernel::sceKernelAllocateDirectMemory(
            0, Libraries::Kernel::sceKernelGetDirectMemorySize(), 0x10000, 0x10000, 3, &phys_addr);
        if (result == 0) {
            void* addr{reinterpret_cast<void*>(0xfe0000000)};
            result = Libraries::Kernel::sceKernelMapNamedDirectMemory(
                &addr, 0x10000, 0x13, 0, phys_addr, 0x10000, "SceGnmDriver");
        }
        ASSERT_MSG(result == 0, "Unable to emulate libSceGnmDriver initialization");

        // Add all guest arguments, we will always have the executable path in argv[0]
        EntryParams& params = Libraries::Kernel::entry_params;
        constexpr int MaxArgs = sizeof(params.argv) / sizeof(params.argv[0]);
        params.argc = std::min<int>(args.size(), MaxArgs);
        for (int i = 0; i < params.argc; i++) {
            params.argv[i] = args[i].c_str();
        }

        // Run the game's entry function
        params.entry_addr = module->GetEntryAddress();
        RunMainEntry(&params);
    });
}

s32 Linker::LoadModule(const std::filesystem::path& elf_name, bool is_dynamic) {
    std::scoped_lock lk{mutex};
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    const std::string as_guest = elf_name.generic_string();
    std::unique_ptr<Core::FileSys::IFile> handle;
    if (!as_guest.empty() && as_guest.front() == '/') {
        handle = mnt->Open(as_guest, /*writable=*/false);
    }
    if (!handle) {
        if (!std::filesystem::exists(elf_name)) {
            LOG_ERROR(Core_Linker, "Provided file {} does not exist", elf_name.string());
            return -1;
        }
        auto host = std::make_unique<Core::FileSys::HostFile>(
            elf_name, Common::FS::FileAccessMode::Read, /*read_only=*/true);
        if (!host->IsOpen()) {
            LOG_ERROR(Core_Linker, "Provided file {} could not be opened", elf_name.string());
            return -1;
        }
        handle = std::move(host);
    }

    s32 mod_id = m_modules.size();
    auto module =
        std::make_unique<Module>(memory, elf_name, std::move(handle), max_tls_index, mod_id);
    if (!module->IsValid())
        throw std::runtime_error("invalid module: " + elf_name.string());

    num_static_modules += !is_dynamic;
    m_modules.emplace_back(std::move(module));

    Core::Devtools::Widget::ModuleList::AddModule(elf_name.filename().string(), elf_name);

    return mod_id;
}

s32 Linker::LoadAndStartModule(const std::filesystem::path& path, u64 args, const void* argp,
                               int* pRes) {
    u32 handle = FindByName(path);
    if (handle != -1) {
        return handle;
    }
    handle = LoadModule(path, true);
    if (handle == -1) {
        return -1;
    }
    auto* module = GetModule(handle);
    RelocateAnyImports(module);

    // If the new module has a TLS image, trigger its load when TlsGetAddr is called.
    if (module->tls.image_size != 0) {
        AdvanceGenerationCounter();
    }

    // Retrieve and verify proc param according to libkernel.
    auto* param = module->GetProcParam<OrbisProcParam*>();
    ASSERT_MSG(!param || param->size >= 0x18, "Invalid module param size: {}", param->size);
    s32 ret = module->Start(args, argp, param);
    if (pRes) {
        *pRes = ret;
    }

    return handle;
}

Module* Linker::FindByAddress(VAddr address) {
    for (auto& module : m_modules) {
        const VAddr base = module->GetBaseAddress();
        if (address >= base && address < base + module->aligned_base_size) {
            return module.get();
        }
    }
    return nullptr;
}

void Linker::Relocate(Module* module) {
    module->ForEachRelocation([&](elf_relocation* rel, u32 i, bool is_jmp_rel) {
        const u32 num_relocs = module->dynamic_info.relocation_table_size / sizeof(elf_relocation);
        const u32 bit_idx = (is_jmp_rel ? num_relocs : 0) + i;
        if (module->TestRelaBit(bit_idx)) {
            return;
        }
        auto type = rel->GetType();
        auto symbol = rel->GetSymbol();
        auto addend = rel->rel_addend;
        auto* symbol_table = module->dynamic_info.symbol_table;
        auto* names_tlb = module->dynamic_info.str_table;

        const VAddr rel_base_virtual_addr = module->GetBaseAddress();
        const VAddr rel_virtual_addr = rel_base_virtual_addr + rel->rel_offset;
        if (memory->IsGuestBackend()) {
            bool contained = false;
            for (const auto& p : module->elf.GetProgramHeader()) {
                if ((p.p_type == PT_LOAD || p.p_type == PT_SCE_RELRO) && p.p_memsz >= 8 &&
                    rel->rel_offset >= p.p_vaddr && rel->rel_offset - p.p_vaddr <= p.p_memsz - 8)
                    contained = true;
            }
            if (!contained)
                throw std::runtime_error("relocation target outside module: " + module->name);
            if (type != R_X86_64_RELATIVE && type != R_X86_64_DTPMOD64 &&
                symbol >= module->dynamic_info.symbol_table_total_size / sizeof(elf_symbol))
                throw std::runtime_error("relocation symbol outside table: " + module->name);
        }
        bool rel_is_resolved = false;
        u64 rel_value = 0;
        Loader::SymbolType rel_sym_type = Loader::SymbolType::Unknown;
        std::string rel_name;

        switch (type) {
        case R_X86_64_RELATIVE:
            rel_value = rel_base_virtual_addr + addend;
            rel_is_resolved = true;
            module->SetRelaBit(bit_idx);
            break;
        case R_X86_64_DTPMOD64:
        case R_X86_64_DTPOFF64:
        case R_X86_64_TPOFF64: {
            Module* target = module;
            u64 tls_value = 0;
            if (symbol != 0) {
                if (symbol >= module->dynamic_info.symbol_table_total_size / sizeof(elf_symbol))
                    throw std::runtime_error("TLS relocation symbol outside table");
                const auto& tls_symbol = symbol_table[symbol];
                // The supplied PS4 libc/Fios SELF uses an unnamed, zero local
                // STT_SECTION entry as the current-module DTPMOD64 marker.
                // It names a module, not a TLS variable/export. Do not apply
                // this exception to offset relocations or named symbols.
                const bool self_module_marker = memory->IsGuestBackend() &&
                    type == R_X86_64_DTPMOD64 && tls_symbol.GetType() == STT_SECTION &&
                    tls_symbol.GetBind() == STB_LOCAL && tls_symbol.st_name == 0 &&
                    tls_symbol.st_shndx == 0 && tls_symbol.st_value == 0 && addend == 0;
                if (!self_module_marker && tls_symbol.GetType() != STT_TLS)
                    throw std::runtime_error("TLS relocation references non-TLS symbol: " +
                        module->name + " symbol=" + std::to_string(symbol));
                if (self_module_marker) {
                    if (!module->tls.modid || !module->tls.image_size)
                        throw std::runtime_error("TLS module marker without an image");
                } else if (tls_symbol.st_shndx != 0)
                    tls_value = tls_symbol.st_value;
                else {
                    Loader::SymbolRecord resolved{};
                    if (!Resolve(names_tlb + tls_symbol.st_name, Loader::SymbolType::Tls, module,
                                 &resolved))
                        throw std::runtime_error("unresolved guest TLS symbol");
                    target = FindByAddress(resolved.virtual_address);
                    if (!target)
                        throw std::runtime_error("TLS export has no owning guest module");
                    tls_value = resolved.virtual_address - target->GetBaseAddress();
                }
            }
            if (type == R_X86_64_DTPMOD64)
                rel_value = target->tls.modid;
            else
                rel_value =
                    tls_value + addend - (type == R_X86_64_TPOFF64 ? target->tls.offset : 0);
            rel_is_resolved = true;
            rel_sym_type = Loader::SymbolType::Tls;
            module->SetRelaBit(bit_idx);
            break;
        }
        case R_X86_64_GLOB_DAT:
        case R_X86_64_JUMP_SLOT:
            addend = 0;
        case R_X86_64_64: {
            auto sym = symbol_table[symbol];
            auto sym_bind = sym.GetBind();
            auto sym_type = sym.GetType();
            auto sym_visibility = sym.GetVisibility();
            u64 symbol_virtual_addr = 0;
            Loader::SymbolRecord symrec{};
            switch (sym_type) {
            case STT_FUN:
                rel_sym_type = Loader::SymbolType::Function;
                break;
            case STT_OBJECT:
                rel_sym_type = Loader::SymbolType::Object;
                break;
            case STT_NOTYPE:
                rel_sym_type = Loader::SymbolType::NoType;
                break;
            default:
                if (memory->IsGuestBackend())
                    throw std::runtime_error("unsupported guest relocation symbol type");
                ASSERT_MSG(0, "unknown symbol type {}", sym_type);
            }

            if (sym_visibility != 0) {
                LOG_INFO(Core_Linker, "symbol visibility !=0");
            }

            switch (sym_bind) {
            case STB_LOCAL:
                symbol_virtual_addr = rel_base_virtual_addr + sym.st_value;
                module->SetRelaBit(bit_idx);
                break;
            case STB_GLOBAL:
            case STB_WEAK: {
                rel_name = names_tlb + sym.st_name;
                if (Resolve(rel_name, rel_sym_type, module, &symrec)) {
                    // Only set the rela bit if the symbol was actually resolved and not stubbed.
                    module->SetRelaBit(bit_idx);
                }
                symbol_virtual_addr = symrec.virtual_address;
                break;
            }
            default:
                if (memory->IsGuestBackend())
                    throw std::runtime_error("unsupported guest symbol binding");
                UNREACHABLE_MSG("Unknown bind type {}", sym_bind);
            }
            rel_is_resolved = (symbol_virtual_addr != 0);
            rel_value = (rel_is_resolved ? symbol_virtual_addr + addend : 0);
            rel_name = symrec.name;
            break;
        }
        default:
            if (memory->IsGuestBackend())
                throw std::runtime_error("unsupported guest relocation " + std::to_string(type));
            LOG_INFO(Core_Linker, "UNK type {:#010x} rel symbol : {:#010x}", type, symbol);
        }

        if (rel_is_resolved) {
            std::memcpy(reinterpret_cast<void*>(rel_virtual_addr), &rel_value, sizeof(rel_value));
        } else if (rel_sym_type == Loader::SymbolType::Function) {
            LOG_INFO(Core_Linker, "Function not patched! {}", rel_name);
        }
    });
}

bool Linker::Resolve(const std::string& name, Loader::SymbolType sym_type, Module* m,
                     Loader::SymbolRecord* return_info) {
    const auto ids = Common::SplitString(name, '#');
    if (ids.size() != 3) {
        return_info->virtual_address = 0;
        return_info->name = name;
        LOG_ERROR(Core_Linker, "Not Resolved {}", name);
        return false;
    }

    const LibraryInfo* library = m->FindLibrary(ids[1]);
    const ModuleInfo* module = m->FindModule(ids[2]);
    if ((!library || !module) && memory->IsGuestBackend())
        throw std::runtime_error("import references unknown module/library: " + name);
    ASSERT_MSG(library && module, "Unable to find library and module");

    Loader::SymbolResolver sr{};
    sr.name = ids.at(0);
    sr.library = library->name;
    sr.library_version = library->version;
    sr.module = module->name;
    sr.type = sym_type;

    const auto* record = m_hle_symbols.FindSymbol(sr);
    if (record) {
        *return_info = *record;
        if (memory->IsGuestBackend()) {
            if (sym_type == Loader::SymbolType::Object && guest_data_resolver) {
                return_info->virtual_address = guest_data_resolver(*record);
                return true;
            }
            if (sym_type != Loader::SymbolType::Function || !guest_hle_resolver)
                throw std::runtime_error("guest ABI data import needs an explicit descriptor: " +
                                         name);
            return_info->virtual_address = guest_hle_resolver(*record);
        }
        Core::Devtools::Widget::ModuleList::AddModule(sr.library);
        return true;
    }

    // Check if it an exported function from one of our loaded libraries
    for (const auto& mod : m_modules) {
        if (!std::ranges::contains(mod->GetExportLibs(), *library) ||
            !std::ranges::contains(mod->GetExportModules(), *module)) {
            continue;
        }
        if (mod->export_sym.GetSize() == 0) {
            continue;
        }
        record = mod->export_sym.FindSymbol(sr);
        if (record) {
            *return_info = *record;
            return true;
        }
    }

    // libc and libSceLibcInternal are the same PS4 library under two names: a game
    // links libc.prx (which exports its objects/functions under `libc`) yet also
    // imports some of them under `libSceLibcInternal` (the system-module name).
    // When the system module is not present as its own file, resolve those imports
    // against the loaded libc module's `libc` exports rather than synthesising a
    // host object -- the guest's own libc owns _Stdin/_Stdout/_Stderr and the rest.
    if (library->name == "libSceLibcInternal" || module->name == "libSceLibcInternal") {
        Loader::SymbolResolver alias = sr;
        alias.library = "libc";
        alias.module = "libc";
        for (const auto& mod : m_modules) {
            if (mod->export_sym.GetSize() == 0) {
                continue;
            }
            if (const auto* aliased = mod->export_sym.FindSymbol(alias)) {
                *return_info = *aliased;
                return true;
            }
        }
    }

    if (memory->IsGuestBackend()) {
        Loader::SymbolRecord missing{Loader::SymbolsResolver::GenerateName(sr), sr.name, 0, {}};
        if (sym_type == Loader::SymbolType::Object && guest_data_resolver) {
            *return_info = missing;
            return_info->virtual_address = guest_data_resolver(missing);
            return true;
        }
        if (sym_type != Loader::SymbolType::Function || !guest_hle_resolver)
            throw std::runtime_error("unresolved guest data import: " + missing.name);
        *return_info = missing;
        return_info->virtual_address = guest_hle_resolver(missing);
        return false;
    }
    const auto aeronid = AeroLib::FindByNid(sr.name.c_str());
    if (sym_type == Loader::SymbolType::Object) {
        return_info->name = aeronid ? aeronid->name : "Unknown object";
        return_info->virtual_address = 0;
    } else if (aeronid) {
        return_info->name = aeronid->name;
        return_info->virtual_address = AeroLib::GetStub(aeronid->nid);
    } else {
        return_info->virtual_address = AeroLib::GetStub(sr.name.c_str());
        return_info->name = "Unknown !!!";
    }
    if (library->name != "libc" && library->name != "libSceFios2") {
        LOG_WARNING(Core_Linker, "Linker: Stub resolved {} as {} (lib: {}, mod: {})", sr.name,
                    return_info->name, library->name, module->name);
    } else {
        if (library->name == "libc" && return_info->name == "Need_sceLibc") {
            Libraries::SysModule::g_need_scelibc = true;
        }
        if (library->name == "libSceFios2" && return_info->name == "sceFiosInitialize") {
            Libraries::SysModule::g_need_scelibc = true;
        }
    }
    return false;
}

void* Linker::TlsGetAddr(u64 module_index, u64 offset) {
    if (guest_tls_resolver)
        return guest_tls_resolver(module_index, offset);
    if (memory->IsGuestBackend())
        throw std::logic_error("guest TLS resolver is not bound");
    std::scoped_lock lk{mutex};

    DtvEntry* dtv_table = GetTcbBase()->tcb_dtv;
    if (dtv_table[0].counter != dtv_generation_counter) {
        // Generation counter changed, a dynamic module was either loaded or unloaded.
        const u32 old_num_dtvs = dtv_table[1].counter;
        ASSERT_MSG(max_tls_index > old_num_dtvs, "Module unloading unsupported");
        // Module was loaded, increase DTV table size.
        DtvEntry* new_dtv_table = new DtvEntry[max_tls_index + 2]{};
        std::memcpy(new_dtv_table + 2, dtv_table + 2, old_num_dtvs * sizeof(DtvEntry));
        new_dtv_table[0].counter = dtv_generation_counter;
        new_dtv_table[1].counter = max_tls_index;
        delete[] dtv_table;

        // Update TCB pointer.
        GetTcbBase()->tcb_dtv = new_dtv_table;
        dtv_table = new_dtv_table;
    }

    u8* addr = dtv_table[module_index + 1].pointer;
    Module* module = m_modules[module_index - 1].get();
    if (!addr) {
        // Module was just loaded by above code. Allocate TLS block for it.
        const u32 init_image_size = module->tls.init_image_size;
        u8* dest{};
        if (heap_api && heap_api->heap_malloc) {
            dest = reinterpret_cast<u8*>(heap_api->heap_malloc(module->tls.image_size));
        } else {
            dest = reinterpret_cast<u8*>(std::malloc(module->tls.image_size));
        }
        const u8* src = reinterpret_cast<const u8*>(module->tls.image_virtual_addr);
        std::memcpy(dest, src, init_image_size);
        std::memset(dest + init_image_size, 0, module->tls.image_size - init_image_size);
        dtv_table[module_index + 1].pointer = dest;
        addr = dest;
    }
    return addr + offset;
}

void* Linker::AllocateTlsForThread(bool is_primary) {
    if (guest_tls_allocate)
        return guest_tls_allocate(Common::AlignUp(static_tls_size, 32ULL) + 64);
    if (memory->IsGuestBackend())
        throw std::logic_error("guest TLS allocator is not bound");
    static constexpr size_t TcbSize = 0x40;
    static constexpr size_t TlsAllocAlign = 0x20;
    const size_t total_tls_size = Common::AlignUp(static_tls_size, TlsAllocAlign) + TcbSize;

    // If sceKernelMapNamedFlexibleMemory is being called from libkernel and addr = 0
    // it automatically places mappings in system reserved area instead of managed.
    // Since the system reserved area already has a mapping in it, this address is slightly higher.
    static constexpr VAddr KernelAllocBase = 0x881000000ULL;

    // The kernel module has a few different paths for TLS allocation.
    // For SDK < 1.7 it allocates both main and secondary thread blocks using libc mspace/malloc.
    // In games compiled with newer SDK, the main thread gets mapped from flexible memory,
    // with addr = 0, so system managed area. Here we will only implement the latter.
    void* addr_out{reinterpret_cast<void*>(KernelAllocBase)};
    if (is_primary) {
        const size_t tls_aligned = Common::AlignUp(total_tls_size, 16_KB);
        const int ret = Libraries::Kernel::sceKernelMapNamedFlexibleMemory(
            &addr_out, tls_aligned, 3, 0, "SceKernelPrimaryTcbTls");
        ASSERT_MSG(ret == 0, "Unable to allocate TLS+TCB for the primary thread");
    } else {
        if (heap_api && heap_api->heap_malloc) {
            addr_out = heap_api->heap_malloc(total_tls_size);
        } else {
            addr_out = std::malloc(total_tls_size);
        }
    }
    return addr_out;
}

void Linker::FreeTlsForNonPrimaryThread(void* pointer) {
    if (guest_tls_free) {
        guest_tls_free(pointer);
        return;
    }
    if (memory->IsGuestBackend())
        throw std::logic_error("guest TLS owner is not bound");
    if (heap_api && heap_api->heap_free) {
        heap_api->heap_free(pointer);
    } else {
        std::free(pointer);
    }
}

void Linker::PrepareGuest() {
    if (!memory->IsGuestBackend() || m_modules.empty())
        throw std::logic_error("production guest Linker has no executable");
    static_tls_size = 0;
    for (auto& module : m_modules) {
        if (!module->tls.image_size)
            continue;
        const u64 alignment = std::max<u64>(module->tls.align, 32);
        static_tls_size = Common::AlignUp(static_tls_size + module->tls.image_size, alignment);
        if (static_tls_size > 64_MB)
            throw std::runtime_error("static TLS exceeds guest policy");
        module->tls.offset = static_tls_size;
    }
    static_tls_size = Common::AlignUp(static_tls_size, size_t{32});
    RelocateAllImports();
    for (auto& module : m_modules)
        module->FinalizeGuestPermissions();
}

void Linker::DebugDump() {
    const auto& log_dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
    const std::filesystem::path debug(log_dir / "debugdump");
    std::filesystem::create_directory(debug);
    for (const auto& m : m_modules) {
        Module* module = m.get();
        auto& elf = module->elf;
        const std::filesystem::path filepath(debug / module->file.stem());
        std::filesystem::create_directory(filepath);
        module->import_sym.DebugDump(filepath / "imports.txt");
        module->export_sym.DebugDump(filepath / "exports.txt");
        if (elf.IsSelfFile()) {
            elf.SelfHeaderDebugDump(filepath / "selfHeader.txt");
            elf.SelfSegHeaderDebugDump(filepath / "selfSegHeaders.txt");
        }
        elf.ElfHeaderDebugDump(filepath / "elfHeader.txt");
        elf.PHeaderDebugDump(filepath / "elfPHeaders.txt");
    }
}

} // namespace Core
