// in_cuesheet - CUE sheet support for WACUP / Winamp.
//
// * Claims the .CUE extension. When a .cue entry shows up in the playlist it
//   is replaced by one playlist entry per track. Track entries use the
//   pseudo file name  "<full path of .cue>,<n>.cuetrack"  (n = 1-based track
//   ordinal; the older form without ".cuetrack" is still understood).
// * Playing a track entry wraps the input plug-in that normally plays the
//   referenced audio file (e.g. WACUP's MP3 decoder): it is started, seeked to
//   the track's INDEX 01 and stopped/advanced at the next track's start.
// * When one track rolls into the next track of the same file, the decoder is
//   kept running so the transition is gapless.

#include "winamp_sdk.h"
#include "cue_parser.h"
#include <shlwapi.h>
#include <tlhelp32.h>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <cstdio>
#include <cstdarg>
#include <algorithm>
#include <climits>

#define PLUGIN_NAME "CUE Sheet Support v1.0"

extern In_Module mod;

// ------------------------------------------------------------------ logging
static wchar_t g_logPath[MAX_PATH];
static bool g_log = false;

static void Log(const char *fmt, ...) {
    if (!g_log) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf_s(buf, sizeof(buf) - 2, _TRUNCATE, fmt, ap);
    va_end(ap);
    if (n < 0) n = (int)strlen(buf);
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[2200];
    int ln = sprintf_s(line, "%02d:%02d:%02d.%03d [%5lu] %s\r\n", st.wHour, st.wMinute, st.wSecond,
                       st.wMilliseconds, GetCurrentThreadId(), buf);
    HANDLE h = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);  // deleting the file stops logging
    if (h != INVALID_HANDLE_VALUE) {
        DWORD w;
        WriteFile(h, line, ln, &w, nullptr);
        CloseHandle(h);
    }
}

static std::string U8(const std::wstring &w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

// ------------------------------------------------------------- name helpers
static bool EndsWithCue(const wchar_t *s, size_t len) {
    return len >= 4 && _wcsnicmp(s + len - 4, L".cue", 4) == 0;
}

static bool IsCueFile(const wchar_t *fn) {
    return fn && EndsWithCue(fn, wcslen(fn));
}

// Track entries are named "<path>.cue,<n>.cuetrack". The own extension
// matters: tools that look for "<file name with extension swapped>.cue"
// (e.g. the waveform seeker's CUE markers) must not pick up the whole-file
// sheet for a single track. The older "<path>.cue,<n>" form is still accepted.
static const wchar_t kTrackExt[] = L".cuetrack";

// Parses a track entry name -> cue path + 0-based track index.
static bool ParseTrackName(const wchar_t *fn, std::wstring *cuePath, int *track) {
    if (!fn) return false;
    size_t len = wcslen(fn);
    const size_t extLen = _countof(kTrackExt) - 1;
    if (len > extLen && _wcsicmp(fn + len - extLen, kTrackExt) == 0) len -= extLen;
    const wchar_t *comma = nullptr;
    for (size_t i = len; i > 0; i--)
        if (fn[i - 1] == L',') { comma = fn + i - 1; break; }
    if (!comma || comma + 1 == fn + len) return false;
    for (const wchar_t *p = comma + 1; p < fn + len; p++)
        if (*p < L'0' || *p > L'9') return false;
    if (!EndsWithCue(fn, comma - fn)) return false;
    int n = _wtoi(comma + 1);
    if (n < 1) return false;
    if (cuePath) cuePath->assign(fn, comma);
    if (track) *track = n - 1;
    return true;
}

static std::wstring MakeTrackName(const std::wstring &cuePath, int track) {
    wchar_t num[16];
    swprintf_s(num, L",%02d", track + 1);
    return cuePath + num + kTrackExt;
}

static std::wstring TrackDisplayTitle(const CueSheet &cue, int i) {
    const CueTrack &t = cue.tracks[i];
    std::wstring perf = cue.TrackPerformer(i);
    std::wstring title = t.title;
    if (title.empty()) {
        wchar_t b[32];
        swprintf_s(b, L"Track %02d", t.number);
        title = b;
    }
    return perf.empty() ? title : perf + L" - " + title;
}

// ------------------------------------------------- inner plug-in management
static DWORD g_mainThread = 0;
static HWND g_wnd = nullptr;
static std::mutex g_mtx;  // guards the caches below
static std::map<std::wstring, In_Module *> g_modByExt;
static std::map<std::wstring, int> g_lenByFile;

static bool IsUnicode(In_Module *m) { return (m->version & IN_UNICODE) == IN_UNICODE; }

static std::wstring LowerExt(const std::wstring &path) {
    const wchar_t *e = PathFindExtensionW(path.c_str());
    std::wstring s = e;
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static bool Readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || !VirtualQuery(p, &mbi, sizeof(mbi))) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    return (const char *)p + n <= (const char *)mbi.BaseAddress + mbi.RegionSize;
}

// Guards against IPC results that aren't In_Module pointers.
static bool LooksLikeInModule(In_Module *m) {
    if (!m || m == &mod || !Readable(m, sizeof(In_Module))) return false;
    // Low bits are IN_VER (0x100); WACUP uses 0x101/0x102 plus flag bits on top.
    return (m->version & 0xF00) == IN_VER && m->Play && m->Stop && m->GetOutputTime && m->SetOutputTime;
}

// True if an input plug-in's FileExtensions list ("MP3;MP2\0desc\0...\0\0")
// contains ext (without the dot, case-insensitive).
static bool ClaimsExtension(In_Module *m, const std::wstring &ext) {
    const char *p = m->FileExtensions;
    if (!p || !Readable(p, 1) || ext.empty()) return false;
    char e[32];
    WideCharToMultiByte(CP_ACP, 0, ext.c_str(), -1, e, sizeof(e), nullptr, nullptr);
    for (int guard = 0; *p && guard < 512; guard++) {
        // p = "EXT1;EXT2", then description, then next pair
        const char *s = p;
        while (*s) {
            const char *semi = strchr(s, ';');
            size_t n = semi ? (size_t)(semi - s) : strlen(s);
            if (n == strlen(e) && _strnicmp(s, e, n) == 0) return true;
            if (!semi) break;
            s = semi + 1;
        }
        p += strlen(p) + 1;      // skip extensions
        if (!*p) break;
        p += strlen(p) + 1;      // skip description
    }
    return false;
}

// Finds the input plug-in for an audio file by asking every loaded in_*.dll
// for its In_Module (IPC_CANPLAY doesn't return the module in WACUP).
static In_Module *FindModuleByExtension(const std::wstring &audio) {
    std::wstring ext = LowerExt(audio);
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    In_Module *found = nullptr;
    MODULEENTRY32W me = {sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok && !found; ok = Module32NextW(snap, &me)) {
        if (_wcsnicmp(me.szModule, L"in_", 3) != 0 || me.hModule == mod.hDllInstance) continue;
        typedef In_Module *(*GetIn)();
        GetIn get = (GetIn)GetProcAddress(me.hModule, "winampGetInModule2");
        In_Module *m = get ? get() : nullptr;
        if (LooksLikeInModule(m) && ClaimsExtension(m, ext)) {
            Log("module %s claims .%s", U8(me.szModule).c_str(), U8(ext).c_str());
            found = m;
        }
    }
    CloseHandle(snap);
    return found;
}

// x64 WACUP builds its MP3 decoder into winamp.dll without exporting the
// In_Module. Look for it in the module's writable data: a struct that passes
// the sanity checks, whose callbacks point into the module's code and which
// claims the extension.
static In_Module *FindModuleInImage(HMODULE h, const std::wstring &audio) {
    if (!h) return nullptr;
    std::wstring ext = LowerExt(audio);
    if (!ext.empty() && ext[0] == L'.') ext.erase(0, 1);
    auto base = (const BYTE *)h;
    auto dos = (const IMAGE_DOS_HEADER *)base;
    auto nt = (const IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    const BYTE *imgEnd = base + nt->OptionalHeader.SizeOfImage;
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        const BYTE *p = base + sec->VirtualAddress;
        const BYTE *end = p + sec->Misc.VirtualSize - sizeof(In_Module);
        if (!Readable(p, sec->Misc.VirtualSize)) continue;
        for (; p <= end; p += sizeof(void *)) {
            auto m = (In_Module *)p;
            if ((m->version & 0xF00) != IN_VER) continue;
            auto inImg = [&](const void *f) { return (const BYTE *)f >= base && (const BYTE *)f < imgEnd; };
            if (!inImg((const void *)m->Play) || !inImg((const void *)m->Stop) ||
                !inImg((const void *)m->GetOutputTime) || !inImg((const void *)m->SetOutputTime))
                continue;
            if (LooksLikeInModule(m) && ClaimsExtension(m, ext)) {
                Log("built-in module at %p claims .%s", m, U8(ext).c_str());
                return m;
            }
        }
    }
    return nullptr;
}

static In_Module *FindInnerModule(const std::wstring &audio) {
    std::wstring ext = LowerExt(audio);
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_modByExt.find(ext);
        if (it != g_modByExt.end()) return it->second;
    }
    DWORD_PTR r = 0;
    In_Module *m = nullptr;
    // IPC_CANPLAY takes a wide string on unicode builds; fall back to ANSI.
    if (SendMessageTimeoutW(mod.hMainWindow, WM_WA_IPC, (WPARAM)audio.c_str(), IPC_CANPLAY,
                            SMTO_NORMAL, 3000, &r))
        m = (In_Module *)r;
    Log("IPC_CANPLAY(W) raw=%p", (void *)r);
    if (!LooksLikeInModule(m)) {
        m = nullptr;
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, audio.c_str(), -1, a, sizeof(a), nullptr, nullptr);
        r = 0;
        if (SendMessageTimeoutA(mod.hMainWindow, WM_WA_IPC, (WPARAM)a, IPC_CANPLAY, SMTO_NORMAL, 3000, &r))
            m = (In_Module *)r;
        Log("IPC_CANPLAY(A) raw=%p", (void *)r);
        if (!LooksLikeInModule(m)) m = nullptr;
    }
    if (!m) m = FindModuleByExtension(audio);
    if (!m) m = FindModuleInImage(GetModuleHandleW(L"winamp.dll"), audio);
    Log("IPC_CANPLAY(%s) -> %p ver=%08x desc=%s", U8(audio).c_str(), m, m ? m->version : 0,
        m && m->description ? m->description : "");
    if (m) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_modByExt[ext] = m;
    }
    return m;
}

static int InnerPlay(In_Module *m, const std::wstring &f) {
    if (IsUnicode(m)) return m->Play(f.c_str());
    char a[MAX_PATH * 2];
    WideCharToMultiByte(CP_ACP, 0, f.c_str(), -1, a, sizeof(a), nullptr, nullptr);
    return m->Play(a);
}

// Length of a whole audio file in ms (cached), -1 if unknown.
static int AudioLength(const std::wstring &audio) {
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        auto it = g_lenByFile.find(audio);
        if (it != g_lenByFile.end()) return it->second;
    }
    if (!FileExists(audio)) return -1;
    In_Module *m = FindInnerModule(audio);
    if (!m) return -1;
    int len = -1;
    if (IsUnicode(m)) {
        static wchar_t title[GETFILEINFO_TITLE_LENGTH];
        thread_local wchar_t t[GETFILEINFO_TITLE_LENGTH];
        t[0] = 0;
        m->GetFileInfo(audio.c_str(), t, &len);
        (void)title;
    } else {
        thread_local char t[GETFILEINFO_TITLE_LENGTH];
        char a[MAX_PATH * 2];
        WideCharToMultiByte(CP_ACP, 0, audio.c_str(), -1, a, sizeof(a), nullptr, nullptr);
        t[0] = 0;
        m->GetFileInfo(a, t, &len);
    }
    Log("AudioLength(%s) = %d", U8(audio).c_str(), len);
    if (len > 0) {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_lenByFile[audio] = len;
    }
    return len;
}

static int TrackLength(const CueSheet &cue, int i) {
    const CueTrack &t = cue.tracks[i];
    if (t.endMs > 0) return t.endMs - t.startMs;
    int total = AudioLength(cue.files[t.fileIndex]);
    return total > t.startMs ? total - t.startMs : -1;
}

// ----------------------------------------------------------- playback state
enum { TIMER_MONITOR = 1, TIMER_DEFERRED_STOP = 2, TIMER_SCAN = 3, TIMER_INFO = 4 };
enum { MSG_EXPAND = WM_APP + 1 };

struct Playback {
    In_Module *inner = nullptr;
    std::shared_ptr<const CueSheet> cue;
    int track = -1;
    std::wstring audio;
    int startMs = 0;
    int endMs = -1;
    bool eofPosted = false;     // we told WACUP the track ended
    bool pendingStop = false;   // Stop() deferred to allow gapless continue
    int seekRetries = 0;
    DWORD startTick = 0;
    void (*origSetInfo)(int, int, int, int) = nullptr;
};
static Playback g_pb;
static int g_lastInfo[4] = {-1, -1, -1, -1};
static int g_eqOn = 0, g_eqPreamp = 0;
static char g_eqData[10];
static bool g_eqSet = false;

static std::wstring g_playingCue;  // a raw .cue that WACUP asked us to "play"

static void HookSetInfo(int bitrate, int srate, int stereo, int synched) {
    // -1 means "unchanged" (decoders send e.g. SetInfo(-1,-1,-1,1) as a sync tick)
    int v[4] = {bitrate, srate, stereo, synched};
    for (int i = 0; i < 4; i++)
        if (v[i] != -1) g_lastInfo[i] = v[i];
    if (g_pb.origSetInfo) g_pb.origSetInfo(bitrate, srate, stereo, synched);
}

// --- visualisation timestamps
// The wrapped decoder stamps the PCM it hands to the visualisation with its
// position in the whole audio file, while WACUP asks *us* for the playback
// position, which is relative to the current track. Translate the stamps to
// track time. The decoder runs ahead of what is heard, so data belonging to
// the next track arrives before the switch: subtract the start of whichever
// track contains the stamp, not just the current track's start.
static SRWLOCK g_visLock = SRWLOCK_INIT;
static std::vector<int> g_visStarts;   // sorted track starts within the playing file
static void (*g_origSAAddPCMData)(void *, int, int, int);
static int (*g_origSAAdd)(void *, int, int);
static void (*g_origVSAAddPCMData)(void *, int, int, int);
static int (*g_origVSAAdd)(void *, int);

static int TrackTime(int fileTime) {
    AcquireSRWLockShared(&g_visLock);
    int base = g_visStarts.empty() ? 0 : g_visStarts[0];
    for (int s : g_visStarts) {
        if (s <= fileTime) base = s;
        else break;
    }
    ReleaseSRWLockShared(&g_visLock);
    return fileTime - base;
}

static void HookSAAddPCMData(void *pcm, int nch, int bps, int ts) {
    g_origSAAddPCMData(pcm, nch, bps, TrackTime(ts));
}
static int HookSAAdd(void *data, int ts, int csa) { return g_origSAAdd(data, TrackTime(ts), csa); }
static void HookVSAAddPCMData(void *pcm, int nch, int bps, int ts) {
    g_origVSAAddPCMData(pcm, nch, bps, TrackTime(ts));
}
static int HookVSAAdd(void *data, int ts) { return g_origVSAAdd(data, TrackTime(ts)); }

static void SetVisTracks(const CueSheet &cue, int fileIndex) {
    std::vector<int> starts;
    for (auto &t : cue.tracks)
        if (t.fileIndex == fileIndex) starts.push_back(t.startMs);
    std::sort(starts.begin(), starts.end());
    AcquireSRWLockExclusive(&g_visLock);
    g_visStarts.swap(starts);
    ReleaseSRWLockExclusive(&g_visLock);
}

// Routes the wrapped decoder's callbacks through our hooks. The original
// pointers are kept (never cleared) so a late call from the decoder thread
// still lands somewhere valid.
static void InstallHooks(In_Module *m) {
    g_pb.origSetInfo = m->SetInfo;
    m->SetInfo = HookSetInfo;
    if (m->SAAddPCMData) { g_origSAAddPCMData = m->SAAddPCMData; m->SAAddPCMData = HookSAAddPCMData; }
    if (m->SAAdd) { g_origSAAdd = m->SAAdd; m->SAAdd = HookSAAdd; }
    if (m->VSAAddPCMData) { g_origVSAAddPCMData = m->VSAAddPCMData; m->VSAAddPCMData = HookVSAAddPCMData; }
    if (m->VSAAdd) { g_origVSAAdd = m->VSAAdd; m->VSAAdd = HookVSAAdd; }
}

static void RemoveHooks(In_Module *m) {
    if (g_pb.origSetInfo) m->SetInfo = g_pb.origSetInfo;
    if (m->SAAddPCMData == HookSAAddPCMData) m->SAAddPCMData = g_origSAAddPCMData;
    if (m->SAAdd == HookSAAdd) m->SAAdd = g_origSAAdd;
    if (m->VSAAddPCMData == HookVSAAddPCMData) m->VSAAddPCMData = g_origVSAAddPCMData;
    if (m->VSAAdd == HookVSAAdd) m->VSAAdd = g_origVSAAdd;
}

static void RealStop() {
    KillTimer(g_wnd, TIMER_DEFERRED_STOP);
    KillTimer(g_wnd, TIMER_MONITOR);
    g_pb.pendingStop = false;
    if (g_pb.inner) {
        Log("RealStop: stopping inner");
        g_pb.inner->Stop();
        RemoveHooks(g_pb.inner);
    }
    g_pb = Playback();
}

// ----------------------------------------------------- playlist expansion
static HWND PlaylistWnd() {
    return (HWND)SendMessageW(mod.hMainWindow, WM_WA_IPC, IPC_GETWND_PE, IPC_GETWND);
}

static std::wstring PlaylistFile(int i) {
    const wchar_t *p = (const wchar_t *)SendMessageW(mod.hMainWindow, WM_WA_IPC, i, IPC_GETPLAYLISTFILEW);
    return p ? std::wstring(p) : std::wstring();
}

static bool InsertPlaylistFile(HWND pe, const std::wstring &f, int index) {
    fileinfoW fi = {};
    wcsncpy_s(fi.file, f.c_str(), _TRUNCATE);
    fi.index = index;
    COPYDATASTRUCT cds = {IPC_PE_INSERTFILENAMEW, sizeof(fi), &fi};
    SendMessageW(pe, WM_COPYDATA, (WPARAM)mod.hMainWindow, (LPARAM)&cds);
    return true;
}

static int g_lastScanLen = -1;
static bool g_expanding = false;

// Replaces every raw .cue entry in the playlist by its tracks.
static void ExpandPlaylist() {
    if (g_expanding) return;
    g_expanding = true;
    HWND pe = PlaylistWnd();
    int len = (int)SendMessageW(mod.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
    bool startPlay = false;
    int playIndex = -1;

    for (int i = len - 1; i >= 0 && pe; i--) {
        std::wstring f = PlaylistFile(i);
        if (!IsCueFile(f.c_str())) continue;
        auto cue = LoadCueSheet(f);
        if (!cue) { Log("Expand: can't parse %s", U8(f).c_str()); continue; }
        Log("Expand: %s at %d -> %d tracks", U8(f).c_str(), i, (int)cue->tracks.size());

        int cur = (int)SendMessageW(mod.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTPOS);
        bool wasPlayingThis = !g_playingCue.empty() && _wcsicmp(g_playingCue.c_str(), f.c_str()) == 0 && cur == i;

        SendMessageW(pe, WM_WA_IPC, IPC_PE_DELETEINDEX, i);
        int n = (int)cue->tracks.size();
        for (int t = 0; t < n; t++) InsertPlaylistFile(pe, MakeTrackName(f, t), i + t);

        // Drop a copy of the referenced audio file sitting right next to the
        // sheet (happens when a whole folder is added).
        auto isOurAudio = [&](const std::wstring &p) {
            for (auto &a : cue->files) if (_wcsicmp(a.c_str(), p.c_str()) == 0) return true;
            return false;
        };
        int first = i;
        while (first > 0 && isOurAudio(PlaylistFile(first - 1))) {
            SendMessageW(pe, WM_WA_IPC, IPC_PE_DELETEINDEX, first - 1);
            first--;
        }
        int after = first + n, removedAfter = 0;
        int total = (int)SendMessageW(mod.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
        while (after < total && isOurAudio(PlaylistFile(after))) {
            SendMessageW(pe, WM_WA_IPC, IPC_PE_DELETEINDEX, after);
            total--;
            removedAfter++;
        }
        if (playIndex > i) playIndex += (n - 1) - (i - first) - removedAfter;
        if (wasPlayingThis) { startPlay = true; playIndex = first; }
    }
    g_lastScanLen = (int)SendMessageW(mod.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
    SendMessageW(mod.hMainWindow, WM_WA_IPC, 0, IPC_REFRESHPLCACHE);
    g_expanding = false;

    if (startPlay) {
        g_playingCue.clear();
        Log("Expand: start playing index %d", playIndex);
        SendMessageW(mod.hMainWindow, WM_COMMAND, WINAMP_BUTTON4_STOP, 0);
        SendMessageW(mod.hMainWindow, WM_WA_IPC, playIndex, IPC_SETPLAYLISTPOS);
        SendMessageW(mod.hMainWindow, WM_COMMAND, WINAMP_BUTTON2_PLAY, 0);
    }
}

static void ScanIfChanged() {
    int len = (int)SendMessageW(mod.hMainWindow, WM_WA_IPC, 0, IPC_GETLISTLENGTH);
    if (len == g_lastScanLen) return;
    g_lastScanLen = len;
    for (int i = 0; i < len; i++) {
        std::wstring f = PlaylistFile(i);
        if (IsCueFile(f.c_str())) { ExpandPlaylist(); return; }
    }
}

static void RequestExpand() {
    if (g_wnd) PostMessageW(g_wnd, MSG_EXPAND, 0, 0);
}

// ------------------------------------------------------- monitor / window
static void Monitor() {
    if (!g_pb.inner || g_pb.pendingStop) return;
    int t = g_pb.inner->GetOutputTime();
    // Some decoders ignore a seek issued before their decode thread is ready.
    if (g_pb.startMs > 0 && t >= 0 && t < g_pb.startMs - 2000 && g_pb.seekRetries < 10 &&
        GetTickCount() - g_pb.startTick > 300) {
        g_pb.seekRetries++;
        g_pb.startTick = GetTickCount();
        Log("Monitor: re-seeking to %d (at %d)", g_pb.startMs, t);
        g_pb.inner->SetOutputTime(g_pb.startMs);
        return;
    }
    if (!g_pb.eofPosted && g_pb.endMs > 0 && t >= g_pb.endMs) {
        g_pb.eofPosted = true;
        Log("Monitor: track %d ended at %d (end %d)", g_pb.track + 1, t, g_pb.endMs);
        PostMessageW(mod.hMainWindow, WM_WA_MPEG_EOF, 0, 0);
    }
}

static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER:
        if (wp == TIMER_MONITOR) Monitor();
        else if (wp == TIMER_DEFERRED_STOP) {
            if (g_pb.pendingStop) { Log("Deferred stop fired"); RealStop(); }
            KillTimer(h, TIMER_DEFERRED_STOP);
        } else if (wp == TIMER_SCAN) ScanIfChanged();
        else if (wp == TIMER_INFO) {
            // WACUP blanks kbps/kHz when a new entry starts; restore them.
            KillTimer(h, TIMER_INFO);
            Log("info refresh: %d kbps %d kHz %d ch sync %d", g_lastInfo[0], g_lastInfo[1], g_lastInfo[2], g_lastInfo[3]);
            if (g_pb.inner && g_lastInfo[1] > 0)
                mod.SetInfo(g_lastInfo[0], g_lastInfo[1], g_lastInfo[2], g_lastInfo[3]);
        }
        return 0;
    case MSG_EXPAND:
        ExpandPlaylist();
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ------------------------------------------------------ In_Module callbacks
static void Config(HWND parent) {
    MessageBoxW(parent,
                L"CUE Sheet Support\n\n"
                L"Opening a .cue file adds one playlist entry per track of the\n"
                L"audio file it references. No settings to configure.",
                L"CUE Sheet Support", MB_OK | MB_ICONINFORMATION);
}

static void About(HWND parent) {
    MessageBoxW(parent, L"CUE Sheet Support v1.0\n\nPlays tracks of single-file albums / DJ mixes\n"
                        L"described by .cue sheets.",
                L"About CUE Sheet Support", MB_OK | MB_ICONINFORMATION);
}

static int Init() {
    g_mainThread = GetWindowThreadProcessId(mod.hMainWindow, nullptr);
    Log("Init: thread %lu, main thread %lu, hMain=%p", GetCurrentThreadId(), g_mainThread, mod.hMainWindow);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = mod.hDllInstance;
    wc.lpszClassName = L"in_cuesheet_msg";
    RegisterClassW(&wc);
    if (GetCurrentThreadId() == g_mainThread) {
        g_wnd = CreateWindowW(wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, mod.hDllInstance, nullptr);
        SetTimer(g_wnd, TIMER_SCAN, 500, nullptr);
    }
    return 0;
}

// Called from main-thread entry points when Init ran on another thread.
static void EnsureWindow() {
    if (g_wnd || GetCurrentThreadId() != g_mainThread) return;
    g_wnd = CreateWindowW(L"in_cuesheet_msg", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, mod.hDllInstance, nullptr);
    SetTimer(g_wnd, TIMER_SCAN, 500, nullptr);
    Log("EnsureWindow: created %p", g_wnd);
}

static void Quit() {
    if (g_pb.inner) RealStop();
    if (g_wnd) { DestroyWindow(g_wnd); g_wnd = nullptr; }
    UnregisterClassW(L"in_cuesheet_msg", mod.hDllInstance);
}

static void GetFileInfo(const void *file_, void *title_, int *length) {
    const wchar_t *file = (const wchar_t *)file_;
    wchar_t *title = (wchar_t *)title_;
    EnsureWindow();
    std::wstring cuePath;
    int track = -1;
    std::shared_ptr<const CueSheet> cue;
    if (!file || !*file) {
        cue = g_pb.cue;
        track = g_pb.track;
    } else if (ParseTrackName(file, &cuePath, &track)) {
        cue = LoadCueSheet(cuePath);
    } else if (IsCueFile(file)) {
        RequestExpand();
        if (title) wcscpy_s(title, GETFILEINFO_TITLE_LENGTH, PathFindFileNameW(file));
        if (length) *length = -1;
        return;
    }
    if (!cue || track < 0 || track >= (int)cue->tracks.size()) {
        if (title && file) wcscpy_s(title, GETFILEINFO_TITLE_LENGTH, PathFindFileNameW(file));
        if (length) *length = -1;
        return;
    }
    if (title) wcsncpy_s(title, GETFILEINFO_TITLE_LENGTH, TrackDisplayTitle(*cue, track).c_str(), _TRUNCATE);
    if (length) *length = TrackLength(*cue, track);
}

static int InfoBox(const void *file_, HWND parent) {
    const wchar_t *file = (const wchar_t *)file_;
    std::wstring cuePath;
    int track;
    if (!ParseTrackName(file, &cuePath, &track)) return 1;
    auto cue = LoadCueSheet(cuePath);
    if (!cue || track >= (int)cue->tracks.size()) return 1;
    const CueTrack &t = cue->tracks[track];
    wchar_t buf[4096];
    int len = TrackLength(*cue, track);
    swprintf_s(buf,
               L"Track:\t%02d\nTitle:\t%s\nArtist:\t%s\nAlbum:\t%s\nStart:\t%d:%02d.%03d\nLength:\t%d:%02d\n\n"
               L"CUE sheet:\n%s\n\nAudio file:\n%s",
               t.number, t.title.c_str(), cue->TrackPerformer(track).c_str(), cue->title.c_str(),
               t.startMs / 60000, t.startMs / 1000 % 60, t.startMs % 1000, len > 0 ? len / 60000 : 0,
               len > 0 ? len / 1000 % 60 : 0, cue->path.c_str(), cue->files[t.fileIndex].c_str());
    MessageBoxW(parent, buf, L"CUE Track Info", MB_OK | MB_ICONINFORMATION);
    return 1;
}

static int IsOurFile(const void *fn) {
    return ParseTrackName((const wchar_t *)fn, nullptr, nullptr) ? 1 : 0;
}

static int Play(const void *fn_) {
    const wchar_t *fn = (const wchar_t *)fn_;
    EnsureWindow();
    Log("Play(%s)", U8(fn ? fn : L"").c_str());

    std::wstring cuePath;
    int track = -1;
    if (!ParseTrackName(fn, &cuePath, &track)) {
        if (IsCueFile(fn)) {
            // A raw .cue was started before we expanded it: expand now and
            // start the first track once the playlist has been rewritten.
            if (g_pb.inner) RealStop();
            g_playingCue = fn;
            RequestExpand();
            return 0;
        }
        Log("Play: not a CUE track name");
        return 1;
    }
    auto cue = LoadCueSheet(cuePath);
    if (!cue || track >= (int)cue->tracks.size()) {
        Log("Play: can't load sheet %s (track %d)", U8(cuePath).c_str(), track + 1);
        if (g_pb.inner) RealStop();
        return 1;
    }
    const CueTrack &t = cue->tracks[track];
    std::wstring audio = cue->files[t.fileIndex];
    Log("Play: track %d -> %s @ %d ms", track + 1, U8(audio).c_str(), t.startMs);

    // Gapless continuation: previous track ran into this one.
    if (g_pb.pendingStop && g_pb.inner && _wcsicmp(g_pb.audio.c_str(), audio.c_str()) == 0) {
        int now = g_pb.inner->GetOutputTime();
        if (now >= t.startMs - 1500 && now <= t.startMs + 1500 && (t.endMs < 0 || now < t.endMs)) {
            KillTimer(g_wnd, TIMER_DEFERRED_STOP);
            g_pb.pendingStop = false;
            g_pb.eofPosted = false;
            g_pb.cue = cue;
            g_pb.track = track;
            g_pb.startMs = t.startMs;
            g_pb.endMs = t.endMs;
            g_pb.seekRetries = 10;
            Log("Play: gapless continue into track %d (now %d, start %d)", track + 1, now, t.startMs);
            SetTimer(g_wnd, TIMER_INFO, 150, nullptr);
            return 0;
        }
    }
    if (g_pb.inner) RealStop();

    In_Module *inner = FindInnerModule(audio);
    if (!inner) {
        Log("Play: no plug-in can play %s", U8(audio).c_str());
        return 1;
    }
    inner->outMod = mod.outMod;
    SetVisTracks(*cue, t.fileIndex);
    InstallHooks(inner);
    g_pb.inner = inner;
    g_pb.cue = cue;
    g_pb.track = track;
    g_pb.audio = audio;
    g_pb.startMs = t.startMs;
    g_pb.endMs = t.endMs;
    g_pb.startTick = GetTickCount();
    int r = InnerPlay(inner, audio);
    Log("Play: inner Play(%s) = %d, seek to %d, end %d", U8(audio).c_str(), r, t.startMs, t.endMs);
    if (r != 0) {
        RemoveHooks(inner);
        g_pb = Playback();
        return r;
    }
    if (g_eqSet) inner->EQSet(g_eqOn, g_eqData, g_eqPreamp);
    if (t.startMs > 0) inner->SetOutputTime(t.startMs);
    SetTimer(g_wnd, TIMER_MONITOR, 25, nullptr);
    return 0;
}

static void Pause() { if (g_pb.inner) g_pb.inner->Pause(); }
static void UnPause() { if (g_pb.inner) g_pb.inner->UnPause(); }
static int IsPaused() { return g_pb.inner ? g_pb.inner->IsPaused() : 0; }

static void Stop() {
    Log("Stop() eofPosted=%d", g_pb.eofPosted);
    g_playingCue.clear();
    if (g_pb.inner && g_pb.eofPosted && !g_pb.pendingStop) {
        // We ended the track ourselves; keep decoding for a moment in case
        // WACUP continues with the next track of the same file.
        g_pb.pendingStop = true;
        SetTimer(g_wnd, TIMER_DEFERRED_STOP, 400, nullptr);
        return;
    }
    if (g_pb.inner) RealStop();
}

static int GetLength() {
    if (!g_pb.inner || !g_pb.cue) return -1;
    if (g_pb.endMs > 0) return g_pb.endMs - g_pb.startMs;
    int total = g_pb.inner->GetLength();
    return total > g_pb.startMs ? total - g_pb.startMs : -1;
}

static int GetOutputTime() {
    if (!g_pb.inner) return 0;
    int t = g_pb.inner->GetOutputTime() - g_pb.startMs;
    if (t < 0) t = 0;
    if (g_pb.endMs > 0 && t > g_pb.endMs - g_pb.startMs) t = g_pb.endMs - g_pb.startMs;
    return t;
}

static void SetOutputTime(int ms) {
    if (!g_pb.inner) return;
    if (g_pb.pendingStop) return;
    g_pb.eofPosted = false;
    g_pb.inner->SetOutputTime(g_pb.startMs + (ms < 0 ? 0 : ms));
}

static void SetVolume(int v) {
    if (g_pb.inner) g_pb.inner->SetVolume(v);
    else if (mod.outMod) mod.outMod->SetVolume(v);
}

static void SetPan(int p) {
    if (g_pb.inner) g_pb.inner->SetPan(p);
    else if (mod.outMod) mod.outMod->SetPan(p);
}

static void EQSet(int on, char data[10], int preamp) {
    g_eqOn = on;
    g_eqPreamp = preamp;
    memcpy(g_eqData, data, 10);
    g_eqSet = true;
    if (g_pb.inner) g_pb.inner->EQSet(on, data, preamp);
}

In_Module mod = {
    IN_VER_UNICODE,
    (char *)PLUGIN_NAME,
    0, 0,
    (char *)"CUE\0CUE Sheet (*.CUE)\0",
    1, 1,
    Config, About, Init, Quit, GetFileInfo, InfoBox, IsOurFile, Play, Pause, UnPause, IsPaused, Stop,
    GetLength, GetOutputTime, SetOutputTime, SetVolume, SetPan,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    EQSet, 0, 0,
};

extern "C" __declspec(dllexport) In_Module *winampGetInModule2() {
    Log("winampGetInModule2");
    return &mod;
}

// ------------------------------------------------ extended read (decoding)
// Used by WACUP features that decode files without playing them: waveform
// seeker, transcoder, ReplayGain, etc. We open the audio file with the
// wrapped plug-in's own extended-read exports, seek to the track start and
// stop after the track's length.
typedef intptr_t (*XOpenW)(const wchar_t *fn, int *size, int *bps, int *nch, int *srate);
typedef intptr_t (*XGetData)(intptr_t h, char *dest, int len, int *killswitch);
typedef int (*XSetTime)(intptr_t h, int ms);
typedef void (*XClose)(intptr_t h);

struct XRead {
    intptr_t h = 0;
    XGetData getData = nullptr;
    XSetTime setTime = nullptr;
    XClose close = nullptr;
    int startMs = 0;
    int lengthMs = -1;          // -1: until end of file
    int frameBytes = 0;         // bytes per sample frame (all channels)
    int srate = 0;
    long long remaining = -1;   // bytes left in the track, -1 = unlimited
};

static long long TrackBytes(const XRead &x, int fromMs) {
    if (x.lengthMs < 0) return -1;
    long long ms = x.lengthMs - fromMs;
    if (ms < 0) ms = 0;
    return ms * x.srate / 1000 * x.frameBytes;
}

static intptr_t OpenExtendedRead(const wchar_t *fn, int *size, int *bps, int *nch, int *srate, bool asFloat) {
    std::wstring cuePath;
    int track;
    if (!ParseTrackName(fn, &cuePath, &track)) return 0;
    auto cue = LoadCueSheet(cuePath);
    if (!cue || track >= (int)cue->tracks.size()) return 0;
    const CueTrack &t = cue->tracks[track];
    std::wstring audio = cue->files[t.fileIndex];
    In_Module *inner = FindInnerModule(audio);
    if (!inner || !inner->hDllInstance) return 0;
    HMODULE dll = inner->hDllInstance;
    auto open = (XOpenW)GetProcAddress(dll, asFloat ? "winampGetExtendedRead_openW_float"
                                                    : "winampGetExtendedRead_openW");
    auto getData = (XGetData)GetProcAddress(dll, "winampGetExtendedRead_getData");
    auto close = (XClose)GetProcAddress(dll, "winampGetExtendedRead_close");
    auto setTime = (XSetTime)GetProcAddress(dll, "winampGetExtendedRead_setTime");
    if (!open || !getData || !close) {
        Log("ExtendedRead: decoder for %s has no extended read support", U8(audio).c_str());
        return 0;
    }
    int isize = -1, ibps = 0, inch = 0, israte = 0;
    intptr_t h = open(audio.c_str(), &isize, &ibps, &inch, &israte);
    if (!h || ibps <= 0 || inch <= 0 || israte <= 0) {
        if (h) close(h);
        return 0;
    }
    auto x = new XRead;
    x->h = h;
    x->getData = getData;
    x->setTime = setTime;
    x->close = close;
    x->startMs = t.startMs;
    x->lengthMs = TrackLength(*cue, track);
    x->frameBytes = inch * ibps / 8;
    x->srate = israte;

    if (t.startMs > 0) {
        if (!(setTime && setTime(h, t.startMs))) {
            // No seeking: decode and discard up to the track start.
            long long skip = (long long)t.startMs * israte / 1000 * x->frameBytes;
            std::vector<char> buf(65536 - 65536 % x->frameBytes);
            int kill = 0;
            while (skip > 0) {
                int n = getData(h, buf.data(), (int)std::min<long long>(skip, (long long)buf.size()), &kill);
                if (n <= 0) break;
                skip -= n;
            }
        }
    }
    x->remaining = TrackBytes(*x, 0);
    if (size) *size = x->remaining >= 0 && x->remaining < INT_MAX ? (int)x->remaining : -1;
    if (bps) *bps = ibps;
    if (nch) *nch = inch;
    if (srate) *srate = israte;
    Log("ExtendedRead: open track %d (%s) start %d len %d -> %d Hz %d ch %d bit", track + 1,
        asFloat ? "float" : "int", t.startMs, x->lengthMs, israte, inch, ibps);
    return (intptr_t)x;
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW(const wchar_t *fn, int *size, int *bps,
                                                                       int *nch, int *srate) {
    return OpenExtendedRead(fn, size, bps, nch, srate, false);
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW_float(const wchar_t *fn, int *size,
                                                                             int *bps, int *nch, int *srate) {
    return OpenExtendedRead(fn, size, bps, nch, srate, true);
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_getData(intptr_t handle, char *dest, int len,
                                                                         int *killswitch) {
    auto x = (XRead *)handle;
    if (!x || len <= 0) return 0;
    if (x->remaining >= 0) {
        if (x->remaining == 0) return 0;
        long long want = std::min<long long>(len, x->remaining);
        len = (int)(want - want % x->frameBytes);
        if (len <= 0) return 0;
    }
    intptr_t n = x->getData(x->h, dest, len, killswitch);
    if (n > 0 && x->remaining >= 0) x->remaining -= n;
    return n;
}

extern "C" __declspec(dllexport) int winampGetExtendedRead_setTime(intptr_t handle, int ms) {
    auto x = (XRead *)handle;
    if (!x || !x->setTime) return 0;
    if (ms < 0) ms = 0;
    if (!x->setTime(x->h, x->startMs + ms)) return 0;
    x->remaining = TrackBytes(*x, ms);
    return 1;
}

extern "C" __declspec(dllexport) void winampGetExtendedRead_close(intptr_t handle) {
    auto x = (XRead *)handle;
    if (!x) return;
    x->close(x->h);
    delete x;
}

// --------------------------------------------------- extended file info
static bool CopyOut(const std::wstring &v, wchar_t *dest, int destlen) {
    if (!dest || destlen <= 0) return false;
    wcsncpy_s(dest, destlen, v.c_str(), _TRUNCATE);
    return true;
}

extern "C" __declspec(dllexport) int winampGetExtendedFileInfoW(const wchar_t *fn, const char *data,
                                                                  wchar_t *dest, int destlen) {
    if (!fn || !data) return 0;
    std::wstring cuePath;
    int track;
    if (!ParseTrackName(fn, &cuePath, &track)) {
        if (IsCueFile(fn)) {
            RequestExpand();
            if (!_stricmp(data, "type")) return CopyOut(L"0", dest, destlen);
        }
        return 0;
    }
    auto cue = LoadCueSheet(cuePath);
    if (!cue || track >= (int)cue->tracks.size()) return 0;
    const CueTrack &t = cue->tracks[track];
    wchar_t num[32];

    if (!_stricmp(data, "type")) return CopyOut(L"0", dest, destlen);
    if (!_stricmp(data, "title")) {
        if (t.title.empty()) { swprintf_s(num, L"Track %02d", t.number); return CopyOut(num, dest, destlen); }
        return CopyOut(t.title, dest, destlen);
    }
    if (!_stricmp(data, "artist")) return CopyOut(cue->TrackPerformer(track), dest, destlen);
    if (!_stricmp(data, "album")) return CopyOut(cue->title, dest, destlen);
    if (!_stricmp(data, "albumartist")) return CopyOut(cue->performer, dest, destlen);
    if (!_stricmp(data, "composer")) return CopyOut(t.songwriter.empty() ? cue->songwriter : t.songwriter, dest, destlen);
    if (!_stricmp(data, "genre")) return CopyOut(cue->genre, dest, destlen);
    if (!_stricmp(data, "year")) return CopyOut(cue->date, dest, destlen);
    if (!_stricmp(data, "comment")) return CopyOut(cue->comment, dest, destlen);
    if (!_stricmp(data, "track") || !_stricmp(data, "tracknumber")) {
        swprintf_s(num, L"%d", t.number);
        return CopyOut(num, dest, destlen);
    }
    if (!_stricmp(data, "tracks")) {
        swprintf_s(num, L"%d", (int)cue->tracks.size());
        return CopyOut(num, dest, destlen);
    }
    if (!_stricmp(data, "length")) {
        int len = TrackLength(*cue, track);
        if (len <= 0) return 0;
        swprintf_s(num, L"%d", len);
        return CopyOut(num, dest, destlen);
    }
    if (!_stricmp(data, "formatinformation")) {
        return CopyOut(L"CUE sheet track " + std::to_wstring(t.number) + L" of\n" + cue->files[t.fileIndex],
                       dest, destlen);
    }
    return 0;
}

static void SetupLog(HINSTANCE inst) {
    wchar_t dll[MAX_PATH];
    GetModuleFileNameW(inst, dll, MAX_PATH);
    wcscpy_s(g_logPath, dll);
    PathRemoveExtensionW(g_logPath);
    wcscat_s(g_logPath, L".log");
    g_log = FileExists(g_logPath);
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(inst);
        SetupLog(inst);
        Log("DLL attached");
    } else if (reason == DLL_PROCESS_DETACH) {
        Log("DLL detached");
    }
    return TRUE;
}
