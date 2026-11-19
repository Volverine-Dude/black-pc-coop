// The CO-OP menu (owner, day 15): CO-OP > HOST GAME / JOIN GAME > INTERNET (later) / LAN.
// - HOST GAME > LAN opens the host lobby: a 6-character code (no look-alikes: no 0/O, 1/I), the status and player 2.
//   The host answers code queries on UDP port 44100.
// - JOIN GAME > LAN opens the code page: the keyboard types the code (Backspace deletes; on an empty code it is Back
//   again), a pad uses CODE (left / right: the cursor) and LETTER (left / right: the character under the cursor);
//   CONNECT broadcasts the code on the LAN (and to 127.0.0.1 for two copies on one PC).
// Messages (UDP, port 44100 on the host): "CLDQ" {code} who has this code? -> "CLDA" {code} (from the host's lobby
// socket: the joiner learns its address); "CLJN" {code} join -> "CLOK" {code}; "CLLB" {code} heartbeat both ways every
// 500 ms (3 s without one = gone); "CLLV" {code} leaving.
#include "settings.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include "fe/coop_menu.h"

#include "common.h"
#include "coop/coop.h"
#include "fe/vars.h"
#include "xbe.h"
#include "input/movies.h"
#include "input/bindings.h"

#include <ws2tcpip.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

volatile int g_coop_code_len = -1;
volatile int g_coop_skip_ui;
volatile int g_coop_diffmode_on;          // this session's DIFFICULTY (host: its option; joiner: the host's, from the heartbeat)
volatile int g_coop_special_on = 1;       // this session's SPECIAL WEAPONS (same)
char g_coop_lobby_banner[2][64];          // the lobby's top banners (status / player 2 on the host; the host's options on the joiner)

namespace {

constexpr uint16_t kLobbyPort = 44100;
constexpr char kAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"; // 32 symbols, no 0/O 1/I
constexpr int kCodeLen = 6;
constexpr ULONGLONG kBeatMs = 500, kLostMs = 3000, kSearchMs = 5000, kQueryEveryMs = 400;

enum Page { None = 0, Menu = 200, HostMode = 210, JoinMode = 211, HostLobby = 220, JoinCode = 230 };

struct Msg {
    uint32_t magic;
    char code[8];
};
constexpr uint32_t kQuery = 0x51444C43 /* CLDQ */, kAnswer = 0x41444C43 /* CLDA */, kJoin = 0x4E4A4C43 /* CLJN */,
                   kOk = 0x4B4F4C43 /* CLOK */, kBeat = 0x424C4C43 /* CLLB */, kLeave = 0x564C4C43 /* CLLV */,
                   kActivate = 0x43414C43 /* CLAC: the host started - co-op on */, kGo = 0x4F474C43 /* CLGO: the mission */;
// CLGO carries the host's LevelSelect and SelectedDifficulty parameters after the code
struct GoMsg {
    uint32_t magic;
    char code[8];
    char level[8], difficulty[8];
};

int g_page;
SOCKET g_sock = INVALID_SOCKET;
bool g_wsa;

// step 3: the host left the lobby for mission select with player 2 in it -> co-op is on, the mission follows
bool g_started;
// step 4 (the lobby): the joiner's READY (rides in the heartbeat: code byte 7) gates the host's START MISSION
bool g_ready;          // joiner: our READY
bool g_peer_ready;     // host: player 2's READY
bool g_start_refused;  // host: START pressed without a ready player 2 (shown until it changes)
char g_level[8] = "0";        // the host's last LevelSelect parameter
int g_go_sends;               // CLGO repeats left (host)
GoMsg g_go;

// BACK TO THE LOBBY (owner, day 15: "after the stats and the save, put the players back in the lobby"): set when a co-op
// level ends; the next "MissionModeSelect" page (the game's own after the autosave) is replaced by the lobby page in
// REJOIN mode: the host keeps its code, the joiner keeps the code and reconnects by itself, READY off.
int g_back_role;      // 1 / 2: go back when mission select opens
bool g_to_title;      // the co-op session ended: the next menu page becomes the title
bool g_rejoin;        // the next lobby page opens in rejoin mode
ULONGLONG g_rejoin_until;   // joiner: keep searching for the host's lobby until then (it may open later than ours)

// host
char g_code[kCodeLen + 1];
bool g_have_joiner;
sockaddr_in g_joiner{};
ULONGLONG g_joiner_ms;

// joiner
char g_entry[kCodeLen + 1];
int g_cursor;
enum class Join { Idle, Searching, Joining, InLobby, NotFound, Lost } g_join;
sockaddr_in g_host{};
ULONGLONG g_join_ms, g_query_ms, g_host_ms;

ULONGLONG g_beat_ms;
std::wstring g_shown[fevar::kRows];

// typed characters from the window thread
std::atomic<uint32_t> g_kr, g_kw;
wchar_t g_keys[32];

bool net_open(uint16_t port)
{
    if (!g_wsa) {
        WSADATA w;
        if (WSAStartup(MAKEWORD(2, 2), &w) != 0) return false;
        g_wsa = true;
    }
    if (g_sock != INVALID_SOCKET) return true;
    g_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_sock == INVALID_SOCKET) return false;
    BOOL on = TRUE;
    setsockopt(g_sock, SOL_SOCKET, SO_BROADCAST, (const char *)&on, sizeof on);
    u_long nb = 1;
    ioctlsocket(g_sock, FIONBIO, &nb);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (const sockaddr *)&a, sizeof a) != 0) {
        logf("coop menu: cannot open UDP port %u (error %d) - is another copy hosting on this PC?", port, WSAGetLastError());
        closesocket(g_sock);
        g_sock = INVALID_SOCKET;
        return false;
    }
    return true;
}

void net_close()
{
    if (g_sock != INVALID_SOCKET) closesocket(g_sock);
    g_sock = INVALID_SOCKET;
}

void send_to(const sockaddr_in &to, uint32_t magic, const char *code)
{
    if (g_sock == INVALID_SOCKET) return;
    Msg m{};
    m.magic = magic;
    memcpy(m.code, code, kCodeLen);
    if (magic == kBeat) { m.code[6] = code[6]; m.code[7] = code[7]; }   // host: [6] = SPRINT on, [7] = 0x80 | options; joiner: [7] = READY (buffers are 8 long)
    sendto(g_sock, (const char *)&m, sizeof m, 0, (const sockaddr *)&to, sizeof to);
}

sockaddr_in addr(uint32_t ip, uint16_t port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    return a;
}

std::string ip_text(const sockaddr_in &a)
{
    char b[32];
    inet_ntop(AF_INET, &a.sin_addr, b, sizeof b);
    return b;
}

void show(int row, const std::string &s)
{
    std::wstring w(s.begin(), s.end());
    if (w == g_shown[row]) return;
    g_shown[row] = w;
    fe_var_set(fevar::kValue + row, w);
}

void clear_values()
{
    for (int i = 0; i < fevar::kRows; i++) {
        g_shown[i] = L"\x1";   // force the next show
        show(i, "");
    }
}

// ---------------------------------------------------------------------------------------------------- host lobby
// switch the front end to a page (what MissionModeSelect_StartLevel does for FMVPlayer: the name at FE object + 0x3820,
// then the change flag +0x3884, or +0x388C when no page is up)
void fe_goto(const char *page)
{
    uint32_t fe = *(const uint32_t *)(uintptr_t)xbe_va(0x002D199C);
    if (!fe) return;
    strncpy((char *)(uintptr_t)(fe + 0x3820), page, 0x1F);
    if (*(const char *)(uintptr_t)(fe + 0x3840)) *(volatile int32_t *)(uintptr_t)(fe + 0x3884) = 1;
    else *(volatile int32_t *)(uintptr_t)(fe + 0x388C) = 1;
}

void host_open()
{
    g_peer_ready = g_start_refused = false;
    g_started = false; g_go_sends = 0;
    bool keep = g_rejoin && g_code[0];
    unsigned seed = (unsigned)GetTickCount64() ^ (GetCurrentProcessId() << 16);
    for (int i = 0; i < kCodeLen && !keep; i++) {
        seed = seed * 1103515245u + 12345u;
        g_code[i] = kAlphabet[(seed >> 16) % 32];
    }
    g_code[kCodeLen] = 0;
    if (keep) logf("coop menu: HOST lobby: back from the mission - same code %s, waiting for player 2 to rejoin", g_code);
    {   // tests: COOP_LOBBY_CODE=<6 characters> fixes the code
        char e[16] = {0};
        if (GetEnvironmentVariableA("COOP_LOBBY_CODE", e, sizeof e) && strlen(e) == kCodeLen) memcpy(g_code, e, kCodeLen);
    }
    g_have_joiner = false;
    bool ok = net_open(kLobbyPort);
    logf("coop menu: HOST lobby (LAN) open - code %s, UDP %u %s", g_code, kLobbyPort, ok ? "listening" : "FAILED");
}

void host_poll(ULONGLONG now)
{
    Msg m;
    sockaddr_in from{};
    int fl = sizeof from;
    int r;
    while (g_sock != INVALID_SOCKET && (r = recvfrom(g_sock, (char *)&m, sizeof m, 0, (sockaddr *)&from, &fl)) >= (int)sizeof m) {
        if (memcmp(m.code, g_code, kCodeLen) != 0) continue;   // another lobby's code: not ours
        switch (m.magic) {
        case kQuery: send_to(from, kAnswer, g_code); break;
        case kJoin:
            if (!g_have_joiner || from.sin_addr.s_addr != g_joiner.sin_addr.s_addr || from.sin_port != g_joiner.sin_port)
                logf("coop menu: HOST lobby: player 2 joined from %s:%u", ip_text(from).c_str(), ntohs(from.sin_port));
            g_joiner = from;
            g_have_joiner = true;
            g_joiner_ms = now;
            send_to(from, kOk, g_code);
            break;
        case kBeat:
            if (g_have_joiner && from.sin_addr.s_addr == g_joiner.sin_addr.s_addr && from.sin_port == g_joiner.sin_port) {
                g_joiner_ms = now;
                bool rd = m.code[7] == 1;
                if (rd != g_peer_ready) { logf("coop menu: HOST lobby: player 2 is %s", rd ? "READY" : "not ready"); g_start_refused = false; }
                g_peer_ready = rd;
            }
            break;
        case kLeave:
            if (g_have_joiner && from.sin_port == g_joiner.sin_port) {
                logf("coop menu: HOST lobby: player 2 left");
                g_have_joiner = false;
                g_peer_ready = false;
            }
            break;
        }
        fl = sizeof from;
    }
    if (g_have_joiner && now - g_joiner_ms > kLostMs) {
        logf("coop menu: HOST lobby: player 2 lost (no heartbeat for 3 s)");
        g_have_joiner = false;
        g_peer_ready = false;
    }
    if (g_have_joiner && now - g_beat_ms >= kBeatMs) {
        g_beat_ms = now;
        char hb[8]; memcpy(hb, g_code, sizeof hb); hb[6] = g_cfg.coop_sprint ? 1 : 0;   // the host's SPRINT option rides the heartbeat
        hb[7] = (char)(0x80 | (g_cfg.coop_diffmode ? 1 : 0) | (g_cfg.coop_special ? 2 : 0));   // and DIFFICULTY / SPECIAL WEAPONS (0x80 = present)
        send_to(g_joiner, kBeat, hb);
        if (g_started) send_to(g_joiner, kActivate, g_code);   // until the mission goes (a lost CLAC)
    }

    if (g_go_sends > 0 && g_have_joiner) {
        g_go_sends--;
        sendto(g_sock, (const char *)&g_go, sizeof g_go, 0, (const sockaddr *)&g_joiner, sizeof g_joiner);
        if (!g_go_sends) { logf("coop menu: HOST: the mission went to the joiner - lobby closed"); net_close(); g_page = None; return; }
    }
    show(0, g_sock == INVALID_SOCKET ? "-" : g_code);
    show(1, g_cfg.coop_diffmode ? "CO-OP" : "VANILLA");
    show(2, g_cfg.coop_special ? "ON" : "OFF");
    show(3, g_cfg.coop_sprint ? "ON" : "OFF");
    // the lobby redesign (owner 20261118): STATUS and PLAYER 2 are banners at the top of the screen
    snprintf(g_coop_lobby_banner[0], sizeof g_coop_lobby_banner[0], "STATUS: %s", g_sock == INVALID_SOCKET ? "PORT BUSY" : g_started ? "STARTING..." :
             g_start_refused ? "PLAYER 2 NOT READY" : !g_have_joiner ? "WAITING FOR PLAYER 2" : g_peer_ready ? "ALL READY" : "PLAYER 2 JOINED");
    snprintf(g_coop_lobby_banner[1], sizeof g_coop_lobby_banner[1], "PLAYER 2: %s", !g_have_joiner ? "-" : g_peer_ready ? "READY" : "NOT READY");
    g_coop_sprint_allowed = g_cfg.coop_sprint ? 1 : 0;   // the host's own options
    g_coop_diffmode_on = g_cfg.coop_diffmode ? 1 : 0;
    g_coop_special_on = g_cfg.coop_special ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------- join code
std::string entry_text()
{
    std::string s;
    for (int i = 0; i < kCodeLen; i++) {
        char c = g_entry[i] ? g_entry[i] : '_';
        if (i == g_cursor) { s += '['; s += c; s += ']'; }
        else s += c;
        if (i + 1 < kCodeLen) s += ' ';
    }
    return s;
}

int entry_len()
{
    int n = 0;
    while (n < kCodeLen && g_entry[n]) n++;
    return n;
}

void join_connect(ULONGLONG now);
void join_open()
{
    g_ready = false;
    g_started = false;
    if (g_rejoin && g_entry[0]) {
        g_cursor = kCodeLen - 1;
        g_kr = g_kw.load();
        logf("coop menu: JOIN: back from the mission - rejoining the host's lobby with code %s", g_entry);
        g_rejoin_until = GetTickCount64() + 60000;
        join_connect(GetTickCount64());
        return;
    }
    memset(g_entry, 0, sizeof g_entry);
    g_cursor = 0;
    g_join = Join::Idle;
    g_kr = g_kw.load();   // drop keys typed before the page
    {   // tests: COOP_JOIN_CODE=<6 characters> types the code in
        char e[16] = {0};
        if (GetEnvironmentVariableA("COOP_JOIN_CODE", e, sizeof e) && strlen(e) == kCodeLen) { memcpy(g_entry, e, kCodeLen); g_cursor = kCodeLen - 1; }
    }
    logf("coop menu: JOIN (LAN) code page open");
}

void join_leave()
{
    if (g_join == Join::InLobby || g_join == Join::Joining) send_to(g_host, kLeave, g_entry);
    g_join = Join::Idle;
}

void type_char(wchar_t c)
{
    if (g_join == Join::Searching || g_join == Join::Joining || g_join == Join::InLobby) return;
    if (c == L'\b') {
        int n = entry_len();
        if (n > 0) {
            if (g_cursor >= n) g_cursor = n;
            int at = g_cursor > 0 ? g_cursor - 1 : 0;
            memmove(g_entry + at, g_entry + at + 1, kCodeLen - at);
            g_entry[kCodeLen] = 0;
            g_cursor = at;
        }
        g_join = Join::Idle;
        return;
    }
    if (c >= L'a' && c <= L'z') c = (wchar_t)(c - 32);
    if (c == L'0') c = L'O';   // look-alikes the code never uses
    if (c == L'1') c = L'I';
    if (!strchr(kAlphabet, (char)c) || c == 0) return;
    int n = entry_len();
    if (g_cursor > n) g_cursor = n;
    if (g_cursor >= kCodeLen) return;
    g_entry[g_cursor] = (char)c;
    if (g_cursor < kCodeLen - 1) g_cursor++;
    g_join = Join::Idle;
}

void join_connect(ULONGLONG now)
{
    if (entry_len() < kCodeLen) {
        logf("coop menu: JOIN: CONNECT with an incomplete code (%d of %d)", entry_len(), kCodeLen);
        g_join = Join::NotFound;
        return;
    }
    if (!net_open(0)) { g_join = Join::Lost; return; }
    g_join = Join::Searching;
    g_join_ms = now;
    g_query_ms = 0;
    logf("coop menu: JOIN: searching the LAN for code %s", g_entry);
}

void start_mission(const char *level, const char *difficulty);

void join_poll(ULONGLONG now)
{
    wchar_t c;
    while (g_kr != g_kw) { c = g_keys[g_kr % 32]; g_kr++; type_char(c); }
    if (g_join == Join::Searching && now - g_query_ms >= kQueryEveryMs) {
        g_query_ms = now;
        send_to(addr(htonl(INADDR_BROADCAST), kLobbyPort), kQuery, g_entry);
        send_to(addr(htonl(INADDR_LOOPBACK), kLobbyPort), kQuery, g_entry);   // a host on this PC
        if (now - g_join_ms > kSearchMs) {
            logf("coop menu: JOIN: no host answered code %s in 5 s", g_entry);
            g_join = Join::NotFound;
        }
    }
    GoMsg gm;
    Msg &m = *(Msg *)&gm;
    sockaddr_in from{};
    int fl = sizeof from;
    int r;
    while (g_sock != INVALID_SOCKET && (r = recvfrom(g_sock, (char *)&gm, sizeof gm, 0, (sockaddr *)&from, &fl)) >= (int)sizeof m) {
        fl = sizeof from;
        if (memcmp(m.code, g_entry, kCodeLen) != 0) continue;
        if (gm.magic == kGo && r >= (int)sizeof gm && g_started) {   // the host's mission: start it here
            gm.level[7] = gm.difficulty[7] = 0;
            start_mission(gm.level, gm.difficulty);
            net_close();
            g_page = None;
            return;
        }
        if (m.magic == kAnswer && g_join == Join::Searching) {
            g_host = from;
            g_join = Join::Joining;
            g_join_ms = now;
            logf("coop menu: JOIN: host found at %s:%u - joining", ip_text(from).c_str(), ntohs(from.sin_port));
            send_to(g_host, kJoin, g_entry);
        } else if (m.magic == kOk && g_join == Join::Joining) {
            g_join = Join::InLobby;
            g_host_ms = now;
            logf("coop menu: JOIN: in the host's lobby (%s)", ip_text(from).c_str());
            // owner (20261115): "have a warning when a joiner joins the host's game that his progress will be overwritten by
            // the host's game progress" - missions played in co-op are the host's (mission and difficulty) and the game's
            // autosave writes them into OUR profile (start_mission keeps that unlock)
            strncpy(g_coop_warn_msg, "CO-OP: THE HOST'S MISSIONS YOU PLAY ARE SAVED TO YOUR PROFILE", sizeof g_coop_warn_msg - 1);
            g_coop_warn_msg_until = GetTickCount64() + 8000;
        } else if (m.magic == kBeat && g_join == Join::InLobby) {
            g_host_ms = now;
            int sp = m.code[6] == 1;   // the host's SPRINT option
            if (sp != g_coop_sprint_allowed) logf("coop menu: JOIN: the host turned SPRINT %s", sp ? "ON" : "OFF");
            g_coop_sprint_allowed = sp;
            uint8_t opt = (uint8_t)m.code[7];   // the host's DIFFICULTY / SPECIAL WEAPONS (an older host sends 0: vanilla, on)
            int dm = (opt & 0x80) && (opt & 1) ? 1 : 0, sw = (opt & 0x80) ? ((opt & 2) ? 1 : 0) : 1;
            if (dm != g_coop_diffmode_on || sw != g_coop_special_on)
                logf("coop menu: JOIN: the host's DIFFICULTY %s, SPECIAL WEAPONS %s", dm ? "CO-OP" : "VANILLA", sw ? "ON" : "OFF");
            g_coop_diffmode_on = dm; g_coop_special_on = sw;
        } else if (m.magic == kActivate && g_join == Join::InLobby && !g_started) {
            g_started = true;
            g_host_ms = now;
            logf("coop menu: JOIN: the host started - co-op on, waiting for the host's mission");
            coop_activate(2, g_host.sin_addr.s_addr);
        }
    }
    if (g_join == Join::Joining && now - g_join_ms > 1000) { send_to(g_host, kJoin, g_entry); g_join_ms = now; }
    if ((g_join == Join::NotFound || g_join == Join::Lost) && g_rejoin_until && now < g_rejoin_until) join_connect(now);   // rejoin: keep looking
    if (g_join == Join::InLobby) g_rejoin_until = 0;
    if (g_join == Join::InLobby) {
        if (now - g_beat_ms >= kBeatMs) {
            g_beat_ms = now;
            char c[8] = {0};
            memcpy(c, g_entry, kCodeLen);
            c[7] = g_ready ? 1 : 0;   // the READY byte after the code
            send_to(g_host, kBeat, c);
        }
        if (now - g_host_ms > kLostMs) { logf("coop menu: JOIN: the host is gone (no heartbeat for 3 s)"); g_join = Join::Lost; }
    }
    g_coop_code_len = entry_len();
    show(0, entry_text());
    char cur = g_cursor < kCodeLen && g_entry[g_cursor] ? g_entry[g_cursor] : '-';
    show(1, std::string(1, cur));
    const char *st = "ENTER CODE";   // the value field shows ~14 characters
    switch (g_join) {
    case Join::Idle: st = entry_len() == kCodeLen ? "PRESS CONNECT" : "ENTER CODE"; break;
    case Join::Searching: st = "SEARCHING..."; break;
    case Join::Joining: st = "JOINING..."; break;
    case Join::InLobby: st = g_started ? "HOST PICKS..." : "IN THE LOBBY"; break;
    case Join::NotFound: st = entry_len() < kCodeLen ? "CODE TOO SHORT" : "NO HOST FOUND"; break;
    case Join::Lost: st = "HOST LOST"; break;
    }
    show(3, st);
    show(4, g_join == Join::InLobby ? (g_ready ? "YES" : "NO") : "-");
    if (g_join == Join::InLobby) {   // the host's options, shown to the joiner as the lobby banners
        snprintf(g_coop_lobby_banner[0], sizeof g_coop_lobby_banner[0], "DIFFICULTY: %s", g_coop_diffmode_on ? "CO-OP" : "VANILLA");
        snprintf(g_coop_lobby_banner[1], sizeof g_coop_lobby_banner[1], "SPECIAL WEAPONS: %s", g_coop_special_on ? "ON" : "OFF");
    } else g_coop_lobby_banner[0][0] = g_coop_lobby_banner[1][0] = 0;
}

// The joiner starts the host's mission as the host's own menus did: the LevelSelect handler (FUN_0011B890) with the
// host's level, then SelectedDifficulty (FUN_00124F00): the difficulty to game +0x2014C, then
// MissionModeSelect_StartLevel (FUN_00124CF0), which plays the briefing (page FMVPlayer) and loads the mission. That
// one returns unless the level is unlocked at that difficulty in OUR profile (byte 0x2D0D9E + (index + difficulty * 8)
// * 0x20, index = the slot of game +0x2020C in the table at 0x288648): the flag is set for the call and put back, so
// the joiner's save never changes.
void start_mission(const char *level, const char *difficulty)
{
    typedef void(__cdecl * Handler)(const char *);
    Handler level_select = (Handler)(uintptr_t)xbe_va(0x0011B890), selected_difficulty = (Handler)(uintptr_t)xbe_va(0x00124F00);
    // owner play 20261115: the joiner's fresh save had only mission 1 on Easy unlocked; the host started mission 1 on
    // Normal -> both disconnected, the retry crashed. The unlock used to be set only AFTER LevelSelect, from the level the
    // handler had picked (game +0x2020C) - a LevelSelect that refuses a level locked in our profile kept the previous one.
    // Now the requested slot is unlocked at ALL four difficulties before LevelSelect and put back after the start.
    int slot = atoi(level), diff = atoi(difficulty);
    volatile uint8_t *flags[4] = {}; uint8_t keep[4] = {};
    if (slot >= 0 && slot < 8)
        for (int d = 0; d < 4; d++) {
            flags[d] = (volatile uint8_t *)(uintptr_t)xbe_va(0x002D0D9E + (uint32_t)(slot + d * 8) * 0x20);
            keep[d] = *flags[d];
        }
    {   // COOP_TEST_LOCK=1 (tests): this mission locked at every difficulty first (a fresh save)
        char e[8] = {0};
        if (GetEnvironmentVariableA("COOP_TEST_LOCK", e, sizeof e) && e[0] == '1')
            for (int d = 0; d < 4; d++) if (flags[d]) { *flags[d] = 0; keep[d] = 0; }
    }
    bool unlocked = false;
    char late_e[8] = {0};
    bool late = GetEnvironmentVariableA("COOP_UNLOCK_LATE", late_e, sizeof late_e) && late_e[0] == '1';   // test: the old order
    if (!late) for (int d = 0; d < 4; d++) if (flags[d]) { if (*flags[d] != 1) unlocked = true; *flags[d] = 1; }
    level_select(level);
    if (late) for (int d = 0; d < 4; d++) if (flags[d]) { if (*flags[d] != 1) unlocked = true; *flags[d] = 1; }
    uint32_t game = *(const uint32_t *)(uintptr_t)xbe_va(0x002D45F0);
    uint8_t got = game ? *(const uint8_t *)(uintptr_t)(game + 0x2020C) : 0xFF;
    const int8_t *tbl = (const int8_t *)(uintptr_t)xbe_va(0x00288648);
    uint8_t want = (slot >= 0 && slot < 8) ? (uint8_t)tbl[slot] : 0xFF;
    logf("coop menu: JOIN: starting the host's mission - LevelSelect(%s) (level id %u, wanted %u), SelectedDifficulty(%s)%s", level, got, want,
         difficulty, unlocked ? " - unlocked for the start (not saved)" : "");
    if (got != want) logf("coop menu: JOIN: WARNING the level picked (%u) is not the host's (%u)", got, want);
    selected_difficulty(difficulty);
    // owner (20261115): "wouldn't that corrupt the joiner's save - a mission cleared on a difficulty he didn't unlock?"
    // -> co-op progress counts: the mission at the difficulty actually played STAYS unlocked (the game's autosave after
    // the results then writes a consistent save: unlocked + completed + the next mission); the other difficulties are put back.
    for (int d = 0; d < 4; d++) if (flags[d] && d != diff) *flags[d] = keep[d];
    if (diff >= 0 && diff < 4 && flags[diff] && keep[diff] != 1)
        logf("coop menu: JOIN: mission slot %d at difficulty %d stays unlocked in our profile (played in co-op)", slot, diff);
}

void leave_all()
{
    if (g_cfg.coop_role && !g_started && !g_to_title) {   // a lobby after a co-op mission: leaving it leaves the session
        logf("coop menu: left the lobby during a co-op session - the session ends for both");
        coop_session_end(0);
    }
    if (g_page == HostLobby) logf("coop menu: HOST lobby closed");
    if (g_page == JoinCode) join_leave();
    g_have_joiner = false;
    net_close();
}

} // namespace

bool coop_menu_page(int id)
{
    bool coop = id >= 200 && id < 300;
    if (id != g_page) {
        if (g_page == HostLobby || g_page == JoinCode) leave_all();
        g_coop_code_len = -1;
        if (coop || g_page != None) clear_values();   // PC SETTINGS pages keep their values
        g_page = coop ? id : None;
        if (g_page == HostLobby) host_open();
        if (g_page == JoinCode) join_open();
        if (g_page == HostLobby || g_page == JoinCode) g_rejoin = false;
    }
    return coop;
}

void coop_menu_action(int id)
{
    ULONGLONG now = GetTickCount64();
    switch (id) {
    case 11:
    case 21: logf("coop menu: INTERNET selected - not available yet (LAN first)"); break;
    case 31: if (g_page == JoinCode) join_connect(now); break;
    case 32:   // LEAVE LOBBY (owner: "a quit button in the lobby that makes the player who pressed it quit"): back to JOIN GAME
    case 42:   // ... the host's: back to HOST GAME; the other player sees the lobby partner gone
        if (g_page != JoinCode && g_page != HostLobby) break;
        logf("coop menu: %s: LEAVE LOBBY", g_page == HostLobby ? "HOST" : "JOIN");
        fe_goto(g_page == HostLobby ? "CoopHost" : "CoopJoin");   // the page change closes the lobby (leave_all)
        break;
    case 41:   // START MISSION: player 2 in and READY -> co-op on for both, mission select
        if (g_page != HostLobby || g_started) break;
        if (!g_have_joiner || !g_peer_ready) {
            g_start_refused = true;
            logf("coop menu: HOST: START MISSION refused - player 2 %s", g_have_joiner ? "is not ready" : "is not in the lobby");
            break;
        }
        g_started = true;
        logf("coop menu: HOST: START MISSION with player 2 ready - co-op on, the joiner follows");
        for (int r = 0; r < 3; r++) send_to(g_joiner, kActivate, g_code);
        coop_activate(1, g_joiner.sin_addr.s_addr);
        fe_goto("MissionModeSelect");
        break;
    default: logf("coop menu: action %d", id); break;
    }
}

void coop_menu_change(int id, int dir)
{
    if (id == 409 || id == 410) {   // the host lobby: DIFFICULTY (VANILLA / CO-OP) and SPECIAL WEAPONS (host only; to the joiner with the heartbeat)
        (void)dir;
        if (g_page == HostLobby && !g_started) {
            if (id == 409) g_cfg.coop_diffmode = g_cfg.coop_diffmode ? 0 : 1;
            else g_cfg.coop_special = !g_cfg.coop_special;
            settings_save();
            logf("coop menu: HOST: %s %s", id == 409 ? "DIFFICULTY" : "SPECIAL WEAPONS",
                 id == 409 ? (g_cfg.coop_diffmode ? "CO-OP" : "VANILLA") : (g_cfg.coop_special ? "ON" : "OFF"));
        }
        return;
    }
    if (id == 408) {   // the host lobby: SPRINT on / off (host only; it reaches the joiner with the heartbeat)
        (void)dir;
        if (g_page == HostLobby && !g_started) {
            g_cfg.coop_sprint = !g_cfg.coop_sprint;
            settings_save();
            logf("coop menu: HOST: SPRINT %s", g_cfg.coop_sprint ? "ON" : "OFF");
        }
        return;
    }
    if (id == 3) {   // the CO-OP page: HIT MARKER on / off (local to this player)
        (void)dir;
        g_cfg.coop_hitmarker = !g_cfg.coop_hitmarker;
        settings_save();
        logf("coop menu: HIT MARKER %s", g_cfg.coop_hitmarker ? "ON" : "OFF");
        show(2, g_cfg.coop_hitmarker ? "ON" : "OFF");
        return;
    }
    if (g_page != JoinCode) return;
    if (id == 407) {   // READY: in the lobby only, until the host starts
        if (g_join == Join::InLobby && !g_started) { g_ready = !g_ready; logf("coop menu: JOIN: %s", g_ready ? "READY" : "not ready"); }
        return;
    }
    if (g_join == Join::Searching || g_join == Join::Joining || g_join == Join::InLobby) return;
    if (id == 401) {   // CODE: the cursor
        int n = entry_len();
        int hi = n < kCodeLen ? n : kCodeLen - 1;
        g_cursor += dir;
        if (g_cursor < 0) g_cursor = 0;
        if (g_cursor > hi) g_cursor = hi;
    } else if (id == 402) {   // LETTER: the character under the cursor
        int n = entry_len();
        if (g_cursor > n) g_cursor = n;
        const char *p = g_entry[g_cursor] ? strchr(kAlphabet, g_entry[g_cursor]) : nullptr;
        int k = p ? (int)(p - kAlphabet) : (dir > 0 ? -1 : 0);
        k = (k + dir + 32) % 32;
        g_entry[g_cursor] = kAlphabet[k];
        g_join = Join::Idle;
    }
}

void coop_menu_after_mission(int role)
{
    g_back_role = role;
    logf("coop menu: the co-op mission ended - back to the %s after the results", role == 1 ? "host lobby" : "lobby (rejoin)");
}

void coop_menu_session_ended(int reason)
{
    g_back_role = 0;
    g_to_title = true;
    logf("coop menu: the co-op session ended (reason %d) - the menus go to the title", reason);
}

void coop_menu_poll()
{
    // the partner's CLQT also reaches us in the menus - NOT in a mission: this poll runs with the in-game front end
    // too (HUD / pause) and coop_poll_control drops everything but control messages (play 20261114_164423: the
    // joiner's enemy / anim / AI packets were eaten - "joiner enemies didn't sync at all")
    if (g_cfg.coop_role && !coop_in_mission()) coop_poll_control();
    if (g_to_title) {   // the session ended: from mission select or a co-op page, to the title
        uint32_t fe = *(const uint32_t *)(uintptr_t)xbe_va(0x002D199C);
        const char *page = fe ? (const char *)(uintptr_t)(fe + 0x3840) : "";
        if (_strnicmp(page, "MissionModeSelect", 17) == 0 || _strnicmp(page, "Coop", 4) == 0) {
            logf("coop menu: %s -> BlackTitleMenu (the co-op session ended)", page);
            g_to_title = false;
            if (g_page == HostLobby || g_page == JoinCode) { if (g_page == JoinCode) join_leave(); g_have_joiner = false; net_close(); }
            g_page = None;
            fe_goto("BlackTitleMenu");
        } else if (_strnicmp(page, "BlackTitleMenu", 14) == 0) g_to_title = false;
    }
    if (g_back_role) {   // after a co-op mission: the game's mission select becomes our lobby page
        uint32_t fe = *(const uint32_t *)(uintptr_t)xbe_va(0x002D199C);
        const char *page = fe ? (const char *)(uintptr_t)(fe + 0x3840) : "";
        if (_strnicmp(page, "MissionModeSelect", 17) == 0) {
            const char *to = g_back_role == 1 ? "CoopLobby" : "CoopCode";
            logf("coop menu: mission select after a co-op mission -> %s", to);
            g_rejoin = true;
            g_back_role = 0;
            fe_goto(to);
        }
    }
    if (g_page == Menu || g_page == Menu + 1) show(2, g_cfg.coop_hitmarker ? "ON" : "OFF");   // the CO-OP page's HIT MARKER row
    if (g_page != HostLobby && g_page != JoinCode) { g_coop_lobby_banner[0][0] = g_coop_lobby_banner[1][0] = 0; return; }
    ULONGLONG now = GetTickCount64();
    if (g_page == HostLobby) host_poll(now);
    else join_poll(now);
}

void coop_menu_char(wchar_t c)
{
    if (g_coop_code_len < 0) return;
    uint32_t w = g_kw;
    if (w - g_kr >= 32) return;
    g_keys[w % 32] = c;
    g_kw = w + 1;
}

// The SHARED CINEMATIC SKIP (owner, day 15): a movie is skipped when BOTH players pressed; the first one keeps watching
// (owner: "the first one that wants to skip should still be able to see the cinematic").
// sk2: holding the game's TriggerFMVDone was too late - the briefing page had already stopped the video at the press (a
// black screen for the first presser). So the PRESS is gated (xinput, after every input source is merged): while a
// movie plays in co-op the first press of A / START / Enter never reaches the game, it only sends CLSK and shows the
// popup; a press when the partner's CLSK is already there goes through (the game skips, and CLSK tells the partner);
// the CLSK of the partner while we wait makes one real A press for a few polls (the game skips the normal way). The
// co-op receive loop runs in the play tick, which does not run during a movie: the gate reads the partner's CLSK
// itself (coop_poll_control). A movie that just ends goes on as always.
static bool g_skip_wait, g_skip_pass, g_skip_was_down;
static int g_skip_inject;
bool coop_skip_holding() { return g_cfg.coop_role && movies_playing() && !g_skip_pass && g_skip_inject == 0; }
void coop_skip_gate(uint16_t &buttons, uint8_t analog[8], bool &enter)
{
    bool movie = g_cfg.coop_role && movies_playing();
    static ULONGLONG off_since;
    ULONGLONG now = GetTickCount64();
    if (movie) off_since = 0; else if (!off_since) off_since = now;
    // sk4: the button is followed all the time - a press held from the menu into the movie's start is no skip request
    bool down = analog[0] > 30 || (buttons & 0x10) || enter;
    bool press = down && !g_skip_was_down;
    g_skip_was_down = down;
    if (!movie) {   // between movies: reset; a partner's press is stale 5 s after the movie
        g_skip_wait = g_skip_pass = false;
        g_skip_inject = 0;
        if (g_coop_peer_skip && off_since && now - off_since > 5000) g_coop_peer_skip = 0;
        g_coop_skip_ui = 0;
        return;
    }
    coop_poll_control();
    if (g_coop_peer_skip && now - g_coop_peer_skip_ms > 60000) g_coop_peer_skip = 0;
    if (g_skip_wait && g_coop_peer_skip && !g_skip_inject) {   // both pressed: one real A press for the game
        logf("coop: skip: both players pressed - the movie is skipped");
        g_skip_wait = false;
        g_coop_peer_skip = 0;
        g_skip_pass = true;
        g_skip_inject = 8;
    }
    if (g_skip_inject > 0) {
        g_skip_inject--;
        analog[0] = g_skip_inject > 2 ? 255 : 0;   // down 6 polls, then up (a press, not a hold)
    } else if (!g_skip_pass) {
        if (press) {
            if (g_coop_peer_skip) {   // the partner pressed first: ours completes it - through to the game
                logf("coop: skip: we pressed too - the movie is skipped");
                coop_send_skip();
                g_coop_peer_skip = 0;
                g_skip_pass = true;
            } else if (!g_skip_wait) {
                logf("coop: skip: we pressed - waiting for the partner (the movie plays on)");
                g_skip_wait = true;
                coop_send_skip();
            }
        }
        if (!g_skip_pass) {   // held: the game sees no skip press
            analog[0] = 0;
            buttons &= ~0x10;
            enter = false;
        }
    }
    g_coop_skip_ui = g_skip_wait ? 1 : (g_coop_peer_skip ? 2 : 0);
}

// SHARED INTRO-TEXT SKIP (owner two-PC play 20261115: "the intro text at the beginning of the level should be skipped
// for all players when a player presses a button"). While a mission's intro text types out (bindings::intro_text) a
// START press (Enter = START there) goes to the game as always and tells the partner (CLIT); the partner's CLIT makes one
// START press here (down 6 polls). The play tick's receive loop does not run yet: the gate reads it (coop_poll_control).
void coop_send_intro_skip();
void coop_intro_gate(uint16_t &buttons, bool enter)
{
    static bool was_down; static int inject; static ULONGLONG pressed_ms;
    bool intro = g_cfg.coop_role && coop_in_mission() && !movies_playing() && bindings::intro_text();
    ULONGLONG now = GetTickCount64();
    { static bool was; static int n; if (intro != was && n++ < 20) logf("coop: intro text: phase %s", intro ? "ON" : "off"); was = intro; }
    bool down = (buttons & 0x10) || enter;
    bool press = down && !was_down;
    was_down = down;
    if (!intro) {
        inject = 0;
        if (g_coop_peer_intro && now - g_coop_peer_intro_ms > 8000) g_coop_peer_intro = 0;
        // Our START ended the intro: tell the partner only when it took us INTO PLAY (the "4 DAYS EARLIER" card was up:
        // flow test 20261115, both copies in play 3.2 s after the joiner's press). During the plain fade-in with no card
        // (COOP_FORCE_LEVEL tests) START queues the pause menu instead (intro4: both copies paused 2 s later).
        if (pressed_ms && now - pressed_ms < 1500 && bindings::in_play()) {
            logf("coop: intro text: we skipped it - telling the partner (CLIT)");
            coop_send_intro_skip();
            pressed_ms = 0;
        } else if (pressed_ms && now - pressed_ms >= 1500) pressed_ms = 0;
        return;
    }
    coop_poll_control();
    if (inject > 0) {
        inject--;
        if (inject > 2) buttons |= 0x10; else buttons &= (uint16_t)~0x10;   // down, then up: one press
        return;
    }
    if (press) { pressed_ms = now; g_coop_peer_intro = 0; return; }   // CLIT goes once START has taken us into play (above)
    if (g_coop_peer_intro) {
        g_coop_peer_intro = 0;
        if (now - g_coop_peer_intro_ms < 8000) {
            logf("coop: intro text: the partner skipped it - skipping here too (START)");
            inject = 8;
        }
    }
}

bool coop_menu_game_call(const char *name, const char *p1, const char *, const char *)
{
    if (strcmp(name, "QuitLevel") == 0 && g_cfg.coop_role) {   // the pause menu's QUIT: this player leaves the session
        logf("coop menu: QuitLevel in a co-op mission - the session ends for both");
        coop_session_end(0);
        return false;   // the game quits the level as usual
    }
    if (strcmp(name, "LevelSelect") == 0) { strncpy(g_level, p1, sizeof g_level - 1); return false; }
    {   // tests (the squad survey): COOP_FORCE_LEVEL=<n> starts mission n at the difficulty confirm (unlocked for the call)
        char e[8] = {0};
        if (strcmp(name, "SelectedDifficulty") == 0 && GetEnvironmentVariableA("COOP_FORCE_LEVEL", e, sizeof e) && e[0]) {
            logf("coop menu: test: COOP_FORCE_LEVEL=%s - starting that mission instead", e);
            start_mission(e, p1);
            return true;
        }
    }
    if (strcmp(name, "SelectedDifficulty") == 0 && g_page == HostLobby && g_started && g_have_joiner) {
        memset(&g_go, 0, sizeof g_go);
        g_go.magic = kGo;
        memcpy(g_go.code, g_code, kCodeLen);
        strncpy(g_go.level, g_level, 7);
        strncpy(g_go.difficulty, p1, 7);
        g_go_sends = 5;
        logf("coop menu: HOST: mission start LevelSelect(%s) SelectedDifficulty(%s) -> the joiner", g_go.level, g_go.difficulty);
        return false;   // the host's own start goes on
    }
    return false;
}

bool coop_lobby_peer(int *role, uint32_t *ip, uint16_t *port)
{
    if (g_page == HostLobby && g_have_joiner) { *role = 1; *ip = g_joiner.sin_addr.s_addr; *port = g_joiner.sin_port; return true; }
    if (g_page == JoinCode && g_join == Join::InLobby) { *role = 2; *ip = g_host.sin_addr.s_addr; *port = g_host.sin_port; return true; }
    return false;
}
