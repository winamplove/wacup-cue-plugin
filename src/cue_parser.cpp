#include "cue_parser.h"
#include <windows.h>
#include <shlwapi.h>
#include <map>
#include <mutex>

bool FileExists(const std::wstring &path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
    return s;
}

static std::wstring DecodeText(const std::string &bytes) {
    const char *p = bytes.data();
    int n = (int)bytes.size();
    if (n >= 2 && (unsigned char)p[0] == 0xFF && (unsigned char)p[1] == 0xFE) {
        return std::wstring((const wchar_t *)(p + 2), (n - 2) / 2);
    }
    if (n >= 2 && (unsigned char)p[0] == 0xFE && (unsigned char)p[1] == 0xFF) {
        std::wstring w((n - 2) / 2, L'\0');
        for (size_t i = 0; i < w.size(); i++)
            w[i] = (wchar_t)(((unsigned char)p[2 + i * 2] << 8) | (unsigned char)p[3 + i * 2]);
        return w;
    }
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) {
        p += 3; n -= 3; flags = 0;
    }
    if (n == 0) return std::wstring();
    int len = MultiByteToWideChar(cp, flags, p, n, nullptr, 0);
    if (len <= 0) { cp = CP_ACP; flags = 0; len = MultiByteToWideChar(cp, 0, p, n, nullptr, 0); }
    std::wstring w(len, L'\0');
    MultiByteToWideChar(cp, flags, p, n, &w[0], len);
    return w;
}

static void SkipWs(const wchar_t *&s) { while (*s == L' ' || *s == L'\t') s++; }

static std::wstring ReadWord(const wchar_t *&s) {
    SkipWs(s);
    const wchar_t *b = s;
    while (*s && *s != L' ' && *s != L'\t') s++;
    return std::wstring(b, s);
}

// Reads a (possibly quoted) value. For quoted values the closing quote is the
// last quote on the line, so stray quotes inside titles survive. 'rest'
// receives whatever follows the value (e.g. the FILE type).
static std::wstring ReadValue(const wchar_t *&s, std::wstring *rest = nullptr) {
    SkipWs(s);
    std::wstring v;
    if (*s == L'"') {
        const wchar_t *b = s + 1;
        const wchar_t *e = wcsrchr(b, L'"');
        if (!e) e = b + wcslen(b);
        v.assign(b, e);
        s = *e ? e + 1 : e;
    } else if (rest) {
        // unquoted FILE name: the type is the last word
        const wchar_t *b = s;
        const wchar_t *end = s + wcslen(s);
        const wchar_t *lastSp = end;
        while (lastSp > b && lastSp[-1] != L' ' && lastSp[-1] != L'\t') lastSp--;
        if (lastSp > b) { v.assign(b, lastSp); s = lastSp; }
        else { v.assign(b, end); s = end; }
        while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
    } else {
        v = s;
        s += wcslen(s);
        while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
    }
    if (rest) { SkipWs(s); *rest = s; }
    return v;
}

static int ParseMsf(const std::wstring &t) {
    int mm = 0, ss = 0, ff = 0;
    if (swscanf_s(t.c_str(), L"%d:%d:%d", &mm, &ss, &ff) < 2) return -1;
    return (int)(((long long)mm * 60 + ss) * 1000 + (long long)ff * 1000 / 75);
}

static std::wstring DirOf(const std::wstring &p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? std::wstring() : p.substr(0, s + 1);
}
static std::wstring NameOf(const std::wstring &p) {
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}
static std::wstring StripExt(const std::wstring &n) {
    size_t d = n.find_last_of(L'.');
    return d == std::wstring::npos ? n : n.substr(0, d);
}
static std::wstring ExtOf(const std::wstring &n) {
    size_t d = n.find_last_of(L'.');
    return d == std::wstring::npos ? std::wstring() : n.substr(d);
}

static const wchar_t *kAudioExts[] = {
    L".mp3", L".flac", L".ape", L".wv", L".ogg", L".opus", L".m4a", L".wav",
    L".tta", L".mpc", L".wma", L".aac", L".mp2", L".aiff", L".aif",
};

static std::wstring ResolveAudio(const std::wstring &cuePath, const std::wstring &raw, bool singleFile) {
    std::wstring dir = DirOf(cuePath);
    bool absolute = raw.size() > 1 && (raw[1] == L':' || (raw[0] == L'\\' && raw[1] == L'\\'));
    std::wstring first = absolute ? raw : dir + raw;
    if (FileExists(first)) return first;

    std::wstring name = NameOf(raw);
    if (FileExists(dir + name)) return dir + name;

    // Same name, different extension (e.g. sheet says .wav, file is .mp3).
    std::wstring base = StripExt(name);
    for (auto e : kAudioExts)
        if (FileExists(dir + base + e)) return dir + base + e;

    // Audio file named after the .cue itself.
    if (singleFile) {
        std::wstring cueBase = StripExt(NameOf(cuePath));
        std::wstring ext = ExtOf(name);
        if (!ext.empty() && FileExists(dir + cueBase + ext)) return dir + cueBase + ext;
        for (auto e : kAudioExts)
            if (FileExists(dir + cueBase + e)) return dir + cueBase + e;
    }
    return first;
}

static std::shared_ptr<CueSheet> ParseCue(const std::wstring &cuePath, const std::string &bytes) {
    auto cue = std::make_shared<CueSheet>();
    cue->path = cuePath;
    std::wstring text = DecodeText(bytes);

    CueTrack *cur = nullptr;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t eol = text.find_first_of(L"\r\n", pos);
        if (eol == std::wstring::npos) eol = text.size();
        std::wstring line = text.substr(pos, eol - pos);
        pos = eol + 1;

        const wchar_t *s = line.c_str();
        std::wstring kw = ReadWord(s);
        for (auto &c : kw) c = (wchar_t)towupper(c);
        if (kw.empty()) continue;

        if (kw == L"FILE") {
            std::wstring type;
            std::wstring f = ReadValue(s, &type);
            cue->rawFiles.push_back(f);
            // 'cur' is kept: a FILE line may sit between a track's INDEX 00
            // and INDEX 01; INDEX 01 then binds the track to the new file.
        } else if (kw == L"TRACK") {
            std::wstring num = ReadWord(s);
            std::wstring type = ReadWord(s);
            for (auto &c : type) c = (wchar_t)towupper(c);
            if (cue->rawFiles.empty() || (!type.empty() && type != L"AUDIO")) { cur = nullptr; continue; }
            CueTrack t;
            t.number = _wtoi(num.c_str());
            t.fileIndex = (int)cue->rawFiles.size() - 1;
            cue->tracks.push_back(t);
            cur = &cue->tracks.back();
        } else if (kw == L"TITLE") {
            (cur ? cur->title : cue->title) = ReadValue(s);
        } else if (kw == L"PERFORMER") {
            (cur ? cur->performer : cue->performer) = ReadValue(s);
        } else if (kw == L"SONGWRITER") {
            (cur ? cur->songwriter : cue->songwriter) = ReadValue(s);
        } else if (kw == L"ISRC") {
            if (cur) cur->isrc = ReadValue(s);
        } else if (kw == L"INDEX") {
            if (!cur) continue;
            int idx = _wtoi(ReadWord(s).c_str());
            int ms = ParseMsf(ReadWord(s));
            if (ms < 0) continue;
            // Tracks play from INDEX 01 to the next track's INDEX 01, so any
            // pregap (INDEX 00) belongs to the previous track.
            if (idx == 1) { cur->startMs = ms; cur->fileIndex = (int)cue->rawFiles.size() - 1; }
        } else if (kw == L"REM") {
            std::wstring key = ReadWord(s);
            for (auto &c : key) c = (wchar_t)towupper(c);
            std::wstring v = ReadValue(s);
            if (cur) continue;
            if (key == L"GENRE") cue->genre = v;
            else if (key == L"DATE") cue->date = v;
            else if (key == L"COMMENT") cue->comment = v;
            else if (key == L"DISCID") cue->discId = v;
        }
    }

    // Drop tracks without a start position.
    std::vector<CueTrack> ok;
    for (auto &t : cue->tracks) if (t.startMs >= 0) ok.push_back(t);
    cue->tracks.swap(ok);
    if (cue->tracks.empty()) return nullptr;

    for (size_t i = 0; i + 1 < cue->tracks.size(); i++) {
        auto &t = cue->tracks[i], &n = cue->tracks[i + 1];
        if (n.fileIndex == t.fileIndex && n.startMs > t.startMs) t.endMs = n.startMs;
    }

    bool single = cue->rawFiles.size() == 1;
    for (auto &f : cue->rawFiles) cue->files.push_back(ResolveAudio(cuePath, f, single));
    return cue;
}

std::shared_ptr<const CueSheet> LoadCueSheet(const std::wstring &cuePath) {
    static std::mutex mtx;
    static std::map<std::wstring, std::shared_ptr<CueSheet>> cache;

    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(cuePath.c_str(), GetFileExInfoStandard, &fad)) return nullptr;
    unsigned long long stamp = ((unsigned long long)fad.ftLastWriteTime.dwHighDateTime << 32 |
                                fad.ftLastWriteTime.dwLowDateTime) ^ fad.nFileSizeLow;
    std::wstring key = Lower(cuePath);
    {
        std::lock_guard<std::mutex> lk(mtx);
        auto it = cache.find(key);
        if (it != cache.end() && it->second->stamp == stamp) return it->second;
    }
    if (fad.nFileSizeHigh || fad.nFileSizeLow > 4 * 1024 * 1024) return nullptr;

    HANDLE h = CreateFileW(cuePath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return nullptr;
    std::string bytes(fad.nFileSizeLow, '\0');
    DWORD rd = 0;
    BOOL ok = bytes.empty() || ReadFile(h, &bytes[0], (DWORD)bytes.size(), &rd, nullptr);
    CloseHandle(h);
    if (!ok) return nullptr;
    bytes.resize(rd);

    auto cue = ParseCue(cuePath, bytes);
    if (!cue) return nullptr;
    cue->stamp = stamp;
    std::lock_guard<std::mutex> lk(mtx);
    cache[key] = cue;
    return cue;
}
