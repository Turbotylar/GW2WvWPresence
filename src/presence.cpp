// GW2 WvW Presence: a Nexus addon that shows your WvW player kills and next Realm Avenger tier
// as Discord Rich Presence ("Playing ..." status) while you're in game.
//
// It talks to the Discord desktop app over its local pipe (\\.\pipe\discord-ipc-N), the same way games
// do. No bot token is needed: only an application ID (public) and your GW2 API key.
//
// Kills: every Realm Avenger achievement (I to X) reads the same kill counter, and finished ones keep
// counting past their max, so the highest "current" across the chain is the true total.
//
// The GW2 API key is encrypted with Windows' per-user encryption (DPAPI) and kept in
// %APPDATA%\GW2WvWPresence\secrets.bin, not in the game folder.
#include <windows.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>

#include <map>

#include "nexus/Nexus.h"
#include "imgui/imgui.h"
#include "mumble_link.h"

static AddonAPI* API = nullptr;
static AddonDefinition Def{};

/* ---- settings (not secret) ------------------------------------------------------------------- */

struct Settings
{
    bool enabled = true;
    bool showTimer = true;
    bool showTier = true;
    bool showChar = true;
    bool showSpec = true;
    bool showMap = true;
    bool classIcon = true;
    int interval = 60; /* seconds between GW2 API checks */
    char clientId[32] = "1573296519287668879";
    char largeImage[64] = "gw2";
};

static Settings Cfg;
static std::string SettingsPath;
static std::string SecretsPath;

/* ---- shared state ---------------------------------------------------------------------------- */

static std::mutex Lock;
static std::condition_variable Wake;
static std::thread Worker;
static bool Stopping = false;
static bool Dirty = true; /* settings changed, resend presence */
static bool KeyChanged = false;
static std::string ApiKey;
static std::string Status = "Starting...";
static std::string DiscordStatus = "Not connected";
static long long Kills = -1, Goal = -1;
static std::string TierName;
static time_t LastCheck = 0, SessionStart = 0;

/* Realm Avenger chain (World vs World category). Tier I has its own small tiers, II-X are one tier each. */
static const char* CHAIN_IDS = "283,7911,7855,7932,7870,7906,7924,7858,7912,7885";
struct Tier { long long count; const char* name; };
static const Tier TIERS[] = {
    { 1, "Realm Avenger I" }, { 1000, "Realm Avenger I" }, { 3000, "Realm Avenger I" }, { 10000, "Realm Avenger I" },
    { 50000, "Realm Avenger I" }, { 250000, "Realm Avenger I" }, { 500000, "Realm Avenger II" },
    { 750000, "Realm Avenger III" }, { 1000000, "Realm Avenger IV" }, { 1250000, "Realm Avenger V" },
    { 1500000, "Realm Avenger VI" }, { 1750000, "Realm Avenger VII" }, { 2000000, "Realm Avenger VIII" },
    { 2250000, "Realm Avenger IX" }, { 2500000, "Realm Avenger X" },
};

static long long NextGoal(long long kills, std::string& name)
{
    for (const Tier& t : TIERS)
    {
        if (kills < t.count) { name = t.name; return t.count; }
    }
    name = "Past Realm Avenger X";
    return ((kills / 250000) + 1) * 250000;
}

static void SetStatus(const std::string& s) { std::lock_guard<std::mutex> g(Lock); Status = s; }
static void SetDiscord(const std::string& s) { std::lock_guard<std::mutex> g(Lock); DiscordStatus = s; }

static std::string Commas(long long v)
{
    std::string s = std::to_string(v < 0 ? -v : v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) { s.insert(i, ","); }
    return v < 0 ? "-" + s : s;
}

static std::string JsonEscape(const std::string& s)
{
    std::string o;
    for (char c : s)
    {
        if (c == '"' || c == '\\') { o += '\\'; o += c; }
        else if ((unsigned char)c < 0x20) { o += ' '; }
        else { o += c; }
    }
    return o;
}

/* ---- settings file --------------------------------------------------------------------------- */

static void LoadSettings()
{
    FILE* f = fopen(SettingsPath.c_str(), "r");
    if (!f) { return; }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq) { continue; }
        *eq = 0;
        char* v = eq + 1;
        v[strcspn(v, "\r\n")] = 0;
        if (!strcmp(line, "enabled")) { Cfg.enabled = atoi(v) != 0; }
        else if (!strcmp(line, "timer")) { Cfg.showTimer = atoi(v) != 0; }
        else if (!strcmp(line, "tier")) { Cfg.showTier = atoi(v) != 0; }
        else if (!strcmp(line, "char")) { Cfg.showChar = atoi(v) != 0; }
        else if (!strcmp(line, "spec")) { Cfg.showSpec = atoi(v) != 0; }
        else if (!strcmp(line, "map")) { Cfg.showMap = atoi(v) != 0; }
        else if (!strcmp(line, "icon")) { Cfg.classIcon = atoi(v) != 0; }
        else if (!strcmp(line, "interval")) { Cfg.interval = atoi(v); }
        else if (!strcmp(line, "app")) { strncpy(Cfg.clientId, v, sizeof(Cfg.clientId) - 1); }
        else if (!strcmp(line, "image")) { strncpy(Cfg.largeImage, v, sizeof(Cfg.largeImage) - 1); }
    }
    fclose(f);
    if (Cfg.interval < 30) { Cfg.interval = 30; }
    /* move anyone still on an older default app to the current one */
    if (!strcmp(Cfg.clientId, "1524018255650361477") || !strcmp(Cfg.clientId, "1573289596710690887"))
    {
        strcpy(Cfg.clientId, "1573296519287668879");
    }
}

static void SaveSettings()
{
    FILE* f = fopen(SettingsPath.c_str(), "w");
    if (!f) { return; }
    fprintf(f, "enabled=%d\ntimer=%d\ntier=%d\nchar=%d\nspec=%d\nmap=%d\nicon=%d\ninterval=%d\napp=%s\nimage=%s\n",
            Cfg.enabled ? 1 : 0, Cfg.showTimer ? 1 : 0, Cfg.showTier ? 1 : 0, Cfg.showChar ? 1 : 0, Cfg.showSpec ? 1 : 0,
            Cfg.showMap ? 1 : 0, Cfg.classIcon ? 1 : 0, Cfg.interval, Cfg.clientId, Cfg.largeImage);
    fclose(f);
}

/* ---- secret (DPAPI) -------------------------------------------------------------------------- */

static void LoadSecret()
{
    FILE* f = fopen(SecretsPath.c_str(), "rb");
    if (!f) { return; }
    std::string blob;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) { blob.append(buf, n); }
    fclose(f);
    DATA_BLOB in{ (DWORD)blob.size(), (BYTE*)&blob[0] }, out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) { return; }
    std::lock_guard<std::mutex> g(Lock);
    ApiKey.assign((char*)out.pbData, out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
}

static bool SaveSecret(std::string key)
{
    DATA_BLOB in{ (DWORD)key.size(), (BYTE*)&key[0] }, out{};
    BOOL ok = CryptProtectData(&in, L"GW2WvWPresence", nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out);
    if (!ok) { return false; }
    FILE* f = fopen(SecretsPath.c_str(), "wb");
    bool written = f && fwrite(out.pbData, 1, out.cbData, f) == out.cbData;
    if (f) { fclose(f); }
    LocalFree(out.pbData);
    return written;
}

/* ---- GW2 API --------------------------------------------------------------------------------- */

static DWORD HttpGet(const std::wstring& path, const std::string& key, std::string& out)
{
    out.clear();
    DWORD status = 0;
    HINTERNET session = WinHttpOpen(L"GW2WvWPresence/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { return 0; }
    WinHttpSetTimeouts(session, 5000, 5000, 5000, 8000);
    HINTERNET connect = WinHttpConnect(session, L"api.guildwars2.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect ? WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::wstring header = key.empty() ? L"" : L"Authorization: Bearer " + std::wstring(key.begin(), key.end()) + L"\r\n";
    if (request && WinHttpSendRequest(request, header.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header.c_str(), (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr))
    {
        DWORD size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0)
        {
            std::string chunk(available, 0);
            DWORD read = 0;
            if (!WinHttpReadData(request, &chunk[0], available, &read) || read == 0) { break; }
            out.append(chunk.data(), read);
        }
    }
    if (!header.empty()) { SecureZeroMemory(&header[0], header.size() * sizeof(wchar_t)); }
    if (request) { WinHttpCloseHandle(request); }
    if (connect) { WinHttpCloseHandle(connect); }
    WinHttpCloseHandle(session);
    return status;
}

static std::string JsonText(const std::string& s, const char* name)
{
    std::string k = std::string("\"") + name + "\"";
    size_t p = s.find(k);
    if (p == std::string::npos) { return ""; }
    p = s.find('"', s.find(':', p + k.size()));
    if (p == std::string::npos) { return ""; }
    size_t e = s.find('"', p + 1);
    return e == std::string::npos ? "" : s.substr(p + 1, e - p - 1);
}

/* returns true if Kills/Goal changed */
static bool PollKills()
{
    std::string key;
    {
        std::lock_guard<std::mutex> g(Lock);
        key = ApiKey;
    }
    if (key.empty()) { SetStatus("Enter your GW2 API key in the options."); return false; }

    std::string body;
    std::wstring path = L"/v2/account/achievements?ids=";
    for (const char* c = CHAIN_IDS; *c; ++c) { path += (wchar_t)*c; }
    DWORD st = HttpGet(path, key, body);
    SecureZeroMemory(&key[0], key.size());
    {
        std::lock_guard<std::mutex> g(Lock);
        LastCheck = time(nullptr);
    }
    std::string text = JsonText(body, "text");
    bool noKills = st >= 400 && text.find("invalid") != std::string::npos;
    if (st != 200 && st != 206 && !noKills)
    {
        char msg[256];
        if (st == 0) { snprintf(msg, sizeof(msg), "GW2 API unreachable, retrying."); }
        else { snprintf(msg, sizeof(msg), "GW2 API error %lu%s%s", st, text.empty() ? "" : ": ", text.c_str()); }
        SetStatus(msg);
        return false;
    }
    long long current = 0;
    for (size_t p = body.find("\"current\""); !noKills && p != std::string::npos; p = body.find("\"current\"", p + 9))
    {
        size_t colon = body.find(':', p);
        if (colon == std::string::npos) { break; }
        long long v = atoll(body.c_str() + colon + 1);
        if (v > current) { current = v; }
    }
    std::string tier;
    long long goal = NextGoal(current, tier);
    std::lock_guard<std::mutex> g(Lock);
    Status = "Kills up to date.";
    bool changed = current != Kills || goal != Goal;
    Kills = current;
    Goal = goal;
    TierName = tier;
    return changed;
}

/* ---- character and map (MumbleLink via Nexus, names from the public GW2 API) ----------------- */

static Mumble::Identity* MumbleIdentity = nullptr;

struct Where
{
    std::string name, spec, map, icon;
    bool wvw = false;
    bool operator!=(const Where& o) const { return name != o.name || spec != o.spec || map != o.map || icon != o.icon; }
};

struct SpecInfo { std::string name, icon; bool elite = false; bool ok = false; };
static std::map<unsigned, std::string> MapNames;
static std::map<unsigned, SpecInfo> Specs;

static const char* ProfName(Mumble::EProfession p)
{
    static const char* n[] = { "", "Guardian", "Warrior", "Engineer", "Ranger", "Thief", "Elementalist", "Mesmer", "Necromancer", "Revenant" };
    unsigned i = (unsigned)p;
    return i < 10 ? n[i] : "";
}

static std::string Unescape(std::string s)
{
    std::string o;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if (s[i] == '\\' && i + 1 < s.size()) { ++i; o += s[i] == '/' ? '/' : s[i]; }
        else { o += s[i]; }
    }
    return o;
}

static std::string MapName(unsigned id)
{
    auto it = MapNames.find(id);
    if (it != MapNames.end()) { return it->second; }
    std::string body;
    DWORD st = HttpGet(L"/v2/maps/" + std::to_wstring(id), "", body);
    std::string name = st == 200 ? Unescape(JsonText(body, "name")) : "";
    if (st == 200 || st == 404) { MapNames[id] = name; } /* don't cache network failures */
    return name;
}

static SpecInfo Spec(unsigned id)
{
    auto it = Specs.find(id);
    if (it != Specs.end()) { return it->second; }
    SpecInfo info;
    std::string body;
    DWORD st = HttpGet(L"/v2/specializations/" + std::to_wstring(id), "", body);
    if (st == 200)
    {
        info.ok = true;
        info.name = Unescape(JsonText(body, "name"));
        info.elite = body.find("\"elite\":true") != std::string::npos;
        info.icon = Unescape(JsonText(body, "profession_icon_big"));
    }
    if (st == 200 || st == 404) { Specs[id] = info; }
    return info;
}

static std::string ProfIcon(Mumble::EProfession p)
{
    static std::map<unsigned, std::string> cache;
    unsigned i = (unsigned)p;
    auto it = cache.find(i);
    if (it != cache.end()) { return it->second; }
    std::string body, n = ProfName(p);
    if (n.empty()) { return ""; }
    DWORD st = HttpGet(L"/v2/professions/" + std::wstring(n.begin(), n.end()) + L"?v=2019-12-19T00:00:00.000Z", "", body);
    std::string icon = st == 200 ? Unescape(JsonText(body, "icon_big")) : "";
    if (st == 200) { cache[i] = icon; }
    return icon;
}

static Mumble::Data* MumbleData = nullptr;

static unsigned WNum(const std::wstring& j, const wchar_t* key)
{
    size_t p = j.find(key);
    if (p == std::wstring::npos) { return 0; }
    p = j.find(L':', p);
    return p == std::wstring::npos ? 0 : (unsigned)wcstoul(j.c_str() + p + 1, nullptr, 10);
}

static Where ReadWhere()
{
    Where w;
    if (!MumbleIdentity && API) { MumbleIdentity = (Mumble::Identity*)API->DataLink.Get("DL_MUMBLE_LINK_IDENTITY"); }
    if (!MumbleData && API) { MumbleData = (Mumble::Data*)API->DataLink.Get("DL_MUMBLE_LINK"); }

    std::string nameStr;
    Mumble::EProfession prof = Mumble::EProfession::None;
    unsigned spec = 0, map = 0;
    if (MumbleIdentity && MumbleIdentity->Name[0])
    {
        char name[21] = {};
        memcpy(name, MumbleIdentity->Name, 20);
        nameStr = name;
        prof = MumbleIdentity->Profession;
        spec = MumbleIdentity->Specialization;
        map = MumbleIdentity->MapID;
    }
    else if (MumbleData && MumbleData->Identity[0])
    {
        /* fall back to the raw identity JSON the game writes into MumbleLink */
        std::wstring j(MumbleData->Identity, wcsnlen(MumbleData->Identity, 256));
        size_t p = j.find(L"\"name\":\"");
        if (p != std::wstring::npos)
        {
            p += 8;
            size_t e = j.find(L'"', p);
            std::wstring wn = j.substr(p, e == std::wstring::npos ? 0 : e - p);
            int n = WideCharToMultiByte(CP_UTF8, 0, wn.c_str(), (int)wn.size(), nullptr, 0, nullptr, nullptr);
            nameStr.assign(n, 0);
            WideCharToMultiByte(CP_UTF8, 0, wn.c_str(), (int)wn.size(), &nameStr[0], n, nullptr, nullptr);
        }
        prof = (Mumble::EProfession)WNum(j, L"\"profession\"");
        spec = WNum(j, L"\"spec\"");
        map = WNum(j, L"\"map_id\"");
    }
    if (nameStr.empty()) { return w; }
    w.name = nameStr;

    std::string profName = ProfName(prof);
    w.spec = profName;
    if (spec)
    {
        SpecInfo si = Spec(spec);
        if (si.ok && si.elite) { w.spec = si.name; w.icon = si.icon; }
    }
    if (w.icon.empty()) { w.icon = ProfIcon(prof); }
    if (map) { w.map = MapName(map); }
    static const unsigned wvwMaps[] = { 38, 95, 96, 1099, 899, 968, 1315, 1316, 1143 };
    for (unsigned m : wvwMaps) { if (m == map) { w.wvw = true; } }
    return w;
}

/* ---- Discord IPC ----------------------------------------------------------------------------- */

static HANDLE Pipe = INVALID_HANDLE_VALUE;
static std::string ConnectedId;
static int Nonce = 0;

static void ClosePipe()
{
    if (Pipe != INVALID_HANDLE_VALUE) { CloseHandle(Pipe); }
    Pipe = INVALID_HANDLE_VALUE;
    ConnectedId.clear();
}

static bool WriteFrame(int op, const std::string& json)
{
    std::string frame(8, 0);
    uint32_t hdr[2] = { (uint32_t)op, (uint32_t)json.size() };
    memcpy(&frame[0], hdr, 8);
    frame += json;
    DWORD written = 0;
    if (!WriteFile(Pipe, frame.data(), (DWORD)frame.size(), &written, nullptr) || written != frame.size())
    {
        ClosePipe();
        return false;
    }
    return true;
}

/* reads one frame if one arrives within timeoutMs; returns opcode or -1 */
static int ReadFrame(std::string& json, int timeoutMs)
{
    json.clear();
    for (int waited = 0;; waited += 20)
    {
        DWORD avail = 0;
        if (!PeekNamedPipe(Pipe, nullptr, 0, nullptr, &avail, nullptr)) { ClosePipe(); return -1; }
        if (avail >= 8) { break; }
        if (waited >= timeoutMs) { return -1; }
        Sleep(20);
    }
    uint32_t hdr[2];
    DWORD read = 0;
    if (!ReadFile(Pipe, hdr, 8, &read, nullptr) || read != 8) { ClosePipe(); return -1; }
    if (hdr[1] > 1 << 20) { ClosePipe(); return -1; }
    json.resize(hdr[1]);
    DWORD got = 0;
    while (got < hdr[1])
    {
        if (!ReadFile(Pipe, &json[got], hdr[1] - got, &read, nullptr) || read == 0) { ClosePipe(); return -1; }
        got += read;
    }
    return (int)hdr[0];
}

static void Drain()
{
    std::string j;
    while (Pipe != INVALID_HANDLE_VALUE && ReadFrame(j, 0) >= 0)
    {
        if (j.find("\"evt\":\"ERROR\"") != std::string::npos)
        {
            std::string m = JsonText(j, "message");
            SetDiscord("Discord rejected the update: " + m);
        }
    }
}

static bool Connect(const std::string& clientId)
{
    if (Pipe != INVALID_HANDLE_VALUE && ConnectedId == clientId) { return true; }
    ClosePipe();
    bool denied = false;
    for (int i = 0; i < 10 && Pipe == INVALID_HANDLE_VALUE; ++i)
    {
        char name[64];
        snprintf(name, sizeof(name), "\\\\.\\pipe\\discord-ipc-%d", i);
        Pipe = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (Pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED) { denied = true; }
    }
    if (Pipe == INVALID_HANDLE_VALUE)
    {
        SetDiscord(denied ? "Discord refused access: Discord and GW2 are running with different permissions (one as administrator)."
                          : "Discord desktop app not running.");
        return false;
    }

    if (!WriteFrame(0, "{\"v\":1,\"client_id\":\"" + clientId + "\"}")) { SetDiscord("Lost the connection to Discord."); return false; }
    std::string reply;
    int op = ReadFrame(reply, 3000);
    if (op != 1 || reply.find("READY") == std::string::npos)
    {
        std::string m = JsonText(reply, "message");
        SetDiscord(m.empty() ? "Discord didn't accept the application ID." : "Discord: " + m);
        ClosePipe();
        return false;
    }
    ConnectedId = clientId;
    SetDiscord("Connected.");
    return true;
}

/* Layout on the Discord card:
 *   <app name>
 *   Turbotylar · Firebrand              (details)
 *   Eternal Battlegrounds               (state, line 1 of 2 is details, so map + kills share state)
 *   327,009 kills · next 500,000        ...
 * Discord shows two text lines, so: details = character · spec, state = map · kills.
 * Hovering the big image shows the next tier; hovering the class icon shows character and spec. */
static bool SendActivity(const Settings& c, long long kills, long long goal, const std::string& tier, time_t start, const Where& w, bool clear)
{
    std::string act;
    if (!clear)
    {
        std::string who;
        if (c.showChar && !w.name.empty()) { who = w.name; }
        if (c.showSpec && !w.spec.empty()) { who += (who.empty() ? "" : " \xC2\xB7 ") + w.spec; }

        std::string killsText = kills >= 0 ? Commas(kills) + " WvW kills" : "";
        std::string nextText = kills >= 0 ? "Next: " + Commas(goal) + (c.showTier ? " (" + tier + ")" : "") : "";

        std::string where = (c.showMap && !w.map.empty()) ? w.map : (w.name.empty() ? "In the menus" : "");
        std::string details, state;
        if (!who.empty())
        {
            details = who;
            state = where;
            if (!killsText.empty()) { state += (state.empty() ? "" : " \xC2\xB7 ") + killsText; }
        }
        else
        {
            details = where.empty() ? killsText : where;
            state = where.empty() ? nextText : killsText;
        }
        if (details.empty() && state.empty()) { details = "Guild Wars 2"; }

        act = "{\"type\":0";
        if (!details.empty()) { act += ",\"details\":\"" + JsonEscape(details) + "\""; }
        if (!state.empty()) { act += ",\"state\":\"" + JsonEscape(state) + "\""; }
        if (c.showTimer && start) { act += ",\"timestamps\":{\"start\":" + std::to_string((long long)start) + "}"; }

        std::string assets;
        if (c.largeImage[0])
        {
            assets += "\"large_image\":\"" + JsonEscape(c.largeImage) + "\"";
            std::string hover = !nextText.empty() ? nextText : "Guild Wars 2";
            assets += ",\"large_text\":\"" + JsonEscape(hover) + "\"";
        }
        if (c.classIcon && !w.icon.empty())
        {
            if (!assets.empty()) { assets += ","; }
            assets += "\"small_image\":\"" + JsonEscape(w.icon) + "\",\"small_text\":\"" + JsonEscape(w.spec) + "\"";
        }
        if (!assets.empty()) { act += ",\"assets\":{" + assets + "}"; }
        act += "}";
    }
    std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(GetCurrentProcessId());
    if (!act.empty()) { cmd += ",\"activity\":" + act; }
    cmd += "},\"nonce\":\"" + std::to_string(++Nonce) + "\"}";
    if (!WriteFrame(1, cmd)) { SetDiscord("Lost the connection to Discord."); return false; }
    std::string reply;
    if (ReadFrame(reply, 3000) < 0) { SetDiscord("Discord didn't answer."); ClosePipe(); return false; }
    if (reply.find("\"evt\":\"ERROR\"") != std::string::npos)
    {
        SetDiscord("Discord rejected the update: " + JsonText(reply, "message"));
        return false;
    }
    SetDiscord(act.empty() ? "Connected (status cleared)." : "Showing your status.");
    return true;
}

/* ---- worker ---------------------------------------------------------------------------------- */

static void WorkerLoop()
{
    time_t nextPoll = 0;
    bool needSend = true;
    bool shown = false;
    Where lastWhere;
    for (;;)
    {
        Settings c;
        bool keyChanged;
        {
            std::lock_guard<std::mutex> g(Lock);
            c = Cfg;
            if (Dirty) { needSend = true; Dirty = false; }
            keyChanged = KeyChanged;
            KeyChanged = false;
        }
        if (keyChanged) { nextPoll = 0; }

        time_t now = time(nullptr);
        if (c.enabled && now >= nextPoll)
        {
            if (PollKills()) { needSend = true; }
            nextPoll = now + c.interval;
        }

        Where w;
        if (c.enabled)
        {
            w = ReadWhere();
            if (w != lastWhere) { needSend = true; }
            if (Pipe == INVALID_HANDLE_VALUE || ConnectedId != c.clientId) { needSend = true; }
            if (needSend && Connect(c.clientId))
            {
                long long kills, goal;
                std::string tier;
                time_t start;
                {
                    std::lock_guard<std::mutex> g(Lock);
                    kills = Kills;
                    goal = Goal;
                    tier = TierName;
                    start = SessionStart;
                }
                if (SendActivity(c, kills, goal, tier, start, w, false)) { needSend = false; shown = true; lastWhere = w; }
            }
        }
        else if (shown && Pipe != INVALID_HANDLE_VALUE)
        {
            SendActivity(c, -1, -1, "", 0, Where(), true); /* clear it */
            shown = false;
            SetStatus("Paused.");
        }
        Drain();

        std::unique_lock<std::mutex> g(Lock);
        /* wake every 5 s to follow map changes and pick up a restarted Discord */
        Wake.wait_for(g, std::chrono::seconds(5), [] { return Stopping || Dirty || KeyChanged; });
        if (Stopping) { break; }
    }
    if (Pipe != INVALID_HANDLE_VALUE)
    {
        Settings c;
        {
            std::lock_guard<std::mutex> g(Lock);
            c = Cfg;
        }
        SendActivity(c, -1, -1, "", 0, Where(), true);
        ClosePipe();
    }
}

/* ---- options page ---------------------------------------------------------------------------- */

static char KeyBuf[128];
static std::string SaveMsg;

static void RenderOptions()
{
    std::string status, discord, tier;
    long long kills, goal;
    time_t check;
    bool haveKey;
    {
        std::lock_guard<std::mutex> g(Lock);
        status = Status;
        discord = DiscordStatus;
        kills = Kills;
        goal = Goal;
        tier = TierName;
        check = LastCheck;
        haveKey = !ApiKey.empty();
    }

    ImGui::TextDisabled("GW2 API:");
    ImGui::SameLine();
    ImGui::TextUnformatted(status.c_str());
    ImGui::TextDisabled("Discord:");
    ImGui::SameLine();
    ImGui::TextUnformatted(discord.c_str());
    if (kills >= 0)
    {
        ImGui::Text("WvW kills: %s   Next: %s (%s)", Commas(kills).c_str(), Commas(goal).c_str(), tier.c_str());
    }
    if (check)
    {
        long s = (long)(time(nullptr) - check);
        ImGui::TextDisabled("Last check %ld s ago. The GW2 API itself only refreshes kill counts every few minutes.", s);
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> g(Lock);
        changed |= ImGui::Checkbox("Show on Discord", &Cfg.enabled);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Show tier name", &Cfg.showTier);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Show play time", &Cfg.showTimer);
        changed |= ImGui::Checkbox("Show character name", &Cfg.showChar);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Show profession", &Cfg.showSpec);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Show map", &Cfg.showMap);
        ImGui::SameLine();
        changed |= ImGui::Checkbox("Class icon", &Cfg.classIcon);
    }

    ImGui::Separator();
    ImGui::Text("GW2 API key (needs account + progression): %s", haveKey ? "saved" : "missing");
    ImGui::PushItemWidth(380);
    ImGui::InputText("##key", KeyBuf, sizeof(KeyBuf), ImGuiInputTextFlags_Password);
    ImGui::PopItemWidth();
    ImGui::SameLine();
    if (ImGui::Button("Save key"))
    {
        std::string key(KeyBuf);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\r' || key.back() == '\n')) { key.pop_back(); }
        while (!key.empty() && key[0] == ' ') { key.erase(0, 1); }
        if (key.empty()) { SaveMsg = "Paste a key first."; }
        else if (SaveSecret(key))
        {
            std::lock_guard<std::mutex> g(Lock);
            ApiKey = key;
            KeyChanged = true;
            Wake.notify_all();
            SaveMsg = "Saved.";
        }
        else { SaveMsg = "Could not save."; }
        if (!key.empty()) { SecureZeroMemory(&key[0], key.size()); }
        SecureZeroMemory(KeyBuf, sizeof(KeyBuf));
    }
    if (!SaveMsg.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(SaveMsg.c_str()); }
    ImGui::TextDisabled("Stored encrypted for your Windows account, outside the game folder.");

    if (ImGui::CollapsingHeader("Advanced"))
    {
        std::lock_guard<std::mutex> g(Lock);
        ImGui::PushItemWidth(220);
        changed |= ImGui::InputText("Discord application ID", Cfg.clientId, sizeof(Cfg.clientId), ImGuiInputTextFlags_CharsDecimal);
        changed |= ImGui::InputText("Image asset key or URL", Cfg.largeImage, sizeof(Cfg.largeImage));
        if (ImGui::SliderInt("Check GW2 API every (s)", &Cfg.interval, 30, 600)) { changed = true; }
        ImGui::PopItemWidth();
        ImGui::TextDisabled("The status reads \"Playing <application name>\". Use your own app to change the name or image.");
    }
    if (changed)
    {
        std::lock_guard<std::mutex> g(Lock);
        SaveSettings();
        Dirty = true;
        Wake.notify_all();
    }
}

/* ---- load / unload --------------------------------------------------------------------------- */

static void Load(AddonAPI* aApi)
{
    API = aApi;
    ImGui::SetCurrentContext((ImGuiContext*)API->ImguiContext);
    ImGui::SetAllocatorFunctions((void* (*)(size_t, void*))API->ImguiMalloc, (void (*)(void*, void*))API->ImguiFree);

    const char* dir = API->Paths.GetAddonDirectory("GW2WvWPresence");
    std::string d = dir ? dir : "addons\\GW2WvWPresence";
    CreateDirectoryA(d.c_str(), nullptr);
    SettingsPath = d + "\\settings.txt";
    LoadSettings();

    char appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    std::string sd = (n && n < MAX_PATH) ? std::string(appdata) + "\\GW2WvWPresence" : d;
    CreateDirectoryA(sd.c_str(), nullptr);
    SecretsPath = sd + "\\secrets.bin";
    LoadSecret();

    MumbleIdentity = (Mumble::Identity*)API->DataLink.Get("DL_MUMBLE_LINK_IDENTITY");
    SessionStart = time(nullptr);
    Stopping = false;
    Worker = std::thread(WorkerLoop);
    API->Renderer.Register(ERenderType_OptionsRender, RenderOptions);
}

static void Unload()
{
    if (!API) { return; }
    API->Renderer.Deregister(RenderOptions);
    {
        std::lock_guard<std::mutex> g(Lock);
        Stopping = true;
        Wake.notify_all();
        SaveSettings();
    }
    if (Worker.joinable()) { Worker.join(); }
    {
        std::lock_guard<std::mutex> g(Lock);
        if (!ApiKey.empty()) { SecureZeroMemory(&ApiKey[0], ApiKey.size()); }
        ApiKey.clear();
    }
    API = nullptr;
}

extern "C" __declspec(dllexport) AddonDefinition* GetAddonDef()
{
    Def.Signature = -0x47575750; /* unique negative id for a non-Raidcore addon */
    Def.APIVersion = NEXUS_API_VERSION;
    Def.Name = "GW2 WvW Presence";
    Def.Version.Major = 0;
    Def.Version.Minor = 2;
    Def.Version.Build = 1;
    Def.Version.Revision = 0;
    Def.Author = "Turbotylar";
    Def.Description = "Discord Rich Presence with your character, map, WvW kills and next Realm Avenger tier.";
    Def.Load = Load;
    Def.Unload = Unload;
    Def.Flags = EAddonFlags_None;
    Def.Provider = EUpdateProvider_GitHub; /* Nexus checks the repo's latest release and installs the .dll in it */
    Def.UpdateLink = "https://github.com/Turbotylar/GW2WvWPresence";
    return &Def;
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
