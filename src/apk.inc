// SPDX-License-Identifier: MIT
// Reads files straight out of the user's APK (libgame.so, music) so nothing from the game has to be extracted or redistributed.
#include "miniz.h"

static std::vector<uint8_t> g_apkBytes;
static mz_zip_archive g_apkZip;
static bool g_apkOpen = false;

static bool apkOpen(const std::string& path, std::string& err) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open APK: " + path; return false; }
    g_apkBytes.assign(std::istreambuf_iterator<char>(f), {});
    memset(&g_apkZip, 0, sizeof g_apkZip);
    if (!mz_zip_reader_init_mem(&g_apkZip, g_apkBytes.data(), g_apkBytes.size(), 0)) { err = "not a valid APK/zip: " + path; return false; }
    g_apkOpen = true;
    return true;
}
static bool apkRead(const std::string& name, std::vector<uint8_t>& out) {
    if (!g_apkOpen) return false;
    size_t sz = 0; void* p = mz_zip_reader_extract_file_to_heap(&g_apkZip, name.c_str(), &sz, 0);
    if (!p) return false;
    out.assign((uint8_t*)p, (uint8_t*)p + sz); mz_free(p);
    return true;
}
