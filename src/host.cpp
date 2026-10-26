// Caligo Chaser host: loads the original 32-bit ARM libgame.so into Unicorn,
// resolves its imports to host-side implementations (libc, GLES1, JNI).
// Stage 1: loader + import tracing.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <functional>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <memory>
#include <deque>
#include <sstream>

// ---------- minimal ELF32 definitions (portable; no <elf.h> so Windows builds work) ----------
#pragma pack(push, 1)
struct Elf32_Ehdr { uint8_t e_ident[16]; uint16_t e_type, e_machine; uint32_t e_version, e_entry, e_phoff, e_shoff, e_flags; uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx; };
struct Elf32_Phdr { uint32_t p_type, p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_flags, p_align; };
struct Elf32_Dyn { int32_t d_tag; uint32_t d_val; };
struct Elf32_Sym { uint32_t st_name, st_value, st_size; uint8_t st_info, st_other; uint16_t st_shndx; };
struct Elf32_Rel { uint32_t r_offset, r_info; };
#pragma pack(pop)
enum { PT_LOAD = 1, PT_DYNAMIC = 2 };
enum { DT_NULL = 0, DT_HASH = 4, DT_STRTAB = 5, DT_SYMTAB = 6, DT_REL = 17, DT_RELSZ = 18, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_STRSZ = 10 };
enum { R_ARM_ABS32 = 2, R_ARM_GLOB_DAT = 21, R_ARM_JUMP_SLOT = 22, R_ARM_RELATIVE = 23 };

static bool g_traceFiles = false;
static bool g_trace = false;
#include "cpu.inc"

// ---------- guest heap (simple size-header allocator with exact-size free lists) ----------
static uint32_t g_heapTop = HEAP_BASE;
static std::map<uint32_t, std::vector<uint32_t>> g_free;   // size -> blocks
static uint32_t gmalloc(uint32_t n) {
    n = (n + 15) & ~15u; if (!n) n = 16;
    auto it = g_free.find(n);
    if (it != g_free.end() && !it->second.empty()) { uint32_t p = it->second.back(); it->second.pop_back(); return p; }
    uint32_t p = g_heapTop + 16; g_heapTop += 16 + n;
    if (g_heapTop > HEAP_BASE + HEAP_SIZE) die("guest heap exhausted");
    wr32(p - 8, n);
    return p;
}
static void gfree(uint32_t p) { if (!p) return; g_free[rd32(p - 8)].push_back(p); }
static uint32_t gsize(uint32_t p) { return rd32(p - 8); }

// ---------- call context for import handlers ----------
static uint32_t arg(int i) { return i < 4 ? reg(R0 + i) : rd32(reg(SP) + 4 * (i - 4)); }
static void ret(uint32_t v) { setreg(R0, v); }
static void ret64(uint64_t v) { setreg(R0, (uint32_t)v); setreg(R1, (uint32_t)(v >> 32)); }
static double argd(int i) { uint64_t u = (uint64_t)arg(i + 1) << 32 | arg(i); double d; memcpy(&d, &u, 8); return d; }
static float argf(int i) { uint32_t u = arg(i); float f; memcpy(&f, &u, 4); return f; }

// ---------- varargs reader (ARM EABI) ----------
struct VaReader {
    bool fromList; uint32_t listPtr; int idx;      // idx = next arg slot when reading from regs+stack
    uint32_t next32() {
        if (fromList) { uint32_t v = rd32(listPtr); listPtr += 4; return v; }
        return arg(idx++);
    }
    uint64_t next64() {
        if (fromList) { listPtr = (listPtr + 7) & ~7u; uint64_t lo = rd32(listPtr), hi = rd32(listPtr + 4); listPtr += 8; return hi << 32 | lo; }
        if (idx < 4 && (idx & 1)) idx++;               // even register pair
        else if (idx >= 4) { uint32_t a = reg(SP) + 4 * (idx - 4); if (a & 7) idx++; }
        uint64_t lo = arg(idx), hi = arg(idx + 1); idx += 2; return hi << 32 | lo;
    }
};
static std::string guestFormat(const std::string& f, VaReader va) {
    std::string out;
    for (size_t i = 0; i < f.size(); i++) {
        if (f[i] != '%') { out.push_back(f[i]); continue; }
        std::string spec = "%"; i++;
        while (i < f.size() && strchr("-+ #0123456789.*", f[i])) {
            if (f[i] == '*') spec += std::to_string((int)va.next32()); else spec.push_back(f[i]);
            i++;
        }
        bool l = false, ll = false, h = false;
        while (i < f.size() && strchr("lhzt", f[i])) { if (f[i] == 'l') { if (l) ll = true; l = true; } if (f[i] == 'h') h = true; i++; }
        char c = f[i], buf[512];
        switch (c) {
        case '%': out.push_back('%'); break;
        case 'd': case 'i': { spec += ll ? "lld" : "d"; if (ll) snprintf(buf, sizeof buf, spec.c_str(), (long long)va.next64()); else snprintf(buf, sizeof buf, spec.c_str(), h ? (int)(int16_t)va.next32() : (int)va.next32()); out += buf; break; }
        case 'u': case 'x': case 'X': case 'o': { spec += ll ? std::string("ll") + c : std::string(1, c); if (ll) snprintf(buf, sizeof buf, spec.c_str(), (unsigned long long)va.next64()); else snprintf(buf, sizeof buf, spec.c_str(), (unsigned)va.next32()); out += buf; break; }
        case 'c': { spec += 'c'; snprintf(buf, sizeof buf, spec.c_str(), (int)va.next32()); out += buf; break; }
        case 'p': { snprintf(buf, sizeof buf, "0x%x", va.next32()); out += buf; break; }
        case 's': { std::string s = rdstr(va.next32()); spec += 's'; std::vector<char> b(s.size() + 512); snprintf(b.data(), b.size(), spec.c_str(), s.c_str()); out += b.data(); break; }
        case 'f': case 'g': case 'e': case 'G': case 'E': { uint64_t u = va.next64(); double d; memcpy(&d, &u, 8); spec += c; snprintf(buf, sizeof buf, spec.c_str(), d); out += buf; break; }
        default: out += spec; out.push_back(c);
        }
    }
    return out;
}

// ---------- host file table (guest fd / FILE* -> host FILE*) ----------
static std::string g_dataRoot = ".";   // host dir that stands in for /sdcard (contains Com2uS/com.com2us.caligo/)
static std::string g_saveRoot = "save";                    // writable per-user state (settings, texture cache)
static std::string mapPath(const std::string& p) {
    const std::string pkgData = "/data/data/com.com2us.caligo/";
    std::error_code ec;
    if (p.rfind(pkgData, 0) == 0) { std::filesystem::create_directories(g_saveRoot, ec); return g_saveRoot + "/" + p.substr(pkgData.size()); }   // app-private dir
    if (p.rfind("/sdcard/Com2uS/", 0) == 0) return g_dataRoot + "/" + p.substr(8);                                                              // game data (read-only)
    if (p.rfind("/sdcard/", 0) == 0) { std::filesystem::create_directories(g_saveRoot, ec); return g_saveRoot + "/" + p.substr(8); }            // other sdcard writes
    return p;
}
static std::map<int, FILE*> g_fds;
static int g_nextFd = 10;
static std::map<uint32_t, FILE*> g_files;   // guest FILE* -> host FILE*
static uint32_t g_sF;                        // guest __sF array

// ---------- stubs ----------
struct Stub { std::string name; std::function<void()> fn; };
static std::vector<Stub> g_stubs;
static std::unordered_map<std::string, uint32_t> g_stubAddr;
static std::unordered_map<std::string, std::function<void()>> g_impl;
static std::map<std::string, int> g_unimplCount;

static uint32_t stubFor(const std::string& name) {
    auto it = g_stubAddr.find(name);
    if (it != g_stubAddr.end()) return it->second;
    uint32_t addr = STUB_BASE + STUB_STRIDE * (uint32_t)g_stubs.size();
    if (addr >= STUB_BASE + STUB_SIZE) die("stub region full");
    cpuWriteStub((uint32_t)g_stubs.size());
    auto f = g_impl.find(name);
    g_stubs.push_back({name, f == g_impl.end() ? std::function<void()>() : f->second});
    g_stubAddr[name] = addr;
    return addr;
}

// ---------- JNI emulation (Java side is emulated by host code) ----------
static const uint32_t ENV_ADDR = DATA_BASE + 0x1000, ENVTAB = DATA_BASE + 0x1100, VM_ADDR = DATA_BASE + 0x2000, VMTAB = DATA_BASE + 0x2100;
struct JRef { std::string kind, name; uint32_t ptr = 0, len = 0; };
static std::vector<JRef> g_jrefs(1);                       // handle = index
struct JMethod { std::string cls, name, sig; bool isStatic; };
static std::vector<JMethod> g_jmethods(1);
static uint32_t newRef(const std::string& kind, const std::string& name) { g_jrefs.push_back({kind, name}); return (uint32_t)g_jrefs.size() - 1 + 0x7000000; }
static uint32_t newArray(uint32_t elemSize, uint32_t len) { uint32_t h = newRef("array", ""); JRef& r = g_jrefs.back(); r.ptr = gmalloc(elemSize * len + 8); r.len = len; std::vector<uint8_t> z(elemSize * len, 0); if (!z.empty()) wrbuf(r.ptr, z.data(), z.size()); return h; }
static JRef& refOf(uint32_t h) { static JRef none{"?", "?"}; uint32_t i = h - 0x7000000; return i < g_jrefs.size() ? g_jrefs[i] : none; }

static const char* jniName(int i) {
    static std::map<int, const char*> n = {
        {4,"GetVersion"},{6,"FindClass"},{13,"Throw"},{14,"ThrowNew"},{15,"ExceptionOccurred"},{17,"ExceptionClear"},{21,"NewGlobalRef"},{22,"DeleteGlobalRef"},{23,"DeleteLocalRef"},
        {31,"GetObjectClass"},{33,"GetMethodID"},{34,"CallObjectMethod"},{94,"GetFieldID"},{113,"GetStaticMethodID"},
        {114,"CallStaticObjectMethod"},{115,"CallStaticObjectMethodV"},{116,"CallStaticObjectMethodA"},
        {117,"CallStaticBooleanMethod"},{118,"CallStaticBooleanMethodV"},{119,"CallStaticBooleanMethodA"},
        {129,"CallStaticIntMethod"},{130,"CallStaticIntMethodV"},{131,"CallStaticIntMethodA"},
        {132,"CallStaticLongMethod"},{133,"CallStaticLongMethodV"},{134,"CallStaticLongMethodA"},
        {141,"CallStaticVoidMethod"},{142,"CallStaticVoidMethodV"},{143,"CallStaticVoidMethodA"},
        {144,"GetStaticFieldID"},{163,"NewString"},{164,"GetStringLength"},{167,"NewStringUTF"},{168,"GetStringUTFLength"},{169,"GetStringUTFChars"},{170,"ReleaseStringUTFChars"},
        {171,"GetArrayLength"},{172,"NewObjectArray"},{179,"NewIntArray"},{187,"GetIntArrayElements"},{195,"ReleaseIntArrayElements"},{203,"GetIntArrayRegion"},{211,"SetIntArrayRegion"},
        {215,"RegisterNatives"},{219,"GetJavaVM"},{228,"ExceptionCheck"}};
    auto it = n.find(i); return it == n.end() ? "?" : it->second;
}

// Java static method dispatch: returns value in r0 (and r1 for long). Filled in per-method as we learn them.
static std::map<std::string, std::function<uint64_t(const std::vector<uint32_t>&)>> g_java;
static std::vector<uint32_t> readJniArgs(int variant, uint32_t base, const std::string& sig) {
    // variant 0: C varargs at (regs from index 3..), 1: va_list pointer, 2: jvalue array
    std::vector<uint32_t> out;
    size_t i = sig.find('(') + 1;
    VaReader va; va.fromList = (variant == 1); va.listPtr = base; va.idx = 3;
    uint32_t jv = base;
    while (i < sig.size() && sig[i] != ')') {
        char c = sig[i];
        bool wide = (c == 'J' || c == 'D');
        if (c == 'L') { while (sig[i] != ';') i++; }
        else if (c == '[') { while (sig[i] == '[') i++; if (sig[i] == 'L') while (sig[i] != ';') i++; }
        i++;
        if (variant == 2) { out.push_back(rd32(jv)); if (wide) out.push_back(rd32(jv + 4)); jv += 8; }
        else if (wide) { uint64_t v = va.next64(); out.push_back((uint32_t)v); out.push_back((uint32_t)(v >> 32)); }
        else out.push_back(va.next32());
    }
    return out;
}

static void jniCall(int idx) {
    // arg(0)=env
    int staticBase = 114;                         // CallStatic<T>Method at 114 + 3*type
    if (idx >= 114 && idx <= 143) {
        int type = (idx - staticBase) / 3, variant = (idx - staticBase) % 3;
        uint32_t mid = arg(2);
        JMethod& m = g_jmethods[mid];
        std::vector<uint32_t> a = readJniArgs(variant, variant == 0 ? 0 : arg(3), m.sig);
        std::string key = m.cls + "." + m.name + m.sig;
        auto it = g_java.find(m.name);
        if (g_trace || it == g_java.end()) {
            fprintf(stderr, "[java] %s(", key.c_str());
            for (size_t k = 0; k < a.size(); k++) fprintf(stderr, "%s0x%x", k ? "," : "", a[k]);
            fprintf(stderr, ")%s\n", it == g_java.end() ? "  <UNIMPLEMENTED>" : "");
        }
        uint64_t r = it != g_java.end() ? it->second(a) : 0;
        ret64(r);
        return;
    }
    switch (idx) {
    case 4: ret(0x00010006); break;                                           // GetVersion
    case 6: { std::string n = rdstr(arg(1)); ret(newRef("class", n)); if (g_trace) fprintf(stderr, "[jni] FindClass(%s)\n", n.c_str()); break; }
    case 21: case 25: ret(arg(1)); break;                                     // NewGlobalRef / NewLocalRef
    case 22: case 23: break;                                                  // DeleteGlobalRef / DeleteLocalRef
    case 15: ret(0); break; case 17: break; case 228: ret(0); break;          // exceptions: none
    case 31: ret(newRef("class", "?obj")); break;
    case 28: case 29: case 30: ret(newRef("object", refOf(arg(1)).name)); break;                 // NewObject*
    case 171: ret(refOf(arg(1)).len); break;                                                    // GetArrayLength
    case 183: case 184: case 185: case 186: case 187: case 188: case 189: case 190:            // Get<T>ArrayElements (pinned in place)
        if (arg(2)) wr32(arg(2), 1); ret(refOf(arg(1)).ptr); break;
    case 191: case 192: case 193: case 194: case 195: case 196: case 197: case 198: break;     // Release<T>ArrayElements
    case 199: case 200: case 201: case 202: case 203: case 204: case 205: case 206: {          // Get<T>ArrayRegion
        static const int es[8] = {1,1,2,2,4,8,4,8}; int sz = es[idx - 199]; JRef& r = refOf(arg(1));
        std::vector<uint8_t> b((size_t)arg(3) * sz); rdbuf(r.ptr + arg(2) * sz, b.data(), b.size()); wrbuf(arg(4), b.data(), b.size()); break; }
    case 207: case 208: case 209: case 210: case 211: case 212: case 213: case 214: {          // Set<T>ArrayRegion
        static const int es[8] = {1,1,2,2,4,8,4,8}; int sz = es[idx - 207]; JRef& r = refOf(arg(1));
        std::vector<uint8_t> b((size_t)arg(3) * sz); rdbuf(arg(4), b.data(), b.size()); wrbuf(r.ptr + arg(2) * sz, b.data(), b.size()); break; }
    case 175: case 176: case 177: case 178: case 179: case 180: case 181: case 182: {          // New<T>Array
        static const int es[8] = {1,1,2,2,4,8,4,8}; ret(newArray(es[idx - 175], arg(1))); break; }
    case 33: case 113: {                                                      // GetMethodID / GetStaticMethodID
        std::string c = refOf(arg(1)).name, n = rdstr(arg(2)), s = rdstr(arg(3));
        g_jmethods.push_back({c, n, s, idx == 113});
        ret((uint32_t)g_jmethods.size() - 1);
        if (g_trace) fprintf(stderr, "[jni] %s(%s.%s %s) -> %zu\n", jniName(idx), c.c_str(), n.c_str(), s.c_str(), g_jmethods.size() - 1);
        break; }
    case 94: case 144: ret(1); break;                                         // GetFieldID / GetStaticFieldID
    case 167: { std::string s = rdstr(arg(1)); ret(newRef("string", s)); break; }
    case 169: { std::string s = refOf(arg(1)).name; uint32_t p = gmalloc((uint32_t)s.size() + 1); wrbuf(p, s.c_str(), s.size() + 1); if (arg(2)) { uint8_t t = 1; wrbuf(arg(2), &t, 1); } ret(p); break; }
    case 170: gfree(arg(2)); break;
    case 168: ret((uint32_t)refOf(arg(1)).name.size()); break;
    case 219: wr32(arg(1), VM_ADDR); ret(0); break;
    case 215: ret(0); break;                                                  // RegisterNatives
    default:
        fprintf(stderr, "[jni] UNIMPLEMENTED JNIEnv slot %d (%s)\n", idx, jniName(idx)); ret(0);
    }
}

// ---------- ELF loading ----------
static std::vector<uint8_t> g_elf;
static std::unordered_map<std::string, uint32_t> g_exports;   // name -> guest addr
static uint32_t loadLibMem(const std::vector<uint8_t>& elf) {
    g_elf = elf;
    auto* eh = (Elf32_Ehdr*)g_elf.data();
    if (memcmp(eh->e_ident, "\x7f""ELF", 4) || eh->e_machine != 40) die("not an ARM ELF");
    cpuMap(LIB_BASE, LIB_SIZE);
    uint32_t dynOff = 0, dynSz = 0;
    for (int i = 0; i < eh->e_phnum; i++) {
        auto* ph = (Elf32_Phdr*)(g_elf.data() + eh->e_phoff + i * eh->e_phentsize);
        if (ph->p_type == PT_LOAD) { wrbuf(LIB_BASE + ph->p_vaddr, g_elf.data() + ph->p_offset, ph->p_filesz); }
        if (ph->p_type == PT_DYNAMIC) { dynOff = ph->p_offset; dynSz = ph->p_filesz; }
    }
    uint32_t symtab = 0, strtab = 0, rel = 0, relsz = 0, jmprel = 0, pltrelsz = 0, initArr = 0, initArrSz = 0, hash = 0;
    for (auto* d = (Elf32_Dyn*)(g_elf.data() + dynOff); d->d_tag != DT_NULL; d++) {
        switch (d->d_tag) { case DT_SYMTAB: symtab = d->d_val; break; case DT_STRTAB: strtab = d->d_val; break; case DT_REL: rel = d->d_val; break; case DT_RELSZ: relsz = d->d_val; break;
        case DT_JMPREL: jmprel = d->d_val; break; case DT_PLTRELSZ: pltrelsz = d->d_val; break; case DT_INIT_ARRAY: initArr = d->d_val; break; case DT_INIT_ARRAYSZ: initArrSz = d->d_val; break; case DT_HASH: hash = d->d_val; break; }
    }
    // vaddr == file offset for the first (R+X) segment; read tables from the mapped guest image
    std::vector<uint8_t> img(LIB_SIZE); rdbuf(LIB_BASE, img.data(), 0x120000);
    uint32_t nsyms = *(uint32_t*)(img.data() + hash + 4);
    auto sym = [&](uint32_t i) { return (Elf32_Sym*)(img.data() + symtab + i * 16); };
    auto symname = [&](uint32_t i) { return std::string((char*)img.data() + strtab + sym(i)->st_name); };
    for (uint32_t i = 1; i < nsyms; i++) if (sym(i)->st_shndx != 0) g_exports[symname(i)] = LIB_BASE + sym(i)->st_value;

    // data imports
    uint32_t dataCur = DATA_BASE + 0x10000;
    auto dataSym = [&](const std::string& n) -> uint32_t {
        if (n == "__sF") { if (!g_sF) { g_sF = dataCur; dataCur += 3 * 84; for (int i = 0; i < 3; i++) g_files[g_sF + 84 * i] = i == 0 ? stdin : i == 1 ? stdout : stderr; } return g_sF; }
        uint32_t a = dataCur; dataCur += 16;
        if (n == "__stack_chk_guard") wr32(a, 0x4c6d5e7f);
        if (n == "__page_size") wr32(a, 4096);
        return a;
    };
    auto isData = [](const std::string& n) { return n == "__sF" || n == "__stack_chk_guard" || n == "__page_size" || n == "__dso_handle"; };

    auto applyRel = [&](uint32_t tab, uint32_t size) {
        for (uint32_t off = 0; off < size; off += 8) {
            auto* r = (Elf32_Rel*)(img.data() + tab + off);
            uint32_t type = r->r_info & 0xff, si = r->r_info >> 8, where = LIB_BASE + r->r_offset, val = 0;
            if (type == R_ARM_RELATIVE) val = LIB_BASE + rd32(where);
            else if (type == R_ARM_GLOB_DAT || type == R_ARM_JUMP_SLOT || type == R_ARM_ABS32) {
                std::string n = symname(si);
                if (sym(si)->st_shndx != 0) val = LIB_BASE + sym(si)->st_value;
                else val = isData(n) ? dataSym(n) : stubFor(n);
                if (type == R_ARM_ABS32) val += rd32(where);
            } else { fprintf(stderr, "unknown reloc type %u\n", type); continue; }
            wr32(where, val);
        }
    };
    applyRel(rel, relsz);
    applyRel(jmprel, pltrelsz);
    // run init_array
    for (uint32_t i = 0; i < initArrSz / 4; i++) {
        uint32_t fn = rd32(LIB_BASE + initArr + 4 * i);
        fprintf(stderr, "init_array[%u] = 0x%x\n", i, fn);
        g_exports["__init_" + std::to_string(i)] = fn;
    }
    return initArrSz / 4;
}

// ---------- calling into guest ----------
static uint32_t callGuest(uint32_t fn, std::vector<uint32_t> args = {}) {
    uint32_t sp = cpuStackBase() & ~7u;
    // args beyond 4 go on the stack
    if (args.size() > 4) { sp -= 4 * (uint32_t)(args.size() - 4); sp &= ~7u; for (size_t i = 4; i < args.size(); i++) wr32(sp + 4 * (i - 4), args[i]); }
    return cpuCall(fn, args, sp);
}

static void stubInvoke(uint32_t i) {
    if (i >= g_stubs.size()) return;
    Stub& s = g_stubs[i];
    if (s.name.rfind("JNI:", 0) == 0) { jniCall(atoi(s.name.c_str() + 4)); return; }
    if (g_trace) fprintf(stderr, "[call] %s(0x%x, 0x%x, 0x%x, 0x%x)\n", s.name.c_str(), arg(0), arg(1), arg(2), arg(3));
    if (s.fn) s.fn();
    else { if (g_unimplCount[s.name]++ == 0) fprintf(stderr, "[import] UNIMPLEMENTED %s(0x%x, 0x%x, 0x%x, 0x%x)\n", s.name.c_str(), arg(0), arg(1), arg(2), arg(3)); ret(0); }
}
#ifdef CC_UNICORN
static void onMemInvalidUc(uc_engine*, uc_mem_type type, uint64_t addr, int size, int64_t, void*) {
    fprintf(stderr, "invalid memory access type=%d addr=0x%llx size=%d pc=0x%x (lib+0x%x) lr=0x%x\n", type, (unsigned long long)addr, size, reg(PC), reg(PC) - LIB_BASE, reg(LR));
}
#endif

#include "imports.inc"
#include "overlay.inc"
#include "hud.inc"
#include "gl_soft.inc"
#include "apk.inc"
#include "audio.inc"
#include "java_emu.inc"

