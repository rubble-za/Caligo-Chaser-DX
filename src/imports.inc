// Host-side implementations of the libc/libm/bionic imports used by libgame.so.
// Included by main.cpp (after the call-context helpers are defined).
#include <ctime>

static uint32_t g_errnoAddr = 0;
static std::map<uint32_t, uint32_t> g_tls;          // key -> value
static uint32_t g_nextKey = 1;
static uint32_t g_strtokState = 0;

static uint32_t guestStr(const std::string& s) { uint32_t p = gmalloc((uint32_t)s.size() + 1); wrbuf(p, s.c_str(), s.size() + 1); return p; }

static FILE* hostFile(uint32_t g) { auto it = g_files.find(g); return it == g_files.end() ? nullptr : it->second; }

// Snapshot of all core registers so host code can re-enter the guest from inside a call hook.
struct RegSnap { uint32_t r[16]; uint32_t cpsr; };
static RegSnap saveRegs() { RegSnap s; for (int i = 0; i < 13; i++) s.r[i] = reg(i == 0 ? R0 : i == 1 ? R1 : i == 2 ? R2 : i == 3 ? R3 : UC_ARM_REG_R4 + (i - 4)); s.r[13] = reg(SP); s.r[14] = reg(LR); s.r[15] = reg(PC); uc_reg_read(g_uc, UC_ARM_REG_CPSR, &s.cpsr); return s; }
static void loadRegs(const RegSnap& s) { for (int i = 0; i < 13; i++) setreg(i == 0 ? R0 : i == 1 ? R1 : i == 2 ? R2 : i == 3 ? R3 : UC_ARM_REG_R4 + (i - 4), s.r[i]); setreg(SP, s.r[13]); setreg(LR, s.r[14]); uc_reg_write(g_uc, UC_ARM_REG_CPSR, &s.cpsr); }
static uint32_t callGuestNested(uint32_t fn, std::vector<uint32_t> args) {
    RegSnap s = saveRegs();
    uint32_t sp = (s.r[13] - 256) & ~7u;                 // leave the interrupted frame untouched
    setreg(SP, sp);
    uint32_t r = callGuest(fn, args);
    loadRegs(s);
    return r;
}

static void registerImports() {
    auto I = [](const char* n, std::function<void()> f) { g_impl[n] = f; };

    // ---- JavaVM table ----
    I("JVM:6", [] { wr32(arg(1), ENV_ADDR); ret(0); });                    // GetEnv
    I("JVM:4", [] { wr32(arg(1), ENV_ADDR); ret(0); });                    // AttachCurrentThread
    I("JVM:5", [] { ret(0); });                                            // DetachCurrentThread

    // ---- memory / C++ runtime ----
    I("malloc",  [] { ret(gmalloc(arg(0))); });
    I("free",    [] { gfree(arg(0)); });
    I("realloc", [] { uint32_t p = arg(0), n = arg(1); if (!p) { ret(gmalloc(n)); return; }
                      uint32_t old = gsize(p); if (n <= old) { ret(p); return; }
                      uint32_t q = gmalloc(n); std::vector<uint8_t> b(old); rdbuf(p, b.data(), old); wrbuf(q, b.data(), old); gfree(p); ret(q); });
    I("_Znwj", [] { ret(gmalloc(arg(0))); });  I("_Znaj", [] { ret(gmalloc(arg(0))); });
    I("_ZdlPv", [] { gfree(arg(0)); });        I("_ZdaPv", [] { gfree(arg(0)); });
    I("__aeabi_atexit", [] { ret(0); });
    I("__stack_chk_fail", [] { die("stack smashing detected in guest"); });
    I("abort", [] { fprintf(stderr, "guest abort() lr=0x%x (lib+0x%x)\n", reg(LR), reg(LR) - LIB_BASE); exit(3); });
    I("exit",  [] { fprintf(stderr, "guest exit(%d)\n", (int)arg(0)); exit((int)arg(0)); });
    I("__errno", [] { if (!g_errnoAddr) g_errnoAddr = gmalloc(4); ret(g_errnoAddr); });
    I("__gnu_Unwind_Find_exidx", [] { ret(0); });

    // ---- mem/str ----
    I("memcpy",  [] { uint32_t d = arg(0), s = arg(1), n = arg(2); std::vector<uint8_t> b(n); rdbuf(s, b.data(), n); wrbuf(d, b.data(), n); ret(d); });
    I("memmove", g_impl["memcpy"]);
    I("memset",  [] { uint32_t d = arg(0), n = arg(2); std::vector<uint8_t> b(n, (uint8_t)arg(1)); wrbuf(d, b.data(), n); ret(d); });
    I("memcmp",  [] { uint32_t n = arg(2); std::vector<uint8_t> a(n), b(n); rdbuf(arg(0), a.data(), n); rdbuf(arg(1), b.data(), n); ret((uint32_t)memcmp(a.data(), b.data(), n)); });
    I("strlen",  [] { ret((uint32_t)rdstr(arg(0), 1 << 24).size()); });
    I("strcpy",  [] { std::string s = rdstr(arg(1), 1 << 24); wrbuf(arg(0), s.c_str(), s.size() + 1); ret(arg(0)); });
    I("strncpy", [] { std::string s = rdstr(arg(1), arg(2)); std::vector<char> b(arg(2), 0); memcpy(b.data(), s.data(), s.size()); wrbuf(arg(0), b.data(), b.size()); ret(arg(0)); });
    I("strcat",  [] { std::string d = rdstr(arg(0), 1 << 24), s = rdstr(arg(1), 1 << 24); wrbuf(arg(0) + (uint32_t)d.size(), s.c_str(), s.size() + 1); ret(arg(0)); });
    I("strcmp",  [] { ret((uint32_t)strcmp(rdstr(arg(0), 1 << 24).c_str(), rdstr(arg(1), 1 << 24).c_str())); });
    I("strncmp", [] { ret((uint32_t)strncmp(rdstr(arg(0), arg(2)).c_str(), rdstr(arg(1), arg(2)).c_str(), arg(2))); });
    I("strdup",  [] { ret(guestStr(rdstr(arg(0), 1 << 24))); });
    I("strrchr", [] { std::string s = rdstr(arg(0), 1 << 24); size_t p = arg(1) == 0 ? s.size() : s.rfind((char)arg(1)); ret(p == std::string::npos ? 0 : arg(0) + (uint32_t)p); });
    I("atoi",    [] { ret((uint32_t)atoi(rdstr(arg(0)).c_str())); });
    I("strtod",  [] { std::string s = rdstr(arg(0)); char* e; double d = strtod(s.c_str(), &e); if (arg(1)) wr32(arg(1), arg(0) + (uint32_t)(e - s.c_str())); uint64_t u; memcpy(&u, &d, 8); ret64(u); });
    I("strtok",  [] { uint32_t s = arg(0) ? arg(0) : g_strtokState; if (!s) { ret(0); return; }
                      std::string str = rdstr(s, 1 << 24), delim = rdstr(arg(1)); size_t a = str.find_first_not_of(delim);
                      if (a == std::string::npos) { g_strtokState = 0; ret(0); return; }
                      size_t b = str.find_first_of(delim, a);
                      if (b == std::string::npos) g_strtokState = 0; else { uint8_t z = 0; wrbuf(s + (uint32_t)b, &z, 1); g_strtokState = s + (uint32_t)b + 1; }
                      ret(s + (uint32_t)a); });
    I("qsort", [] { uint32_t base = arg(0), n = arg(1), sz = arg(2), cmp = arg(3);
                    std::vector<uint8_t> tmp(sz), a(sz), b(sz);
                    uint32_t ta = gmalloc(sz), tb = gmalloc(sz);                    // insertion sort, comparator runs in the guest
                    for (uint32_t i = 1; i < n; i++) {
                        rdbuf(base + i * sz, tmp.data(), sz); uint32_t j = i;
                        while (j > 0) {
                            wrbuf(ta, tmp.data(), sz); rdbuf(base + (j - 1) * sz, b.data(), sz); wrbuf(tb, b.data(), sz);
                            if ((int32_t)callGuestNested(cmp, {ta, tb}) >= 0) break;
                            wrbuf(base + j * sz, b.data(), sz); j--;
                        }
                        wrbuf(base + j * sz, tmp.data(), sz);
                    }
                    gfree(ta); gfree(tb); });

    // ---- printf family ----
    I("__android_log_print", [] { std::string s = guestFormat(rdstr(arg(2)), VaReader{false, 0, 3}); fprintf(stderr, "[log:%s] %s\n", rdstr(arg(1)).c_str(), s.c_str()); ret(0); });
    I("printf",   [] { std::string s = guestFormat(rdstr(arg(0)), VaReader{false, 0, 1}); fprintf(stderr, "[stdout] %s", s.c_str()); ret((uint32_t)s.size()); });
    I("puts",     [] { fprintf(stderr, "[stdout] %s\n", rdstr(arg(0)).c_str()); ret(1); });
    I("sprintf",  [] { std::string s = guestFormat(rdstr(arg(1)), VaReader{false, 0, 2}); wrbuf(arg(0), s.c_str(), s.size() + 1); ret((uint32_t)s.size()); });
    I("snprintf", [] { std::string s = guestFormat(rdstr(arg(2)), VaReader{false, 0, 3}); uint32_t n = arg(1); if (n) { std::string t = s.substr(0, n - 1); wrbuf(arg(0), t.c_str(), t.size() + 1); } ret((uint32_t)s.size()); });
    I("vsprintf", [] { std::string s = guestFormat(rdstr(arg(1)), VaReader{true, arg(2), 0}); wrbuf(arg(0), s.c_str(), s.size() + 1); ret((uint32_t)s.size()); });
    I("vsnprintf",[] { std::string s = guestFormat(rdstr(arg(2)), VaReader{true, arg(3), 0}); uint32_t n = arg(1); if (n) { std::string t = s.substr(0, n - 1); wrbuf(arg(0), t.c_str(), t.size() + 1); } ret((uint32_t)s.size()); });
    I("fprintf",  [] { std::string s = guestFormat(rdstr(arg(1)), VaReader{false, 0, 2}); FILE* f = hostFile(arg(0)); if (f == stdout || f == stderr) fprintf(stderr, "[stderr] %s", s.c_str()); else if (f) fwrite(s.data(), 1, s.size(), f); ret((uint32_t)s.size()); });

    // ---- soft-float / 64-bit helpers (armeabi has no VFP) ----
    I("__aeabi_d2f",   [] { float f = (float)argd(0); uint32_t u; memcpy(&u, &f, 4); ret(u); });
    I("__aeabi_d2uiz", [] { double d = argd(0); ret(d <= 0 ? 0 : d >= 4294967295.0 ? 0xffffffffu : (uint32_t)d); });
    I("__aeabi_dcmpeq", [] { ret(argd(0) == argd(2)); });  I("__aeabi_dcmpgt", [] { ret(argd(0) > argd(2)); });
    I("__aeabi_dcmple", [] { ret(argd(0) <= argd(2)); });  I("__aeabi_dcmplt", [] { ret(argd(0) < argd(2)); });
    I("__aeabi_fcmpeq", [] { ret(argf(0) == argf(1)); });
    I("__aeabi_lmul",  [] { ret64((uint64_t)((uint64_t)arg(1) << 32 | arg(0)) * ((uint64_t)arg(3) << 32 | arg(2))); });
    I("__aeabi_uldivmod", [] { uint64_t n = (uint64_t)arg(1) << 32 | arg(0), d = (uint64_t)arg(3) << 32 | arg(2); uint64_t q = d ? n / d : 0, r = d ? n % d : 0;
                               setreg(R0, (uint32_t)q); setreg(R1, (uint32_t)(q >> 32)); setreg(R2, (uint32_t)r); setreg(R3, (uint32_t)(r >> 32)); });
    I("pow", [] { double d = pow(argd(0), argd(2)); uint64_t u; memcpy(&u, &d, 8); ret64(u); });

    // ---- setjmp/longjmp (jmp_buf: r4-r11, sp, lr) ----
    I("setjmp",  [] { uint32_t b = arg(0); for (int i = 0; i < 8; i++) wr32(b + 4 * i, reg(UC_ARM_REG_R4 + i)); wr32(b + 32, reg(SP)); wr32(b + 36, reg(LR)); ret(0); });
    I("longjmp", [] { uint32_t b = arg(0), v = arg(1); for (int i = 0; i < 8; i++) setreg(UC_ARM_REG_R4 + i, rd32(b + 4 * i)); setreg(SP, rd32(b + 32)); setreg(LR, rd32(b + 36)); ret(v ? v : 1); });

    // ---- threads: the game never creates threads, so locks are no-ops ----
    for (const char* n : {"pthread_mutex_init", "pthread_mutex_destroy", "pthread_mutex_lock", "pthread_mutex_unlock", "pthread_mutex_trylock", "pthread_mutexattr_init",
                          "pthread_mutexattr_destroy", "pthread_mutexattr_settype", "pthread_cond_signal"}) I(n, [] { ret(0); });
    I("pthread_cond_wait", [] { fprintf(stderr, "WARNING: pthread_cond_wait would block forever (single-threaded host)\n"); ret(0); });
    I("pthread_self", [] { ret(1); });
    I("pthread_key_create", [] { wr32(arg(0), g_nextKey); g_tls[g_nextKey++] = 0; ret(0); });
    I("pthread_getspecific", [] { ret(g_tls[arg(0)]); });
    I("pthread_setspecific", [] { g_tls[arg(0)] = arg(1); ret(0); });

    // ---- time ----
    I("nanosleep", [] { ret(0); }); I("usleep", [] { ret(0); }); I("sleep", [] { ret(0); });

    // ---- files (guest sees /sdcard/... which maps to the host data folder). Portable: stdio + std::filesystem only. ----
    namespace fs = std::filesystem;
    I("fopen",  [] { std::string p = mapPath(rdstr(arg(0))), m = rdstr(arg(1));
                     if (m.find('b') == std::string::npos) { size_t k = m.find_first_of("+"); if (k == std::string::npos) m += "b"; else m.insert(k, "b"); }   // Android/bionic has no text mode; Windows would mangle 0x0A/0x1A in save data
                     FILE* f = fopen(p.c_str(), m.c_str());
                     if (g_trace || g_traceFiles) fprintf(stderr, "[file] fopen(%s,%s) -> %s\n", p.c_str(), m.c_str(), f ? "ok" : "FAIL");
                     if (!f) { ret(0); return; } uint32_t h = gmalloc(84); g_files[h] = f; ret(h); });
    I("fclose", [] { FILE* f = hostFile(arg(0)); if (f) { fclose(f); g_files.erase(arg(0)); gfree(arg(0)); } ret(0); });
    I("fread",  [] { FILE* f = hostFile(arg(3)); uint32_t sz = arg(1), n = arg(2); if (!f || !sz) { ret(0); return; } std::vector<uint8_t> b((size_t)sz * n); size_t r = fread(b.data(), sz, n, f); wrbuf(arg(0), b.data(), r * sz); ret((uint32_t)r); });
    I("fwrite", [] { FILE* f = hostFile(arg(3)); uint32_t sz = arg(1), n = arg(2); if (!f) { ret(0); return; } std::vector<uint8_t> b((size_t)sz * n); rdbuf(arg(0), b.data(), b.size());
                     if (f == stdout || f == stderr) { fwrite(b.data(), 1, b.size(), stderr); ret(n); } else ret((uint32_t)fwrite(b.data(), sz, n, f)); });
    I("fseek",  [] { FILE* f = hostFile(arg(0)); ret(f ? (uint32_t)fseek(f, (int32_t)arg(1), (int)arg(2)) : (uint32_t)-1); });
    I("fflush", [] { uint32_t a = arg(0); if (!a) fflush(nullptr); else if (FILE* f = hostFile(a)) fflush(f); ret(0); });
    I("open",   [] { std::string p = mapPath(rdstr(arg(0))); int fl = (int)arg(1), acc = fl & 3; bool creat = fl & 0x40, trunc = fl & 0x200, app = fl & 0x400;
                     std::error_code ec; bool exists = fs::exists(p, ec); int fd = -1;
                     if (exists || creat) {
                         const char* mode = acc == 0 ? "rb" : acc == 1 ? (app ? "ab" : (trunc || !exists) ? "wb" : "r+b") : ((trunc || !exists) ? "w+b" : app ? "a+b" : "r+b");
                         if (acc != 0 && !exists) fs::create_directories(fs::path(p).parent_path(), ec);
                         FILE* f = fopen(p.c_str(), mode); if (f) { fd = g_nextFd++; g_fds[fd] = f; }
                     }
                     if (g_trace || g_traceFiles) fprintf(stderr, "[file] open(%s,0x%x) -> %d\n", p.c_str(), fl, fd); ret((uint32_t)fd); });
    I("close",  [] { auto it = g_fds.find((int)arg(0)); if (it == g_fds.end()) { ret((uint32_t)-1); return; } fclose(it->second); g_fds.erase(it); ret(0); });
    I("read",   [] { auto it = g_fds.find((int)arg(0)); if (it == g_fds.end()) { ret((uint32_t)-1); return; } std::vector<uint8_t> b(arg(2)); size_t r = fread(b.data(), 1, b.size(), it->second); if (r) wrbuf(arg(1), b.data(), r); ret((uint32_t)r); });
    I("write",  [] { std::vector<uint8_t> b(arg(2)); rdbuf(arg(1), b.data(), b.size()); int fd = (int)arg(0); if (fd == 1 || fd == 2) { fwrite(b.data(), 1, b.size(), stderr); ret(arg(2)); return; }
                     auto it = g_fds.find(fd); if (it == g_fds.end()) { ret((uint32_t)-1); return; } { size_t w = fwrite(b.data(), 1, b.size(), it->second); fflush(it->second); ret((uint32_t)w); } });
    I("lseek",  [] { auto it = g_fds.find((int)arg(0)); if (it == g_fds.end()) { ret((uint32_t)-1); return; } if (fseek(it->second, (long)(int32_t)arg(1), (int)arg(2)) != 0) { ret((uint32_t)-1); return; } ret((uint32_t)ftell(it->second)); });
    I("fcntl",  [] { ret(0); });
    I("mkdir",  [] { std::error_code ec; ret(fs::create_directory(mapPath(rdstr(arg(0))), ec) ? 0 : (uint32_t)-1); });
    I("rename", [] { std::error_code ec; std::string a = mapPath(rdstr(arg(0))), b = mapPath(rdstr(arg(1))); fs::remove(b, ec); fs::rename(a, b, ec); ret(ec ? (uint32_t)-1 : 0); });
    I("unlink", [] { std::error_code ec; ret(fs::remove(mapPath(rdstr(arg(0))), ec) ? 0 : (uint32_t)-1); });
    auto putStat = [](uint32_t out, bool dir, uint64_t size) {          // bionic/ARM "struct stat" (kernel stat64 layout)
        std::vector<uint8_t> z(104, 0); wrbuf(out, z.data(), z.size());
        wr32(out + 16, dir ? 040755u : 0100644u); wr32(out + 20, 1);
        wr32(out + 48, (uint32_t)size); wr32(out + 52, (uint32_t)(size >> 32));
    };
    I("stat",  [putStat] { std::string p = mapPath(rdstr(arg(0))); std::error_code ec; auto st = fs::status(p, ec); int r = -1;
                           if (!ec && fs::is_directory(st)) { putStat(arg(1), true, 0); r = 0; } else if (!ec && fs::is_regular_file(st)) { putStat(arg(1), false, fs::file_size(p, ec)); r = 0; }
                           if (g_trace || g_traceFiles) fprintf(stderr, "[file] stat(%s) -> %d\n", p.c_str(), r); ret((uint32_t)r); });
    I("lstat", g_impl["stat"]);
    I("fstat", [putStat] { auto it = g_fds.find((int)arg(0)); if (it == g_fds.end()) { ret((uint32_t)-1); return; }
                           long cur = ftell(it->second); fseek(it->second, 0, SEEK_END); long sz = ftell(it->second); fseek(it->second, cur, SEEK_SET);
                           putStat(arg(1), false, (uint64_t)sz); ret(0); });
    I("mmap",  [] { uint32_t len = arg(1), off = arg(5); int fd = (int)arg(4); uint32_t p = gmalloc(len);
                    std::vector<uint8_t> z(len, 0); wrbuf(p, z.data(), len);
                    auto it = g_fds.find(fd);
                    if (it != g_fds.end()) { std::vector<uint8_t> b(len); long cur = ftell(it->second); fseek(it->second, (long)off, SEEK_SET); size_t r = fread(b.data(), 1, len, it->second); fseek(it->second, cur, SEEK_SET); if (r) wrbuf(p, b.data(), r); }
                    if (g_trace || g_traceFiles) fprintf(stderr, "[file] mmap(len=%u fd=%d off=%u) -> 0x%x\n", len, fd, off, p); ret(p); });
    I("munmap", [] { gfree(arg(0)); ret(0); });

    // ---- networking: the original servers are gone, so everything fails cleanly ----
    for (const char* n : {"socket", "connect", "send", "recvfrom", "select"}) I(n, [] { ret((uint32_t)-1); });
    I("getaddrinfo", [] { ret(2 /* EAI_AGAIN-ish */); });
    I("freeaddrinfo", [] { ret(0); });
    I("inet_addr", [] { unsigned a, b, c, d; ret(sscanf(rdstr(arg(0)).c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 ? (a | b << 8 | c << 16 | d << 24) : 0xffffffffu); });
    I("inet_ntoa", [] { uint32_t a = arg(0); char b[32]; snprintf(b, sizeof b, "%u.%u.%u.%u", a & 255, a >> 8 & 255, a >> 16 & 255, a >> 24); ret(guestStr(b)); });
}
