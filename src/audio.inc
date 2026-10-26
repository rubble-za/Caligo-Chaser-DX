// Audio engine: WAV + Ogg Vorbis decoding and a small mixer, plus a re-implementation of the original
// SoundManager (Android SoundPool for effects, MediaPlayer-style clips for music) on top of it.
extern "C" int stb_vorbis_decode_memory(const unsigned char* mem, int len, int* channels, int* sample_rate, short** output);

namespace audio {
struct Pcm { std::vector<int16_t> s; int ch = 1, rate = 22050; size_t frames() const { return ch ? s.size() / ch : 0; } };
struct Voice { int id = 0; std::shared_ptr<Pcm> pcm; double pos = 0; bool loop = false, playing = false, pool = false, autoRemove = false; float vol = 1.f; uint64_t seq = 0; };

static std::mutex mtx;
static std::vector<Voice> voices;
static int devRate = 44100, nextVoice = 1;
static uint64_t seqCounter = 0;
static const int MAX_POOL_STREAMS = 6;                         // SoundPool(6, ...) in the original

static bool decodeWav(const std::vector<uint8_t>& d, Pcm& out) {
    if (d.size() < 12 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return false;
    size_t p = 12; int fmt = 0, ch = 0, rate = 0, bits = 0; const uint8_t* data = nullptr; size_t dlen = 0;
    while (p + 8 <= d.size()) {
        uint32_t len = d[p + 4] | d[p + 5] << 8 | d[p + 6] << 16 | (uint32_t)d[p + 7] << 24; const uint8_t* c = &d[p + 8];
        if (!memcmp(&d[p], "fmt ", 4) && len >= 16) { fmt = c[0] | c[1] << 8; ch = c[2] | c[3] << 8; rate = c[4] | c[5] << 8 | c[6] << 16 | c[7] << 24; bits = c[14] | c[15] << 8; }
        else if (!memcmp(&d[p], "data", 4)) { data = c; dlen = std::min<size_t>(len, d.size() - (p + 8)); break; }
        p += 8 + len + (len & 1);
    }
    if (!data || fmt != 1 || ch < 1 || ch > 2 || (bits != 16 && bits != 8)) return false;
    out.ch = ch; out.rate = rate; size_t n = dlen / (bits / 8); out.s.resize(n);
    for (size_t i = 0; i < n; i++) out.s[i] = bits == 16 ? (int16_t)(data[2 * i] | data[2 * i + 1] << 8) : (int16_t)((data[i] - 128) << 8);
    return true;
}
static bool decodeOgg(const std::vector<uint8_t>& d, Pcm& out) {
    int ch = 0, rate = 0; short* pcm = nullptr;
    int n = stb_vorbis_decode_memory(d.data(), (int)d.size(), &ch, &rate, &pcm);
    if (n <= 0 || !pcm) return false;
    out.ch = ch; out.rate = rate; out.s.assign(pcm, pcm + (size_t)n * ch); free(pcm);
    return true;
}

static Voice* find(int id) { for (auto& v : voices) if (v.id == id) return &v; return nullptr; }
// MediaPlayer-like clip (music): one persistent voice, controlled by play/loop/pause/resume/stop.
static int clipCreate(std::shared_ptr<Pcm> p) { std::lock_guard<std::mutex> l(mtx); Voice v; v.id = nextVoice++; v.pcm = p; voices.push_back(v); return v.id; }
// SoundPool-like stream (effects): a fresh voice per play(), removed when it finishes.
static int poolPlay(std::shared_ptr<Pcm> p, bool loop, float vol) {
    std::lock_guard<std::mutex> l(mtx);
    int pool = 0; for (auto& v : voices) if (v.pool) pool++;
    while (pool >= MAX_POOL_STREAMS) {                                           // steal the oldest effect stream
        auto it = std::min_element(voices.begin(), voices.end(), [](const Voice& a, const Voice& b) { return (a.pool ? a.seq : ~0ull) < (b.pool ? b.seq : ~0ull); });
        voices.erase(it); pool--;
    }
    Voice v; v.id = nextVoice++; v.pcm = p; v.loop = loop; v.vol = vol; v.playing = true; v.pool = true; v.autoRemove = !loop; v.seq = ++seqCounter; voices.push_back(v);
    return v.id;
}
static void start(int id, bool loop, float vol, bool setVol, bool restart) { std::lock_guard<std::mutex> l(mtx); if (Voice* v = find(id)) { if (restart) v->pos = 0; v->loop = loop; if (setVol) v->vol = vol; v->playing = true; } }
static void pause(int id)  { std::lock_guard<std::mutex> l(mtx); if (Voice* v = find(id)) v->playing = false; }
static void resume(int id) { std::lock_guard<std::mutex> l(mtx); if (Voice* v = find(id)) v->playing = true; }
static void stopVoice(int id, bool rewind) { std::lock_guard<std::mutex> l(mtx); if (Voice* v = find(id)) { v->playing = false; v->loop = false; if (v->pool) v->autoRemove = true; if (rewind) v->pos = 0; } }
static void setVol(int id, float vol) { std::lock_guard<std::mutex> l(mtx); if (Voice* v = find(id)) v->vol = vol; }
static bool isPlaying(int id) { std::lock_guard<std::mutex> l(mtx); Voice* v = find(id); return v && v->playing; }
static void release(int id) { std::lock_guard<std::mutex> l(mtx); voices.erase(std::remove_if(voices.begin(), voices.end(), [id](const Voice& v) { return v.id == id; }), voices.end()); }

static void mix(int16_t* out, int frames) {
    std::vector<int32_t> acc((size_t)frames * 2, 0);
    std::lock_guard<std::mutex> l(mtx);
    for (auto& v : voices) {
        if (!v.playing || !v.pcm || v.pcm->frames() == 0) continue;
        const Pcm& p = *v.pcm; double step = (double)p.rate / devRate; size_t nf = p.frames(); int ivol = (int)(v.vol * 256.f);
        for (int i = 0; i < frames && v.playing; i++) {
            size_t i0 = (size_t)v.pos, i1 = i0 + 1 < nf ? i0 + 1 : (v.loop ? 0 : i0); float f = (float)(v.pos - (double)i0);
            int l0 = p.s[i0 * p.ch], r0 = p.s[i0 * p.ch + (p.ch - 1)], l1 = p.s[i1 * p.ch], r1 = p.s[i1 * p.ch + (p.ch - 1)];
            acc[(size_t)i * 2]     += (int)((l0 + (l1 - l0) * f) * ivol) >> 8;
            acc[(size_t)i * 2 + 1] += (int)((r0 + (r1 - r0) * f) * ivol) >> 8;
            v.pos += step;
            if (v.pos >= (double)nf) { if (v.loop) v.pos = fmod(v.pos, (double)nf); else { v.playing = false; v.pos = 0; } }
        }
    }
    voices.erase(std::remove_if(voices.begin(), voices.end(), [](const Voice& v) { return v.pool && v.autoRemove && !v.playing; }), voices.end());
    for (size_t i = 0; i < acc.size(); i++) out[i] = (int16_t)std::max(-32768, std::min(32767, acc[i]));
}
}  // namespace audio

// ---- Re-implementation of com.com2us.caligo.SoundManager (same ids, same semantics) ----
namespace snd {
enum { STREAM = 1, NONSTREAM = 2 };
struct Sound { int type; std::shared_ptr<audio::Pcm> pcm; int clipVoice = 0; int volume = 100; int streamId = -1; bool hasStream = false; };
static std::map<int, Sound> sounds;
static int g_curBGM = -1, g_curEFT = -1, g_volBGM = 100, g_volEFT = 100, clipIndex = 1000, poolIndex = 1;
static std::map<std::string, std::shared_ptr<audio::Pcm>> cache;

static std::shared_ptr<audio::Pcm> loadPcm(const std::string& key, bool stream) {
    auto it = cache.find(key); if (it != cache.end()) return it->second;
    std::vector<uint8_t> d; auto pcm = std::make_shared<audio::Pcm>(); bool ok = false;
    if (stream) { ok = apkRead("res/raw/" + key, d) && audio::decodeOgg(d, *pcm); }
    else {
        std::ifstream f(g_dataRoot + "/Com2uS/com.com2us.caligo/SE/" + key, std::ios::binary);
        if (f) { d.assign(std::istreambuf_iterator<char>(f), {}); ok = audio::decodeWav(d, *pcm); }
    }
    if (!ok) { fprintf(stderr, "[audio] could not load '%s' (%s)\n", key.c_str(), stream ? "stream" : "effect"); pcm.reset(); }
    cache[key] = pcm; return pcm;
}
static void findCur(bool streaming) {
    (streaming ? g_curBGM : g_curEFT) = -1;
    for (auto& kv : sounds) if (kv.second.hasStream && ((kv.second.type == STREAM) == streaming)) { (streaming ? g_curBGM : g_curEFT) = kv.first; return; }
}
static int create(const std::string& name, bool streaming) {
    Sound s; s.type = streaming ? STREAM : NONSTREAM; s.volume = streaming ? g_volBGM : g_volEFT; int id;
    s.pcm = loadPcm(name, streaming);
    if (streaming) { id = clipIndex++; if (s.pcm) s.clipVoice = audio::clipCreate(s.pcm); }
    else { id = s.pcm ? poolIndex++ : 0; if (!s.pcm) return 0; }                    // SoundPool.load returns 0 on failure
    sounds[id] = s; return id;
}
static int destroy(int id) {
    auto it = sounds.find(id); if (it == sounds.end()) return -1;
    if (it->second.type == STREAM) audio::release(it->second.clipVoice);
    sounds.erase(it); return 0;
}
static int play(int id, bool repeat) {
    auto it = sounds.find(id); if (it == sounds.end()) return -1; Sound& s = it->second; float vol = s.volume / 100.f;
    if (s.type == STREAM) {
        if (s.clipVoice) { if (repeat) audio::start(s.clipVoice, true, vol, true, false); else audio::start(s.clipVoice, false, vol, true, audio::isPlaying(s.clipVoice)); }
        s.streamId = id; g_curBGM = id;
    } else { s.streamId = audio::poolPlay(s.pcm, repeat, vol); g_curEFT = id; }
    s.hasStream = true; return 0;
}
static int pause(int id) { auto it = sounds.find(id); if (it == sounds.end()) return -1; audio::pause(it->second.type == STREAM ? it->second.clipVoice : it->second.streamId); return 0; }
static int resume(int id) { auto it = sounds.find(id); if (it == sounds.end()) return -1; audio::resume(it->second.type == STREAM ? it->second.clipVoice : it->second.streamId); return 0; }
static int stop(int id) {
    auto it = sounds.find(id); if (it == sounds.end() || !it->second.hasStream) return -1; Sound& s = it->second;
    audio::stopVoice(s.type == STREAM ? s.clipVoice : s.streamId, false); s.hasStream = false; return 0;
}
static void stopCurrent(bool streaming) { int id = streaming ? g_curBGM : g_curEFT; if (id >= 0) { stop(id); findCur(streaming); } }
static void stopAll(bool streaming) { int guard = 0; while ((streaming ? g_curBGM : g_curEFT) != -1 && guard++ < 4096) stopCurrent(streaming); }
static int setVolume(int level, bool streaming) {
    for (auto& kv : sounds) if ((kv.second.type == STREAM) == streaming) { kv.second.volume = level; if (kv.second.hasStream) audio::setVol(streaming ? kv.second.clipVoice : kv.second.streamId, level / 100.f); }
    (streaming ? g_volBGM : g_volEFT) = level; return 0;
}
static int soundSetVolume(int id, int level) {
    auto it = sounds.find(id); if (it == sounds.end()) return -1; Sound& s = it->second; s.volume = level;
    if (s.hasStream) audio::setVol(s.type == STREAM ? s.clipVoice : s.streamId, level / 100.f); return 0;
}
}  // namespace snd
