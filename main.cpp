// AppleMusicRPC - shows your Apple Music track (with album art) as Discord Rich Presence.
// Windows only. Build with MSVC (C++20, Windows SDK with C++/WinRT).

#include <windows.h>
#include <shellapi.h>
#include <winhttp.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <string>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "winhttp.lib")

using namespace winrt::Windows::Media::Control;

// >>> Put your Discord Application ID here (discord.com/developers/applications) <<<
static const char* CLIENT_ID = "1568644445652516864";

static const wchar_t* RUN_KEY  = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* RUN_NAME = L"AppleMusicRPC";

// ---------- helpers ----------
static std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::string JsonEscape(const std::string& in) {
    std::string out;
    for (unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) { char b[8]; sprintf_s(b, "\\u%04x", c); out += b; }
                else out += (char)c;
        }
    }
    return out;
}

// Discord limits text fields to 128 chars; also fields must be >= 2 chars.
static std::string Clamp(std::string s) {
    if (s.size() > 120) s.resize(120);
    if (s.size() < 2) s += "  ";
    return s;
}

// ---------- autostart ----------
static void SetAutostart(bool enable) {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    if (enable) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring val = L"\"" + std::wstring(path) + L"\"";
        RegSetValueExW(key, RUN_NAME, 0, REG_SZ, (const BYTE*)val.c_str(),
                       (DWORD)((val.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(key, RUN_NAME);
    }
    RegCloseKey(key);
}

// ---------- Discord IPC ----------
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static int g_nonce = 0;

static bool SendPacket(uint32_t op, const std::string& json) {
    std::string buf(8, '\0');
    uint32_t len = (uint32_t)json.size();
    memcpy(&buf[0], &op, 4);
    memcpy(&buf[4], &len, 4);
    buf += json;
    DWORD w = 0;
    return WriteFile(g_pipe, buf.data(), (DWORD)buf.size(), &w, nullptr) && w == buf.size();
}

// Reads and discards one packet (Discord replies to every command).
static bool ReadPacket() {
    uint32_t hdr[2];
    DWORD r = 0;
    if (!ReadFile(g_pipe, hdr, 8, &r, nullptr) || r != 8) return false;
    std::string body(hdr[1], '\0');
    DWORD total = 0;
    while (total < hdr[1]) {
        if (!ReadFile(g_pipe, &body[total], hdr[1] - total, &r, nullptr) || r == 0) return false;
        total += r;
    }
    return true;
}

static void Disconnect() {
    if (g_pipe != INVALID_HANDLE_VALUE) { CloseHandle(g_pipe); g_pipe = INVALID_HANDLE_VALUE; }
}

static bool Connect() {
    for (int i = 0; i < 10; ++i) {
        std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
        g_pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (g_pipe != INVALID_HANDLE_VALUE) break;
    }
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    if (!SendPacket(0, std::string("{\"v\":1,\"client_id\":\"") + CLIENT_ID + "\"}") || !ReadPacket()) {
        Disconnect();
        return false;
    }
    return true;
}

static bool SetActivity(const std::string& activityJson /* empty = clear */) {
    std::string j = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId());
    if (!activityJson.empty()) j += ",\"activity\":" + activityJson;
    j += "},\"nonce\":\"" + std::to_string(++g_nonce) + "\"}";
    if (!SendPacket(1, j) || !ReadPacket()) { Disconnect(); return false; }
    return true;
}

// ---------- album art lookup (iTunes Search API) ----------
static std::string UrlEncode(const std::string& s) {
    std::string o;
    char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else { sprintf_s(b, "%%%02X", c); o += b; }
    }
    return o;
}

static std::string HttpGet(const wchar_t* host, const std::wstring& path) {
    std::string result;
    HINTERNET s = WinHttpOpen(L"AppleMusicRPC/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) return result;
    WinHttpSetTimeouts(s, 4000, 4000, 4000, 4000);
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(r, nullptr)) {
        char buf[4096];
        DWORD n = 0;
        while (WinHttpReadData(r, buf, sizeof(buf), &n) && n) result.append(buf, n);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return result;
}

static std::string GetArtworkUrl(const std::wstring& title, const std::wstring& artist) {
    std::string term = UrlEncode(ToUtf8(title + L" " + artist));
    std::string q = "/search?media=music&entity=song&limit=1&term=" + term;
    std::string body = HttpGet(L"itunes.apple.com", std::wstring(q.begin(), q.end()));
    const std::string key = "\"artworkUrl100\":\"";
    size_t p = body.find(key);
    if (p == std::string::npos) return {};
    p += key.size();
    size_t e = body.find('"', p);
    if (e == std::string::npos) return {};
    std::string url = body.substr(p, e - p);
    size_t sz = url.find("100x100bb");
    if (sz != std::string::npos) url.replace(sz, 9, "512x512bb");
    return url;
}

// ---------- Apple Music via Windows media sessions ----------
struct Track {
    bool found = false, playing = false;
    std::wstring title, artist, album;
    int64_t posSec = 0, durSec = 0;
};

static Track GetAppleMusic(GlobalSystemMediaTransportControlsSessionManager const& mgr) {
    Track t;
    for (auto const& s : mgr.GetSessions()) {
        std::wstring id = s.SourceAppUserModelId().c_str();
        for (auto& ch : id) ch = (wchar_t)towlower(ch);
        if (id.find(L"applemusic") == std::wstring::npos && id.find(L"itunes") == std::wstring::npos) continue;

        auto props = s.TryGetMediaPropertiesAsync().get();
        t.title  = props.Title().c_str();
        t.artist = props.Artist().c_str();
        t.album  = props.AlbumTitle().c_str();
        // Apple Music often reports "Artist — Album" in the artist field.
        if (t.album.empty()) {
            auto p = t.artist.find(L" \u2014 ");
            if (p != std::wstring::npos) { t.album = t.artist.substr(p + 3); t.artist = t.artist.substr(0, p); }
        }
        t.playing = s.GetPlaybackInfo().PlaybackStatus() ==
                    GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
        auto tl = s.GetTimelineProperties();
        t.posSec = std::chrono::duration_cast<std::chrono::seconds>(tl.Position()).count();
        t.durSec = std::chrono::duration_cast<std::chrono::seconds>(tl.EndTime() - tl.StartTime()).count();
        t.found = !t.title.empty();
        break;
    }
    return t;
}

// ---------- main ----------
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && wcscmp(argv[1], L"--uninstall") == 0) {
        SetAutostart(false);
        MessageBoxW(nullptr, L"AppleMusicRPC will no longer start with Windows.", L"AppleMusicRPC", MB_OK);
        return 0;
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\AppleMusicRPC_SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;

    SetAutostart(true);
    winrt::init_apartment();

    GlobalSystemMediaTransportControlsSessionManager mgr{ nullptr };
    std::wstring lastKey, artKey;
    std::string artUrl;
    bool lastPlaying = false;
    int64_t lastStart = 0;
    bool cleared = true;

    for (;;) {
        try {
            if (!mgr) mgr = GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            Track t = GetAppleMusic(mgr);

            if (g_pipe == INVALID_HANDLE_VALUE) {
                if (!Connect()) { Sleep(10000); continue; }
                lastKey.clear(); // force resend after reconnect
            }

            if (!t.found || !t.playing) {
                if (!cleared) { if (SetActivity("")) cleared = true; }
                lastKey.clear();
            } else {
                int64_t now = (int64_t)std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                int64_t start = now - t.posSec;
                std::wstring key = t.title + L"|" + t.artist + L"|" + t.album;
                bool changed = key != lastKey || !lastPlaying || llabs(start - lastStart) > 3;

                if (changed) {
                    // Look up cover art once per track.
                    if (key != artKey) { artUrl = GetArtworkUrl(t.title, t.artist); artKey = key; }

                    std::string a = "{\"type\":2,\"details\":\"" + JsonEscape(Clamp(ToUtf8(t.title))) + "\"";
                    a += ",\"state\":\"" + JsonEscape(Clamp("by " + ToUtf8(t.artist))) + "\"";
                    a += ",\"timestamps\":{\"start\":" + std::to_string(start);
                    if (t.durSec > 0) a += ",\"end\":" + std::to_string(start + t.durSec);
                    a += "}";

                    std::string assets;
                    if (!artUrl.empty()) assets += "\"large_image\":\"" + JsonEscape(artUrl) + "\"";
                    if (!t.album.empty()) {
                        if (!assets.empty()) assets += ",";
                        assets += "\"large_text\":\"" + JsonEscape(Clamp(ToUtf8(t.album))) + "\"";
                    }
                    if (!assets.empty()) a += ",\"assets\":{" + assets + "}";
                    a += "}";

                    if (SetActivity(a)) { lastKey = key; lastStart = start; cleared = false; }
                }
            }
            lastPlaying = t.found && t.playing;
        } catch (...) {
            mgr = nullptr; // media session service hiccup; retry next loop
        }
        Sleep(3000);
    }
    CloseHandle(mutex);
    return 0;
}