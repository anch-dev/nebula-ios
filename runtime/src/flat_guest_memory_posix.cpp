// SPDX-License-Identifier: GPL-3.0-only
// Flat guest memory for POSIX hosts (Linux, macOS, iOS): the same 4 GiB view
// with MEM1/MEM2 mirrored at their six guest aliases as the Windows
// implementation in flat_guest_memory.cpp, and a fastmem fault handler for
// AArch64.
//
// Mapping:
//   Linux        memfd sections, MAP_FIXED|MAP_SHARED views over a PROT_NONE
//                reservation.
//   Apple        vm_allocate host views, vm_remap (copy = FALSE)
//                aliases over the reservation. iOS has no shm/memfd in the app
//                sandbox, and remapping within the task is allowed.
// The reservation asks for kGuestBase but takes any address: iOS limits the
// user address space well below it. Callers use guest_base().
//
// Fastmem faults: generated code reaches MMIO, the locked cache and unmapped
// space through the view; those pages are PROT_NONE, the access raises
// SIGSEGV/SIGBUS, and the handler decodes the faulting AArch64 load/store,
// completes it through the device callbacks, updates the registers and skips
// the instruction. Covered forms: LDR/STR (all widths, signed loads) with
// unsigned-offset, unscaled, pre/post-index and register-offset addressing,
// LDP/STP/LDPSW/LDNP/STNP, the SIMD&FP scalar forms (B/H/S/D/Q) of all of
// those, and LD1/ST1 single-lane forms (B/H/S/D, no offset or post-index).

#if !defined(_WIN32)

#include "galaxy/flat_guest_memory.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <string>

#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <sys/ucontext.h>
#else
#include <sys/syscall.h>
#include <ucontext.h>
#endif

#if defined(__aarch64__)
#define GALAXY_FASTMEM_HANDLER_AARCH64 1
#else
#define GALAXY_FASTMEM_HANDLER_AARCH64 0
#endif

namespace galaxy::host::flat_guest_memory {
namespace {

// The same 64 KiB overrun guard past the 4 GiB view as on Windows.
constexpr std::size_t kOverrunGuard = 0x10000u;

std::mutex g_init_mutex;
std::atomic<bool> g_initialized{false};
std::atomic<bool> g_owner_occupied{false};
bool g_initialization_failed = false;
std::byte* g_guest_base = nullptr;
std::byte* g_mem1 = nullptr;
std::byte* g_mem2 = nullptr;

[[noreturn]] void fail_errno(const char* operation, int error = errno) {
    throw std::runtime_error(std::string(operation) + " failed (errno=" +
                             std::to_string(error) + ": " + std::strerror(error) + ')');
}

std::byte* reserve_guest_space() {
    const std::size_t bytes = static_cast<std::size_t>(kGuestSpaceSize) + kOverrunGuard;
    int flags = MAP_PRIVATE | MAP_ANON;
#if defined(MAP_NORESERVE)
    flags |= MAP_NORESERVE;
#endif
    void* reservation =
        mmap(reinterpret_cast<void*>(kGuestBase), bytes, PROT_NONE, flags, -1, 0);
    if (reservation == MAP_FAILED) {
        // The hint is advisory; a kernel that rejects it outright still gets
        // a second chance anywhere.
        reservation = mmap(nullptr, bytes, PROT_NONE, flags, -1, 0);
    }
    if (reservation == MAP_FAILED) {
        fail_errno("reserving the 4 GiB guest view (on iOS this needs the "
                   "extended virtual addressing entitlement)");
    }
    return static_cast<std::byte*>(reservation);
}

#if defined(__APPLE__)

std::byte* create_section(std::uint32_t size) {
    // vm_* rather than mach_vm_*: <mach/mach_vm.h> is unavailable on iOS,
    // and vm_address_t is 64-bit on arm64.
    vm_address_t address = 0u;
    const kern_return_t result =
        vm_allocate(mach_task_self(), &address, size, VM_FLAGS_ANYWHERE);
    if (result != KERN_SUCCESS) {
        throw std::runtime_error("vm_allocate for guest RAM failed (kern_return " +
                                 std::to_string(result) + ')');
    }
    return reinterpret_cast<std::byte*>(address);
}

void map_guest_alias(std::byte* section, std::uint32_t guest_address, std::uint32_t size) {
    vm_address_t target = reinterpret_cast<vm_address_t>(g_guest_base + guest_address);
    vm_prot_t current = VM_PROT_NONE;
    vm_prot_t maximum = VM_PROT_NONE;
    const kern_return_t result = vm_remap(
        mach_task_self(), &target, size, 0u, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
        mach_task_self(), reinterpret_cast<vm_address_t>(section),
        FALSE, &current, &maximum, VM_INHERIT_NONE);
    if (result != KERN_SUCCESS) {
        throw std::runtime_error("vm_remap guest alias failed (kern_return " +
                                 std::to_string(result) + ')');
    }
    if (target != reinterpret_cast<vm_address_t>(g_guest_base + guest_address)) {
        throw std::runtime_error("vm_remap placed a guest alias away from its address");
    }
    if ((current & (VM_PROT_READ | VM_PROT_WRITE)) != (VM_PROT_READ | VM_PROT_WRITE)) {
        throw std::runtime_error("vm_remap guest alias is not read/write");
    }
}

#else

int g_mem1_fd = -1;
int g_mem2_fd = -1;

int create_section_fd(const char* name, std::uint32_t size) {
    const int fd = static_cast<int>(syscall(SYS_memfd_create, name, 1u /* MFD_CLOEXEC */));
    if (fd < 0) fail_errno("memfd_create for guest RAM");
    if (ftruncate(fd, size) != 0) {
        const int error = errno;
        close(fd);
        fail_errno("sizing guest RAM", error);
    }
    return fd;
}

std::byte* map_section_view(int fd, std::uint32_t size) {
    void* const view = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (view == MAP_FAILED) fail_errno("mapping the host guest-RAM view");
    return static_cast<std::byte*>(view);
}

void map_guest_alias(int fd, std::uint32_t guest_address, std::uint32_t size) {
    void* const target = g_guest_base + guest_address;
    void* const view =
        mmap(target, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
    if (view == MAP_FAILED) fail_errno("mapping a guest RAM alias");
    if (view != target) {
        throw std::runtime_error("a guest RAM alias was placed away from its address");
    }
}

#endif

std::byte* mapped_host_pointer(std::uint32_t guest_address, std::uint32_t size) noexcept {
    if (size == 0u || static_cast<std::uint64_t>(guest_address) + size > kGuestSpaceSize) {
        return nullptr;
    }
    const std::uint32_t alias = guest_address & 0xF0000000u;
    const std::uint32_t offset = guest_address & 0x0FFFFFFFu;
    if ((alias == 0x00000000u || alias == 0x80000000u || alias == 0xC0000000u) &&
        offset <= kMem1Size && size <= kMem1Size - offset) {
        return g_mem1 + offset;
    }
    if ((alias == 0x10000000u || alias == 0x90000000u || alias == 0xD0000000u) &&
        offset <= kMem2Size && size <= kMem2Size - offset) {
        return g_mem2 + offset;
    }
    return nullptr;
}

}  // namespace

void initialize() {
    const std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_initialized.load(std::memory_order_acquire)) return;
    if (g_initialization_failed) {
        throw std::runtime_error(
            "flat guest memory initialization previously failed; restart the process");
    }
    g_initialization_failed = true;

    g_guest_base = reserve_guest_space();
#if defined(__APPLE__)
    g_mem1 = create_section(kMem1Size);
    g_mem2 = create_section(kMem2Size);
    for (const std::uint32_t alias : {0x00000000u, 0x80000000u, 0xC0000000u}) {
        map_guest_alias(g_mem1, alias, kMem1Size);
    }
    for (const std::uint32_t alias : {0x10000000u, 0x90000000u, 0xD0000000u}) {
        map_guest_alias(g_mem2, alias, kMem2Size);
    }
#else
    g_mem1_fd = create_section_fd("galaxy-mem1", kMem1Size);
    g_mem2_fd = create_section_fd("galaxy-mem2", kMem2Size);
    g_mem1 = map_section_view(g_mem1_fd, kMem1Size);
    g_mem2 = map_section_view(g_mem2_fd, kMem2Size);
    for (const std::uint32_t alias : {0x00000000u, 0x80000000u, 0xC0000000u}) {
        map_guest_alias(g_mem1_fd, alias, kMem1Size);
    }
    for (const std::uint32_t alias : {0x10000000u, 0x90000000u, 0xD0000000u}) {
        map_guest_alias(g_mem2_fd, alias, kMem2Size);
    }
#endif
    g_initialization_failed = false;
    g_initialized.store(true, std::memory_order_release);
}

bool is_initialized() noexcept { return g_initialized.load(std::memory_order_acquire); }

std::byte* guest_base() noexcept { return is_initialized() ? g_guest_base : nullptr; }

unsigned long current_thread_id() noexcept {
#if defined(__APPLE__)
    std::uint64_t id = 0u;
    pthread_threadid_np(nullptr, &id);
    return static_cast<unsigned long>(id);
#else
    return static_cast<unsigned long>(syscall(SYS_gettid));
#endif
}

std::byte* mem1_host_view() noexcept { return is_initialized() ? g_mem1 : nullptr; }
std::byte* mem2_host_view() noexcept { return is_initialized() ? g_mem2 : nullptr; }

std::byte* host_pointer(std::uint32_t guest_address, std::uint32_t size) noexcept {
    return is_initialized() ? mapped_host_pointer(guest_address, size) : nullptr;
}

bool is_mapped_ram_span(std::uint32_t guest_address, std::uint32_t size) noexcept {
    return host_pointer(guest_address, size) != nullptr;
}

// ---------------------------------------------------------------------------
// Fault statistics (same reporting as the Windows handler)
// ---------------------------------------------------------------------------

namespace {

FaultAccessCallbacks g_fault_callbacks{};
std::atomic<bool> g_fault_handler_installed{false};
struct sigaction g_previous_segv {};
struct sigaction g_previous_bus {};
std::atomic<std::uint64_t> g_fault_reads{0};
std::atomic<std::uint64_t> g_fault_writes{0};
std::atomic<std::uint64_t> g_fault_locked_cache{0};
std::atomic<std::uint64_t> g_fault_mmio{0};
std::atomic<std::uint64_t> g_fault_unhandled{0};
char g_last_unhandled[192]{};

constexpr std::size_t kFaultPageSlots = 64u;
std::uint32_t g_fault_pages[kFaultPageSlots]{};
std::uint64_t g_fault_page_counts[kFaultPageSlots]{};
std::size_t g_fault_page_used = 0u;
constexpr std::size_t kFaultAddressSlots = 256u;
std::uint32_t g_fault_addresses[kFaultAddressSlots]{};
std::uint64_t g_fault_address_counts[kFaultAddressSlots]{};
std::size_t g_fault_address_used = 0u;
constexpr std::size_t kFaultSiteSlots = 512u;
std::uintptr_t g_fault_sites[kFaultSiteSlots]{};
std::uint32_t g_fault_site_addresses[kFaultSiteSlots]{};
std::uint64_t g_fault_site_counts[kFaultSiteSlots]{};
std::size_t g_fault_site_used = 0u;

void count_fault_site(std::uintptr_t pc, std::uint32_t guest_address) noexcept {
    for (std::size_t i = 0; i < g_fault_site_used; ++i) {
        if (g_fault_sites[i] == pc) {
            ++g_fault_site_counts[i];
            g_fault_site_addresses[i] = guest_address;
            return;
        }
    }
    if (g_fault_site_used < kFaultSiteSlots) {
        g_fault_sites[g_fault_site_used] = pc;
        g_fault_site_addresses[g_fault_site_used] = guest_address;
        g_fault_site_counts[g_fault_site_used] = 1u;
        ++g_fault_site_used;
    }
}

void count_fault_page(std::uint32_t guest_address) noexcept {
    const std::uint32_t page = guest_address >> 12u;
    std::size_t i = 0;
    for (; i < g_fault_page_used; ++i) {
        if (g_fault_pages[i] == page) { ++g_fault_page_counts[i]; break; }
    }
    if (i == g_fault_page_used && g_fault_page_used < kFaultPageSlots) {
        g_fault_pages[g_fault_page_used] = page;
        g_fault_page_counts[g_fault_page_used] = 1u;
        ++g_fault_page_used;
    }
    const std::uint32_t word = guest_address & ~3u;
    for (i = 0; i < g_fault_address_used; ++i) {
        if (g_fault_addresses[i] == word) { ++g_fault_address_counts[i]; return; }
    }
    if (g_fault_address_used < kFaultAddressSlots) {
        g_fault_addresses[g_fault_address_used] = word;
        g_fault_address_counts[g_fault_address_used] = 1u;
        ++g_fault_address_used;
    }
}

void note_unhandled(std::uint32_t guest_address, std::uint32_t instruction,
                    const char* reason) noexcept {
    g_fault_unhandled.fetch_add(1u, std::memory_order_relaxed);
    std::snprintf(g_last_unhandled, sizeof(g_last_unhandled),
                  "fastmem fault at guest 0x%08X not handled (%s): instruction %08X",
                  static_cast<unsigned>(guest_address), reason,
                  static_cast<unsigned>(instruction));
}

bool fault_transfer(std::uint32_t guest_address, std::uint32_t size, bool write,
                    std::byte* image) noexcept {
    const bool accepted =
        write ? (g_fault_callbacks.write != nullptr &&
                 g_fault_callbacks.write(g_fault_callbacks.user, guest_address, size, image))
              : (g_fault_callbacks.read != nullptr &&
                 g_fault_callbacks.read(g_fault_callbacks.user, guest_address, size, image));
    if (accepted) return true;
    if (size <= 1u) return false;
    const std::uint32_t piece = size > 4u ? 4u : size / 2u;
    for (std::uint32_t offset = 0u; offset < size; offset += piece) {
        if (!fault_transfer(guest_address + offset, piece, write, image + offset)) return false;
    }
    return true;
}

#if GALAXY_FASTMEM_HANDLER_AARCH64

// ---------------------------------------------------------------------------
// AArch64 load/store decoding
// ---------------------------------------------------------------------------

struct ArmRegisters {
    std::uint64_t* x = nullptr;       // x0..x30
    std::uint64_t* sp = nullptr;
    std::uint64_t* pc = nullptr;
    unsigned char* v = nullptr;       // v0..v31, 16 bytes each (null if absent)
    // Linux with SVE: when the frame carries Z registers, sigreturn restores
    // V from them, so a written V register is mirrored into Z's low 128 bits.
    unsigned char* z = nullptr;
    std::size_t z_stride = 0u;
};

void sync_vector_register(ArmRegisters& regs, unsigned index) noexcept {
    if (regs.z == nullptr) return;
    std::memcpy(regs.z + regs.z_stride * index, regs.v + 16u * index, 16u);
}

bool registers_from_context(void* raw, ArmRegisters& out) noexcept {
    auto* const context = static_cast<ucontext_t*>(raw);
#if defined(__APPLE__)
    auto* const machine = context->uc_mcontext;
    if (machine == nullptr) return false;
    // __x[0..28], __fp (x29) and __lr (x30) are contiguous 64-bit fields.
    static_assert(offsetof(__darwin_arm_thread_state64, __fp) ==
                  offsetof(__darwin_arm_thread_state64, __x) + 29 * 8);
    static_assert(offsetof(__darwin_arm_thread_state64, __lr) ==
                  offsetof(__darwin_arm_thread_state64, __x) + 30 * 8);
    out.x = reinterpret_cast<std::uint64_t*>(machine->__ss.__x);
    out.sp = reinterpret_cast<std::uint64_t*>(&machine->__ss.__sp);
    out.pc = reinterpret_cast<std::uint64_t*>(&machine->__ss.__pc);
    out.v = reinterpret_cast<unsigned char*>(machine->__ns.__v);
#else
    mcontext_t& machine = context->uc_mcontext;
    out.x = reinterpret_cast<std::uint64_t*>(machine.regs);
    out.sp = reinterpret_cast<std::uint64_t*>(&machine.sp);
    out.pc = reinterpret_cast<std::uint64_t*>(&machine.pc);
    // The SIMD registers live in a tagged record in __reserved.
    unsigned char* record = reinterpret_cast<unsigned char*>(machine.__reserved);
    unsigned char* const end = record + sizeof(machine.__reserved);
    while (record + sizeof(_aarch64_ctx) <= end) {
        auto* const head = reinterpret_cast<_aarch64_ctx*>(record);
        if (head->magic == 0u || head->size == 0u) break;
        if (head->magic == FPSIMD_MAGIC) {
            out.v = reinterpret_cast<unsigned char*>(
                reinterpret_cast<fpsimd_context*>(record)->vregs);
        }
#if defined(SVE_MAGIC)
        if (head->magic == SVE_MAGIC) {
            auto* const sve = reinterpret_cast<sve_context*>(record);
            const unsigned vq = sve->vl / 16u;
            // Register data is present only when the record is larger than
            // its header (SVE was live in the interrupted context); streaming
            // mode (SME) frames are not handled.
            if (vq != 0u && head->size >= SVE_SIG_CONTEXT_SIZE(vq)
#if defined(SVE_SIG_FLAG_SM)
                && (sve->flags & SVE_SIG_FLAG_SM) == 0u
#endif
            ) {
                out.z = record + SVE_SIG_ZREG_OFFSET(vq, 0);
                out.z_stride = SVE_SIG_ZREG_SIZE(vq);
            }
        }
#endif
        if (head->magic == EXTRA_MAGIC) break;  // records continue elsewhere; FPSIMD comes first
        record += head->size;
    }
#endif
    return true;
}

enum class ArmKind : std::uint8_t { Single, Pair, Lane };

struct ArmAccess {
    bool valid = false;
    bool load = false;
    bool simd = false;
    bool sign_extend = false;
    bool dest64 = false;          // integer load writes Xt (else Wt)
    ArmKind kind = ArmKind::Single;
    std::uint32_t size = 0u;      // bytes per element
    unsigned rt = 0u;
    unsigned rt2 = 0u;
    unsigned rn = 0u;
    unsigned lane = 0u;
    std::uint64_t address = 0u;   // effective host address
    bool writeback = false;
    std::uint64_t new_base = 0u;
};

std::uint64_t read_x(const ArmRegisters& regs, unsigned index, bool sp_at_31) noexcept {
    if (index == 31u) return sp_at_31 ? *regs.sp : 0u;
    return regs.x[index];
}

void write_x(ArmRegisters& regs, unsigned index, std::uint64_t value, bool sp_at_31) noexcept {
    if (index == 31u) {
        if (sp_at_31) *regs.sp = value;
        return;  // XZR
    }
    regs.x[index] = value;
}

std::int64_t sign_extend(std::uint64_t value, unsigned bits) noexcept {
    const std::uint64_t sign = std::uint64_t{1} << (bits - 1u);
    value &= (bits >= 64u ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1u);
    return static_cast<std::int64_t>((value ^ sign) - sign);
}

// Width and direction of the single-register forms from size:V:opc.
bool classify_single(unsigned size, bool vector, unsigned opc, ArmAccess& access) noexcept {
    access.simd = vector;
    if (!vector) {
        access.size = 1u << size;
        switch (opc) {
        case 0u: access.load = false; return true;
        case 1u: access.load = true; access.dest64 = size == 3u; return true;
        case 2u:
            if (size == 3u) return false;  // PRFM
            access.load = true; access.sign_extend = true; access.dest64 = true; return true;
        default:
            if (size >= 2u) return false;
            access.load = true; access.sign_extend = true; access.dest64 = false; return true;
        }
    }
    if ((opc & 2u) != 0u) {
        if (size != 0u) return false;
        access.size = 16u;
    } else {
        access.size = 1u << size;
    }
    access.load = (opc & 1u) != 0u;
    return true;
}

unsigned log2_size(std::uint32_t size) noexcept {
    unsigned shift = 0u;
    while ((1u << shift) < size) ++shift;
    return shift;
}

std::uint64_t extend_register(std::uint64_t value, unsigned option) noexcept {
    switch (option) {
    case 2u: return value & 0xFFFFFFFFu;                                         // UXTW
    case 6u: return static_cast<std::uint64_t>(sign_extend(value, 32u));          // SXTW
    case 3u: case 7u: return value;                                               // LSL / SXTX
    default: return ~std::uint64_t{0};                                            // not a load/store form
    }
}

ArmAccess decode_arm(std::uint32_t instruction, const ArmRegisters& regs) noexcept {
    ArmAccess access{};
    access.rt = instruction & 31u;
    access.rn = (instruction >> 5u) & 31u;
    const std::uint64_t base = read_x(regs, access.rn, true);

    // Load/store register, unsigned immediate offset.
    if ((instruction & 0x3B000000u) == 0x39000000u) {
        if (!classify_single(instruction >> 30u, ((instruction >> 26u) & 1u) != 0u,
                             (instruction >> 22u) & 3u, access)) {
            return access;
        }
        const std::uint64_t imm12 = (instruction >> 10u) & 0xFFFu;
        access.address = base + (imm12 << log2_size(access.size));
        access.valid = true;
        return access;
    }
    // Register offset.
    if ((instruction & 0x3B200C00u) == 0x38200800u) {
        if (!classify_single(instruction >> 30u, ((instruction >> 26u) & 1u) != 0u,
                             (instruction >> 22u) & 3u, access)) {
            return access;
        }
        const unsigned option = (instruction >> 13u) & 7u;
        const std::uint64_t index = extend_register(
            read_x(regs, (instruction >> 16u) & 31u, false), option);
        if (option != 2u && option != 3u && option != 6u && option != 7u) return access;
        const unsigned shift = ((instruction >> 12u) & 1u) != 0u ? log2_size(access.size) : 0u;
        access.address = base + (index << shift);
        access.valid = true;
        return access;
    }
    // Unscaled immediate, pre/post-index, unprivileged.
    if ((instruction & 0x3B200000u) == 0x38000000u) {
        if (!classify_single(instruction >> 30u, ((instruction >> 26u) & 1u) != 0u,
                             (instruction >> 22u) & 3u, access)) {
            return access;
        }
        const auto offset =
            static_cast<std::uint64_t>(sign_extend((instruction >> 12u) & 0x1FFu, 9u));
        switch ((instruction >> 10u) & 3u) {
        case 1u:  // post-index
            access.address = base;
            access.writeback = true;
            access.new_base = base + offset;
            break;
        case 3u:  // pre-index
            access.address = base + offset;
            access.writeback = true;
            access.new_base = access.address;
            break;
        default:  // LDUR/STUR, LDTR/STTR
            access.address = base + offset;
            break;
        }
        access.valid = true;
        return access;
    }
    // Load/store pair.
    if ((instruction & 0x3A000000u) == 0x28000000u) {
        const unsigned opc = instruction >> 30u;
        const bool vector = ((instruction >> 26u) & 1u) != 0u;
        const unsigned type = (instruction >> 23u) & 3u;
        access.load = ((instruction >> 22u) & 1u) != 0u;
        access.simd = vector;
        access.kind = ArmKind::Pair;
        access.rt2 = (instruction >> 10u) & 31u;
        if (vector) {
            if (opc == 3u) return access;
            access.size = 4u << opc;
        } else if (opc == 0u) {
            access.size = 4u;
        } else if (opc == 2u) {
            access.size = 8u;
            access.dest64 = true;
        } else if (opc == 1u && access.load) {  // LDPSW
            access.size = 4u;
            access.sign_extend = true;
            access.dest64 = true;
        } else {
            return access;
        }
        const auto offset = static_cast<std::uint64_t>(
            sign_extend((instruction >> 15u) & 0x7Fu, 7u) * static_cast<std::int64_t>(access.size));
        switch (type) {
        case 1u:  // post-index
            access.address = base;
            access.writeback = true;
            access.new_base = base + offset;
            break;
        case 3u:  // pre-index
            access.address = base + offset;
            access.writeback = true;
            access.new_base = access.address;
            break;
        default:  // signed offset, non-temporal
            access.address = base + offset;
            break;
        }
        access.valid = true;
        return access;
    }
    // LD1/ST1 (single structure, one register): no offset or post-index.
    const bool lane_no_offset = (instruction & 0xBFBF0000u) == 0x0D000000u;
    const bool lane_post_index = (instruction & 0xBFA00000u) == 0x0D800000u;
    if (lane_no_offset || lane_post_index) {
        const unsigned q = (instruction >> 30u) & 1u;
        const unsigned opcode = (instruction >> 13u) & 7u;
        const unsigned s = (instruction >> 12u) & 1u;
        const unsigned size_field = (instruction >> 10u) & 3u;
        access.load = ((instruction >> 22u) & 1u) != 0u;
        access.simd = true;
        access.kind = ArmKind::Lane;
        if (opcode == 0u) {
            access.size = 1u;
            access.lane = (q << 3u) | (s << 2u) | size_field;
        } else if (opcode == 2u && (size_field & 1u) == 0u) {
            access.size = 2u;
            access.lane = (q << 2u) | (s << 1u) | (size_field >> 1u);
        } else if (opcode == 4u && size_field == 0u) {
            access.size = 4u;
            access.lane = (q << 1u) | s;
        } else if (opcode == 4u && size_field == 1u && s == 0u) {
            access.size = 8u;
            access.lane = q;
        } else {
            return access;
        }
        access.address = base;
        if (lane_post_index) {
            const unsigned rm = (instruction >> 16u) & 31u;
            access.writeback = true;
            access.new_base = base + (rm == 31u ? access.size : read_x(regs, rm, false));
        }
        access.valid = true;
        return access;
    }
    return access;
}

bool complete_arm_access(ArmRegisters& regs, const ArmAccess& access,
                         std::uint32_t guest_address) noexcept {
    const std::uint32_t elements = access.kind == ArmKind::Pair ? 2u : 1u;
    alignas(16) std::byte image[32]{};
    const unsigned registers[2] = {access.rt, access.rt2};

    if (access.simd && regs.v == nullptr) return false;

    if (!access.load) {
        for (std::uint32_t e = 0; e < elements; ++e) {
            std::byte* const out = image + e * access.size;
            if (access.simd) {
                const unsigned char* const vreg = regs.v + 16u * registers[e];
                const unsigned offset = access.kind == ArmKind::Lane ? access.lane * access.size : 0u;
                std::memcpy(out, vreg + offset, access.size);
            } else {
                const std::uint64_t value = read_x(regs, registers[e], false);
                std::memcpy(out, &value, access.size);  // little-endian host
            }
        }
        if (!fault_transfer(guest_address, access.size * elements, true, image)) return false;
        g_fault_writes.fetch_add(1u, std::memory_order_relaxed);
    } else {
        if (!fault_transfer(guest_address, access.size * elements, false, image)) return false;
        g_fault_reads.fetch_add(1u, std::memory_order_relaxed);
        for (std::uint32_t e = 0; e < elements; ++e) {
            const std::byte* const in = image + e * access.size;
            if (access.simd) {
                unsigned char* const vreg = regs.v + 16u * registers[e];
                if (access.kind == ArmKind::Lane) {
                    std::memcpy(vreg + access.lane * access.size, in, access.size);
                } else {
                    std::memset(vreg, 0, 16u);
                    std::memcpy(vreg, in, access.size);
                }
                sync_vector_register(regs, registers[e]);
            } else {
                std::uint64_t value = 0u;
                std::memcpy(&value, in, access.size);
                if (access.sign_extend) {
                    value = static_cast<std::uint64_t>(sign_extend(value, access.size * 8u));
                }
                if (!access.dest64) value &= 0xFFFFFFFFu;
                write_x(regs, registers[e], value, false);
            }
        }
    }
    if (access.writeback) write_x(regs, access.rn, access.new_base, true);
    *regs.pc += 4u;
    return true;
}

bool handle_fault(siginfo_t* info, void* raw_context) noexcept {
    if (info == nullptr || raw_context == nullptr || g_guest_base == nullptr) return false;
    const auto fault = reinterpret_cast<std::uintptr_t>(info->si_addr);
    const auto view = reinterpret_cast<std::uintptr_t>(g_guest_base);
    if (fault < view || fault >= view + kGuestSpaceSize + kOverrunGuard) return false;
    if (current_thread_id() != g_fault_callbacks.thread_id) return false;

    ArmRegisters regs;
    if (!registers_from_context(raw_context, regs)) return false;
    std::uint32_t instruction = 0u;
    std::memcpy(&instruction, reinterpret_cast<const void*>(*regs.pc), sizeof(instruction));
    const ArmAccess access = decode_arm(instruction, regs);
    const auto fault_guest = static_cast<std::uint32_t>(fault - view);
    if (!access.valid) {
        note_unhandled(fault_guest, instruction, "unknown instruction form");
        return false;
    }
    const std::uint64_t total = std::uint64_t{access.size} *
                                (access.kind == ArmKind::Pair ? 2u : 1u);
    if (access.address < view || access.address + total > view + kGuestSpaceSize) {
        note_unhandled(fault_guest, instruction, "access leaves the guest view");
        return false;
    }
    const auto guest_address = static_cast<std::uint32_t>(access.address - view);
    count_fault_page(guest_address);
    count_fault_site(static_cast<std::uintptr_t>(*regs.pc), guest_address);
    if ((guest_address & 0xFFFF0000u) == 0xE0000000u) {
        g_fault_locked_cache.fetch_add(1u, std::memory_order_relaxed);
    } else if ((guest_address & 0xFF000000u) == 0xCC000000u ||
               (guest_address & 0xFF000000u) == 0x0C000000u) {
        g_fault_mmio.fetch_add(1u, std::memory_order_relaxed);
    }
    if (!complete_arm_access(regs, access, guest_address)) {
        note_unhandled(guest_address, instruction,
                       access.load ? "load not accepted" : "store not accepted");
        return false;
    }
    return true;
}

#else

bool handle_fault(siginfo_t*, void*) noexcept {
    (void)&count_fault_site; (void)&count_fault_page; (void)&note_unhandled; (void)&fault_transfer;
    return false;
}

#endif  // GALAXY_FASTMEM_HANDLER_AARCH64

void chain_signal(int signal_number, siginfo_t* info, void* context) noexcept {
    const struct sigaction& previous = signal_number == SIGBUS ? g_previous_bus : g_previous_segv;
    if ((previous.sa_flags & SA_SIGINFO) != 0 && previous.sa_sigaction != nullptr) {
        previous.sa_sigaction(signal_number, info, context);
        return;
    }
    if (previous.sa_handler != SIG_DFL && previous.sa_handler != SIG_IGN &&
        previous.sa_handler != nullptr) {
        previous.sa_handler(signal_number);
        return;
    }
    // Default action: returning re-executes the access, which now crashes
    // with the ordinary report.
    signal(signal_number, SIG_DFL);
}

void fastmem_signal_handler(int signal_number, siginfo_t* info, void* context) {
    const int saved_errno = errno;
    if (!handle_fault(info, context)) chain_signal(signal_number, info, context);
    errno = saved_errno;
}

}  // namespace

void install_fault_handler(const FaultAccessCallbacks& callbacks) {
    const std::lock_guard<std::mutex> lock(g_init_mutex);
#if !GALAXY_FASTMEM_HANDLER_AARCH64
    (void)callbacks;
    (void)&fastmem_signal_handler;
    (void)g_fault_handler_installed;
    throw std::runtime_error("the fastmem fault handler on POSIX supports AArch64 only");
#else
    g_fault_callbacks = callbacks;
    if (g_fault_handler_installed.load(std::memory_order_acquire)) return;
    struct sigaction action {};
    action.sa_sigaction = &fastmem_signal_handler;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGSEGV, &action, &g_previous_segv) != 0) fail_errno("sigaction(SIGSEGV)");
    if (sigaction(SIGBUS, &action, &g_previous_bus) != 0) fail_errno("sigaction(SIGBUS)");
    g_fault_handler_installed.store(true, std::memory_order_release);
#endif
}

FaultStats fault_stats() noexcept {
    FaultStats stats{};
    stats.handled_reads = g_fault_reads.load(std::memory_order_relaxed);
    stats.handled_writes = g_fault_writes.load(std::memory_order_relaxed);
    stats.locked_cache = g_fault_locked_cache.load(std::memory_order_relaxed);
    stats.mmio = g_fault_mmio.load(std::memory_order_relaxed);
    stats.unhandled = g_fault_unhandled.load(std::memory_order_relaxed);
    bool taken[kFaultPageSlots]{};
    for (std::size_t rank = 0; rank < 8u; ++rank) {
        std::size_t best = kFaultPageSlots;
        for (std::size_t i = 0; i < g_fault_page_used; ++i) {
            if (taken[i]) continue;
            if (best == kFaultPageSlots || g_fault_page_counts[i] > g_fault_page_counts[best]) best = i;
        }
        if (best == kFaultPageSlots) break;
        taken[best] = true;
        stats.hot_pages[rank] = g_fault_pages[best];
        stats.hot_counts[rank] = g_fault_page_counts[best];
    }
    bool taken_address[kFaultAddressSlots]{};
    for (std::size_t rank = 0; rank < 16u; ++rank) {
        std::size_t best = kFaultAddressSlots;
        for (std::size_t i = 0; i < g_fault_address_used; ++i) {
            if (taken_address[i]) continue;
            if (best == kFaultAddressSlots ||
                g_fault_address_counts[i] > g_fault_address_counts[best]) best = i;
        }
        if (best == kFaultAddressSlots) break;
        taken_address[best] = true;
        stats.hot_addresses[rank] = g_fault_addresses[best];
        stats.hot_address_counts[rank] = g_fault_address_counts[best];
    }
    bool taken_site[kFaultSiteSlots]{};
    for (std::size_t rank = 0; rank < 32u; ++rank) {
        std::size_t best = kFaultSiteSlots;
        for (std::size_t i = 0; i < g_fault_site_used; ++i) {
            if (taken_site[i]) continue;
            if (best == kFaultSiteSlots || g_fault_site_counts[i] > g_fault_site_counts[best]) best = i;
        }
        if (best == kFaultSiteSlots) break;
        taken_site[best] = true;
        stats.hot_sites[rank] = g_fault_sites[best];
        stats.hot_site_addresses[rank] = g_fault_site_addresses[best];
        stats.hot_site_counts[rank] = g_fault_site_counts[best];
    }
    return stats;
}

const char* last_unhandled_fault_message() noexcept { return g_last_unhandled; }

OwnerLease::~OwnerLease() {
    if (active_) g_owner_occupied.store(false, std::memory_order_release);
}

bool OwnerLease::acquire() {
    if (active_) return true;
    bool expected = false;
    if (!g_owner_occupied.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        return false;
    }
    try {
        initialize();
        std::memset(mem1_host_view(), 0, kMem1Size);
        std::memset(mem2_host_view(), 0, kMem2Size);
    } catch (...) {
        g_owner_occupied.store(false, std::memory_order_release);
        throw;
    }
    active_ = true;
    return true;
}

}  // namespace galaxy::host::flat_guest_memory

#endif  // !_WIN32
