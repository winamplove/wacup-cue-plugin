#pragma once
#include <string>
#include <vector>
#include <memory>

struct CueTrack {
    int number = 0;            // TRACK nn as written in the sheet
    std::wstring title;
    std::wstring performer;
    std::wstring songwriter;
    std::wstring isrc;
    int fileIndex = -1;        // index into CueSheet::files
    int startMs = -1;          // INDEX 01
    int endMs = -1;            // start of next track in same file, -1 = end of file
};

struct CueSheet {
    std::wstring path;         // full path of the .cue file
    std::wstring title, performer, songwriter, genre, date, comment, discId;
    std::vector<std::wstring> rawFiles;   // FILE values as written
    std::vector<std::wstring> files;      // resolved full paths of audio files
    std::vector<CueTrack> tracks;
    unsigned long long stamp = 0;         // last-write time + size, for caching

    std::wstring TrackPerformer(size_t i) const {
        return tracks[i].performer.empty() ? performer : tracks[i].performer;
    }
};

// Parses a .cue file. Returns nullptr if the file can't be read or has no
// usable tracks. Results are cached (invalidated when the file changes).
std::shared_ptr<const CueSheet> LoadCueSheet(const std::wstring &cuePath);

bool FileExists(const std::wstring &path);
