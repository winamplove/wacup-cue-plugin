// Minimal subset of the Winamp/WACUP plug-in SDK (in2.h, out.h, wa_ipc.h)
// needed by in_cuesheet. Layouts match the official SDK headers.
#pragma once
#include <windows.h>

// ---------------------------------------------------------------- out.h
typedef struct Out_Module {
    int version;
    char *description;
    intptr_t id;
    HWND hMainWindow;
    HINSTANCE hDllInstance;
    void (*Config)(HWND hwndParent);
    void (*About)(HWND hwndParent);
    void (*Init)();
    void (*Quit)();
    int (*Open)(int samplerate, int numchannels, int bitspersamp, int bufferlenms, int prebufferms);
    void (*Close)();
    int (*Write)(char *buf, int len);
    int (*CanWrite)();
    int (*IsPlaying)();
    int (*Pause)(int pause);
    void (*SetVolume)(int volume);
    void (*SetPan)(int pan);
    void (*Flush)(int t);
    int (*GetOutputTime)();
    int (*GetWrittenTime)();
} Out_Module;

// ---------------------------------------------------------------- in2.h
#define IN_VER 0x100
#define IN_UNICODE 0x0F000000
#define IN_VER_UNICODE (IN_UNICODE | IN_VER)
#define GETFILEINFO_TITLE_LENGTH 2048

// Generic layout; the string type of the file-name arguments depends on
// whether (version & IN_UNICODE) is set, so we declare them as void* and
// cast as needed when talking to *other* input plug-ins.
typedef struct In_Module {
    int version;
    char *description;
    HWND hMainWindow;
    HINSTANCE hDllInstance;
    char *FileExtensions;
    int is_seekable;
    int UsesOutputPlug;
    void (*Config)(HWND hwndParent);
    void (*About)(HWND hwndParent);
    int (*Init)();
    void (*Quit)();
    void (*GetFileInfo)(const void *file, void *title, int *length_in_ms);
    int (*InfoBox)(const void *file, HWND hwndParent);
    int (*IsOurFile)(const void *fn);
    int (*Play)(const void *fn);
    void (*Pause)();
    void (*UnPause)();
    int (*IsPaused)();
    void (*Stop)();
    int (*GetLength)();
    int (*GetOutputTime)();
    void (*SetOutputTime)(int time_in_ms);
    void (*SetVolume)(int volume);
    void (*SetPan)(int pan);
    void (*SAVSAInit)(int maxlatency_in_ms, int srate);
    void (*SAVSADeInit)();
    void (*SAAddPCMData)(void *PCMData, int nch, int bps, int timestamp);
    int (*SAGetMode)();
    int (*SAAdd)(void *data, int timestamp, int csa);
    void (*VSAAddPCMData)(void *PCMData, int nch, int bps, int timestamp);
    int (*VSAGetMode)(int *specNch, int *specNumChannels);
    int (*VSAAdd)(void *data, int timestamp);
    void (*VSASetInfo)(int srate, int nch);
    int (*dsp_dosamples)(short int *samples, int numsamples, int bps, int nch, int srate);
    int (*dsp_isactive)();
    void (*EQSet)(int on, char data[10], int preamp);
    void (*SetInfo)(int bitrate, int srate, int stereo, int synched);
    Out_Module *outMod;
} In_Module;

// ---------------------------------------------------------------- wa_ipc.h
#define WM_WA_IPC WM_USER
#define WM_WA_MPEG_EOF (WM_USER + 2)

#define IPC_GETVERSION 0
#define IPC_STARTPLAY 102
#define IPC_ISPLAYING 104
#define IPC_GETOUTPUTTIME 105
#define IPC_JUMPTOTIME 106
#define IPC_SETPLAYLISTPOS 121
#define IPC_GETLISTLENGTH 124
#define IPC_GETLISTPOS 125
#define IPC_GETPLAYLISTFILE 211
#define IPC_GETPLAYLISTTITLE 212
#define IPC_GETPLAYLISTTITLEW 213
#define IPC_GETPLAYLISTFILEW 214
#define IPC_CANPLAY 220
#define IPC_REFRESHPLCACHE 247
#define IPC_GETWND 260
#define IPC_GETWND_PE 1
#define IPC_PLAYFILEW 1100

// Messages for the playlist editor window (IPC_GETWND_PE)
#define IPC_PE_DELETEINDEX 104      // SendMessage(hwndPE, WM_WA_IPC, IPC_PE_DELETEINDEX, index)
#define IPC_PE_INSERTFILENAME 106   // WM_COPYDATA, fileinfo
#define IPC_PE_INSERTFILENAMEW 114  // WM_COPYDATA, fileinfoW

typedef struct { char file[MAX_PATH]; int index; } fileinfo;
typedef struct { wchar_t file[MAX_PATH]; int index; } fileinfoW;

// WM_COMMAND ids on the main window
#define WINAMP_BUTTON2_PLAY 40045
#define WINAMP_BUTTON4_STOP 40047
