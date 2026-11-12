/*
 * appsandbox-clipboard.exe — Guest-side clipboard writer for AppSandbox.
 *
 * Runs as SYSTEM, spawned by the agent service. Listens on AF_HYPERV
 * socket (GUID :0005) for the host IDD display to connect. Handles the
 * host→guest clipboard direction using delayed rendering:
 *   - On FORMAT_LIST from host: stores format metadata, requests data
 *     via FORMAT_DATA_REQ one format at a time
 *   - On FORMAT_DATA_RESP from host: accumulates data, applies all
 *     formats to guest clipboard once complete
 *
 * Guest→host clipboard is handled separately by appsandbox-clipboard-reader.exe
 * (USER, GUID :0006).
 *
 * Logs to C:\Windows\AppSandbox\clipboard.log (beside agent.log).
 */

#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <stdio.h>
#include <stdarg.h>
#include <shellapi.h>
#include <shlobj.h>
#include "../transport/asb_transport.h"   /* AF_HYPERV on PC, ivshmem on Mac */

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")

/* ---- Hyper-V socket definitions ---- */

#define AF_HYPERV 34

typedef struct _SOCKADDR_HV {
    ADDRESS_FAMILY Family;
    USHORT Reserved;
    GUID VmId;
    GUID ServiceId;
} SOCKADDR_HV;

static const GUID HV_GUID_WILDCARD =
    { 0, 0, 0, { 0, 0, 0, 0, 0, 0, 0, 0 } };

/* Clipboard channel: {A5B0CAFE-0005-4000-8000-000000000001} */
static const GUID CLIPBOARD_SERVICE_GUID =
    { 0xa5b0cafe, 0x0005, 0x4000, { 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01 } };

/* ---- Clipboard protocol ---- */

#define CLIP_MAGIC          0x504C4341  /* "ACLP" little-endian */
#define CLIP_READY_MAGIC    0x59444C43  /* "CLDY" little-endian */

/* Protocol: delayed rendering (like MS-RDPECLIP) */
#define CLIP_MSG_FORMAT_LIST      1
#define CLIP_MSG_FORMAT_DATA_REQ  2
#define CLIP_MSG_FORMAT_DATA_RESP 3
#define CLIP_MSG_FILE_DATA        4
#define CLIP_MSG_FILE_SIZE_REQ    5   /* fork: ask the host how big its CF_HDROP is */
#define CLIP_MSG_FILE_SIZE_RESP   6
#define CLIP_FMT_CONFIRMED        0x80000000u  /* fork: DATA_REQ flag, user confirmed a large copy */
#define CLIP_MSG_SYNC_ENABLE      12

#define CLIP_MAX_FORMATS    64
#define CLIP_MAX_PAYLOAD    (64 * 1024 * 1024)  /* 64 MB */

#define WM_CLIP_APPLY_FMTS (WM_USER + 200)
#define WM_CLIP_LARGE_PASTE (WM_USER + 201)
#define WM_CLIP_XFER_DONE   (WM_USER + 202)

/* Fork: files are fetched only when something pastes them. Windows waits 30 s for
   WM_RENDERFORMAT, so pastes over the limit ask first and land in Downloads\From PC. */
#define CLIP_PASTE_LIMIT        (1ull << 30)
#define CLIP_RENDER_BUDGET_MS   25000
#define CLIP_SIZE_QUICK_MS      3000

#pragma pack(push, 1)
typedef struct {
    UINT32 magic;
    UINT32 msg_type;
    UINT32 format;
    UINT32 data_size;
} ClipHeader;
#pragma pack(pop)

/* Format entry: maps local↔remote IDs and holds fetched data */
typedef struct {
    UINT   local_id;
    UINT   remote_id;
    BYTE  *data;
    UINT32 data_size;
    BOOL   is_hdrop;
} ClipFmtEntry;

/* File transfer for CF_HDROP */
#define CLIP_FILE_CHUNK     (1024 * 1024)  /* 1 MB — HV socket is basically memcpy */
static wchar_t g_clip_temp_dir[MAX_PATH];

#pragma pack(push, 1)
typedef struct {
    UINT32 path_len;     /* relative path length in bytes (UTF-16LE, no null) */
    UINT64 file_size;    /* file data bytes that follow (0 for directories) */
    UINT8  is_directory; /* 1 = directory entry, 0 = regular file */
} ClipFileInfo;

typedef struct {
    UINT64 bytes;
    UINT32 files;
    UINT32 over;
} ClipSizeResp;
#pragma pack(pop)

/* Format data: filled by recv thread, applied by wndproc */
static ClipFmtEntry g_clip_fmts[CLIP_MAX_FORMATS];
static int          g_clip_fmt_count = 0;
static int          g_clip_fetch_idx = 0;

/* Echo suppression */
static volatile LONG g_suppress = 0;

/* Focus-gated sync: host sends SYNC_ENABLE(0/1) on :0005.
 * When disabled, we ignore FORMAT_LIST and don't apply to clipboard.
 * The named event is shared with appsandbox-clipboard-reader.exe so
 * it can gate guest->host FORMAT_LIST sending. */
static volatile LONG g_sync_enabled = 0;
static HANDLE g_sync_event = NULL;
#define CLIP_SYNC_EVENT_NAME L"Global\\AppSandboxClipSync"

/* Socket shared between threads */
static AsbConn * volatile g_client_sock = NULL;
static CRITICAL_SECTION g_send_cs;

/* Fork: paste-time file transfer state. */
enum { XFER_NONE, XFER_SYNC, XFER_DETACHED, XFER_DONE };
static volatile LONG g_xfer = XFER_NONE;      /* SYNC = a paste waits; DETACHED = goes to Downloads */
static volatile LONG g_xfer_ok = 0;
static HANDLE        g_xfer_event = NULL;     /* set when a SYNC transfer ends */
static HANDLE        g_size_event = NULL;
static ClipSizeResp  g_size;
static volatile LONG g_large_pending = 0;     /* the large-paste dialog is open or queued */
static volatile LONG g_large_asked = 0;       /* answered for this PC clipboard: don't ask again */
static wchar_t       g_xfer_dest[MAX_PATH];

/* Message window (created on msg_window_thread) */
static HWND g_msg_hwnd = NULL;

/* ---- Logging ---- */

static void clip_log(const char *fmt, ...)
{
    FILE *f;
    va_list ap;
    SYSTEMTIME st;

    if (fopen_s(&f, "C:\\Windows\\AppSandbox\\clipboard.log", "a") != 0 || !f)
        return;
    GetLocalTime(&st);
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] ",
        st.wYear, st.wMonth, st.wDay,
        st.wHour, st.wMinute, st.wSecond);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fprintf(f, "\n");
    fclose(f);
}

/* ---- Reliable send/recv ---- */

static BOOL send_all(AsbConn *s, const void *buf, int len)
{
    return asb_send(s, buf, len) == len;
}

static BOOL recv_exact(AsbConn *s, void *buf, int len)
{
    int got = 0;
    while (got < len) {
        int n = asb_recv(s, (char *)buf + got, len - got);
        if (n <= 0) return FALSE;
        got += n;
    }
    return TRUE;
}

/* ---- Send helpers ---- */

static BOOL send_all_locked(const void *buf, int len)
{
    AsbConn *s = g_client_sock;
    if (!s) return FALSE;
    return asb_send(s, buf, len) == len;
}

/* ---- Send clipboard to host ---- */

static void clip_init_temp_dir(void)
{
    DWORD session_id = WTSGetActiveConsoleSessionId();
    HANDLE user_token = NULL;
    wchar_t tmp[MAX_PATH];

    /* Get the logged-in user's token so we can resolve their %TEMP% */
    if (session_id != 0xFFFFFFFF &&
        WTSQueryUserToken(session_id, &user_token)) {
        if (ExpandEnvironmentStringsForUserW(user_token, L"%TEMP%",
                                              tmp, MAX_PATH)) {
            swprintf_s(g_clip_temp_dir, MAX_PATH, L"%s\\AppSandboxClip", tmp);
            CloseHandle(user_token);
            CreateDirectoryW(g_clip_temp_dir, NULL);
            clip_log("Temp dir: %ls", g_clip_temp_dir);
            return;
        }
        CloseHandle(user_token);
    }

    /* Fallback: SYSTEM temp (should not normally happen) */
    GetTempPathW(MAX_PATH, tmp);
    swprintf_s(g_clip_temp_dir, MAX_PATH, L"%sAppSandboxClip", tmp);
    CreateDirectoryW(g_clip_temp_dir, NULL);
    clip_log("Temp dir (fallback): %ls", g_clip_temp_dir);
}

/* ---- File transfer helpers ---- */

static void clip_remove_dir_recursive(const wchar_t *dir)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    swprintf_s(pattern, MAX_PATH, L"%s\\*", dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        wchar_t path[MAX_PATH];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(path, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            clip_remove_dir_recursive(path);
        else
            DeleteFileW(path);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    RemoveDirectoryW(dir);
}

static void clip_ensure_parent_dir(const wchar_t *file_path)
{
    wchar_t dir[MAX_PATH];
    wchar_t *p;
    wcscpy_s(dir, MAX_PATH, file_path);
    p = wcsrchr(dir, L'\\');
    if (!p) return;
    *p = L'\0';
    for (p = dir; *p; p++) {
        if (*p == L'\\' && p > dir && *(p - 1) != L':') {
            *p = L'\0';
            CreateDirectoryW(dir, NULL);
            *p = L'\\';
        }
    }
    CreateDirectoryW(dir, NULL);
}

static BOOL clip_path_is_safe(const wchar_t *rel_path)
{
    if (wcsstr(rel_path, L"..")) return FALSE;
    if (rel_path[0] == L'\\' || rel_path[0] == L'/') return FALSE;
    if (wcslen(rel_path) > MAX_PATH - 50) return FALSE;
    return TRUE;
}

static HGLOBAL clip_build_hdrop_from_temp(void)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    wchar_t paths[CLIP_MAX_FORMATS][MAX_PATH];
    int count = 0;
    SIZE_T total_size;
    HGLOBAL hMem;
    DROPFILES *df;
    wchar_t *ptr;
    int i;

    swprintf_s(pattern, MAX_PATH, L"%s\\*", g_clip_temp_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return NULL;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(paths[count], MAX_PATH, L"%s\\%s", g_clip_temp_dir, fd.cFileName);
        count++;
        if (count >= CLIP_MAX_FORMATS) break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    if (count == 0) return NULL;

    total_size = sizeof(DROPFILES);
    for (i = 0; i < count; i++)
        total_size += (wcslen(paths[i]) + 1) * sizeof(wchar_t);
    total_size += sizeof(wchar_t);

    hMem = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, total_size);
    if (!hMem) return NULL;
    df = (DROPFILES *)GlobalLock(hMem);
    if (!df) { GlobalFree(hMem); return NULL; }
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    ptr = (wchar_t *)((BYTE *)df + sizeof(DROPFILES));
    for (i = 0; i < count; i++) {
        wcscpy_s(ptr, MAX_PATH, paths[i]);
        ptr += wcslen(paths[i]) + 1;
    }
    *ptr = L'\0';
    GlobalUnlock(hMem);
    return hMem;
}

/* Free all pending format data */
static void clip_free_pending(void)
{
    int i;
    for (i = 0; i < g_clip_fmt_count; i++) {
        if (g_clip_fmts[i].data) {
            HeapFree(GetProcessHeap(), 0, g_clip_fmts[i].data);
            g_clip_fmts[i].data = NULL;
        }
    }
}

/* Send FORMAT_DATA_REQ for the next format, or post WM_CLIP_APPLY_FMTS if all done.
   Called from recv thread. */
static void clip_request_next_format(void)
{
    ClipHeader hdr;

    /* Fork: never fetch files up front; they are fetched on paste (clip_render_hdrop). */
    while (g_clip_fetch_idx < g_clip_fmt_count && g_clip_fmts[g_clip_fetch_idx].remote_id == CF_HDROP) {
        g_clip_fmts[g_clip_fetch_idx].is_hdrop = TRUE;
        g_clip_fetch_idx++;
    }

    if (g_clip_fetch_idx >= g_clip_fmt_count) {
        /* All formats fetched — tell msg window to apply */
        clip_log("All %d formats fetched, applying.", g_clip_fmt_count);
        if (g_msg_hwnd)
            PostMessageW(g_msg_hwnd, WM_CLIP_APPLY_FMTS, 0, 0);
        return;
    }

    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = CLIP_MSG_FORMAT_DATA_REQ;
    hdr.format = g_clip_fmts[g_clip_fetch_idx].remote_id;
    hdr.data_size = 0;

    EnterCriticalSection(&g_send_cs);
    send_all_locked(&hdr, sizeof(hdr));
    LeaveCriticalSection(&g_send_cs);

    clip_log("Requesting format %u (remote=%u) [%d/%d].",
             g_clip_fmts[g_clip_fetch_idx].local_id,
             g_clip_fmts[g_clip_fetch_idx].remote_id,
             g_clip_fetch_idx + 1, g_clip_fmt_count);
}

/* Apply fetched format data to the clipboard.
   Called on the message window thread via WM_CLIP_APPLY_FMTS. */
static void clip_apply_format_list(void)
{
    int i;

    if (g_clip_fmt_count == 0) return;

    InterlockedExchange(&g_suppress, 1);

    if (!OpenClipboard(g_msg_hwnd)) {
        clip_log("clip_apply_format_list: OpenClipboard failed (%lu).", GetLastError());
        InterlockedExchange(&g_suppress, 0);
        clip_free_pending();
        return;
    }

    EmptyClipboard();

    for (i = 0; i < g_clip_fmt_count; i++) {
        if (g_clip_fmts[i].is_hdrop) {
            /* Fork: delayed rendering; WM_RENDERFORMAT fetches the files on paste. */
            SetClipboardData(g_clip_fmts[i].local_id, NULL);
        } else if (g_clip_fmts[i].data && g_clip_fmts[i].data_size > 0) {
            HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, g_clip_fmts[i].data_size);
            if (hMem) {
                void *ptr = GlobalLock(hMem);
                if (ptr) {
                    memcpy(ptr, g_clip_fmts[i].data, g_clip_fmts[i].data_size);
                    GlobalUnlock(hMem);
                    SetClipboardData(g_clip_fmts[i].local_id, hMem);
                } else {
                    GlobalFree(hMem);
                }
            }
        }
    }

    CloseClipboard();
    clip_log("Applied %d formats to clipboard.", g_clip_fmt_count);
    clip_free_pending();
}

/* ---- Paste-time file transfer (fork) ---- */

static BOOL clip_send_msg(UINT32 type, UINT32 format, const void *payload, UINT32 size)
{
    ClipHeader hdr;
    BOOL ok;
    hdr.magic = CLIP_MAGIC;
    hdr.msg_type = type;
    hdr.format = format;
    hdr.data_size = size;
    EnterCriticalSection(&g_send_cs);
    ok = send_all_locked(&hdr, sizeof(hdr)) && (!size || send_all_locked(payload, (int)size));
    LeaveCriticalSection(&g_send_cs);
    return ok;
}

/* Ask the host for the size of its files, counting at most to limit (0 = all). */
static BOOL clip_query_size(UINT64 limit, ClipSizeResp *out, DWORD timeout_ms)
{
    ResetEvent(g_size_event);
    if (!clip_send_msg(CLIP_MSG_FILE_SIZE_REQ, CF_HDROP, &limit, sizeof(limit))) return FALSE;
    if (WaitForSingleObject(g_size_event, timeout_ms) != WAIT_OBJECT_0) return FALSE;
    *out = g_size;
    return TRUE;
}

/* %USERPROFILE%\Downloads\From PC of the logged-in user. The agent normally runs this
   helper as that user; WTSQueryUserToken only covers a SYSTEM launch.
   shortcut: ignores a relocated Downloads folder; use SHGetKnownFolderPath if someone moves it. */
static BOOL clip_downloads_dir(wchar_t *out)
{
    static const wchar_t path[] = L"%USERPROFILE%\\Downloads\\From PC";
    DWORD session_id = WTSGetActiveConsoleSessionId();
    HANDLE token = NULL;
    BOOL ok = FALSE;
    if (session_id != 0xFFFFFFFF && WTSQueryUserToken(session_id, &token)) {
        ok = ExpandEnvironmentStringsForUserW(token, path, out, MAX_PATH);
        CloseHandle(token);
    }
    if (!ok) {
        DWORD n = ExpandEnvironmentStringsW(path, out, MAX_PATH);
        ok = n > 0 && n <= MAX_PATH && out[0] != L'%';
    }
    if (ok) CreateDirectoryW(out, NULL);
    return ok;
}

/* dir\name, or "name (2).ext" ... when taken. */
static BOOL clip_free_name(const wchar_t *dir, const wchar_t *name, BOOL is_dir, wchar_t *out)
{
    wchar_t base[MAX_PATH];
    const wchar_t *ext = is_dir ? NULL : wcsrchr(name, L'.');
    int n;
    swprintf_s(out, MAX_PATH, L"%s\\%s", dir, name);
    if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) return TRUE;
    wcsncpy_s(base, MAX_PATH, name, ext ? (size_t)(ext - name) : _TRUNCATE);
    for (n = 2; n < 1000; n++) {
        swprintf_s(out, MAX_PATH, L"%s\\%s (%d)%s", dir, base, n, ext ? ext : L"");
        if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) return TRUE;
    }
    return FALSE;
}

/* Move everything the host sent from the temp dir into Downloads\From PC. */
static BOOL clip_move_to_downloads(void)
{
    WIN32_FIND_DATAW fd;
    wchar_t pattern[MAX_PATH];
    HANDLE h;
    BOOL ok = TRUE;
    if (!clip_downloads_dir(g_xfer_dest)) return FALSE;
    swprintf_s(pattern, MAX_PATH, L"%s\\*", g_clip_temp_dir);
    h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    do {
        wchar_t src[MAX_PATH], dst[MAX_PATH];
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        swprintf_s(src, MAX_PATH, L"%s\\%s", g_clip_temp_dir, fd.cFileName);
        if (!clip_free_name(g_xfer_dest, fd.cFileName, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, dst) ||
            !MoveFileExW(src, dst, MOVEFILE_COPY_ALLOWED)) {
            clip_log("Move to Downloads failed for %ls (%lu).", fd.cFileName, GetLastError());
            ok = FALSE;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

/* Recv thread: the host's file stream for a paste ended (or the connection dropped). */
static void clip_xfer_finished(BOOL ok)
{
    LONG prev;
    InterlockedExchange(&g_xfer_ok, ok);
    prev = InterlockedCompareExchange(&g_xfer, XFER_DONE, XFER_SYNC);
    if (prev == XFER_SYNC) {
        SetEvent(g_xfer_event);
    } else if (prev == XFER_DETACHED) {
        BOOL moved = ok && clip_move_to_downloads();
        clip_remove_dir_recursive(g_clip_temp_dir);
        CreateDirectoryW(g_clip_temp_dir, NULL);
        InterlockedExchange(&g_xfer, XFER_NONE);
        if (g_msg_hwnd) PostMessageW(g_msg_hwnd, WM_CLIP_XFER_DONE, moved, 0);
    }
}

/* WM_RENDERFORMAT(CF_HDROP): an app is pasting the host's files. Must answer within 30 s
   and must not open the clipboard (SetClipboardData directly). */
static void clip_render_hdrop(void)
{
    ClipSizeResp size = {0};
    ULONGLONG start = GetTickCount64(), spent;
    DWORD wait;

    if (g_xfer != XFER_NONE || g_large_pending) {
        clip_log("Paste ignored: a copy from the PC is already running.");
        return;
    }
    /* Explorer and others read CF_HDROP again; one dialog per copy on the PC. */
    if (g_large_asked) return;
    /* No answer = old host; it then refuses over 1 GB itself. */
    clip_query_size(CLIP_PASTE_LIMIT, &size, CLIP_SIZE_QUICK_MS);
    if (size.over) {
        clip_log("Paste of more than 1 GB: asking before copying to Downloads.");
        InterlockedExchange(&g_large_pending, 1);
        PostMessageW(g_msg_hwnd, WM_CLIP_LARGE_PASTE, 0, 0);
        return;
    }

    clip_remove_dir_recursive(g_clip_temp_dir);
    CreateDirectoryW(g_clip_temp_dir, NULL);
    ResetEvent(g_xfer_event);
    InterlockedExchange(&g_xfer, XFER_SYNC);
    if (!clip_send_msg(CLIP_MSG_FORMAT_DATA_REQ, CF_HDROP, NULL, 0)) {
        InterlockedExchange(&g_xfer, XFER_NONE);
        return;
    }
    spent = GetTickCount64() - start;
    wait = spent < CLIP_RENDER_BUDGET_MS ? (DWORD)(CLIP_RENDER_BUDGET_MS - spent) : 0;
    if (WaitForSingleObject(g_xfer_event, wait) != WAIT_OBJECT_0 &&
        InterlockedCompareExchange(&g_xfer, XFER_DETACHED, XFER_SYNC) == XFER_SYNC) {
        clip_log("Paste still copying after %u s; it continues into Downloads\\From PC.", CLIP_RENDER_BUDGET_MS / 1000);
        return;
    }
    InterlockedExchange(&g_xfer, XFER_NONE);
    if (g_xfer_ok) {
        HGLOBAL h = clip_build_hdrop_from_temp();
        if (h && !SetClipboardData(CF_HDROP, h)) GlobalFree(h);
        clip_log("Pasted files from the PC.");
    }
}

static void clip_fmt_gb(UINT64 bytes, wchar_t *out, size_t n)
{
    swprintf_s(out, n, L"%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
}

/* Message thread: ask before copying a paste over the limit into Downloads\From PC. */
static void clip_large_paste(void)
{
    ClipSizeResp size;
    ULARGE_INTEGER free_bytes;
    wchar_t dest[MAX_PATH], gb[32], free_gb[32], msg[1024];

    InterlockedExchange(&g_large_asked, 1);
    if (!clip_query_size(0, &size, 120000) || !size.bytes) {
        MessageBoxW(NULL, L"Couldn't read the size of the files on the PC, so nothing was copied.",
                    L"AppSandbox", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        goto out;
    }
    if (!clip_downloads_dir(dest)) {
        MessageBoxW(NULL, L"Couldn't find your Downloads folder, so nothing was copied.",
                    L"AppSandbox", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        goto out;
    }
    clip_fmt_gb(size.bytes, gb, 32);
    /* Keep 2 GB spare so a big copy can't fill the VM's disk. */
    if (GetDiskFreeSpaceExW(dest, &free_bytes, NULL, NULL) &&
        free_bytes.QuadPart < size.bytes + (2ull << 30)) {
        clip_fmt_gb(free_bytes.QuadPart, free_gb, 32);
        swprintf_s(msg, 1024, L"Not enough space in this VM to copy %s (%u files) from the PC.\n\nFree: %s. Nothing was copied.",
                   gb, size.files, free_gb);
        MessageBoxW(NULL, msg, L"AppSandbox", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
        goto out;
    }
    swprintf_s(msg, 1024, L"Copy %s (%u files) from the PC into this VM?\n\n"
               L"That's too large to paste directly. The files will be copied to:\n%s\n\n"
               L"You'll get a message when it's done.", gb, size.files, dest);
    if (MessageBoxW(NULL, msg, L"AppSandbox", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND | MB_TOPMOST) != IDYES)
        goto out;
    if (g_xfer != XFER_NONE) goto out;

    clip_remove_dir_recursive(g_clip_temp_dir);
    CreateDirectoryW(g_clip_temp_dir, NULL);
    InterlockedExchange(&g_xfer, XFER_DETACHED);
    if (!clip_send_msg(CLIP_MSG_FORMAT_DATA_REQ, CF_HDROP | CLIP_FMT_CONFIRMED, NULL, 0)) {
        InterlockedExchange(&g_xfer, XFER_NONE);
        MessageBoxW(NULL, L"Lost the connection to the PC; nothing was copied.",
                    L"AppSandbox", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND | MB_TOPMOST);
    } else {
        clip_log("Large paste confirmed: copying %llu bytes to %ls.", size.bytes, dest);
    }
out:
    InterlockedExchange(&g_large_pending, 0);
}

static void clip_notify_done(BOOL ok)
{
    wchar_t msg[MAX_PATH + 128];
    if (ok) swprintf_s(msg, ARRAYSIZE(msg), L"Done. The files from the PC are in:\n%s", g_xfer_dest);
    else wcscpy_s(msg, ARRAYSIZE(msg), L"Copying the files from the PC failed or was interrupted.");
    clip_log("Large paste finished: %s.", ok ? "ok" : "failed");
    MessageBoxW(NULL, msg, L"AppSandbox", MB_OK | (ok ? MB_ICONINFORMATION : MB_ICONWARNING) | MB_SETFOREGROUND | MB_TOPMOST);
}

/* ---- Message window for clipboard monitoring ---- */

static LRESULT CALLBACK clip_msg_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CLIP_APPLY_FMTS:
        clip_log("WM_CLIP_APPLY_FMTS — applying fetched data.");
        clip_apply_format_list();
        return 0;
    case WM_RENDERFORMAT:
        if (wp == CF_HDROP) clip_render_hdrop();
        return 0;
    case WM_CLIP_LARGE_PASTE:
        clip_large_paste();
        return 0;
    case WM_CLIP_XFER_DONE:
        clip_notify_done((BOOL)wp);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void switch_to_input_desktop(void)
{
    HDESK desk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (desk) {
        SetThreadDesktop(desk);
        CloseDesktop(desk);
    }
}

static DWORD WINAPI msg_window_thread(LPVOID param)
{
    WNDCLASSEXW wc;
    MSG msg;
    (void)param;

    /* Must be on the interactive desktop to access the user's clipboard */
    switch_to_input_desktop();

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = clip_msg_wnd_proc;
    wc.hInstance     = GetModuleHandleW(NULL);
    wc.lpszClassName = L"AppSandboxClipHelper";
    RegisterClassExW(&wc);

    g_msg_hwnd = CreateWindowExW(0, L"AppSandboxClipHelper", NULL,
                                  0, 0, 0, 0, 0, HWND_MESSAGE,
                                  NULL, GetModuleHandleW(NULL), NULL);
    if (!g_msg_hwnd) {
        clip_log("Failed to create message window (%lu).", GetLastError());
        return 1;
    }

    clip_log("Message window created (write-only, no clipboard listener).");

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    DestroyWindow(g_msg_hwnd);
    g_msg_hwnd = NULL;
    return 0;
}

/* Receive and write a single FILE_DATA message to disk. */
static BOOL clip_recv_file_data(AsbConn *s, const ClipHeader *hdr)
{
    ClipFileInfo fi;
    wchar_t rel_path[MAX_PATH], full_path[MAX_PATH];

    if (hdr->data_size < sizeof(fi)) return FALSE;
    if (!recv_exact(s, &fi, sizeof(fi))) return FALSE;

    if (fi.path_len == 0 || fi.path_len > (MAX_PATH - 1) * sizeof(wchar_t)) {
        UINT32 skip_hdr = hdr->data_size - (UINT32)sizeof(fi);
        BYTE skip[4096];
        while (skip_hdr > 0) {
            int chunk = skip_hdr > 4096 ? 4096 : (int)skip_hdr;
            if (!recv_exact(s, skip, chunk)) return FALSE;
            skip_hdr -= chunk;
        }
        { UINT64 rem = fi.file_size;
          while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!recv_exact(s, skip, c)) return FALSE; rem -= c; } }
        return TRUE;
    }

    if (!recv_exact(s, rel_path, (int)fi.path_len)) return FALSE;
    rel_path[fi.path_len / sizeof(wchar_t)] = L'\0';

    if (!clip_path_is_safe(rel_path)) {
        BYTE skip[4096];
        UINT64 rem = fi.file_size;
        clip_log("Rejected unsafe path.");
        while (rem > 0) { int c = rem > 4096 ? 4096 : (int)rem;
          if (!recv_exact(s, skip, c)) return FALSE; rem -= c; }
        return TRUE;
    }

    swprintf_s(full_path, MAX_PATH, L"%s\\%s", g_clip_temp_dir, rel_path);

    if (fi.is_directory) {
        clip_ensure_parent_dir(full_path);
        CreateDirectoryW(full_path, NULL);
    } else {
        HANDLE hFile;
        BYTE *chunk;
        clip_ensure_parent_dir(full_path);
        chunk = (BYTE *)HeapAlloc(GetProcessHeap(), 0, CLIP_FILE_CHUNK);
        if (!chunk) return FALSE;
        hFile = CreateFileW(full_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hFile != INVALID_HANDLE_VALUE) {
            UINT64 remaining = fi.file_size;
            while (remaining > 0) {
                DWORD to_read = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (DWORD)remaining;
                DWORD written;
                if (!recv_exact(s, chunk, (int)to_read)) { HeapFree(GetProcessHeap(), 0, chunk); CloseHandle(hFile); return FALSE; }
                WriteFile(hFile, chunk, to_read, &written, NULL);
                remaining -= to_read;
            }
            CloseHandle(hFile);
        } else {
            UINT64 remaining = fi.file_size;
            while (remaining > 0) {
                int c = remaining > CLIP_FILE_CHUNK ? CLIP_FILE_CHUNK : (int)remaining;
                if (!recv_exact(s, chunk, c)) { HeapFree(GetProcessHeap(), 0, chunk); return FALSE; }
                remaining -= c;
            }
        }
        HeapFree(GetProcessHeap(), 0, chunk);
    }
    return TRUE;
}

/* ---- Recv thread: delayed rendering protocol ---- */

static DWORD WINAPI recv_thread_proc(LPVOID param)
{
    AsbConn *s = (AsbConn *)param;
    ClipHeader hdr;

    clip_log("Recv thread started.");

    while (1) {
        if (!recv_exact(s, &hdr, sizeof(hdr)))
            break;

        if (hdr.magic != CLIP_MAGIC) {
            clip_log("Bad magic 0x%08X, disconnecting.", hdr.magic);
            break;
        }

        switch (hdr.msg_type) {

        case CLIP_MSG_SYNC_ENABLE: {
            UINT8 flag = 0;
            if (hdr.data_size >= 1) {
                if (!recv_exact(s, &flag, 1)) goto done;
                if (hdr.data_size > 1) {
                    BYTE skip[256];
                    UINT32 rem = hdr.data_size - 1;
                    while (rem > 0) {
                        int c = rem > 256 ? 256 : (int)rem;
                        if (!recv_exact(s, skip, c)) goto done;
                        rem -= c;
                    }
                }
            }
            InterlockedExchange(&g_sync_enabled, flag ? 1 : 0);
            if (flag && g_sync_event) SetEvent(g_sync_event);
            else if (!flag && g_sync_event) ResetEvent(g_sync_event);
            clip_log("SYNC_ENABLE %s", flag ? "ON" : "OFF");
            break;
        }

        case CLIP_MSG_FORMAT_LIST: {
            BYTE buf[16384];
            UINT32 count, i;
            int off;

            if (hdr.data_size > sizeof(buf)) goto done;
            if (!recv_exact(s, buf, (int)hdr.data_size)) goto done;

            if (!g_sync_enabled) {
                clip_log("FORMAT_LIST ignored (sync disabled).");
                break;
            }

            count = *(UINT32 *)buf;
            if (count > CLIP_MAX_FORMATS) count = CLIP_MAX_FORMATS;
            off = 4;

            clip_free_pending();
            g_clip_fmt_count = 0;
            InterlockedExchange(&g_large_asked, 0);   /* new PC clipboard */
            for (i = 0; i < count && off + 8 <= (int)hdr.data_size; i++) {
                UINT32 fmt_id   = *(UINT32 *)(buf + off); off += 4;
                UINT32 name_len = *(UINT32 *)(buf + off); off += 4;
                UINT local_id = fmt_id;

                /* off is in [8, data_size] here; reject a name_len that runs
                   past the buffer (or whose signed cast would corrupt off)
                   before the unconditional `off += name_len` advance below. */
                if (name_len > (UINT32)((int)hdr.data_size - off)) break;

                if (name_len > 0 && name_len < 256 && off + (int)name_len <= (int)hdr.data_size) {
                    char name[256];
                    memcpy(name, buf + off, name_len);
                    name[name_len] = '\0';
                    local_id = RegisterClipboardFormatA(name);
                    clip_log("Format '%s' remote=%u local=%u.", name, fmt_id, local_id);
                }
                off += (int)name_len;

                g_clip_fmts[g_clip_fmt_count].local_id = local_id;
                g_clip_fmts[g_clip_fmt_count].remote_id = fmt_id;
                g_clip_fmts[g_clip_fmt_count].data = NULL;
                g_clip_fmts[g_clip_fmt_count].data_size = 0;
                g_clip_fmts[g_clip_fmt_count].is_hdrop = FALSE;
                g_clip_fmt_count++;
            }
            g_clip_fetch_idx = 0;

            clip_log("Received format list (%u formats), fetching data...", g_clip_fmt_count);

            /* Clean temp dir for potential file transfers (not while a paste is still copying) */
            if (g_xfer == XFER_NONE) {
                clip_remove_dir_recursive(g_clip_temp_dir);
                CreateDirectoryW(g_clip_temp_dir, NULL);
            }

            /* Start fetching format data one at a time */
            clip_request_next_format();
            break;
        }

        case CLIP_MSG_FORMAT_DATA_RESP: {
            int idx = g_clip_fetch_idx;
            clip_log("Data response for format %u (%u bytes).", hdr.format, hdr.data_size);

            /* Fork: end marker of a paste-time file transfer. */
            if (hdr.format == CF_HDROP && hdr.data_size == 0 &&
                (g_xfer == XFER_SYNC || g_xfer == XFER_DETACHED)) {
                clip_xfer_finished(TRUE);
                break;
            }

            if (idx >= 0 && idx < g_clip_fmt_count) {
                if (hdr.format == CF_HDROP && hdr.data_size == 0) {
                    g_clip_fmts[idx].is_hdrop = TRUE;
                } else if (hdr.data_size > 0 && hdr.data_size <= CLIP_MAX_PAYLOAD) {
                    BYTE *data = (BYTE *)HeapAlloc(GetProcessHeap(), 0, hdr.data_size);
                    if (data && recv_exact(s, data, (int)hdr.data_size)) {
                        g_clip_fmts[idx].data = data;
                        g_clip_fmts[idx].data_size = hdr.data_size;
                    } else {
                        if (data) HeapFree(GetProcessHeap(), 0, data);
                    }
                }
                g_clip_fetch_idx++;
                clip_request_next_format();
            }
            break;
        }

        case CLIP_MSG_FILE_DATA:
            CreateDirectoryW(g_clip_temp_dir, NULL);
            if (!clip_recv_file_data(s, &hdr)) goto done;
            break;

        case CLIP_MSG_FILE_SIZE_RESP: {
            ClipSizeResp r = {0};
            UINT32 used = 0;
            BYTE skip[256];
            if (hdr.data_size >= sizeof(r)) {
                if (!recv_exact(s, &r, sizeof(r))) goto done;
                used = sizeof(r);
            }
            while (used < hdr.data_size) {
                int c = hdr.data_size - used > sizeof(skip) ? (int)sizeof(skip) : (int)(hdr.data_size - used);
                if (!recv_exact(s, skip, c)) goto done;
                used += c;
            }
            g_size = r;
            SetEvent(g_size_event);
            break;
        }

        default:
            clip_log("Unknown msg_type %u.", hdr.msg_type);
            if (hdr.data_size > 0) {
                BYTE skip[256];
                UINT32 remaining = hdr.data_size;
                while (remaining > 0) {
                    int chunk = remaining > 256 ? 256 : (int)remaining;
                    if (!recv_exact(s, skip, chunk)) goto done;
                    remaining -= chunk;
                }
            }
            break;
        }
    }

done:
    clip_log("Recv thread exiting.");
    return 0;
}

/* ---- Handle one client connection ---- */

static void handle_client(AsbConn *s)
{
    UINT32 ready = CLIP_READY_MAGIC;
    HANDLE recv_th;

    /* Send ready signal */
    if (asb_send(s, &ready, sizeof(ready)) != (int)sizeof(ready)) {
        clip_log("Failed to send ready signal.");
        return;
    }
    clip_log("Sent ready signal to host.");

    g_client_sock = s;

    /* Start recv thread */
    recv_th = CreateThread(NULL, 0, recv_thread_proc, (LPVOID)s, 0, NULL);
    if (!recv_th) {
        clip_log("Failed to create recv thread (%lu).", GetLastError());
        g_client_sock = NULL;
        return;
    }

    /* Wait for recv thread to finish (host disconnected) */
    WaitForSingleObject(recv_th, INFINITE);
    CloseHandle(recv_th);

    g_client_sock = NULL;
    clip_free_pending();
    g_clip_fmt_count = 0;
    if (g_xfer == XFER_SYNC || g_xfer == XFER_DETACHED) clip_xfer_finished(FALSE);
    clip_remove_dir_recursive(g_clip_temp_dir);
    clip_log("Client disconnected.");
}

/* ---- Main ---- */

int main(void)
{
    HANDLE msg_th;
    AsbListener *l;

    clip_log("Starting (PID=%lu, session=%lu).",
             GetCurrentProcessId(),
             WTSGetActiveConsoleSessionId());

    InitializeCriticalSection(&g_send_cs);
    g_clip_fetch_idx = 0;
    clip_init_temp_dir();

    g_xfer_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_size_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    g_sync_event = CreateEventW(NULL, TRUE, FALSE, CLIP_SYNC_EVENT_NAME);
    if (!g_sync_event)
        clip_log("Warning: failed to create sync event (%lu).", GetLastError());

    /* Start message window thread (clipboard listener) */
    msg_th = CreateThread(NULL, 0, msg_window_thread, NULL, 0, NULL);
    if (!msg_th) {
        clip_log("Failed to create message window thread (%lu).", GetLastError());
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }

    /* Wait a moment for message window to be created */
    Sleep(100);

    if (asb_transport_init() != 0) {
        clip_log("asb_transport_init failed.");
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }

    l = asb_listen(ASB_CH_CLIPBOARD);
    if (!l) {
        clip_log("asb_listen(ASB_CH_CLIPBOARD) failed.");
        DeleteCriticalSection(&g_send_cs);
        return 1;
    }
    clip_log("Listening on clipboard channel (transport=%s).",
             asb_transport_is_ivshmem() ? "ivshmem" : "hyperv");

    /* Accept loop — one client at a time */
    for (;;) {
        AsbConn *c = asb_accept(l, -1);
        if (!c) {
            Sleep(100);
            continue;
        }
        clip_log("Host connected.");
        handle_client(c);
        asb_close(c);
    }
}
