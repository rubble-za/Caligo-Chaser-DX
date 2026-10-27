// SPDX-License-Identifier: MIT
// The ARM CPU behind the host. Default: dynarmic, a JIT that translates the guest's ARM/Thumb code to the host CPU (x86-64 or arm64) and runs it directly.
// -DCC_UNICORN builds the original Unicorn (QEMU TCG) interpreter instead: slower, but it has the dev hooks (CC_PROFILE, CC_HOOKVK, CC_WLOG).
// Either way the rest of the host only sees: rd32/wr32/rdbuf/wrbuf/rdstr (guest memory), reg/setreg (registers), callGuest (run guest code until it returns),
// stub slots (guest addresses whose execution calls a host function) and function hooks (a guest function whose entry runs a host function first).
//
// Guest address space (flat host buffers):   see the constants below. Nothing else is mapped; any other access is reported and ends the program.
static const uint32_t LIB_BASE   = 0x10000000, LIB_SIZE = 0x00500000;
static const uint32_t STUB_BASE  = 0x20000000, STUB_SIZE = 0x00020000;
static const uint32_t DATA_BASE  = 0x21000000, DATA_SIZE = 0x00100000;   // misc guest data (JNIEnv, __sF, ...)
static const uint32_t RET_ADDR   = 0x22000000;                           // sentinel return address: executing it ends callGuest
static const uint32_t STACK_BASE = 0x30000000, STACK_SIZE = 0x00800000;
static const uint32_t HEAP_BASE  = 0x40000000, HEAP_SIZE = 0x10000000;

static void die(const char* msg) { fprintf(stderr, "FATAL: %s\n", msg); exit(1); }

// Core registers by number (r0..r12, sp, lr, pc, cpsr).
enum { R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC, CPSR };

// ---- host-side hooks that both backends call ----
static void stubInvoke(uint32_t index);                                  // a stub slot was executed (host.cpp)
struct FnHook { uint32_t addr; std::function<bool()> fn; };              // fn returns true: return to the caller at once, false: run the function
static std::vector<FnHook> g_fnHooks;

#ifdef CC_UNICORN
// =============================== Unicorn backend ===============================
#include <unicorn/unicorn.h>
static const uint32_t STUB_STRIDE = 4;                                   // one "bx lr" per stub
static uc_engine* g_uc;
#define UC_CHECK(x) do { uc_err e_ = (x); if (e_ != UC_ERR_OK) { fprintf(stderr, "uc error %s at %s:%d\n", uc_strerror(e_), __FILE__, __LINE__); exit(1);} } while (0)
static const int g_ucReg[17] = { UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_CPSR };
static uint32_t rd32(uint32_t a) { uint32_t v = 0; UC_CHECK(uc_mem_read(g_uc, a, &v, 4)); return v; }
static void wr32(uint32_t a, uint32_t v) { UC_CHECK(uc_mem_write(g_uc, a, &v, 4)); }
static std::string rdstr(uint32_t a, size_t max = 4096) {
    std::string s; char c;
    while (s.size() < max && uc_mem_read(g_uc, a++, &c, 1) == UC_ERR_OK && c) s.push_back(c);
    return s;
}
static void rdbuf(uint32_t a, void* dst, size_t n) { UC_CHECK(uc_mem_read(g_uc, a, dst, n)); }
static void wrbuf(uint32_t a, const void* src, size_t n) { UC_CHECK(uc_mem_write(g_uc, a, src, n)); }
static uint32_t reg(int r) { uint32_t v; uc_reg_read(g_uc, g_ucReg[r], &v); return v; }
static void setreg(int r, uint32_t v) { uc_reg_write(g_uc, g_ucReg[r], &v); }
static void cpuMap(uint32_t base, uint32_t size) { UC_CHECK(uc_mem_map(g_uc, base, size, UC_PROT_ALL)); }
static void onMemInvalidUc(uc_engine*, uc_mem_type type, uint64_t addr, int size, int64_t, void*);
static void onCodeUc(uc_engine*, uint64_t addr, uint32_t, void*) { uint32_t i = ((uint32_t)addr - STUB_BASE) / STUB_STRIDE; stubInvoke(i); }
static void cpuInit() {
    UC_CHECK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &g_uc));
    cpuMap(STUB_BASE, STUB_SIZE); cpuMap(DATA_BASE, DATA_SIZE); cpuMap(RET_ADDR, 0x1000); cpuMap(STACK_BASE, STACK_SIZE); cpuMap(HEAP_BASE, HEAP_SIZE);
    setreg(SP, STACK_BASE + STACK_SIZE - 64);
}
static void cpuInstallHooks() {
    static uc_hook h1, h2;
    UC_CHECK(uc_hook_add(g_uc, &h1, UC_HOOK_CODE, (void*)onCodeUc, nullptr, STUB_BASE, STUB_BASE + STUB_SIZE - 1));
    UC_CHECK(uc_hook_add(g_uc, &h2, UC_HOOK_MEM_READ_UNMAPPED | UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_FETCH_UNMAPPED, (void*)onMemInvalidUc, nullptr, 1, 0));
}
static void cpuWriteStub(uint32_t index) { uint32_t bxlr = 0xe12fff1e; wrbuf(STUB_BASE + STUB_STRIDE * index, &bxlr, 4); }
static void onFnHookUc(uc_engine*, uint64_t, uint32_t, void* ud) { if (g_fnHooks[(size_t)(uintptr_t)ud].fn()) setreg(PC, reg(LR)); }   // "return immediately" (LR keeps the Thumb bit)
static void cpuHookFunction(uint32_t addr, std::function<bool()> fn) {
    static uc_hook hooks[16]; size_t id = g_fnHooks.size(); g_fnHooks.push_back({addr, std::move(fn)}); uint32_t a = addr & ~1u;
    UC_CHECK(uc_hook_add(g_uc, &hooks[id], UC_HOOK_CODE, (void*)onFnHookUc, (void*)(uintptr_t)id, a, a));
}
static uint32_t cpuCall(uint32_t fn, const std::vector<uint32_t>& args, uint32_t sp) {
    for (size_t i = 0; i < args.size() && i < 4; i++) setreg(R0 + (int)i, args[i]);
    setreg(SP, sp); setreg(LR, RET_ADDR);
    uc_err e = uc_emu_start(g_uc, fn, RET_ADDR, 0, 0);                   // bit0 = Thumb, honoured by Unicorn
    if (e != UC_ERR_OK) {
        uint32_t pc = reg(PC); fprintf(stderr, "emulation error: %s at pc=0x%x (lib+0x%x) lr=0x%x\n", uc_strerror(e), pc, pc - LIB_BASE, reg(LR));
        for (int i = 0; i < 13; i++) fprintf(stderr, " r%d=%08x", i, reg(i)); fprintf(stderr, "\n");
        exit(2);
    }
    return reg(R0);
}
static uint32_t cpuStackBase() { uint32_t sp = reg(SP); return sp ? sp : STACK_BASE + STACK_SIZE - 64; }
static uint32_t cpuOuterSp() { return reg(SP); }
#else
// =============================== dynarmic backend ===============================
#include <array>
#include <dynarmic/interface/A32/a32.h>
#include <dynarmic/interface/A32/config.h>
static const uint32_t STUB_STRIDE = 8;                                   // "svc #(0x100 + index)" then "bx lr"
using DynJit = Dynarmic::A32::Jit;

static std::array<uint8_t*, 1u << 20>* g_pt;                             // page table: guest page -> host page (nullptr = unmapped)
static void cpuMap(uint32_t base, uint32_t size) {
    if (!g_pt) { g_pt = new std::array<uint8_t*, 1u << 20>(); g_pt->fill(nullptr); }
    uint8_t* m = (uint8_t*)calloc(size, 1); if (!m) die("out of memory mapping guest memory");
    for (uint32_t i = 0; i < size / 4096; i++) (*g_pt)[(base >> 12) + i] = m + (size_t)i * 4096;
}
[[noreturn]] static void badAccess(const char* what, uint32_t a, int size);
static inline uint8_t* hostAddr(uint32_t a) { uint8_t* p = (*g_pt)[a >> 12]; return p ? p + (a & 0xfff) : nullptr; }
static void rdbuf(uint32_t a, void* dst, size_t n) {
    uint8_t* d = (uint8_t*)dst;
    while (n) { uint8_t* p = hostAddr(a); if (!p) badAccess("read", a, (int)n); size_t c = std::min<size_t>(n, 4096 - (a & 0xfff)); memcpy(d, p, c); d += c; a += (uint32_t)c; n -= c; }
}
static void wrbuf(uint32_t a, const void* src, size_t n) {
    const uint8_t* s = (const uint8_t*)src;
    while (n) { uint8_t* p = hostAddr(a); if (!p) badAccess("write", a, (int)n); size_t c = std::min<size_t>(n, 4096 - (a & 0xfff)); memcpy(p, s, c); s += c; a += (uint32_t)c; n -= c; }
}
static uint32_t rd32(uint32_t a) { uint32_t v; uint8_t* p = hostAddr(a); if (p && (a & 0xfff) <= 4092) memcpy(&v, p, 4); else rdbuf(a, &v, 4); return v; }
static void wr32(uint32_t a, uint32_t v) { uint8_t* p = hostAddr(a); if (p && (a & 0xfff) <= 4092) memcpy(p, &v, 4); else wrbuf(a, &v, 4); }
static std::string rdstr(uint32_t a, size_t max = 4096) {
    std::string s;
    while (s.size() < max) { uint8_t* p = hostAddr(a++); if (!p || !*p) break; s.push_back((char)*p); }
    return s;
}

static int g_depth = 0;                                                  // number of guest runs in progress (host stubs can call back into the guest)
static void onSvc(uint32_t swi);
struct DynCpu : Dynarmic::A32::UserCallbacks {
    std::unique_ptr<DynJit> jit;
    std::uint8_t MemoryRead8(uint32_t a) override { uint8_t v; rdbuf(a, &v, 1); return v; }
    std::uint16_t MemoryRead16(uint32_t a) override { uint16_t v; rdbuf(a, &v, 2); return v; }
    std::uint32_t MemoryRead32(uint32_t a) override { uint32_t v; rdbuf(a, &v, 4); return v; }
    std::uint64_t MemoryRead64(uint32_t a) override { uint64_t v; rdbuf(a, &v, 8); return v; }
    void MemoryWrite8(uint32_t a, std::uint8_t v) override { wrbuf(a, &v, 1); }
    void MemoryWrite16(uint32_t a, std::uint16_t v) override { wrbuf(a, &v, 2); }
    void MemoryWrite32(uint32_t a, std::uint32_t v) override { wrbuf(a, &v, 4); }
    void MemoryWrite64(uint32_t a, std::uint64_t v) override { wrbuf(a, &v, 8); }
    void InterpreterFallback(uint32_t pc, size_t n) override { fprintf(stderr, "dynarmic: interpreter fallback at pc=0x%x (lib+0x%x), %zu instructions\n", pc, pc - LIB_BASE, n); exit(2); }
    void CallSVC(std::uint32_t swi) override { onSvc(swi); }
    void ExceptionRaised(uint32_t pc, Dynarmic::A32::Exception e) override { fprintf(stderr, "dynarmic: exception %d at pc=0x%x (lib+0x%x)\n", (int)e, pc, pc - LIB_BASE); exit(2); }
    void AddTicks(std::uint64_t) override {}
    std::uint64_t GetTicksRemaining() override { return 1ull << 60; }
};
static std::vector<std::unique_ptr<DynCpu>> g_cpus;
static DynCpu* cpuGet(size_t i) {
    while (g_cpus.size() <= i) {
        auto c = std::make_unique<DynCpu>();
        Dynarmic::A32::UserConfig cfg; cfg.callbacks = c.get();
        cfg.page_table = g_pt; cfg.absolute_offset_page_table = false;
        cfg.detect_misaligned_access_via_page_table = 16 | 32 | 64; cfg.only_detect_misalignment_via_page_table_on_page_boundary = true;   // page-straddling accesses take the callbacks
        cfg.enable_cycle_counting = false;                               // we never stop on a tick budget, only when the guest returns to RET_ADDR
        cfg.arch_version = Dynarmic::A32::ArchVersion::v7; cfg.define_unpredictable_behaviour = true;
        c->jit = std::make_unique<DynJit>(cfg);
        g_cpus.push_back(std::move(c));
    }
    return g_cpus[i].get();
}
static DynJit* cur() { return cpuGet(g_depth > 0 ? (size_t)g_depth - 1 : 0)->jit.get(); }
static uint32_t reg(int r) { return r == CPSR ? cur()->Cpsr() : cur()->Regs()[r]; }
static void setreg(int r, uint32_t v) { if (r == CPSR) cur()->SetCpsr(v); else cur()->Regs()[r] = v; }
[[noreturn]] static void badAccess(const char* what, uint32_t a, int size) {
    fprintf(stderr, "invalid memory %s addr=0x%x size=%d pc=0x%x (lib+0x%x) lr=0x%x\n", what, a, size, reg(PC), reg(PC) - LIB_BASE, reg(LR));
    exit(2);
}
static void cpuInit() {
    cpuMap(STUB_BASE, STUB_SIZE); cpuMap(DATA_BASE, DATA_SIZE); cpuMap(RET_ADDR, 0x1000); cpuMap(STACK_BASE, STACK_SIZE); cpuMap(HEAP_BASE, HEAP_SIZE);
    wr32(RET_ADDR, 0xef000000u | 0xffffff);                              // svc #0xffffff: callGuest's return sentinel
    cpuGet(0);
}
static void cpuInstallHooks() {}
static void cpuWriteStub(uint32_t index) { wr32(STUB_BASE + STUB_STRIDE * index, 0xef000000u | (0x100 + index)); wr32(STUB_BASE + STUB_STRIDE * index + 4, 0xe12fff1eu); }
// A hooked Thumb function gets its first instruction (push {r4-r7, lr}, which every hooked function starts with) replaced by "svc #id"; the hook runs, then either returns to the
// caller or does the displaced push by hand and carries on at the second instruction.
static void cpuHookFunction(uint32_t addr, std::function<bool()> fn) {
    if (!(addr & 1) || g_fnHooks.size() >= 16) die("cannot hook this function");
    uint16_t first = 0; rdbuf(addr & ~1u, &first, 2); if (first != 0xb5f0) die("hooked function does not start with push {r4-r7, lr}");
    uint16_t svc = (uint16_t)(0xdf00 | g_fnHooks.size()); wrbuf(addr & ~1u, &svc, 2);
    g_fnHooks.push_back({addr, std::move(fn)});
    for (auto& c : g_cpus) c->jit->ClearCache();
}
static void onSvc(uint32_t swi) {
    if (swi >= 0x100 && swi < 0x10000) { stubInvoke(swi - 0x100); return; }
    if (swi == 0xffffff) { cur()->HaltExecution(); return; }
    if (swi < g_fnHooks.size()) {
        FnHook& h = g_fnHooks[swi];
        if (h.fn()) { uint32_t lr = reg(LR); setreg(PC, lr & ~1u); setreg(CPSR, (reg(CPSR) & ~0x20u) | ((lr & 1) ? 0x20u : 0)); }
        else { uint32_t sp = reg(SP) - 20; wr32(sp, reg(R4)); wr32(sp + 4, reg(R5)); wr32(sp + 8, reg(R6)); wr32(sp + 12, reg(R7)); wr32(sp + 16, reg(LR)); setreg(SP, sp); setreg(PC, (h.addr & ~1u) + 2); }
        return;
    }
    fprintf(stderr, "dynarmic: unexpected svc #%u at pc=0x%x (lib+0x%x)\n", swi, reg(PC), reg(PC) - LIB_BASE); exit(2);
}
static uint32_t cpuStackBase() { return g_depth == 0 ? (reg(SP) ? reg(SP) : STACK_BASE + STACK_SIZE - 64) : cpuGet((size_t)g_depth - 1)->jit->Regs()[13] - 256; }
static uint32_t cpuOuterSp() { return reg(SP); }
static uint32_t cpuCall(uint32_t fn, const std::vector<uint32_t>& args, uint32_t sp) {
    DynJit* j = cpuGet((size_t)g_depth)->jit.get(); g_depth++;
    for (size_t i = 0; i < args.size() && i < 4; i++) j->Regs()[i] = args[i];
    j->Regs()[13] = sp; j->Regs()[14] = RET_ADDR; j->Regs()[15] = fn & ~1u; j->SetCpsr(0x10 | ((fn & 1) ? 0x20u : 0));
    j->ClearHalt(); j->Run();
    g_depth--;
    return j->Regs()[0];
}
#endif
