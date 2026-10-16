// MiniYT - lightweight YouTube browser/player. Native Win32, C++17, no runtime deps.
// Search/playlists via YouTube's web API (WinHTTP), playback via embedded mpv.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <winhttp.h>
#include <shlwapi.h>
#include <algorithm>
using std::min; using std::max;
#include <objidl.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <deque>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>

using std::string; using std::wstring; using std::vector;

#ifdef _MSC_VER   // Visual Studio: link the libraries and turn on visual styles without any project settings
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "msimg32.lib")
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#endif

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#define MY_WINHTTP_OPT_DECOMPRESSION 118
#define MY_WINHTTP_OPT_HTTP_PROTOCOL 133
#define MY_EM_SETCUEBANNER 0x1501

// ------------------------------------------------------------------ utils
static wstring W(const string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
static string U(const wstring& w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
static int g_dpi = 96;
static int S(int v) { return MulDiv(v, g_dpi, 96); }
static string JEsc(const string& s) {
    string o; o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; snprintf(b, 8, "\\u%04x", c); o += b; }
                else o += (char)c;
        }
    }
    return o;
}
static bool StartsWith(const string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
static bool Contains(const string& s, const char* p) { return s.find(p) != string::npos; }
static string FmtDur(double sec) {
    if (sec <= 0) return "LIVE";
    int s = (int)sec, h = s / 3600, m = (s % 3600) / 60, r = s % 60;
    char b[32];
    if (h) snprintf(b, 32, "%d:%02d:%02d", h, m, r); else snprintf(b, 32, "%d:%02d", m, r);
    return b;
}

static wstring g_data, g_mpvDir, g_exeDir;
static wstring Join(const wstring& a, const wstring& b) { return a + L"\\" + b; }
static bool FileExists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
static wstring FindTool(const wchar_t* name) {
    wstring n = wstring(name) + L".exe";
    for (const wstring* d : {&g_exeDir, &g_data, &g_mpvDir}) {
        wstring p = Join(*d, n);
        if (FileExists(p)) return p;
    }
    wchar_t buf[MAX_PATH];
    if (SearchPathW(nullptr, n.c_str(), nullptr, MAX_PATH, buf, nullptr)) return buf;
    return L"";
}

// ------------------------------------------------------------------- JSON
struct J {
    enum T : char { Null, Bool, Num, Str, Arr, Obj };
    T t = Null; bool b = false; double n = 0; string s;
    vector<J> a; vector<std::pair<string, J>> o;
    static const J& Nul() { static J j; return j; }
    const J& operator[](const char* k) const {
        if (t == Obj) for (auto& p : o) if (p.first == k) return p.second;
        return Nul();
    }
    const J& idx(size_t i) const { return (t == Arr && i < a.size()) ? a[i] : Nul(); }
    size_t size() const { return t == Arr ? a.size() : t == Obj ? o.size() : 0; }
};
struct JP {
    const char *p, *e;
    explicit JP(const string& s) : p(s.c_str()), e(s.c_str() + s.size()) {}
    void ws() { while (p < e && (unsigned char)*p <= ' ') ++p; }
    static void put(string& o, unsigned cp) {
        if (cp < 0x80) o += (char)cp;
        else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 63)); }
        else if (cp < 0x10000) { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
        else { o += (char)(0xF0 | (cp >> 18)); o += (char)(0x80 | ((cp >> 12) & 63)); o += (char)(0x80 | ((cp >> 6) & 63)); o += (char)(0x80 | (cp & 63)); }
    }
    unsigned hex4() {
        unsigned v = 0;
        for (int i = 0; i < 4 && p < e; i++) {
            char c = *p++; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        }
        return v;
    }
    bool str(string& o) {
        ++p;
        while (p < e) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') { o += c; continue; }
            if (p >= e) return false;
            c = *p++;
            switch (c) {
                case 'n': o += '\n'; break; case 't': o += '\t'; break; case 'r': o += '\r'; break;
                case 'b': o += '\b'; break; case 'f': o += '\f'; break;
                case 'u': {
                    unsigned cp = hex4();
                    if (cp >= 0xD800 && cp < 0xDC00 && p + 1 < e && p[0] == '\\' && p[1] == 'u') {
                        p += 2; unsigned lo = hex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    put(o, cp); break;
                }
                default: o += c;
            }
        }
        return false;
    }
    bool val(J& j, int d = 0) {
        if (d > 200) return false;
        ws(); if (p >= e) return false;
        char c = *p;
        if (c == '{') {
            j.t = J::Obj; ++p; ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                ws(); if (p >= e || *p != '"') return false;
                string k; if (!str(k)) return false;
                ws(); if (p >= e || *p != ':') return false; ++p;
                J v; if (!val(v, d + 1)) return false;
                j.o.emplace_back(std::move(k), std::move(v));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return false;
            }
        }
        if (c == '[') {
            j.t = J::Arr; ++p; ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                J v; if (!val(v, d + 1)) return false;
                j.a.push_back(std::move(v));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return false;
            }
        }
        if (c == '"') { j.t = J::Str; return str(j.s); }
        if (e - p >= 4 && !strncmp(p, "true", 4)) { j.t = J::Bool; j.b = true; p += 4; return true; }
        if (e - p >= 5 && !strncmp(p, "false", 5)) { j.t = J::Bool; j.b = false; p += 5; return true; }
        if (e - p >= 4 && !strncmp(p, "null", 4)) { p += 4; return true; }
        char* end = nullptr; j.n = strtod(p, &end);
        if (end == p) return false;
        j.t = J::Num; p = end; return true;
    }
};
static bool ParseJson(const string& s, J& out) { JP p(s); return p.val(out); }

static string TextOf(const J& n) {
    if (n.t == J::Str) return n.s;
    if (n.t != J::Obj) return "";
    if (n["simpleText"].t == J::Str) return n["simpleText"].s;
    if (n["content"].t == J::Str) return n["content"].s;
    const J& r = n["runs"]; string out;
    if (r.t == J::Arr) for (auto& x : r.a) out += x["text"].s;
    return out;
}
static const J* FindKey(const J& n, const char* key, int depth = 0) {
    if (depth > 14) return nullptr;
    if (n.t == J::Obj) {
        for (auto& p : n.o) if (p.first == key) return &p.second;
        for (auto& p : n.o) if (auto* r = FindKey(p.second, key, depth + 1)) return r;
    } else if (n.t == J::Arr) {
        for (auto& x : n.a) if (auto* r = FindKey(x, key, depth + 1)) return r;
    }
    return nullptr;
}
static string LastUrl(const J& n) {
    const J* a = &n;
    if (n.t == J::Obj && n["thumbnails"].t == J::Arr) a = &n["thumbnails"];
    if (a->t == J::Arr && a->size()) {
        const J& f = a->a[0];
        if (f.t == J::Obj && f["thumbnails"].t == J::Arr) a = &f["thumbnails"];
    }
    string u;
    if (a->t == J::Arr) for (auto& x : a->a) if (x["url"].t == J::Str) u = x["url"].s;
    if (u.compare(0, 2, "//") == 0) u = "https:" + u;
    return u;
}

// ------------------------------------------------------------------- HTTP
static HINTERNET g_ses = nullptr;
static std::mutex g_connMu;
static std::map<wstring, HINTERNET> g_conn;
struct HttpResp { DWORD status = 0; string body; wstring finalUrl; };
typedef std::function<bool(const char*, size_t, unsigned long long)> Sink;

static void HttpInit() {
    const wchar_t* ua = L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    g_ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!g_ses) g_ses = WinHttpOpen(ua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!g_ses) return;
    DWORD pr = 0x1;   // allow HTTP/2: one connection carries many thumbnail requests
    WinHttpSetOption(g_ses, MY_WINHTTP_OPT_HTTP_PROTOCOL, &pr, sizeof pr);
    WinHttpSetTimeouts(g_ses, 8000, 8000, 15000, 30000);
}
static bool HttpDo(const wstring& url, const wchar_t* method, const wstring& hdrs, const string& body,
                   HttpResp& r, const Sink& sink = nullptr) {
    if (!g_ses) return false;
    URL_COMPONENTS uc; memset(&uc, 0, sizeof uc); uc.dwStructSize = sizeof uc;
    wchar_t host[256], path[2048], extra[2048];
    uc.lpszHostName = host; uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
    uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return false;
    wstring full = wstring(path, uc.dwUrlPathLength) + wstring(extra, uc.dwExtraInfoLength);
    wstring hostS(host, uc.dwHostNameLength);
    HINTERNET con;
    {
        std::lock_guard<std::mutex> lk(g_connMu);
        wstring key = hostS + L":" + std::to_wstring(uc.nPort);
        auto it = g_conn.find(key);
        if (it == g_conn.end()) {
            con = WinHttpConnect(g_ses, hostS.c_str(), uc.nPort, 0);
            if (!con) return false;
            g_conn[key] = con;
        } else con = it->second;
    }
    bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
    HINTERNET rq = WinHttpOpenRequest(con, method, full.c_str(), nullptr, WINHTTP_NO_REFERER,
                                      WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!rq) return false;
    DWORD dec = 3; WinHttpSetOption(rq, MY_WINHTTP_OPT_DECOMPRESSION, &dec, sizeof dec);
    if (!hdrs.empty())
        WinHttpAddRequestHeaders(rq, hdrs.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
    bool ok = WinHttpSendRequest(rq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                 body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                 (DWORD)body.size(), (DWORD)body.size(), 0) &&
              WinHttpReceiveResponse(rq, nullptr);
    if (ok) {
        DWORD st = 0, sz = sizeof st;
        WinHttpQueryHeaders(rq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX);
        r.status = st;
        wchar_t fu[4096]; DWORD fl = sizeof fu;
        if (WinHttpQueryOption(rq, WINHTTP_OPTION_URL, fu, &fl)) r.finalUrl = fu;
        unsigned long long total = 0; wchar_t cl[32]; DWORD cls = sizeof cl;
        if (WinHttpQueryHeaders(rq, WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX, cl, &cls, WINHTTP_NO_HEADER_INDEX))
            total = _wcstoui64(cl, nullptr, 10);
        char buf[65536]; DWORD got;
        while (WinHttpReadData(rq, buf, sizeof buf, &got) && got) {
            if (sink) { if (!sink(buf, got, total)) { ok = false; break; } }
            else r.body.append(buf, got);
        }
    }
    WinHttpCloseHandle(rq);
    return ok;
}
static bool HttpGetStr(const wstring& url, string& out, wstring* fin = nullptr) {
    HttpResp r; bool ok = HttpDo(url, L"GET", L"", "", r);
    out = std::move(r.body); if (fin) *fin = r.finalUrl;
    return ok && r.status >= 200 && r.status < 300;
}
static bool DownloadFile(const wstring& url, const wstring& path, const std::function<void(int)>& prog = nullptr) {
    wstring tmp = path + L".part";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    unsigned long long done = 0; int last = -1;
    HttpResp r;
    bool ok = HttpDo(url, L"GET", L"", "", r, [&](const char* d, size_t n, unsigned long long total) {
        DWORD w; WriteFile(f, d, (DWORD)n, &w, nullptr); done += n;
        if (prog && total) { int p = (int)(done * 100 / total); if (p != last) { last = p; prog(p); } }
        return true;
    });
    CloseHandle(f);
    if (!ok || r.status < 200 || r.status >= 300) { DeleteFileW(tmp.c_str()); return false; }
    return MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}
// run a hidden process, capture stdout (optional), wait
static bool RunHidden(wstring cmd, string* out = nullptr, DWORD timeoutMs = 120000, const wchar_t* cwd = nullptr) {
    SECURITY_ATTRIBUTES sa = {sizeof sa, nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    CreatePipe(&rd, &wr, &sa, 0);
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr; si.hStdError = wr; si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    BOOL ok = CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return false; }
    std::thread reader([&] {
        char b[8192]; DWORD n;
        while (ReadFile(rd, b, sizeof b, &n, nullptr) && n) if (out) out->append(b, n);
    });
    DWORD w = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (w == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    reader.join();
    CloseHandle(rd);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return code == 0;
}

// ------------------------------------------------------- YouTube data model
struct Item { int kind = 0; /*0 video, 1 playlist*/ string id, title, channel, dur, count, thumb, channelId; };
static string ThumbUrl(const string& vid) { return "https://i.ytimg.com/vi/" + vid + "/mqdefault.jpg"; }

static void ParseVideoRenderer(const J& r, vector<Item>& out) {
    Item it; it.kind = 0; it.id = r["videoId"].s;
    if (it.id.size() != 11) return;
    if (r["isPlayable"].t == J::Bool && !r["isPlayable"].b) return;
    it.title = TextOf(r["title"]);
    if (it.title.empty() || it.title == "[Private video]" || it.title == "[Deleted video]") return;
    it.channel = TextOf(r["ownerText"]);
    if (it.channel.empty()) it.channel = TextOf(r["longBylineText"]);
    if (it.channel.empty()) it.channel = TextOf(r["shortBylineText"]);
    it.dur = TextOf(r["lengthText"]);
    if (it.dur.empty() && r["lengthSeconds"].t == J::Str) it.dur = FmtDur(atof(r["lengthSeconds"].s.c_str()));
    if (it.dur.empty()) it.dur = "LIVE";
    const J& runs = r["ownerText"]["runs"];
    it.channelId = runs.idx(0)["navigationEndpoint"]["browseEndpoint"]["browseId"].s;
    if (it.channelId.empty())
        it.channelId = r["shortBylineText"]["runs"].idx(0)["navigationEndpoint"]["browseEndpoint"]["browseId"].s;
    it.thumb = ThumbUrl(it.id);
    out.push_back(std::move(it));
}
static void ParsePlaylistRenderer(const J& r, vector<Item>& out) {
    Item it; it.kind = 1; it.id = r["playlistId"].s;
    if (it.id.size() < 10 || StartsWith(it.id, "RD") || StartsWith(it.id, "UC")) return;
    it.title = TextOf(r["title"]);
    if (it.title.empty()) return;
    it.channel = TextOf(r["longBylineText"]);
    if (it.channel.empty()) it.channel = TextOf(r["shortBylineText"]);
    it.count = r["videoCount"].t == J::Str ? r["videoCount"].s : TextOf(r["videoCountText"]);
    if (it.count.empty()) it.count = TextOf(r["videoCountShortText"]);
    it.thumb = LastUrl(r["thumbnails"].t != J::Null ? r["thumbnails"] : r["thumbnail"]);
    out.push_back(std::move(it));
}
static void ParseLockup(const J& r, vector<Item>& out, int want) {
    string type = r["contentType"].s, id = r["contentId"].s;
    Item it; it.id = id;
    bool isPl = Contains(type, "PLAYLIST"), isVid = Contains(type, "VIDEO");
    if (isPl) { if (want == 0 || id.size() < 10 || StartsWith(id, "RD") || StartsWith(id, "UC")) return; it.kind = 1; }
    else if (isVid) { if (want == 1 || id.size() != 11) return; it.kind = 0; }
    else return;
    const J& m = r["metadata"]["lockupMetadataViewModel"];
    it.title = TextOf(m["title"]);
    if (it.title.empty()) return;
    const J& rows = m["metadata"]["contentMetadataViewModel"]["metadataRows"];
    bool got = false;
    if (rows.t == J::Arr)
        for (auto& row : rows.a) {
            const J& parts = row["metadataParts"];
            if (parts.t == J::Arr)
                for (auto& p : parts.a) {
                    string t = TextOf(p["text"]);
                    if (!t.empty()) { it.channel = t; got = true; break; }
                }
            if (got) break;
        }
    const J* bd = FindKey(r["contentImage"], "thumbnailBadgeViewModel");
    if (bd) { string t = TextOf((*bd)["text"]); if (isPl) it.count = t; else it.dur = t; }
    if (isPl) { const J* src = FindKey(r["contentImage"], "sources"); if (src) it.thumb = LastUrl(*src); }
    else it.thumb = ThumbUrl(it.id);
    if (it.kind == 0 && it.dur.empty()) it.dur = "LIVE";
    out.push_back(std::move(it));
}
// want: 0 videos only, 1 playlists only, -1 both
static void Collect(const J& n, vector<Item>& out, int want, int depth = 0) {
    if (depth > 60) return;
    if (n.t == J::Obj) {
        for (auto& p : n.o) {
            const string& k = p.first;
            if (k == "videoRenderer" || k == "playlistVideoRenderer" || k == "compactVideoRenderer") { if (want != 1) ParseVideoRenderer(p.second, out); }
            else if (k == "playlistRenderer" || k == "gridPlaylistRenderer") { if (want != 0) ParsePlaylistRenderer(p.second, out); }
            else if (k == "lockupViewModel") ParseLockup(p.second, out, want);
            else if (k == "adSlotRenderer" || k == "promotedSparklesWebRenderer") continue;
            else Collect(p.second, out, want, depth + 1);
        }
    } else if (n.t == J::Arr) for (auto& x : n.a) Collect(x, out, want, depth + 1);
}
static void Dedup(vector<Item>& v) {
    std::unordered_set<string> seen; vector<Item> o;
    for (auto& i : v) if (seen.insert(std::to_string(i.kind) + i.id).second) o.push_back(std::move(i));
    v = std::move(o);
}
static string ContToken(const J& root) {
    const J* c = FindKey(root, "continuationItemRenderer");
    if (!c) return "";
    return (*c)["continuationEndpoint"]["continuationCommand"]["token"].s;
}

// ---------------------------------------------------------- InnerTube calls
static const char* kWebVer = "2.20260915.01.00";
static string g_gl = "US", g_country = "";   // the user's own region (from Windows), so Home / search use local content
static void DetectRegion() {
    wchar_t b[16] = {0}, nm[128] = {0};
    GEOID id = GetUserGeoID(GEOCLASS_NATION);
    if (id && GetGeoInfoW(id, GEO_ISO2, b, 16, 0) >= 3 && b[0] && b[1]) {
        g_gl = string(1, (char)toupper(b[0])) + (char)toupper(b[1]);
        if (GetGeoInfoW(id, GEO_FRIENDLYNAME, nm, 128, 0) > 0) g_country = U(nm);
    } else if (GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SISO3166CTRYNAME, b, 16) >= 3) {
        g_gl = string(1, (char)toupper(b[0])) + (char)toupper(b[1]);
    }
}
static string WebCtx() {
    return string("\"context\":{\"client\":{\"clientName\":\"WEB\",\"clientVersion\":\"") + kWebVer +
           "\",\"hl\":\"en\",\"gl\":\"" + g_gl + "\"}}";
}
static bool YtPost(const string& path, const string& body, J& root, const wstring& hdr = L"") {
    wstring h = hdr.empty() ? (L"Content-Type: application/json\r\nOrigin: https://www.youtube.com\r\n"
                               L"X-Youtube-Client-Name: 1\r\nX-Youtube-Client-Version: " + W(kWebVer) + L"\r\n") : hdr;
    HttpResp r;
    if (!HttpDo(L"https://www.youtube.com" + W(path), L"POST", h, body, r) || r.status != 200) return false;
    return ParseJson(r.body, root);
}
static bool InnerSearch(const string& q, int kind, vector<Item>& out) {
    string body = "{" + WebCtx() + ",\"query\":\"" + JEsc(q) + "\"" +
                  (kind == 2 ? "" : string(",\"params\":\"") + (kind == 1 ? "EgIQAw%3D%3D" : "EgIQAQ%3D%3D") + "\"") + "}";
    J root;
    if (!YtPost("/youtubei/v1/search?prettyPrint=false", body, root)) return false;
    Collect(root, out, kind == 2 ? -1 : kind);
    Dedup(out);
    return !out.empty();
}
struct Video { string id, title, channel, dur; };
struct Playlist { string id, title, channel; bool local = false; vector<Video> videos; };

static bool InnerPlaylist(const string& pid, Playlist& pl) {
    pl.id = pid; pl.videos.clear();
    string token; bool first = true;
    for (int page = 0; page < 60; page++) {
        string body = first ? "{" + WebCtx() + ",\"browseId\":\"VL" + JEsc(pid) + "\"}"
                            : "{" + WebCtx() + ",\"continuation\":\"" + JEsc(token) + "\"}";
        J root;
        if (!YtPost("/youtubei/v1/browse?prettyPrint=false", body, root)) break;
        if (first) {
            pl.title = root["metadata"]["playlistMetadataRenderer"]["title"].s;
            const J* h = FindKey(root, "playlistHeaderRenderer");
            if (h) pl.channel = TextOf((*h)["ownerText"]);
            if (pl.title.empty()) { const J* ph = FindKey(root, "pageHeaderRenderer"); if (ph) pl.title = (*ph)["pageTitle"].s; }
        }
        vector<Item> items; Collect(root, items, 0);
        for (auto& it : items) pl.videos.push_back({it.id, it.title, it.channel, it.dur});
        token = ContToken(root); first = false;
        if (token.empty()) break;
    }
    if (pl.channel.empty() && !pl.videos.empty()) pl.channel = pl.videos[0].channel;
    return !pl.videos.empty();
}
static bool InnerChannelPlaylists(const string& chanId, vector<Item>& out) {
    string token; bool first = true;
    for (int page = 0; page < 4; page++) {
        string body = first ? "{" + WebCtx() + ",\"browseId\":\"" + JEsc(chanId) + "\",\"params\":\"EglwbGF5bGlzdHM%3D\"}"
                            : "{" + WebCtx() + ",\"continuation\":\"" + JEsc(token) + "\"}";
        J root;
        if (!YtPost("/youtubei/v1/browse?prettyPrint=false", body, root)) break;
        Collect(root, out, 1);
        token = ContToken(root); first = false;
        if (token.empty()) break;
    }
    Dedup(out);
    return !out.empty();
}
static bool InnerHome(vector<Item>& out) {   // trending / home feed; several sources, first one that gives videos wins
    const char* ids[] = {"FEtrending", "FEwhat_to_watch"};
    for (const char* id : ids) {
        J root; out.clear();
        if (YtPost("/youtubei/v1/browse?prettyPrint=false", "{" + WebCtx() + ",\"browseId\":\"" + id + "\"}", root)) { Collect(root, out, 0); Dedup(out); }
        if (out.size() >= 5) return true;
    }
    out.clear();
    if (InnerSearch("trending now" + (g_country.empty() ? string() : " in " + g_country), 0, out) && !out.empty()) return true;   // last resort: a search
    return false;
}
static bool InnerRelated(const string& vid, vector<Item>& out) {
    string body = "{" + WebCtx() + ",\"videoId\":\"" + JEsc(vid) + "\"}";
    J root;
    if (!YtPost("/youtubei/v1/next?prettyPrint=false", body, root)) return false;
    const J* sec = FindKey(root, "secondaryResults");
    Collect(sec ? *sec : root, out, 0);
    Dedup(out);
    return !out.empty();
}
static string InnerResolveChannel(const string& url) {
    string body = "{" + WebCtx() + ",\"url\":\"" + JEsc(url) + "\"}";
    J root;
    if (!YtPost("/youtubei/v1/navigation/resolve_url?prettyPrint=false", body, root)) return "";
    const J* be = FindKey(root, "browseEndpoint");
    return be ? (*be)["browseId"].s : "";
}

// ---- stream resolving (direct, avoids starting yt-dlp: the main speed-up)
struct Fmt { string url, mime; int itag = 0, w = 0, h = 0; double br = 0; bool audio = false, avc = false, muxed = false; };
static const char* kVrUA = "com.google.android.apps.youtube.vr.oculus/1.60.19 (Linux; U; Android 12L; eureka-user Build/SQ3A.220605.009.A1) gzip";
static bool ResolveFormats(const string& vid, vector<Fmt>& out, string& err) {
    string body = string("{\"context\":{\"client\":{\"clientName\":\"ANDROID_VR\",\"clientVersion\":\"1.60.19\","
                         "\"deviceMake\":\"Oculus\",\"deviceModel\":\"Quest 3\",\"osName\":\"Android\",\"osVersion\":\"12L\","
                         "\"androidSdkVersion\":32,\"hl\":\"en\",\"gl\":\"US\"}},\"videoId\":\"") + JEsc(vid) +
                  "\",\"contentCheckOk\":true,\"racyCheckOk\":true}";
    wstring h = wstring(L"Content-Type: application/json\r\nUser-Agent: ") + W(kVrUA) +
                L"\r\nX-YouTube-Client-Name: 28\r\nX-YouTube-Client-Version: 1.60.19\r\n";
    J root;
    if (!YtPost("/youtubei/v1/player?prettyPrint=false", body, root, h)) { err = "request failed"; return false; }
    string st = root["playabilityStatus"]["status"].s;
    if (st != "OK") { err = st + " " + root["playabilityStatus"]["reason"].s; return false; }
    const J& sd = root["streamingData"];
    for (int pass = 0; pass < 2; pass++) {
        const J& arr = sd[pass == 0 ? "adaptiveFormats" : "formats"];
        if (arr.t != J::Arr) continue;
        for (auto& f : arr.a) {
            Fmt x; x.url = f["url"].s;
            if (x.url.empty()) continue;
            x.mime = f["mimeType"].s; x.itag = (int)f["itag"].n; x.w = (int)f["width"].n; x.h = (int)f["height"].n;
            x.br = f["bitrate"].n; x.audio = StartsWith(x.mime, "audio/"); x.avc = Contains(x.mime, "avc1");
            x.muxed = (pass == 1);
            out.push_back(std::move(x));
        }
    }
    if (out.empty()) { err = "no direct streams"; return false; }
    return true;
}
struct Pick { string video, audio; bool ok = false; int h = 0; };
static Pick PickStreams(const vector<Fmt>& f, int maxH) {   // maxH 0 = audio only
    Pick p; const Fmt* bestA = nullptr;
    for (auto& x : f) if (x.audio) {
        bool m4a = Contains(x.mime, "mp4"), bm = bestA && Contains(bestA->mime, "mp4");
        if (!bestA || (m4a && !bm) || (m4a == bm && x.br > bestA->br)) bestA = &x;
    }
    if (maxH == 0) { if (bestA) { p.video = bestA->url; p.ok = true; } return p; }
    const Fmt *bAvc = nullptr, *bAny = nullptr;
    for (auto& x : f) if (!x.audio && !x.muxed && x.h > 0 && x.h <= maxH) {
        if (!bAny || x.h > bAny->h || (x.h == bAny->h && x.br > bAny->br)) bAny = &x;
        if (x.avc && (!bAvc || x.h > bAvc->h || (x.h == bAvc->h && x.br > bAvc->br))) bAvc = &x;
    }
    const Fmt* v = bAny;                       // prefer H.264: old laptops decode it in hardware
    if (bAvc && (!bAny || bAvc->h * 10 >= bAny->h * 7)) v = bAvc;
    if (v && bestA) { p.video = v->url; p.audio = bestA->url; p.h = v->h; p.ok = true; return p; }
    const Fmt* bm = nullptr;
    for (auto& x : f) if (x.muxed && x.h <= maxH && (!bm || x.h > bm->h)) bm = &x;
    if (!bm) for (auto& x : f) if (x.muxed && (!bm || x.h < bm->h)) bm = &x;
    if (bm) { p.video = bm->url; p.h = bm->h; p.ok = true; }
    return p;
}
static const int kHeights[5] = {360, 480, 720, 1080, 0};
static string YtdlFormat(int q) {
    int h = kHeights[q];
    if (h == 0) return "ba/b";
    char b[256];
    snprintf(b, sizeof b, "bv*[height<=%d][vcodec^=avc1]+ba[ext=m4a]/bv*[height<=%d]+ba/b[height<=%d]/b", h, h, h);
    return b;
}

// ---- yt-dlp fallback (only used if the direct calls above stop working)
static bool YtDlpJson(const vector<string>& args, vector<J>& rows) {
    wstring exe = FindTool(L"yt-dlp");
    if (exe.empty()) return false;
    wstring cmd = L"\"" + exe + L"\" --flat-playlist --dump-json --no-warnings";
    for (auto& a : args) cmd += L" \"" + W(a) + L"\"";
    string out; RunHidden(cmd, &out, 120000);
    size_t pos = 0;
    while (pos < out.size()) {
        size_t e = out.find('\n', pos); if (e == string::npos) e = out.size();
        J j; if (e > pos && ParseJson(out.substr(pos, e - pos), j) && j.t == J::Obj) rows.push_back(std::move(j));
        pos = e + 1;
    }
    return !rows.empty();
}
static void RowsToItems(const vector<J>& rows, int kind, vector<Item>& out) {
    for (auto& d : rows) {
        Item it; it.kind = kind;
        it.id = d["id"].s; it.title = d["title"].s;
        it.channel = d["channel"].t == J::Str ? d["channel"].s : (d["uploader"].t == J::Str ? d["uploader"].s : d["playlist_uploader"].s);
        if (it.title.empty() || it.title == "[Private video]" || it.title == "[Deleted video]") continue;
        if (kind == 0) {
            if (it.id.size() != 11) continue;
            it.dur = FmtDur(d["duration"].n); it.thumb = ThumbUrl(it.id);
            it.channelId = d["channel_id"].s;
        } else {
            string url = d["url"].s; size_t p = url.find("list=");
            if (p != string::npos) it.id = url.substr(p + 5);
            if (it.id.size() < 10 || StartsWith(it.id, "UC") || url.find("/@") != string::npos) continue;
            if (d["playlist_count"].t == J::Num) it.count = std::to_string((int)d["playlist_count"].n) + " videos";
            const J& th = d["thumbnails"];
            if (th.t == J::Arr && th.size()) it.thumb = th.a.back()["url"].s;
        }
        out.push_back(std::move(it));
    }
}

// ------------------------------------------------------------ app messages
#define WM_RUN      (WM_APP + 1)
#define WM_THUMB    (WM_APP + 3)
#define WM_LISTEV   (WM_APP + 10)
static HWND g_main = nullptr;
static HINSTANCE g_hi = nullptr;
static HFONT g_f = nullptr, g_fb = nullptr;
static void RunOnUI(std::function<void()> f) {
    if (!g_main) return;
    auto* p = new std::function<void()>(std::move(f));
    if (!PostMessageW(g_main, WM_RUN, 0, (LPARAM)p)) delete p;
}

// ------------------------------------------------------------------- state
// Same file/schema as the earlier Python version, so your watched ticks carry over.
static string g_lastReal = "local", g_relVid;
static bool IsTemp(const string& id) { return id == "related" || id == "history"; }   // lists that are never saved
struct Hist { string id, title, channel, dur; long long t = 0; };
static string FmtDate(long long t) {
    time_t tt = (time_t)t; struct tm* lt = localtime(&tt); if (!lt) return "";
    char b[32]; strftime(b, sizeof b, "%Y-%m-%d %H:%M", lt); return b;
}
struct State {
    std::map<string, Playlist> pls;
    vector<string> recent;
    string current = "local";
    std::unordered_set<string> watched;
    int quality = 2;     // index into kHeights (default 720p)
    int leftW = 0;       // 0 = default
    int volume = 100;
    vector<Hist> hist;   // recently watched, newest first, max 100
} g;
static const int kMaxLists = 30, kMaxHist = 100;
static string HistJson() {
    string o = "["; bool f = true;
    for (auto& h : g.hist) {
        if (!f) o += ','; f = false;
        o += "{\"id\":\"" + JEsc(h.id) + "\",\"title\":\"" + JEsc(h.title) + "\",\"channel\":\"" + JEsc(h.channel) +
             "\",\"dur\":\"" + JEsc(h.dur) + "\",\"t\":" + std::to_string(h.t) + "}";
    }
    return o + "]";
}

static wstring StatePath() { return Join(g_data, L"state.json"); }
static bool ReadFileStr(const wstring& p, string& out) {
    HANDLE f = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    char b[65536]; DWORD n;
    while (ReadFile(f, b, sizeof b, &n, nullptr) && n) out.append(b, n);
    CloseHandle(f); return true;
}
static void LoadState() {
    string s; J root;
    if (ReadFileStr(StatePath(), s) && ParseJson(s, root) && root.t == J::Obj) {
        for (auto& p : root["watched"].o) g.watched.insert(p.first);
        g.current = root["current"].s;
        for (auto& x : root["recent"].a) if (x.t == J::Str) g.recent.push_back(x.s);
        for (auto& p : root["playlists"].o) {
            Playlist pl; pl.id = p.first;
            pl.title = p.second["title"].s; pl.channel = p.second["channel"].s;
            pl.local = p.second["local"].t == J::Bool && p.second["local"].b;
            for (auto& v : p.second["videos"].a)
                pl.videos.push_back({v["id"].s, v["title"].s, v["channel"].s, v["dur"].s});
            g.pls[pl.id] = std::move(pl);
        }
        for (auto& x : root["history"].a)
            if (x["id"].t == J::Str && (int)g.hist.size() < kMaxHist) {
                Hist h; h.id = x["id"].s; h.title = x["title"].s; h.channel = x["channel"].s; h.dur = x["dur"].s; h.t = (long long)x["t"].n;
                g.hist.push_back(h);
            }
        const J& st = root["settings"];
        if (st["quality"].t == J::Num) g.quality = std::max(0, std::min(4, (int)st["quality"].n));
        if (st["left"].t == J::Num) g.leftW = (int)st["left"].n;
        if (st["volume"].t == J::Num) g.volume = std::max(0, std::min(100, (int)st["volume"].n));
    }
    if (!g.pls.count("local")) { Playlist l; l.id = "local"; l.title = "My playlist"; l.local = true; g.pls["local"] = l; }
    if (!g.pls.count(g.current)) g.current = "local";
}
static void SaveState() {
    string o = "{\"watched\":{"; bool f = true;
    for (auto& id : g.watched) { if (!f) o += ','; f = false; o += "\"" + JEsc(id) + "\":1"; }
    o += "},\"current\":\"" + JEsc(IsTemp(g.current) ? g_lastReal : g.current) + "\",\"recent\":[";
    f = true; for (auto& r : g.recent) { if (!f) o += ','; f = false; o += "\"" + JEsc(r) + "\""; }
    o += "],\"settings\":{\"quality\":" + std::to_string(g.quality) + ",\"left\":" + std::to_string(g.leftW) + ",\"volume\":" + std::to_string(g.volume) + "},\"history\":" + HistJson() + ",\"playlists\":{";
    f = true;
    for (auto& kv : g.pls) {
        const Playlist& p = kv.second;
        if (IsTemp(p.id)) continue;
        if (!f) o += ','; f = false;
        o += "\"" + JEsc(p.id) + "\":{\"title\":\"" + JEsc(p.title) + "\",\"channel\":\"" + JEsc(p.channel) +
             "\",\"local\":" + (p.local ? "true" : "false") + ",\"videos\":[";
        bool ff = true;
        for (auto& v : p.videos) {
            if (!ff) o += ','; ff = false;
            o += "{\"id\":\"" + JEsc(v.id) + "\",\"title\":\"" + JEsc(v.title) + "\",\"channel\":\"" + JEsc(v.channel) +
                 "\",\"dur\":\"" + JEsc(v.dur) + "\"}";
        }
        o += "]}";
    }
    o += "}}";
    wstring tmp = StatePath() + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w; BOOL ok = WriteFile(h, o.data(), (DWORD)o.size(), &w, nullptr);
    CloseHandle(h);
    if (ok) MoveFileExW(tmp.c_str(), StatePath().c_str(), MOVEFILE_REPLACE_EXISTING);
}
static void ScheduleSave() { if (g_main) SetTimer(g_main, 1, 600, nullptr); }
static Playlist& Cur() { return g.pls[g.current]; }
static bool Watched(const string& id) { return g.watched.count(id) > 0; }

// --------------------------------------------------------------- thumbnails
static std::mutex g_tmu;
static std::unordered_map<string, HBITMAP> g_tbm;
static std::unordered_set<string> g_tpend, g_tfail;
static std::deque<string> g_tq;
static std::condition_variable g_tcv;
static std::atomic<bool> g_thumbDirty{false};

static HBITMAP DecodeThumb(const string& bytes, int tw, int th) {
    IStream* st = SHCreateMemStream((const BYTE*)bytes.data(), (UINT)bytes.size());
    if (!st) return nullptr;
    HBITMAP hb = nullptr;
    {
        Gdiplus::Bitmap src(st);
        if (src.GetLastStatus() == Gdiplus::Ok) {
            Gdiplus::Bitmap dst(tw, th, PixelFormat32bppRGB);
            Gdiplus::Graphics gr(&dst);
            gr.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
            gr.DrawImage(&src, 0, 0, tw, th);
            dst.GetHBITMAP(Gdiplus::Color(0, 0, 0), &hb);
        }
    }
    st->Release();
    return hb;
}
static HBITMAP GetThumb(const string& url) {
    if (url.empty()) return nullptr;
    std::lock_guard<std::mutex> lk(g_tmu);
    auto it = g_tbm.find(url);
    if (it != g_tbm.end()) return it->second;
    if (StartsWith(url, "demo:")) {      // offline preview colours (used by /demo)
        unsigned h = 2166136261u; for (char c : url) h = (h ^ (unsigned char)c) * 16777619u;
        HDC sd = GetDC(nullptr); HDC md = CreateCompatibleDC(sd);
        HBITMAP bm = CreateCompatibleBitmap(sd, 160, 90); HGDIOBJ o = SelectObject(md, bm);
        HBRUSH b = CreateSolidBrush(RGB(60 + (h & 127), 60 + ((h >> 8) & 127), 60 + ((h >> 16) & 127)));
        RECT r = {0, 0, 160, 90}; FillRect(md, &r, b); DeleteObject(b);
        SelectObject(md, o); DeleteDC(md); ReleaseDC(nullptr, sd);
        g_tbm[url] = bm; return bm;
    }
    if (g_tpend.count(url) || g_tfail.count(url)) return nullptr;
    g_tpend.insert(url); g_tq.push_back(url); g_tcv.notify_one();
    return nullptr;
}
static void ThumbWorker() {
    for (;;) {
        string url;
        { std::unique_lock<std::mutex> lk(g_tmu); g_tcv.wait(lk, [] { return !g_tq.empty(); });
          url = g_tq.front(); g_tq.pop_front(); }
        string body; HBITMAP hb = nullptr;
        if (HttpGetStr(W(url), body)) hb = DecodeThumb(body, 160, 90);
        { std::lock_guard<std::mutex> lk(g_tmu);
          g_tpend.erase(url);
          if (hb) g_tbm[url] = hb; else g_tfail.insert(url); }
        if (hb && !g_thumbDirty.exchange(true)) PostMessageW(g_main, WM_THUMB, 0, 0);
    }
}
static void PurgeThumbsIfBig() {   // UI thread only, called when lists are replaced
    std::lock_guard<std::mutex> lk(g_tmu);
    if (g_tbm.size() < 400) return;
    for (auto& p : g_tbm) DeleteObject(p.second);
    g_tbm.clear();
}

// -------------------------------------------------------- owner-drawn list
enum { LE_DBL = 1, LE_CHECK, LE_CTX, LE_WARM };
struct Row { int kind = 0; string id, title, meta, thumb, badge; bool check = false; };
static const wchar_t* kListCls = L"MiniYTList";

struct ListCtl {
    HWND h = nullptr; int ctlId = 0; vector<Row> rows;
    int rowH = 0, tw = 0, th = 0, sel = -1, playing = -1, scroll = 0, hoverRow = -1; POINT ctxPt = {0, 0};

    void Create(HWND parent, int id, bool wide) {
        ctlId = id;
        rowH = wide ? S(104) : S(70); tw = wide ? S(160) : S(96); th = wide ? S(90) : S(54);
        h = CreateWindowExW(WS_EX_CLIENTEDGE, kListCls, L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_CLIPSIBLINGS | WS_TABSTOP,
                            0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, g_hi, this);
    }
    int ClientH() { RECT r; GetClientRect(h, &r); return r.bottom; }
    void UpdateScroll() {
        int total = (int)rows.size() * rowH, page = ClientH();
        int mx = std::max(0, total - page);
        scroll = std::max(0, std::min(scroll, mx));
        SCROLLINFO si; si.cbSize = sizeof si; si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        si.nMin = 0; si.nMax = std::max(total, 1) - 1; si.nPage = page; si.nPos = scroll;
        SetScrollInfo(h, SB_VERT, &si, TRUE);
    }
    void SetRows(vector<Row>&& r, bool keep = false) {
        rows = std::move(r);
        if (!keep) { scroll = 0; sel = -1; }
        if (sel >= (int)rows.size()) sel = -1;
        UpdateScroll(); InvalidateRect(h, nullptr, FALSE);
    }
    void ScrollTo(int y) { scroll = y; UpdateScroll(); InvalidateRect(h, nullptr, FALSE); }
    void Ensure(int i) {
        if (i < 0 || i >= (int)rows.size()) return;
        int top = i * rowH, page = ClientH();
        if (top < scroll) ScrollTo(top); else if (top + rowH > scroll + page) ScrollTo(top + rowH - page);
    }
    int Hit(int y) { int i = (y + scroll) / rowH; return (i >= 0 && i < (int)rows.size()) ? i : -1; }
    void Notify(int ev, int idx) { SendMessageW(GetParent(h), WM_LISTEV, MAKEWPARAM(ev, ctlId), idx); }
    bool InCheck(const Row& r, int x) { return r.check && x < S(10) + S(18) + S(8); }

    void DrawRow(HDC dc, int i, int y, int cw) {
        const Row& r = rows[i];
        RECT rr = {0, y, cw, y + rowH};
        HBRUSH b = CreateSolidBrush(i == sel ? RGB(214, 230, 250) : i == playing ? RGB(232, 245, 236) : r.kind == 1 ? RGB(255, 245, 224) : RGB(255, 255, 255));
        FillRect(dc, &rr, b); DeleteObject(b);
        HPEN pen = CreatePen(PS_SOLID, 1, RGB(230, 230, 230)); HGDIOBJ op = SelectObject(dc, pen);
        MoveToEx(dc, 0, y + rowH - 1, nullptr); LineTo(dc, cw, y + rowH - 1);
        SelectObject(dc, op); DeleteObject(pen);
        if (i == playing) { RECT ab = {0, y, S(4), y + rowH}; HBRUSH gb = CreateSolidBrush(RGB(46, 158, 87)); FillRect(dc, &ab, gb); DeleteObject(gb); }
        int x = S(10); bool wd = r.check && Watched(r.id);
        if (r.kind == 1) {   // playlist: orange side bar + folder-like arrow (tree look)
            RECT ab = {0, y, S(4), y + rowH - 1}; HBRUSH ob2 = CreateSolidBrush(RGB(232, 140, 20)); FillRect(dc, &ab, ob2); DeleteObject(ob2);
            POINT tri[3] = {{x, y + rowH / 2 - S(6)}, {x, y + rowH / 2 + S(6)}, {x + S(9), y + rowH / 2}};
            HBRUSH tb = CreateSolidBrush(RGB(232, 140, 20)); HPEN tp = CreatePen(PS_SOLID, 1, RGB(232, 140, 20));
            HGDIOBJ o1 = SelectObject(dc, tb), o2 = SelectObject(dc, tp); Polygon(dc, tri, 3); SelectObject(dc, o1); SelectObject(dc, o2); DeleteObject(tb); DeleteObject(tp);
            x += S(20);
        }
        if (r.check) {
            int sz = S(18), cy = y + (rowH - sz) / 2;
            COLORREF col = wd ? RGB(46, 158, 87) : RGB(150, 150, 150);
            HBRUSH cb = CreateSolidBrush(wd ? col : RGB(255, 255, 255)); HPEN cp = CreatePen(PS_SOLID, 1, col);
            HGDIOBJ ob = SelectObject(dc, cb), opn = SelectObject(dc, cp);
            RoundRect(dc, x, cy, x + sz, cy + sz, S(4), S(4));
            SelectObject(dc, ob); SelectObject(dc, opn); DeleteObject(cb); DeleteObject(cp);
            if (wd) {
                HPEN wp = CreatePen(PS_SOLID, std::max(2, S(2)), RGB(255, 255, 255)); HGDIOBJ o2 = SelectObject(dc, wp);
                MoveToEx(dc, x + sz * 22 / 100, cy + sz * 52 / 100, nullptr);
                LineTo(dc, x + sz * 42 / 100, cy + sz * 72 / 100);
                LineTo(dc, x + sz * 80 / 100, cy + sz * 28 / 100);
                SelectObject(dc, o2); DeleteObject(wp);
            }
            x += sz + S(10);
        }
        int ty = y + (rowH - th) / 2;
        if (r.kind == 1 && ty - y >= S(6)) {   // stack of cards behind the thumbnail
            HBRUSH s1 = CreateSolidBrush(RGB(190, 190, 190)), s2 = CreateSolidBrush(RGB(150, 150, 150));
            RECT a1 = {x + S(12), ty - S(6), x + tw - S(12), ty - S(3)}, a2 = {x + S(6), ty - S(3), x + tw - S(6), ty};
            FillRect(dc, &a1, s1); FillRect(dc, &a2, s2); DeleteObject(s1); DeleteObject(s2);
        }
        HBITMAP hb = GetThumb(r.thumb);
        if (hb) {
            HDC m = CreateCompatibleDC(dc); HGDIOBJ o = SelectObject(m, hb);
            BITMAP bm; GetObject(hb, sizeof bm, &bm);
            SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, x, ty, tw, th, m, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(m, o); DeleteDC(m);
        } else {
            RECT t = {x, ty, x + tw, ty + th}; HBRUSH gb = CreateSolidBrush(RGB(216, 216, 216)); FillRect(dc, &t, gb); DeleteObject(gb);
        }
        if (r.kind == 1) {   // dark "PLAYLIST" tag on the thumbnail
            SetBkMode(dc, TRANSPARENT); SelectObject(dc, g_fb);
            const wchar_t* tag = L"\x2261 PLAYLIST"; SIZE sz2; GetTextExtentPoint32W(dc, tag, (int)wcslen(tag), &sz2);
            RECT bg = {x + tw - sz2.cx - S(10), ty + th - sz2.cy - S(4), x + tw, ty + th};
            HBRUSH bb = CreateSolidBrush(RGB(25, 25, 25)); FillRect(dc, &bg, bb); DeleteObject(bb);
            SetTextColor(dc, RGB(255, 255, 255)); RECT tt = bg; DrawTextW(dc, tag, -1, &tt, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        }
        int tx = x + tw + S(10), pad = S(6), metaH = S(18);
        SetBkMode(dc, TRANSPARENT);
        RECT tr = {tx, y + pad, cw - S(8), y + rowH - pad - metaH};
        SelectObject(dc, g_fb); SetTextColor(dc, wd ? RGB(140, 140, 140) : RGB(20, 20, 20));
        wstring wt = W(r.title); DrawTextW(dc, wt.c_str(), -1, &tr, DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX);
        RECT mr = {tx, y + rowH - pad - metaH, cw - S(8), y + rowH - pad};
        SelectObject(dc, g_f); SetTextColor(dc, RGB(110, 110, 110));
        wstring wm = W(r.meta); DrawTextW(dc, wm.c_str(), -1, &mr, DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX | DT_VCENTER);
    }
    void Paint(HDC hdc) {
        RECT rc; GetClientRect(h, &rc); int cw = rc.right, ch = rc.bottom;
        if (cw <= 0 || ch <= 0) return;
        HDC dc = CreateCompatibleDC(hdc); HBITMAP bmp = CreateCompatibleBitmap(hdc, cw, ch);
        HGDIOBJ ob = SelectObject(dc, bmp);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        if (!rows.empty()) {
            int first = scroll / rowH, last = std::min((scroll + ch) / rowH, (int)rows.size() - 1);
            for (int i = first; i <= last; i++) DrawRow(dc, i, i * rowH - scroll, cw);
        }
        BitBlt(hdc, 0, 0, cw, ch, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob); DeleteObject(bmp); DeleteDC(dc);
    }
};

static LRESULT CALLBACK ListProc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    ListCtl* L = (ListCtl*)GetWindowLongPtrW(hw, GWLP_USERDATA);
    if (m == WM_NCCREATE) {
        L = (ListCtl*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hw, GWLP_USERDATA, (LONG_PTR)L); L->h = hw;
        return DefWindowProcW(hw, m, wp, lp);
    }
    if (!L) return DefWindowProcW(hw, m, wp, lp);
    switch (m) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: { PAINTSTRUCT ps; HDC dc = BeginPaint(hw, &ps); L->Paint(dc); EndPaint(hw, &ps); return 0; }
        case WM_SIZE: L->UpdateScroll(); return 0;
        case WM_GETDLGCODE: return DLGC_WANTARROWS | DLGC_WANTCHARS;
        case WM_VSCROLL: {
            int pos = L->scroll, page = L->ClientH();
            switch (LOWORD(wp)) {
                case SB_LINEUP: pos -= L->rowH / 2; break; case SB_LINEDOWN: pos += L->rowH / 2; break;
                case SB_PAGEUP: pos -= page; break; case SB_PAGEDOWN: pos += page; break;
                case SB_TOP: pos = 0; break; case SB_BOTTOM: pos = 1 << 28; break;
                case SB_THUMBTRACK: case SB_THUMBPOSITION: {
                    SCROLLINFO si; si.cbSize = sizeof si; si.fMask = SIF_TRACKPOS; GetScrollInfo(hw, SB_VERT, &si); pos = si.nTrackPos; break; }
            }
            L->ScrollTo(pos); return 0;
        }
        case WM_MOUSEWHEEL: {
            int d = GET_WHEEL_DELTA_WPARAM(wp);
            L->ScrollTo(L->scroll - d * (L->rowH * 3 / 2) / WHEEL_DELTA); return 0;
        }
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
            SetFocus(hw);
            int i = L->Hit(GET_Y_LPARAM(lp));
            if (i < 0) return 0;
            L->sel = i; InvalidateRect(hw, nullptr, FALSE);
            bool inChk = L->InCheck(L->rows[i], GET_X_LPARAM(lp));
            if (inChk) L->Notify(LE_CHECK, i);
            else if (m == WM_LBUTTONDBLCLK) L->Notify(LE_DBL, i);
            return 0;
        }
        case WM_MOUSEMOVE: {   // resting the mouse on a row for a moment = "you may play this": prepare it
            int i = L->Hit(GET_Y_LPARAM(lp));
            if (i != L->hoverRow) { L->hoverRow = i; KillTimer(hw, 1); if (i >= 0) SetTimer(hw, 1, 350, nullptr); }
            return 0;
        }
        case WM_TIMER: if (wp == 1) { KillTimer(hw, 1); if (L->hoverRow >= 0) L->Notify(LE_WARM, L->hoverRow); } return 0;
        case WM_RBUTTONDOWN: {
            SetFocus(hw);
            int i = L->Hit(GET_Y_LPARAM(lp));
            if (i >= 0) { L->sel = i; InvalidateRect(hw, nullptr, FALSE); }
            return 0;
        }
        case WM_CONTEXTMENU: {
            int i = L->sel; POINT pt = {(short)LOWORD(lp), (short)HIWORD(lp)};
            if (lp == -1) { pt.x = 20; pt.y = (i >= 0 ? i * L->rowH - L->scroll + L->rowH / 2 : 20); ClientToScreen(hw, &pt); }
            else { POINT c = pt; ScreenToClient(hw, &c); int hi = L->Hit(c.y); if (hi >= 0) i = hi; }
            if (i >= 0) { L->ctxPt = pt; L->Notify(LE_CTX, i); }
            return 0;
        }
        case WM_KEYDOWN: {
            int n = (int)L->rows.size(), s = L->sel, page = std::max(1, L->ClientH() / L->rowH);
            switch (wp) {
                case VK_DOWN: s = std::min(n - 1, s + 1); break; case VK_UP: s = std::max(0, s < 0 ? 0 : s - 1); break;
                case VK_NEXT: s = std::min(n - 1, s + page); break; case VK_PRIOR: s = std::max(0, s - page); break;
                case VK_HOME: s = 0; break; case VK_END: s = n - 1; break;
                case VK_RETURN: if (s >= 0 && s < n) L->Notify(LE_DBL, s); return 0;
                case VK_SPACE: if (s >= 0 && s < n && L->rows[s].check) L->Notify(LE_CHECK, s); return 0;
                default: return DefWindowProcW(hw, m, wp, lp);
            }
            if (n > 0) { L->sel = s; L->Ensure(s); InvalidateRect(hw, nullptr, FALSE); L->hoverRow = s; KillTimer(hw, 1); SetTimer(hw, 1, 350, nullptr); }
            return 0;
        }
    }
    return DefWindowProcW(hw, m, wp, lp);
}

// ====================================================================== APP
enum { ID_EDIT = 101, ID_MODE, ID_QUAL, ID_MY, ID_UNW, ID_REF, ID_BACK, ID_LEFT, ID_BROWSE, ID_STATUS, ID_LTITLE, ID_BTITLE, ID_GO = 900 };
enum { ID_PREV = 120, ID_PLAY, ID_NEXT, ID_MUTE, ID_FULL, ID_SEEK, ID_VOL, ID_TIME, ID_DUR, ID_SLOW, ID_SPEED, ID_FAST, ID_PB };
enum { IDM_PLAY = 201, IDM_ADDLOCAL, IDM_CHANNEL, IDM_TOGGLE, IDM_OPEN, IDM_FORGET, IDM_REMOVE };
enum { BK_RESULTS = 1, BK_MYLISTS = 2 };

struct App {
    HWND edit = 0, cbMode = 0, cbQual = 0, btnMy = 0, stLeft = 0, btnUnw = 0, btnRef = 0, video = 0, stBrowse = 0, btnBack = 0, stStatus = 0;
    HWND stTime = 0, stDur = 0, sbSeek = 0, sbVol = 0, btnPrev = 0, btnPlay = 0, btnNext = 0, btnMute = 0, btnFull = 0, btnSlow = 0, btnSpeed = 0, btnFast = 0, pb = 0, btnSearch = 0;
    ListCtl left, browse;
    bool browseMode = false, fullscreen = false, dragging = false;
    WINDOWPLACEMENT savedPl; LONG_PTR savedStyle = 0;
} A;
static vector<Item> g_bitems;
static int g_bkind = 0;
static bool g_demo = false;
static const wchar_t* kReadyText = L"Ready. Type a name (or paste a channel / playlist link) and press Enter.";

struct PlayCtx { string vid, pl, title; int idx = -1; bool fast = false; string pendingAudio; int q = 2; string audioUrl; bool audioRetry = false; } P;
static std::atomic<int> g_playGen{0}, g_searchGen{0};
static std::atomic<double> g_timePos{0};
static std::atomic<bool> g_fileReady{false};   // true only while the CURRENT file is open: time-pos readings from an old file are ignored

static void Status(const wstring& s) { if (A.stStatus) SetWindowTextW(A.stStatus, s.c_str()); }
static void StatusUI(const wstring& s) { RunOnUI([s] { Status(s); }); }
#ifndef PBS_MARQUEE
#define PBS_MARQUEE 0x08
#endif
#ifndef PBM_SETMARQUEE
#define PBM_SETMARQUEE (WM_USER + 10)
#endif
static bool g_busy = false;
// pct: -1 = moving "working..." bar, 0..100 = real percentage, -2 = hide
static void ProgressSet(int pct) {
    if (!A.pb) return;
    LONG_PTR st = GetWindowLongPtrW(A.pb, GWL_STYLE);
    if (pct == -2) { g_busy = false; SendMessageW(A.pb, PBM_SETMARQUEE, FALSE, 0); ShowWindow(A.pb, SW_HIDE); return; }
    g_busy = true;
    if (pct < 0) {
        if (!(st & PBS_MARQUEE)) SetWindowLongPtrW(A.pb, GWL_STYLE, st | PBS_MARQUEE);
        SendMessageW(A.pb, PBM_SETMARQUEE, TRUE, 30);
    } else {
        if (st & PBS_MARQUEE) { SendMessageW(A.pb, PBM_SETMARQUEE, FALSE, 0); SetWindowLongPtrW(A.pb, GWL_STYLE, st & ~(LONG_PTR)PBS_MARQUEE); }
        SendMessageW(A.pb, PBM_SETPOS, pct, 0);
    }
    ShowWindow(A.pb, SW_SHOW);
}
static void ProgressUI(int pct) { RunOnUI([pct] { ProgressSet(pct); }); }
static void Layout();
static void ShowVideo();
static void Play(const string& vid, const string& pl, int idx, const string& title, double start = 0);
static void OnEndFile(const string& reason);
static void OnFileLoaded();
static void OnMpvMessage(const string& a);
static void OnMpvReady();
static void OnMpvDied();
static void SpawnMpv();
static void FetchRelated(const string& vid, const string& title);
static void AudioSafetyNet();
static void PlayUnwatched();
static void UpdateControls();
static std::atomic<double> g_dur{0}, g_vol{100}, g_speed{1.0}, g_lastTP{-1};
static std::atomic<int> g_stage{0};   // 0 idle/playing, 1 loading requested, 2 file opened (waiting for picture to move)
static std::atomic<bool> g_paused{false}, g_mute{false}, g_ctlDirty{false};
static bool g_idle = true;     // nothing loaded / stopped / finished
static bool g_seeking = false;
static void PostCtl() { if (!g_ctlDirty.exchange(true)) RunOnUI(UpdateControls); }

static string Trim(string s) {
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t i = 0; while (i < s.size() && isspace((unsigned char)s[i])) i++;
    return s.substr(i);
}
static string UrlEnc(const string& s) {
    string o; char b[4];
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
        else if (c == ' ') o += '+';
        else { snprintf(b, 4, "%%%02X", c); o += b; }
    }
    return o;
}
static string Param(const string& url, const string& key) {
    size_t p = url.find(key); if (p == string::npos) return "";
    p += key.size(); size_t e = p;
    while (e < url.size() && (isalnum((unsigned char)url[e]) || url[e] == '_' || url[e] == '-')) e++;
    return url.substr(p, e - p);
}

// ------------------------------------------------------------------- mpv
static HANDLE g_mpvProc = nullptr;
static HANDLE g_pipe = INVALID_HANDLE_VALUE;
static std::mutex g_pipeMu;
static bool g_mpvReady = false;
static std::function<void()> g_afterReady;

static bool MpvSend(const string& json) {
    std::lock_guard<std::mutex> lk(g_pipeMu);
    if (g_pipe == INVALID_HANDLE_VALUE) return false;
    string d = json + "\n";
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD n = 0; BOOL ok = WriteFile(g_pipe, d.data(), (DWORD)d.size(), &n, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(g_pipe, &ov, &n, TRUE);
    CloseHandle(ov.hEvent);
    return ok != 0;
}
static void HandleMpvLine(const string& line) {
    J j; if (!ParseJson(line, j)) return;
    if (j["request_id"].t == J::Num && (int)j["request_id"].n >= 100000) {   // reply: which audio track is selected
        int gen = (int)j["request_id"].n - 100000;
        if (j["data"].t != J::Num) RunOnUI([gen] { if (gen == (int)g_playGen) AudioSafetyNet(); });
        return;
    }
    if (j["request_id"].t == J::Num && (int)j["request_id"].n == 11) {   // reply to our time-pos poll
        if (j["data"].t == J::Num && g_fileReady) {
            g_timePos = j["data"].n;
            if (g_stage == 2) {   // opened: hide the bar once the time really moves (or the user paused)
                double t = j["data"].n;
                if (g_lastTP < 0) g_lastTP = t;
                else if (t > g_lastTP + 0.3 || g_paused) {
                    g_stage = 0; RunOnUI([] { ProgressSet(-2); Status(L"Playing: " + W(P.title)); });
                    char q[96]; snprintf(q, sizeof q, "{\"command\":[\"get_property\",\"aid\"],\"request_id\":%d}", 100000 + (int)g_playGen);
                    MpvSend(q);
                }
            }
        }
        PostCtl(); return;
    }
    const string& ev = j["event"].s;
    if (ev == "property-change") {
        const string& nm = j["name"].s; const J& d = j["data"];
        if (nm == "duration") g_dur = (d.t == J::Num) ? d.n : 0;
        else if (nm == "pause") g_paused = (d.t == J::Bool && d.b);
        else if (nm == "mute") g_mute = (d.t == J::Bool && d.b);
        else if (nm == "speed" && d.t == J::Num) g_speed = d.n;
        else if (nm == "paused-for-cache" && d.t == J::Bool) {
            bool b = d.b; RunOnUI([b] { if (b) ProgressSet(-1); else if (g_stage == 0) ProgressSet(-2); });
        }
        else if (nm == "volume" && d.t == J::Num) {
            g_vol = d.n; int v = (int)d.n;
            RunOnUI([v] { if (v != g.volume) { g.volume = v; ScheduleSave(); } });
        }
        PostCtl();
    }
    else if (ev == "end-file") { g_fileReady = false; string r = j["reason"].s; RunOnUI([r] { OnEndFile(r); }); }
    else if (ev == "file-loaded") { g_stage = 2; g_lastTP = -1; g_fileReady = true; RunOnUI(OnFileLoaded); }
    else if (ev == "client-message") { string a = j["args"].idx(0).s; RunOnUI([a] { OnMpvMessage(a); }); }
}
static void IpcThread(wstring name) {
    HANDLE h = INVALID_HANDLE_VALUE;
    for (int i = 0; i < 300; i++) {
        h = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        Sleep(50); if (!g_mpvProc) return;
    }
    if (h == INVALID_HANDLE_VALUE) { StatusUI(L"The video player did not start."); return; }
    { std::lock_guard<std::mutex> lk(g_pipeMu); g_pipe = h; }
    MpvSend("{\"command\":[\"observe_property\",2,\"duration\"]}");
    MpvSend("{\"command\":[\"observe_property\",3,\"pause\"]}");
    MpvSend("{\"command\":[\"observe_property\",4,\"volume\"]}");
    MpvSend("{\"command\":[\"observe_property\",5,\"mute\"]}");
    MpvSend("{\"command\":[\"observe_property\",6,\"speed\"]}");
    MpvSend("{\"command\":[\"observe_property\",7,\"paused-for-cache\"]}");
    RunOnUI(OnMpvReady);
    string acc; char buf[8192];
    OVERLAPPED ov; memset(&ov, 0, sizeof ov); ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    for (;;) {
        ResetEvent(ov.hEvent); DWORD n = 0;
        BOOL ok = ReadFile(h, buf, sizeof buf, &n, &ov);
        if (!ok) { if (GetLastError() != ERROR_IO_PENDING) break; if (!GetOverlappedResult(h, &ov, &n, TRUE)) break; }
        if (n == 0) break;
        acc.append(buf, n);
        size_t pos;
        while ((pos = acc.find('\n')) != string::npos) { string line = acc.substr(0, pos); acc.erase(0, pos + 1); HandleMpvLine(line); }
    }
    CloseHandle(ov.hEvent);
    { std::lock_guard<std::mutex> lk(g_pipeMu); if (g_pipe == h) g_pipe = INVALID_HANDLE_VALUE; }
    CloseHandle(h);
}
static void MpvWaiter(HANDLE proc) {
    WaitForSingleObject(proc, INFINITE);
    RunOnUI([proc] { if (g_mpvProc == proc) OnMpvDied(); CloseHandle(proc); });
}
static void WriteInputConf() {
    string c = "q ignore\nQ ignore\nf script-message minyt-fullscreen\nF11 script-message minyt-fullscreen\n"
               "MBTN_LEFT_DBL script-message minyt-fullscreen\nESC script-message minyt-escape\n"
               "> script-message minyt-next\n< script-message minyt-prev\n";
    HANDLE f = CreateFileW(Join(g_data, L"input.conf").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, c.data(), (DWORD)c.size(), &w, nullptr); CloseHandle(f);
}
static void SpawnMpv() {
    if (g_mpvProc) return;
    wstring exe = FindTool(L"mpv");
    if (exe.empty()) { Status(L"Video player (mpv) not found."); return; }
    WriteInputConf();
    wstring pipe = L"\\\\.\\pipe\\minyt-" + std::to_wstring(GetCurrentProcessId());
    wstring cmd = L"\"" + exe + L"\" --wid=" + std::to_wstring((unsigned long long)(uintptr_t)A.video) +
        L" --idle=yes --force-window=yes --no-terminal --msg-level=all=no --input-ipc-server=" + pipe +
        L" --input-conf=\"" + Join(g_data, L"input.conf") + L"\" --profile=fast --vo=gpu --hwdec=auto-safe"
        L" --cache=yes --demuxer-max-bytes=120MiB --demuxer-readahead-secs=30 --cursor-autohide=1500"
        L" --save-position-on-quit=no --keep-open=no --osd-bar=yes --osc=no --volume=" + std::to_wstring(g.volume);
    STARTUPINFOW si; memset(&si, 0, sizeof si); si.cb = sizeof si;
    PROCESS_INFORMATION pi; memset(&pi, 0, sizeof pi);
    if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Status(L"Could not start the video player."); return;
    }
    CloseHandle(pi.hThread);
    g_mpvProc = pi.hProcess; g_mpvReady = false;
    std::thread(IpcThread, pipe).detach();
    HANDLE dup; DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS);
    std::thread(MpvWaiter, dup).detach();
}
static void OnMpvReady() {
    g_mpvReady = true;
    if (g_afterReady) { auto f = std::move(g_afterReady); g_afterReady = nullptr; f(); }
}
static void OnMpvDied() {
    g_mpvProc = nullptr; g_mpvReady = false; g_stage = 0; ProgressSet(-2);
    Status(L"The video player closed. It will restart when you play something.");
}
static void MpvQuit() {
    MpvSend("{\"command\":[\"quit\"]}");
    if (g_mpvProc && WaitForSingleObject(g_mpvProc, 600) == WAIT_TIMEOUT) TerminateProcess(g_mpvProc, 0);
}

// ----------------------------------------------------------------- playback
static std::mutex g_fcMu;
struct CacheEnt { vector<Fmt> f; ULONGLONG t = 0; };
static std::map<string, CacheEnt> g_fc;
static bool GetFormats(const string& vid, vector<Fmt>& out) {
    { std::lock_guard<std::mutex> lk(g_fcMu);
      auto it = g_fc.find(vid);
      if (it != g_fc.end() && GetTickCount64() - it->second.t < 4ULL * 3600 * 1000) { out = it->second.f; return true; } }
    string err; vector<Fmt> f;
    if (!ResolveFormats(vid, f, err)) return false;
    { std::lock_guard<std::mutex> lk(g_fcMu);
      if (g_fc.size() > 200) g_fc.clear();
      g_fc[vid] = CacheEnt{f, GetTickCount64()}; }
    out = std::move(f); return true;
}
static std::atomic<int> g_warming{0};
static void WarmFormats(const string& id) {   // fetch + cache the stream addresses in the background
    { std::lock_guard<std::mutex> lk(g_fcMu);
      auto it = g_fc.find(id);
      if (it != g_fc.end() && GetTickCount64() - it->second.t < 4ULL * 3600 * 1000) return; }
    if (g_warming >= 3) return;
    g_warming++;
    std::thread([id] { vector<Fmt> f; GetFormats(id, f); g_warming--; }).detach();
}
static int NextUnwatched(const string& pl, int idx) {
    auto it = g.pls.find(pl); if (it == g.pls.end()) return -1;
    auto& v = it->second.videos;
    for (int j = idx + 1; j < (int)v.size(); j++) if (!Watched(v[j].id)) return j;
    return -1;
}
static void UpdatePlayingMark() {
    int pi = -1;
    if (!P.vid.empty() && P.pl == g.current && P.idx >= 0 && P.idx < (int)Cur().videos.size() && Cur().videos[P.idx].id == P.vid) pi = P.idx;
    if (A.left.playing != pi) { A.left.playing = pi; InvalidateRect(A.left.h, nullptr, FALSE); }
}
static void LoadFile(const string& url, double start) {
    MpvSend("{\"command\":[\"set_property\",\"pause\",false]}");   // a paused player would otherwise stay paused
    g_fileReady = false; g_timePos = start > 1 ? start : 0; g_dur = 0; g_idle = false;
    string cmd = "{\"command\":[\"loadfile\",\"" + JEsc(url) + "\",\"replace\"";
    if (start > 1) { char b[48]; snprintf(b, sizeof b, ",-1,\"start=%.1f\"", start); cmd += b; }
    else cmd += ",-1,\"start=0\"";
    MpvSend(cmd + "]}");
}
static void Fallback(const string& vid, int q, double start) {
    P.fast = false; P.pendingAudio.clear(); g_stage = 1; P.audioUrl.clear();
    MpvSend("{\"command\":[\"set_property\",\"audio-files\",[]]}");
    if (FindTool(L"yt-dlp").empty()) Status(L"Backup method needs yt-dlp (still downloading) ...");
    MpvSend("{\"command\":[\"set_property\",\"user-agent\",\"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36\"]}");
    MpvSend("{\"command\":[\"set_property\",\"ytdl-format\",\"" + JEsc(YtdlFormat(q)) + "\"]}");
    LoadFile("https://www.youtube.com/watch?v=" + vid, start);
}
static void StartPlayback(const string& vid, const Pick& pk, double start, int q) {
    P.pendingAudio.clear();
    if (pk.ok) {
        P.fast = true;
        // the separate audio stream is given to mpv BEFORE the video opens, so both load together and audio can't be missed
        P.audioUrl = (q != 4 && !pk.audio.empty()) ? pk.audio : ""; P.audioRetry = false;
        MpvSend(string("{\"command\":[\"set_property\",\"audio-files\",[") + (P.audioUrl.empty() ? "" : "\"" + JEsc(P.audioUrl) + "\"") + "]]}");
        MpvSend(string("{\"command\":[\"set_property\",\"user-agent\",\"") + JEsc(kVrUA) + "\"]}");
        LoadFile(pk.video, start);
    } else Fallback(vid, q, start);
}
static void Play(const string& vid, const string& pl, int idx, const string& title, double start) {
    P.vid = vid; P.pl = pl; P.idx = idx; P.title = title;
    int gen = ++g_playGen;
    g_idle = false;
    ShowVideo(); UpdatePlayingMark();
    Status(L"Loading: " + W(title)); ProgressSet(-1); g_stage = 1; g_lastTP = -1;
    if (!g_mpvReady) {
        g_afterReady = [=] { Play(vid, pl, idx, title, start); };
        if (!g_mpvProc) SpawnMpv();
        return;
    }
    if (pl.empty() || pl == "related") FetchRelated(vid, title);
    int q = g.quality; P.q = q;
    int nx = pl.empty() ? -1 : NextUnwatched(pl, idx);
    string nextId = (nx >= 0) ? g.pls[pl].videos[nx].id : "";
    std::thread([=] {
        vector<Fmt> f; Pick pk;
        if (GetFormats(vid, f)) pk = PickStreams(f, kHeights[q]);
        RunOnUI([=] { if (gen != g_playGen) return; StartPlayback(vid, pk, start, q); });
        if (!nextId.empty()) { vector<Fmt> f2; GetFormats(nextId, f2); }   // warm the next video: it will start instantly
    }).detach();
}
static void PlayFromList(const string& pl, int idx) {
    auto it = g.pls.find(pl);
    if (it == g.pls.end() || idx < 0 || idx >= (int)it->second.videos.size()) return;
    auto& v = it->second.videos[idx];
    Play(v.id, pl, idx, v.title);
}
static void AddHistory(const string& vid, const string& title) {
    if (vid.empty() || (!g.hist.empty() && g.hist[0].id == vid)) return;
    Hist h; h.id = vid; h.title = title; h.t = (long long)time(nullptr);
    bool found = false;
    for (auto& it : g_bitems) if (it.kind == 0 && it.id == vid) { h.title = it.title; h.channel = it.channel; h.dur = it.dur; found = true; break; }
    if (!found) for (auto& kv : g.pls) {
        if (kv.first == "history") continue;
        for (auto& v : kv.second.videos) if (v.id == vid) { h.title = v.title; h.channel = v.channel; h.dur = v.dur; found = true; break; }
        if (found) break;
    }
    if (!found) for (auto& o : g.hist) if (o.id == vid) { h.channel = o.channel; h.dur = o.dur; break; }
    g.hist.erase(std::remove_if(g.hist.begin(), g.hist.end(), [&](const Hist& x) { return x.id == vid; }), g.hist.end());
    g.hist.insert(g.hist.begin(), h);
    if ((int)g.hist.size() > kMaxHist) g.hist.resize(kMaxHist);
    ScheduleSave();
}
static void AudioSafetyNet() {   // picture is moving but no audio track got selected: add it once, the old way
    if (!P.fast || P.audioUrl.empty() || P.audioRetry) return;
    P.audioRetry = true;
    MpvSend("{\"command\":[\"audio-add\",\"" + JEsc(P.audioUrl) + "\",\"select\"]}");
}
static void OnFileLoaded() {
    Status(L"Buffering: " + W(P.title));
    AddHistory(P.vid, P.title);
}
static void OnEndFile(const string& reason) {
    if (reason == "eof") {
        int n = P.pl.empty() ? -1 : NextUnwatched(P.pl, P.idx);
        if (n >= 0) PlayFromList(P.pl, n); else { g_idle = true; g_fileReady = false; g_timePos = 0; g_dur = 0; g_stage = 0; ProgressSet(-2); PostCtl(); Status(L"Finished. Tick it if you've watched it."); }
    } else if (reason == "error") {
        if (P.fast) { Status(L"Trying the backup method ..."); Fallback(P.vid, P.q, g_fileReady ? (double)g_timePos : 0.0); }
        else { g_idle = true; g_stage = 0; ProgressSet(-2); PostCtl(); Status(L"Could not play this video."); }
    }
}

// --------------------------------------------------------- player controls
static wstring FmtTime(double sec) {
    if (sec < 0) sec = 0;
    int s = (int)sec, h = s / 3600, m = (s % 3600) / 60, r = s % 60; wchar_t b[32];
    if (h) swprintf(b, 32, L"%d:%02d:%02d", h, m, r); else swprintf(b, 32, L"%d:%02d", m, r);
    return b;
}
static void SetText(HWND h, const wstring& t) {
    wchar_t cur[64]; GetWindowTextW(h, cur, 64);
    if (t != cur) SetWindowTextW(h, t.c_str());
}
static wstring FmtSpeed(double v) {
    wchar_t b[32]; swprintf(b, 32, L"%.2f", v);
    wstring t = b;
    while (t.size() > 3 && t.back() == L'0') t.pop_back();   // 1.00 -> 1.0, 1.25 stays
    return t + L"x";
}
static void SetSpeed(double v) {
    v = std::max(0.25, std::min(4.0, v));
    char b[96]; snprintf(b, sizeof b, "{\"command\":[\"set_property\",\"speed\",%.3f]}", v);
    MpvSend(b); g_speed = v; PostCtl();
}
static void ChangeSpeed(int dir) {      // dir -1 = slower, +1 = faster, 0 = back to normal
    static const double steps[] = {0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0};
    double cur = g_speed;
    if (dir == 0) { SetSpeed(1.0); return; }
    if (dir > 0) { for (double s : steps) if (s > cur + 0.001) { SetSpeed(s); return; } SetSpeed(steps[8]); }
    else { for (int i = 8; i >= 0; i--) if (steps[i] < cur - 0.001) { SetSpeed(steps[i]); return; } SetSpeed(steps[0]); }
}
static int g_playState = -1;   // 0 nothing playing, 1 playing, 2 paused
static void UpdateControls() {
    g_ctlDirty = false;
    bool active = !P.vid.empty() && !g_idle;
    double t = active ? (double)g_timePos : 0, d = active ? (double)g_dur : 0;
    if (!g_seeking) {
        SendMessageW(A.sbSeek, TBM_SETPOS, TRUE, d > 0 ? (LPARAM)std::min(1000.0, t / d * 1000.0) : 0);
        SetText(A.stTime, FmtTime(t));
    }
    SetText(A.stDur, d > 0 ? FmtTime(d) : L"--:--");
    SetText(A.btnPlay, (active && !g_paused) ? L"Pause" : L"Play");
    int ps = !active ? 0 : g_paused ? 2 : 1;
    if (ps != g_playState) { g_playState = ps; InvalidateRect(A.btnPlay, nullptr, TRUE); }
    SetText(A.btnMute, g_mute ? L"Unmute" : L"Mute");
    SetText(A.btnSpeed, FmtSpeed(g_speed));
    if (GetCapture() != A.sbVol) SendMessageW(A.sbVol, TBM_SETPOS, TRUE, (LPARAM)(int)g_vol.load());
}
static void PlayNext() {
    if (P.pl.empty()) return;
    int n = NextUnwatched(P.pl, P.idx); if (n < 0) n = P.idx + 1;
    PlayFromList(P.pl, n);
}
static void PlayPrev() { if (!P.pl.empty() && P.idx > 0) PlayFromList(P.pl, P.idx - 1); }
static void TogglePause() {
    if (!g_mpvReady || g_idle || P.vid.empty()) {
        if (!P.vid.empty()) { Play(P.vid, P.pl, P.idx, P.title); return; }
        int s = A.left.sel;
        if (s >= 0 && s < (int)Cur().videos.size()) PlayFromList(g.current, s); else PlayUnwatched();
        return;
    }
    MpvSend("{\"command\":[\"cycle\",\"pause\"]}");
}
static void TickPlayer() {
    if (g_mpvReady && !P.vid.empty() && !g_idle) MpvSend("{\"command\":[\"get_property\",\"time-pos\"],\"request_id\":11}");
}

// -------------------------------------------------------------- fullscreen
static void ToggleFullscreen() {
    if (!A.fullscreen) {
        A.savedStyle = GetWindowLongPtrW(g_main, GWL_STYLE);
        A.savedPl.length = sizeof A.savedPl; GetWindowPlacement(g_main, &A.savedPl);
        MONITORINFO mi; mi.cbSize = sizeof mi;
        GetMonitorInfoW(MonitorFromWindow(g_main, MONITOR_DEFAULTTONEAREST), &mi);
        A.fullscreen = true; A.browseMode = false;
        SetWindowLongPtrW(g_main, GWL_STYLE, (A.savedStyle & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(g_main, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
        Layout(); RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    } else {
        A.fullscreen = false;
        SetWindowLongPtrW(g_main, GWL_STYLE, A.savedStyle);
        SetWindowPlacement(g_main, &A.savedPl);
        SetWindowPos(g_main, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        Layout(); RedrawWindow(g_main, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
}
static void OnMpvMessage(const string& a) {
    if (a == "minyt-fullscreen") ToggleFullscreen();
    else if (a == "minyt-escape") { if (A.fullscreen) ToggleFullscreen(); }
    else if (a == "minyt-next") PlayNext();
    else if (a == "minyt-prev") PlayPrev();
}

// -------------------------------------------------------------- list views
static Row ItemRow(const Item& it) {
    Row r; r.kind = it.kind; r.id = it.id; r.title = it.title; r.thumb = it.thumb;
    const char* dot = "  \xC2\xB7  ";
    if (it.kind == 0) { r.meta = it.channel + dot + it.dur; }
    else { r.meta = it.channel; if (!it.count.empty()) r.meta += (r.meta.empty() ? "" : dot) + it.count; }
    if (it.kind == 1) r.badge = "PLAYLIST";
    return r;
}
static void UpdateLeftHeader() {
    Playlist& p = Cur(); int done = 0;
    for (auto& v : p.videos) if (Watched(v.id)) done++;
    wstring t = std::to_wstring(done) + L"/" + std::to_wstring(p.videos.size()) + L"  -  " + W(p.title);
    SetWindowTextW(A.stLeft, t.c_str());
    EnableWindow(A.btnRef, !p.local && !IsTemp(p.id));
}
static void RefreshLeft(bool keep = false) {
    PurgeThumbsIfBig();
    Playlist& p = Cur(); vector<Row> rows; int n = 0;
    for (auto& v : p.videos) {
        Row r; r.id = v.id; r.title = v.title; r.check = true;
        r.thumb = g_demo ? "demo:" + v.id : ThumbUrl(v.id);
        r.meta = "#" + std::to_string(++n) + "  \xC2\xB7  " + v.dur + (((p.local || IsTemp(p.id)) && !v.channel.empty()) ? "  \xC2\xB7  " + v.channel : "");
        if (p.id == "history" && v.channel.size() >= 16)   // date first so it is never cut off
            r.meta = v.channel.substr(0, 16) + "  \xC2\xB7  " + v.dur + v.channel.substr(16);
        rows.push_back(std::move(r));
    }
    A.left.SetRows(std::move(rows), keep);
    A.left.playing = -1; UpdatePlayingMark(); UpdateLeftHeader();
}
static void FetchRelated(const string& vid, const string& title) {
    if (vid == g_relVid) return;
    g_relVid = vid;
    std::thread([=] {
        vector<Item> items;
        bool ok = InnerRelated(vid, items);
        if (!ok) { items.clear(); InnerSearch(title, 0, items); }   // backup: videos matching the title
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item& i) { return i.id == vid; }), items.end());
        auto sp = std::make_shared<vector<Item>>(std::move(items));
        RunOnUI([=] {
            if (P.vid != vid || sp->empty()) return;
            Playlist pl; pl.id = "related"; pl.title = "Related videos"; pl.local = false;
            for (auto& it : *sp) pl.videos.push_back({it.id, it.title, it.channel, it.dur});
            if (!IsTemp(g.current)) g_lastReal = g.current;
            g.pls["related"] = std::move(pl);
            g.current = "related";
            P.pl = "related"; P.idx = -1;     // when this video ends, the first related one plays next
            RefreshLeft();
            if (!g.pls["related"].videos.empty()) WarmFormats(g.pls["related"].videos[0].id);   // so auto-advance starts instantly
        });
    }).detach();
}
static void ShowBrowse(const wstring& title, vector<Item>&& items, int kind) {
    PurgeThumbsIfBig();
    g_bitems = std::move(items); g_bkind = kind;
    vector<Row> rows; for (auto& it : g_bitems) rows.push_back(ItemRow(it));
    A.browse.SetRows(std::move(rows));
    SetWindowTextW(A.stBrowse, title.c_str());
    A.browseMode = true; Layout();
}
static void ShowVideo() { if (A.browseMode) { A.browseMode = false; Layout(); } }
static void SetCurrent(const string& pid) {
    g.current = pid;
    if (pid != "local") {
        g.recent.erase(std::remove(g.recent.begin(), g.recent.end(), pid), g.recent.end());
        g.recent.insert(g.recent.begin(), pid);
        while ((int)g.recent.size() > kMaxLists) { g.pls.erase(g.recent.back()); g.recent.pop_back(); }
    }
    ScheduleSave();
}
static void OpenHistory() {
    Playlist pl; pl.id = "history"; pl.title = "Recently watched"; pl.local = false;
    for (auto& h : g.hist) pl.videos.push_back({h.id, h.title, FmtDate(h.t) + "  \xC2\xB7  " + h.channel, h.dur});
    if (!IsTemp(g.current)) g_lastReal = g.current;
    g.pls["history"] = std::move(pl); g.current = "history";
    RefreshLeft(); ShowVideo();
    Status(g.hist.empty() ? L"Nothing watched yet." : L"Recently watched: newest first. Double-click to play again.");
}
static void ShowMyLists() {
    vector<Item> items; vector<string> ids = {"local"};
    for (auto& r : g.recent) if (g.pls.count(r) && r != "local") ids.push_back(r);
    {
        Item hi; hi.kind = 1; hi.id = "history"; hi.title = "Recently watched videos";
        hi.count = std::to_string(g.hist.size()) + " videos";
        if (!g.hist.empty()) { hi.channel = "last: " + FmtDate(g.hist[0].t); hi.thumb = g_demo ? "demo:" + g.hist[0].id : ThumbUrl(g.hist[0].id); }
        items.push_back(std::move(hi));
    }
    for (auto& id : ids) {
        const Playlist& p = g.pls[id]; int done = 0;
        for (auto& v : p.videos) if (Watched(v.id)) done++;
        Item it; it.kind = 1; it.id = id; it.title = p.title; it.channel = p.channel;
        it.count = std::to_string(done) + "/" + std::to_string(p.videos.size()) + " watched";
        if (!p.videos.empty()) it.thumb = g_demo ? "demo:" + p.videos[0].id : ThumbUrl(p.videos[0].id);
        items.push_back(std::move(it));
    }
    ShowBrowse(L"My playlists  (double-click to open, right-click for more)", std::move(items), BK_MYLISTS);
}
static void ToggleWatched(const string& id) {
    if (Watched(id)) g.watched.erase(id); else g.watched.insert(id);
    ScheduleSave();
    InvalidateRect(A.left.h, nullptr, FALSE); InvalidateRect(A.browse.h, nullptr, FALSE);
    UpdateLeftHeader();
}

// ----------------------------------------------------- search / playlists
static void DoSearch(const string& q, int kind) {
    int gen = ++g_searchGen; Status(L"Searching ...");
    std::thread([=] {
        vector<Item> items;
        bool ok = InnerSearch(q, kind, items);
        if (!ok) {   // backup: yt-dlp
            items.clear(); vector<J> rows; vector<string> args;
            if (kind == 1) args = {"--playlist-end", "20", "https://www.youtube.com/results?search_query=" + UrlEnc(q) + "&sp=EgIQAw%3D%3D"};
            else args = {"ytsearch20:" + q};
            if (YtDlpJson(args, rows)) RowsToItems(rows, kind == 2 ? 0 : kind, items);
        }
        auto sp = std::make_shared<vector<Item>>(std::move(items));
        RunOnUI([=] {
            if (gen != g_searchGen) return;
            wstring t = (kind == 1 ? L"Playlists: " : kind == 2 ? L"Results: " : L"Videos: ") + W(q);
            if (sp->empty()) { Status(L"Nothing found (or no internet)."); return; }
            size_t n = sp->size();
            ShowBrowse(t, std::move(*sp), BK_RESULTS);
            Status(std::to_wstring(n) + L" results. Double-click to play / open, right-click for more.");
        });
    }).detach();
}
static void ShowHome() {
    int gen = ++g_searchGen; Status(L"Loading trending videos ...");
    std::thread([=] {
        vector<Item> items; bool ok = InnerHome(items);
        if (!ok) { items.clear(); vector<J> rows; if (YtDlpJson({"ytsearch20:trending now" + (g_country.empty() ? string() : " in " + g_country)}, rows)) RowsToItems(rows, 0, items); }
        auto sp = std::make_shared<vector<Item>>(std::move(items));
        RunOnUI([=] {
            if (gen != g_searchGen) return;
            if (sp->empty()) { Status(L"Could not load the home page (no internet?)."); return; }
            size_t n = sp->size();
            ShowBrowse(L"Home Page - trending videos", std::move(*sp), BK_RESULTS);
            Status(std::to_wstring(n) + L" videos. Double-click to play, right-click for more.");
        });
    }).detach();
}
static void OpenPlaylist(const string& pid, const string& hint) {
    if (g.pls.count(pid)) { SetCurrent(pid); RefreshLeft(); ShowVideo(); }   // saved copy shows instantly
    Status(L"Loading playlist ...");
    std::thread([=] {
        Playlist pl; bool ok = InnerPlaylist(pid, pl);
        if (!ok) {
            vector<J> rows; vector<Item> items;
            if (YtDlpJson({"https://www.youtube.com/playlist?list=" + pid}, rows)) {
                RowsToItems(rows, 0, items); pl.id = pid; pl.videos.clear();
                for (auto& it : items) pl.videos.push_back({it.id, it.title, it.channel, it.dur});
                pl.title = rows[0]["playlist_title"].s;
                pl.channel = rows[0]["playlist_uploader"].s;
                ok = !pl.videos.empty();
            }
        }
        auto sp = std::make_shared<Playlist>(std::move(pl));
        RunOnUI([=] {
            if (!ok) { Status(g.pls.count(pid) ? L"Could not refresh (showing saved copy)." : L"Could not load that playlist."); return; }
            if (sp->title.empty()) sp->title = hint.empty() ? "Playlist" : hint;
            sp->local = false;
            bool wasCur = (g.current == pid);
            size_t n = sp->videos.size();
            g.pls[pid] = *sp; SetCurrent(pid);
            RefreshLeft(wasCur); ShowVideo();
            Status(L"'" + W(sp->title) + L"': " + std::to_wstring(n) + L" videos. Tick the ones you've watched.");
        });
    }).detach();
}
static void ChannelPlaylists(const Item& src) {
    string cid = src.channelId, name = src.channel;
    int gen = ++g_searchGen; Status(L"Loading channel playlists ...");
    std::thread([=] {
        vector<Item> items;
        bool ok = !cid.empty() && InnerChannelPlaylists(cid, items);
        if (!ok) { items.clear(); ok = InnerSearch(name, 1, items); }
        auto sp = std::make_shared<vector<Item>>(std::move(items));
        RunOnUI([=] {
            if (gen != g_searchGen) return;
            if (sp->empty()) { Status(L"No playlists found for this channel."); return; }
            ShowBrowse(L"Playlists of " + W(name), std::move(*sp), BK_RESULTS);
            Status(L"Open a playlist to load it on the left.");
        });
    }).detach();
}
static void HandleUrl(string q) {
    if (q[0] == '@') q = "https://www.youtube.com/" + q;
    else if (!StartsWith(q, "http")) q = "https://" + q;
    string list = Param(q, "list=");
    if (!list.empty()) { OpenPlaylist(list, ""); return; }
    string v = Contains(q, "youtu.be/") ? Param(q, "youtu.be/") : Param(q, "v=");
    if (v.size() == 11) { Play(v, "", -1, "video " + v); return; }
    int gen = ++g_searchGen; Status(L"Opening channel ...");
    std::thread([=] {
        string cid = Contains(q, "/channel/") ? Param(q, "/channel/") : InnerResolveChannel(q);
        vector<Item> items; bool ok = !cid.empty() && InnerChannelPlaylists(cid, items);
        if (!ok) {
            string base = q; while (!base.empty() && base.back() == '/') base.pop_back();
            vector<J> rows; if (YtDlpJson({"--playlist-end", "100", base + "/playlists"}, rows)) { items.clear(); RowsToItems(rows, 1, items); }
        }
        auto sp = std::make_shared<vector<Item>>(std::move(items));
        RunOnUI([=] {
            if (gen != g_searchGen) return;
            if (sp->empty()) { Status(L"Could not read that link."); return; }
            ShowBrowse(L"Channel playlists", std::move(*sp), BK_RESULTS);
            Status(L"Open a playlist to load it on the left.");
        });
    }).detach();
}
static void SearchFromEdit() {
    wchar_t buf[1024]; GetWindowTextW(A.edit, buf, 1024);
    string q = Trim(U(buf)); if (q.empty()) return;
    bool url = StartsWith(q, "http") || StartsWith(q, "www.") || StartsWith(q, "youtu") || q[0] == '@';
    if (url) HandleUrl(q); else DoSearch(q, [] { int m = (int)SendMessageW(A.cbMode, CB_GETCURSEL, 0, 0); return m == 1 ? 0 : m == 2 ? 1 : 2; }());
}
static void AddLocal(const Item& it) {
    Playlist& l = g.pls["local"];
    for (auto& v : l.videos) if (v.id == it.id) { Status(L"Already in My playlist."); return; }
    l.videos.push_back({it.id, it.title, it.channel, it.dur});
    ScheduleSave();
    if (g.current == "local") RefreshLeft(true); else UpdateLeftHeader();
    Status(L"Added to My playlist: " + W(it.title));
}
static void ForgetList(const string& pid) {
    if (pid == "history") {
        if (g.hist.empty()) return;
        if (MessageBoxW(g_main, L"Clear the recently watched list?\n(Your watched ticks are kept.)", L"Mini YT", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        g.hist.clear(); ScheduleSave();
        if (g.current == "history") OpenHistory();
        if (A.browseMode && g_bkind == BK_MYLISTS) ShowMyLists();
        return;
    }
    if (pid == "local") {
        if (g.pls["local"].videos.empty()) return;
        if (MessageBoxW(g_main, L"Empty My playlist?", L"Mini YT", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        g.pls["local"].videos.clear();
    } else {
        if (MessageBoxW(g_main, L"Remove this playlist from your list?\n(Your watched ticks are kept.)", L"Mini YT", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        g.pls.erase(pid); g.recent.erase(std::remove(g.recent.begin(), g.recent.end(), pid), g.recent.end());
        if (g.current == pid) g.current = "local";
    }
    ScheduleSave(); RefreshLeft(); if (A.browseMode && g_bkind == BK_MYLISTS) ShowMyLists();
}
static void PlayUnwatched() {
    auto& v = Cur().videos;
    for (int i = 0; i < (int)v.size(); i++) if (!Watched(v[i].id)) { PlayFromList(g.current, i); return; }
    Status(L"Nothing left to watch in this playlist.");
}

// ------------------------------------------------------------ list events
static void OnListEvent(int ev, int listId, int idx) {
    if (ev == LE_WARM) {
        string id;
        if (listId == ID_LEFT) { auto& v = Cur().videos; if (idx >= 0 && idx < (int)v.size()) id = v[idx].id; }
        else if (idx >= 0 && idx < (int)g_bitems.size() && g_bitems[idx].kind == 0) id = g_bitems[idx].id;
        if (!id.empty() && !g_demo) WarmFormats(id);
        return;
    }
    if (listId == ID_LEFT) {
        auto& vids = Cur().videos;
        if (idx < 0 || idx >= (int)vids.size()) return;
        if (ev == LE_DBL) { PlayFromList(g.current, idx); return; }
        if (ev == LE_CHECK) { ToggleWatched(vids[idx].id); return; }
        HMENU m = CreatePopupMenu();
        AppendMenuW(m, MF_STRING, IDM_PLAY, L"Play");
        AppendMenuW(m, MF_STRING, IDM_TOGGLE, Watched(vids[idx].id) ? L"Mark as not watched" : L"Mark as watched");
        if (Cur().local) AppendMenuW(m, MF_STRING, IDM_REMOVE, L"Remove from My playlist");
        int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, A.left.ctxPt.x, A.left.ctxPt.y, 0, g_main, nullptr);
        DestroyMenu(m);
        if (c == IDM_PLAY) PlayFromList(g.current, idx);
        else if (c == IDM_TOGGLE) ToggleWatched(vids[idx].id);
        else if (c == IDM_REMOVE) { vids.erase(vids.begin() + idx); ScheduleSave(); RefreshLeft(true); }
        return;
    }
    if (idx < 0 || idx >= (int)g_bitems.size()) return;
    Item it = g_bitems[idx];
    if (ev == LE_CHECK) { if (it.kind == 0) ToggleWatched(it.id); return; }
    auto open = [&] { if (g_bkind == BK_MYLISTS) { if (it.id == "history") OpenHistory(); else { SetCurrent(it.id); RefreshLeft(); ShowVideo(); } } else OpenPlaylist(it.id, it.title); };
    if (ev == LE_DBL) { if (it.kind == 0) Play(it.id, "", -1, it.title); else open(); return; }
    HMENU m = CreatePopupMenu();
    if (it.kind == 0) {
        AppendMenuW(m, MF_STRING, IDM_PLAY, L"Play");
        AppendMenuW(m, MF_STRING, IDM_ADDLOCAL, L"Add to My playlist");
        AppendMenuW(m, MF_STRING, IDM_CHANNEL, L"This channel's playlists");
    } else {
        AppendMenuW(m, MF_STRING, IDM_OPEN, L"Open");
        if (g_bkind == BK_MYLISTS) AppendMenuW(m, MF_STRING, IDM_FORGET, it.id == "history" ? L"Clear history" : it.id == "local" ? L"Clear" : L"Forget");
    }
    int c = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, A.browse.ctxPt.x, A.browse.ctxPt.y, 0, g_main, nullptr);
    DestroyMenu(m);
    if (c == IDM_PLAY) Play(it.id, "", -1, it.title);
    else if (c == IDM_ADDLOCAL) AddLocal(it);
    else if (c == IDM_CHANNEL) ChannelPlaylists(it);
    else if (c == IDM_OPEN) open();
    else if (c == IDM_FORGET) ForgetList(it.id);
}

// ----------------------------------------------------------------- layout
static void LayoutBar(int x, int y, int w) {
    int r1 = S(26), tw = S(52), bh = S(28), by = y + r1 + S(4), gap = S(6);
    MoveWindow(A.stTime, x, y + S(5), tw, S(18), TRUE);
    MoveWindow(A.sbSeek, x + tw, y, std::max(10, w - 2 * tw), r1, TRUE);
    MoveWindow(A.stDur, x + w - tw, y + S(5), tw, S(18), TRUE);
    struct { HWND h; int w; } L[] = {{A.btnPrev, S(52)}, {A.btnPlay, S(64)}, {A.btnNext, S(52)}};
    int bx = x; for (auto& b : L) { MoveWindow(b.h, bx, by, b.w, bh, TRUE); bx += b.w + gap; }
    bx += gap;
    MoveWindow(A.btnSlow, bx, by, S(46), bh, TRUE); bx += S(46) + gap;
    MoveWindow(A.btnSpeed, bx, by, S(52), bh, TRUE); bx += S(52) + gap;
    MoveWindow(A.btnFast, bx, by, S(46), bh, TRUE);
    int fw = S(86), vw = S(84), mw = S(62);
    MoveWindow(A.btnFull, x + w - fw, by, fw, bh, TRUE);
    MoveWindow(A.sbVol, x + w - fw - gap - vw, by + S(2), vw, bh - S(2), TRUE);
    MoveWindow(A.btnMute, x + w - fw - gap - vw - gap - mw, by, mw, bh, TRUE);
}
static void Layout() {
    if (!A.video) return;
    RECT rc; GetClientRect(g_main, &rc); int Wd = rc.right, Ht = rc.bottom;
    int barH = S(66), pbH = S(5);
    HWND all[] = {A.btnSearch, A.edit, A.cbMode, A.cbQual, A.btnMy, A.stLeft, A.btnUnw, A.btnRef, A.left.h, A.stStatus, A.stBrowse, A.btnBack, A.browse.h};
    HWND bar[] = {A.stTime, A.sbSeek, A.stDur, A.btnPrev, A.btnPlay, A.btnNext, A.btnMute, A.sbVol, A.btnFull, A.btnSlow, A.btnSpeed, A.btnFast};
    for (HWND c : bar) ShowWindow(c, SW_SHOW);
    if (A.fullscreen) {
        for (HWND c : all) ShowWindow(c, SW_HIDE);
        MoveWindow(A.video, 0, 0, Wd, Ht - barH, TRUE); ShowWindow(A.video, SW_SHOW);
        MoveWindow(A.pb, 0, Ht - barH, Wd, S(4), TRUE); ShowWindow(A.pb, g_busy ? SW_SHOW : SW_HIDE);
        LayoutBar(S(8), Ht - barH + S(4), Wd - S(16)); return;
    }
    int m = S(8), topH = S(44), stH = S(22), hdrH = S(28), spl = S(6), ctlH = S(28);
    int leftW = g.leftW > 0 ? g.leftW : S(340);
    leftW = std::max(S(220), std::min(leftW, (int)(Wd * 6 / 10)));
    int bodyY = topH, bodyH = Ht - topH - stH - m, cy = (topH - ctlH) / 2;
    int myW = S(130), qW = S(104), mW = S(96);
    int myX = Wd - m - myW, qX = myX - m - qW, mX = qX - m - mW;
    for (HWND c : {A.btnSearch, A.edit, A.cbMode, A.cbQual, A.btnMy, A.stLeft, A.btnUnw, A.btnRef, A.left.h, A.stStatus}) ShowWindow(c, SW_SHOW);
    int sbW = S(84);
    MoveWindow(A.edit, m, cy, mX - 2 * m - sbW - S(2), ctlH, TRUE);
    MoveWindow(A.btnSearch, mX - m - sbW, cy, sbW, ctlH, TRUE);
    MoveWindow(A.cbMode, mX, cy, mW, S(220), TRUE);
    MoveWindow(A.cbQual, qX, cy, qW, S(220), TRUE);
    MoveWindow(A.btnMy, myX, cy, myW, ctlH, TRUE);
    int b1 = S(112), b2 = S(64), bh = hdrH - S(2);
    MoveWindow(A.stLeft, m, bodyY + S(5), leftW - b1 - b2 - S(8), hdrH - S(8), TRUE);
    MoveWindow(A.btnUnw, m + leftW - b1 - b2 - S(4), bodyY, b1, bh, TRUE);
    MoveWindow(A.btnRef, m + leftW - b2, bodyY, b2, bh, TRUE);
    MoveWindow(A.left.h, m, bodyY + hdrH, leftW, bodyH - hdrH, TRUE);
    int x0 = m + leftW + spl, rw = Wd - x0 - m;
    if (A.browseMode) {
        ShowWindow(A.video, SW_HIDE);
        for (HWND c : {A.stBrowse, A.btnBack, A.browse.h}) ShowWindow(c, SW_SHOW);
        MoveWindow(A.stBrowse, x0, bodyY + S(5), rw - S(150), hdrH - S(8), TRUE);
        MoveWindow(A.btnBack, x0 + rw - S(146), bodyY, S(146), bh, TRUE);
        MoveWindow(A.browse.h, x0, bodyY + hdrH, rw, bodyH - hdrH - barH - pbH, TRUE);
    } else {
        for (HWND c : {A.stBrowse, A.btnBack, A.browse.h}) ShowWindow(c, SW_HIDE);
        MoveWindow(A.video, x0, bodyY, rw, bodyH - barH - pbH, TRUE); ShowWindow(A.video, SW_SHOW);
    }
    MoveWindow(A.pb, x0, bodyY + bodyH - barH - pbH, rw, pbH, TRUE); ShowWindow(A.pb, g_busy ? SW_SHOW : SW_HIDE);
    LayoutBar(x0, bodyY + bodyH - barH + S(4), rw);
    MoveWindow(A.stStatus, m, Ht - stH, Wd - 2 * m, stH - S(2), TRUE);
}
static bool InSplitter(int x, int y) {
    RECT rc; GetClientRect(g_main, &rc);
    int m = S(8), leftW = g.leftW > 0 ? g.leftW : S(340);
    leftW = std::max(S(220), std::min(leftW, (int)(rc.right * 6 / 10)));
    return !A.fullscreen && x >= m + leftW && x < m + leftW + S(6) && y > S(44) && y < rc.bottom - S(22);
}

static LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_KEYDOWN && wp == VK_RETURN) { PostMessageW(GetParent(h), WM_COMMAND, ID_GO, 0); return 0; }
    if (m == WM_KEYDOWN && wp == VK_ESCAPE) { SetWindowTextW(h, L""); return 0; }
    if (m == WM_CHAR && wp == '\r') return 0;
    if (m == WM_CHAR && wp == 1) { SendMessageW(h, EM_SETSEL, 0, -1); return 0; }
    return DefSubclassProc(h, m, wp, lp);
}
static LRESULT CALLBACK TrackProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_LBUTTONDOWN) {   // click anywhere on the bar jumps there (then you can keep dragging)
        RECT th, ch; SendMessageW(h, TBM_GETTHUMBRECT, 0, (LPARAM)&th); SendMessageW(h, TBM_GETCHANNELRECT, 0, (LPARAM)&ch);
        POINT p = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!PtInRect(&th, p)) {
            int lo = (int)SendMessageW(h, TBM_GETRANGEMIN, 0, 0), hi = (int)SendMessageW(h, TBM_GETRANGEMAX, 0, 0);
            int tw = th.right - th.left; double span = std::max(1, (int)(ch.right - ch.left - tw));
            double f = (p.x - ch.left - tw / 2.0) / span; f = std::max(0.0, std::min(1.0, f));
            SendMessageW(h, TBM_SETPOS, TRUE, (LPARAM)(lo + f * (hi - lo)));
        }
    }
    return DefSubclassProc(h, m, wp, lp);
}
static LRESULT CALLBACK VideoProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_ERASEBKGND) { RECT r; GetClientRect(h, &r); FillRect((HDC)wp, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    if (m == WM_SIZE) { HWND c = GetWindow(h, GW_CHILD); if (c) SetWindowPos(c, nullptr, 0, 0, LOWORD(lp), HIWORD(lp), SWP_NOZORDER | SWP_NOACTIVATE); }
    return DefWindowProcW(h, m, wp, lp);
}

static void CreateUI(HWND hw) {
    auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int id, DWORD ex = 0) {
        HWND h = CreateWindowExW(ex, cls, txt, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, hw, (HMENU)(INT_PTR)id, g_hi, nullptr);
        SendMessageW(h, WM_SETFONT, (WPARAM)g_f, TRUE); return h;
    };
    A.edit = mk(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, ID_EDIT, WS_EX_CLIENTEDGE);
    SendMessageW(A.edit, MY_EM_SETCUEBANNER, TRUE, (LPARAM)L"Search YouTube  (or paste a channel / playlist link)");
    SetWindowSubclass(A.edit, EditProc, 1, 0);
    A.btnSearch = mk(L"BUTTON", L"Search", BS_PUSHBUTTON | WS_TABSTOP, ID_GO);
    A.cbMode = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_MODE);
    SendMessageW(A.cbMode, CB_ADDSTRING, 0, (LPARAM)L"All"); SendMessageW(A.cbMode, CB_ADDSTRING, 0, (LPARAM)L"Videos"); SendMessageW(A.cbMode, CB_ADDSTRING, 0, (LPARAM)L"Playlists");
    SendMessageW(A.cbMode, CB_SETCURSEL, 0, 0);
    A.cbQual = mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, ID_QUAL);
    for (const wchar_t* q : {L"360p", L"480p", L"720p", L"1080p", L"Audio only"}) SendMessageW(A.cbQual, CB_ADDSTRING, 0, (LPARAM)q);
    SendMessageW(A.cbQual, CB_SETCURSEL, g.quality, 0);
    A.btnMy = mk(L"BUTTON", L"My playlists", BS_OWNERDRAW | WS_TABSTOP, ID_MY);
    A.stLeft = mk(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_LTITLE);
    SendMessageW(A.stLeft, WM_SETFONT, (WPARAM)g_fb, TRUE);
    A.btnUnw = mk(L"BUTTON", L"Home Page", BS_PUSHBUTTON | WS_TABSTOP, ID_UNW);
    A.btnRef = mk(L"BUTTON", L"Refresh", BS_PUSHBUTTON | WS_TABSTOP, ID_REF);
    A.stBrowse = mk(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_BTITLE);
    SendMessageW(A.stBrowse, WM_SETFONT, (WPARAM)g_fb, TRUE);
    A.btnBack = mk(L"BUTTON", L"< Back to video", BS_PUSHBUTTON | WS_TABSTOP, ID_BACK);
    A.stStatus = mk(L"STATIC", L"Starting ...", SS_ENDELLIPSIS | SS_NOPREFIX, ID_STATUS);
    A.stTime = mk(L"STATIC", L"0:00", SS_CENTER | SS_NOPREFIX, ID_TIME);
    A.stDur = mk(L"STATIC", L"--:--", SS_CENTER | SS_NOPREFIX, ID_DUR);
    A.sbSeek = mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS | TBS_HORZ | WS_TABSTOP, ID_SEEK);
    SendMessageW(A.sbSeek, TBM_SETRANGE, TRUE, MAKELPARAM(0, 1000)); SendMessageW(A.sbSeek, TBM_SETPAGESIZE, 0, 50);
    SetWindowSubclass(A.sbSeek, TrackProc, 2, 0);
    A.sbVol = mk(TRACKBAR_CLASSW, L"", TBS_NOTICKS | TBS_HORZ | WS_TABSTOP, ID_VOL);
    SendMessageW(A.sbVol, TBM_SETRANGE, TRUE, MAKELPARAM(0, 100)); SendMessageW(A.sbVol, TBM_SETPAGESIZE, 0, 10);
    SendMessageW(A.sbVol, TBM_SETPOS, TRUE, g.volume); SetWindowSubclass(A.sbVol, TrackProc, 3, 0);
    A.btnPrev = mk(L"BUTTON", L"Prev", BS_PUSHBUTTON | WS_TABSTOP, ID_PREV);
    A.btnPlay = mk(L"BUTTON", L"Play", BS_OWNERDRAW | WS_TABSTOP, ID_PLAY);
    A.btnNext = mk(L"BUTTON", L"Next", BS_PUSHBUTTON | WS_TABSTOP, ID_NEXT);
    A.btnMute = mk(L"BUTTON", L"Mute", BS_PUSHBUTTON | WS_TABSTOP, ID_MUTE);
    A.btnFull = mk(L"BUTTON", L"Fullscreen", BS_PUSHBUTTON | WS_TABSTOP, ID_FULL);
    A.btnSlow = mk(L"BUTTON", L"Slow", BS_PUSHBUTTON | WS_TABSTOP, ID_SLOW);
    A.btnSpeed = mk(L"BUTTON", L"1.0x", BS_PUSHBUTTON | WS_TABSTOP, ID_SPEED);
    A.btnFast = mk(L"BUTTON", L"Fast", BS_PUSHBUTTON | WS_TABSTOP, ID_FAST);
    A.pb = mk(PROGRESS_CLASSW, L"", PBS_SMOOTH, ID_PB); ShowWindow(A.pb, SW_HIDE);
    A.video = CreateWindowExW(0, L"MiniYTVideo", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 10, 10, hw, nullptr, g_hi, nullptr);
    A.left.Create(hw, ID_LEFT, false);
    A.browse.Create(hw, ID_BROWSE, true);
}

static LRESULT CALLBACK MainProc(HWND hw, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
        case WM_CREATE: g_main = hw; CreateUI(hw); SetTimer(hw, 2, 500, nullptr); return 0;
        case WM_SIZE: if (wp != SIZE_MINIMIZED) Layout(); return 0;
        case WM_GETMINMAXINFO: { MINMAXINFO* mm = (MINMAXINFO*)lp; mm->ptMinTrackSize.x = S(1020); mm->ptMinTrackSize.y = S(460); return 0; }
        case WM_RUN: { auto* f = (std::function<void()>*)lp; (*f)(); delete f; return 0; }
        case WM_THUMB: g_thumbDirty = false; InvalidateRect(A.left.h, nullptr, FALSE); InvalidateRect(A.browse.h, nullptr, FALSE); return 0;
        case WM_TIMER: if (wp == 1) { KillTimer(hw, 1); SaveState(); } else if (wp == 2) TickPlayer(); return 0;
        case WM_LISTEV: OnListEvent(LOWORD(wp), HIWORD(wp), (int)lp); return 0;
        case WM_HSCROLL: {
            HWND src = (HWND)lp; int code = LOWORD(wp);
            if (src == A.sbSeek) {
                double d = g_dur; int pos = (int)SendMessageW(A.sbSeek, TBM_GETPOS, 0, 0);
                if (code == TB_THUMBTRACK) { g_seeking = true; SetText(A.stTime, FmtTime(pos / 1000.0 * d)); }
                else if (code == TB_ENDTRACK) {
                    g_seeking = false;
                    if (d > 0 && g_fileReady && !g_idle && !P.vid.empty()) {
                        char b[96]; snprintf(b, sizeof b, "{\"command\":[\"seek\",%.2f,\"absolute\"]}", pos / 1000.0 * d);
                        MpvSend(b); g_timePos = pos / 1000.0 * d; PostCtl();
                    }
                }
            } else if (src == A.sbVol) {
                int v = (int)SendMessageW(A.sbVol, TBM_GETPOS, 0, 0); g.volume = v; ScheduleSave();
                char b[96]; snprintf(b, sizeof b, "{\"command\":[\"set_property\",\"volume\",%d]}", v); MpvSend(b);
            }
            return 0;
        }
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case ID_GO: SearchFromEdit(); return 0;
                case ID_MY: ShowMyLists(); return 0;
                case ID_BACK: ShowVideo(); return 0;
                case ID_PREV: PlayPrev(); return 0;
                case ID_PLAY: TogglePause(); return 0;
                case ID_NEXT: PlayNext(); return 0;
                case ID_MUTE: MpvSend("{\"command\":[\"cycle\",\"mute\"]}"); return 0;
                case ID_FULL: ToggleFullscreen(); return 0;
                case ID_SLOW: ChangeSpeed(-1); return 0;
                case ID_FAST: ChangeSpeed(1); return 0;
                case ID_SPEED: ChangeSpeed(0); return 0;
                case ID_UNW: ShowHome(); return 0;
                case ID_REF: if (!Cur().local && !IsTemp(g.current)) OpenPlaylist(g.current, Cur().title); return 0;
                case ID_QUAL:
                    if (HIWORD(wp) == CBN_SELCHANGE) {
                        g.quality = (int)SendMessageW(A.cbQual, CB_GETCURSEL, 0, 0); ScheduleSave();
                        if (!P.vid.empty() && g_mpvReady) Play(P.vid, P.pl, P.idx, P.title, g_fileReady ? (double)g_timePos : 0.0);   // same video, new quality, same position
                    }
                    return 0;
            }
            break;
        case WM_DRAWITEM: {
            DRAWITEMSTRUCT* d = (DRAWITEMSTRUCT*)lp;
            if (d->CtlID == ID_PLAY) {
                bool active = !P.vid.empty() && !g_idle; int ps = !active ? 0 : g_paused ? 2 : 1;
                COLORREF col = ps == 1 ? RGB(130, 210, 150) : ps == 2 ? RGB(240, 140, 140) : RGB(190, 190, 190);
                bool down = (d->itemState & ODS_SELECTED) != 0;
                HBRUSH b = CreateSolidBrush(down ? RGB(232, 232, 232) : RGB(255, 255, 255)); FillRect(d->hDC, &d->rcItem, b); DeleteObject(b);
                HBRUSH fb = CreateSolidBrush(col); RECT r = d->rcItem;
                for (int i = 0; i < std::max(2, S(2)); i++) { FrameRect(d->hDC, &r, fb); InflateRect(&r, -1, -1); }
                DeleteObject(fb);
                wchar_t tx[32]; GetWindowTextW(d->hwndItem, tx, 32);
                SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, RGB(20, 20, 20)); SelectObject(d->hDC, g_f);
                DrawTextW(d->hDC, tx, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            if (d->CtlID == ID_MY) {
                bool down = (d->itemState & ODS_SELECTED) != 0;
                HBRUSH b = CreateSolidBrush(down ? RGB(230, 170, 0) : RGB(255, 192, 0)); FillRect(d->hDC, &d->rcItem, b); DeleteObject(b);
                HBRUSH fb = CreateSolidBrush(RGB(191, 144, 0)); FrameRect(d->hDC, &d->rcItem, fb); DeleteObject(fb);
                SetBkMode(d->hDC, TRANSPARENT); SetTextColor(d->hDC, RGB(40, 30, 0)); SelectObject(d->hDC, g_fb);
                DrawTextW(d->hDC, L"My playlists", -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
                return TRUE;
            }
            break;
        }
        case WM_LBUTTONDOWN: if (InSplitter(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) { A.dragging = true; SetCapture(hw); } return 0;
        case WM_MOUSEMOVE:
            if (A.dragging) { g.leftW = GET_X_LPARAM(lp) - S(8) - S(3); Layout(); }
            return 0;
        case WM_LBUTTONUP: if (A.dragging) { A.dragging = false; ReleaseCapture(); ScheduleSave(); } return 0;
        case WM_SETCURSOR: {
            POINT p; GetCursorPos(&p); ScreenToClient(hw, &p);
            if (A.dragging || InSplitter(p.x, p.y)) { SetCursor(LoadCursorW(nullptr, IDC_SIZEWE)); return TRUE; }
            break;
        }
        case WM_CLOSE: SaveState(); MpvQuit(); DestroyWindow(hw); return 0;
        case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hw, m, wp, lp);
}

// ------------------------------------------------------------- first-run setup
static string FindMpvAsset(const string& html) {
    size_t pos = 0;
    while ((pos = html.find("href=\"", pos)) != string::npos) {
        pos += 6; size_t e = html.find('"', pos); if (e == string::npos) break;
        string h = html.substr(pos, e - pos); pos = e;
        size_t k = h.find("/mpv-x86_64-");   // plain build (not -v3: that one needs a newer CPU)
        if (k == string::npos || h.size() < 4 || h.substr(h.size() - 3) != ".7z") continue;
        if (k + 12 >= h.size() || !isdigit((unsigned char)h[k + 12])) continue;
        return h;
    }
    return "";
}
static void SetupFail(const wchar_t* why) {
    ProgressUI(-2);
    StatusUI(wstring(why) + L" Copy mpv.exe next to MiniYT.exe, or check the internet connection and restart.");
}
static void SetupThread() {
    CreateDirectoryW(g_data.c_str(), nullptr);
    if (FindTool(L"mpv").empty()) {
        ProgressUI(-1); StatusUI(L"First start: preparing the video player ...");
        string html; wstring fin;
        if (!HttpGetStr(L"https://github.com/shinchiro/mpv-winbuild-cmake/releases/latest", html, &fin)) { SetupFail(L"No internet?"); return; }
        wstring tag = fin.substr(fin.rfind(L'/') + 1);
        if (!HttpGetStr(L"https://github.com/shinchiro/mpv-winbuild-cmake/releases/expanded_assets/" + tag, html)) { SetupFail(L"Could not reach the player list."); return; }
        string href = FindMpvAsset(html);
        if (href.empty()) { SetupFail(L"Could not find the player download."); return; }
        wstring arc = Join(g_data, L"mpv.7z");
        if (!DownloadFile(L"https://github.com" + W(href), arc, [](int p) {
                ProgressUI(p); StatusUI(L"First start: downloading the video player ... " + std::to_wstring(p) + L"%"); })) {
            SetupFail(L"Download failed."); return;
        }
        ProgressUI(-1); StatusUI(L"First start: unpacking the video player ...");
        CreateDirectoryW(g_mpvDir.c_str(), nullptr);
        RunHidden(L"tar -xf \"" + arc + L"\" -C \"" + g_mpvDir + L"\"");
        if (!FileExists(Join(g_mpvDir, L"mpv.exe"))) {
            wstring sz = Join(g_data, L"7zr.exe");
            if (!FileExists(sz)) DownloadFile(L"https://www.7-zip.org/a/7zr.exe", sz);
            RunHidden(L"\"" + sz + L"\" x -y -o\"" + g_mpvDir + L"\" \"" + arc + L"\"");
        }
        if (!FileExists(Join(g_mpvDir, L"mpv.exe"))) { SetupFail(L"Could not unpack the player."); return; }
        DeleteFileW(arc.c_str());
    }
    RunOnUI([] { ProgressSet(-2); SpawnMpv(); Status(kReadyText); });
    // backup helper (only used if YouTube changes its internal API); best effort, silent
    if (FindTool(L"yt-dlp").empty())
        DownloadFile(L"https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe", Join(g_data, L"yt-dlp.exe"));
}

// --------------------------------------------------------------------- main
static void LoadDemo() {
    g_demo = true;
    Playlist pl; pl.id = "demo"; pl.title = "CCNA 200-301 Complete Course"; pl.channel = "Jeremy's IT Lab";
    const char* names[] = {"Welcome to the course", "Network Devices", "Interfaces and Cables", "OSI Model & TCP/IP Suite",
        "Ethernet LAN Switching Part 1", "Ethernet LAN Switching Part 2", "IPv4 Addressing Part 1", "IPv4 Addressing Part 2 and a much longer title that wraps",
        "Subnetting (Part 1)", "DHCP", "VLANs (Part 1)", "VLANs (Part 2)"};
    for (int i = 0; i < 12; i++) pl.videos.push_back({"demo" + std::to_string(100 + i), string("Free CCNA | ") + names[i] + " | Day " + std::to_string(i + 1), "Jeremy's IT Lab", "30:" + std::to_string(10 + i)});
    g.pls["demo"] = pl; g.current = "demo"; g.recent = {"demo"};
    g.watched = {"demo100", "demo101", "demo102"};
    P.vid = "demo104"; P.pl = "demo"; P.idx = 4; P.title = pl.videos[4].title;
    RefreshLeft();
    vector<Item> items;
    for (int i = 0; i < 8; i++) { Item it; it.kind = 0; it.id = "demoRes" + std::to_string(i); it.title = "Jeremy's IT Lab video number " + std::to_string(i + 1) + " - Networking explained"; it.channel = "Jeremy's IT Lab"; it.dur = "12:3" + std::to_string(i); it.thumb = "demo:" + it.id; if (i % 3 == 1) { it.kind = 1; it.title = "Playlist: CCNA course part " + std::to_string(i); it.count = "42 videos"; } items.push_back(it); }
    ShowBrowse(L"Videos: jeremy it lab", std::move(items), BK_RESULTS);
    g_idle = false; if (GetEnvironmentVariableW(L"MINIYT_DEMO_PAUSED", nullptr, 0)) g_paused = true;
    for (int i = 0; i < 3; i++) g.hist.push_back({"demo" + std::to_string(100 + i), "Free CCNA | Demo " + std::to_string(i + 1), "Jeremy's IT Lab", "30:1" + std::to_string(i), (long long)time(nullptr) - 3600 * (i + 1)});
    PostCtl();
    Status(L"Demo mode (offline sample data).");
}

#ifndef MINIYT_TEST
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int show) {
    g_hi = hi; DetectRegion();
    SetProcessDPIAware();
    HDC sd = GetDC(nullptr); g_dpi = GetDeviceCaps(sd, LOGPIXELSX); ReleaseDC(nullptr, sd);
    bool demo = cmd && wcsstr(cmd, L"/demo");
    wchar_t ex[MAX_PATH]; GetModuleFileNameW(nullptr, ex, MAX_PATH);
    g_exeDir = ex; g_exeDir = g_exeDir.substr(0, g_exeDir.rfind(L'\\'));
    wchar_t la[MAX_PATH]; DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    g_data = demo ? Join(_wgetenv(L"TEMP") ? _wgetenv(L"TEMP") : L"C:\\Windows\\Temp", L"MiniYT-demo") : Join(n ? la : g_exeDir, L"MiniYT");
    g_mpvDir = Join(g_data, L"mpv");
    CreateDirectoryW(g_data.c_str(), nullptr);
    { wchar_t path[32768]; DWORD k = GetEnvironmentVariableW(L"PATH", path, 32768);
      wstring np = g_data + L";" + g_mpvDir + L";" + g_exeDir + L";" + (k ? wstring(path) : L"");
      SetEnvironmentVariableW(L"PATH", np.c_str()); }

    INITCOMMONCONTROLSEX ic = {sizeof ic, ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES | ICC_BAR_CLASSES | ICC_PROGRESS_CLASS}; InitCommonControlsEx(&ic);
    ULONG_PTR gdiTok; Gdiplus::GdiplusStartupInput gsi; Gdiplus::GdiplusStartup(&gdiTok, &gsi, nullptr);
    HttpInit();
    LoadState();
    g_f = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fb = CreateFontW(-MulDiv(9, g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");

    WNDCLASSEXW wc; memset(&wc, 0, sizeof wc); wc.cbSize = sizeof wc; wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = ListProc; wc.lpszClassName = kListCls; wc.style = CS_DBLCLKS; RegisterClassExW(&wc);
    wc.lpfnWndProc = VideoProc; wc.lpszClassName = L"MiniYTVideo"; wc.style = 0; RegisterClassExW(&wc);
    wc.lpfnWndProc = MainProc; wc.lpszClassName = L"MiniYTMain"; wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(100), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0);
    wc.hIconSm = (HICON)LoadImageW(hi, MAKEINTRESOURCEW(100), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    for (int i = 0; i < 3; i++) std::thread(ThumbWorker).detach();
    HWND hw = CreateWindowExW(0, L"MiniYTMain", L"Mini YT", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                              CW_USEDEFAULT, CW_USEDEFAULT, S(1180), S(700), nullptr, nullptr, hi, nullptr);
    if (!hw) return 1;
    Layout();
    ShowWindow(hw, show); UpdateWindow(hw);
    if (demo) LoadDemo();
    else { RefreshLeft(); std::thread(SetupThread).detach(); }
    Status(demo ? L"Demo mode (offline sample data)." : L"Starting ...");

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN) {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (msg.wParam == 'L' && ctrl) { SetFocus(A.edit); SendMessageW(A.edit, EM_SETSEL, 0, -1); continue; }
            if (msg.wParam == VK_F11) { ToggleFullscreen(); continue; }
            if (msg.wParam == VK_ESCAPE && A.fullscreen) { ToggleFullscreen(); continue; }
        }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    Gdiplus::GdiplusShutdown(gdiTok);
    return 0;
}
#endif
