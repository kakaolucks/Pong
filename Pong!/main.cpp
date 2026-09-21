// ==========================================================
//  Console-style Online Pong  (Win32 GUI, fullscreen, NOT a console app)
//  - Always fullscreen borderless window. Retro-style text
//    screens for menus; gameplay drawn as solid white blocks.
//  - No dedicated server required: the HOST's own process
//    listens on a TCP port and acts as the server for the
//    session; the other player JOINs by typing (or pasting)
//    the host's IP address / hostname and port.
//  - After joining, both players see a LOBBY and must press
//    START before the round begins.
//  - Ball speed increases gradually over the 5-minute match,
//    plus a small kick on every paddle hit (resets on score).
//  - After every point, both players vote "ready" before the
//    next serve (like a mini lobby).
//  - Random items appear on the field. Three of them run a
//    shared "freeze -> ease back to speed" sequence on the
//    ball (all content-animation only, no window rotation):
//      SPEED UP   - ball moves faster for 5s
//      REWIND     - visually rewinds the last 5s like a
//                   reverse-playback, then eases back to speed
//      BOUNCE     - ball just bounces off it
//      PADDLE+    - last hitter's paddle is faster for 10s
//      REVERSE    - last hitter's controls are reversed for 10s;
//                   ball freezes 5s (first 1.7s shows a big
//                   "REVERSED!" banner on their screen only),
//                   then eases back to speed over 5s
//      ROTATE     - the field layout morphs to portrait for 30s
//                   (eased content animation, no real window
//                   rotation); ball freezes 5s then eases back
//                   to speed over 5s; move with A/D or Left/Right
//                   while rotated
//  - After a match ends, both players get 15 seconds to vote
//    "play again"; if both agree, a new round starts on the
//    same connection. Otherwise both return to the main menu.
//
//  Build (MinGW):
//    g++ -O2 -std=c++17 -mwindows -o pong.exe pong.cpp -lgdi32 -luser32 -lws2_32
//
//  Controls:
//    Menu    : click a button, or press 1 / 2 / 3
//    Text box: type normally; Ctrl+V pastes from clipboard
//    Lobby   : ENTER to mark yourself ready / start
//    Game    : W/S, Up/Down, A/D or Left/Right to move, ESC to quit match
//    Result  : ENTER to vote "play again"
// ==========================================================

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <string>
#include <vector>
#include <deque>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include <random>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

// ---------------- Game constants ----------------
static const int   FIELD_W = 60;
static const int   FIELD_H = 20;
static const int   PADDLE_H = 4;
static const float PADDLE_ZONE = 2.0f; // field units from the edge where a paddle's front face sits (used for BOTH collision and drawing, so they always match)
static const float BALL_WALL_MARGIN = 0.3f; // field units of margin for the top/bottom wall bounce
static const float PADDLE_SPD = 0.45f;
static const float BALL_SPD_INIT = 0.3f;
static const int   WIN_SCORE = 10;
static const int   TICK_MS = 16;   // ~60 fps / ticks per second, for smooth motion
static const int   MATCH_SECONDS = 300;  // 5-minute match
static const float SPEED_MAX_MULT = 3.0f; // ball is up to 3x faster by the end of the match
static const float HIT_SPEEDUP = 1.03f;
static const int   REMATCH_SECONDS = 15;

// ---------------- Item constants ----------------
static const uint8_t ITEM_NONE = 0, ITEM_SPEEDUP = 1, ITEM_REWIND = 2, ITEM_BOUNCE = 3,
ITEM_PADDLESPEED = 4, ITEM_REVERSE = 5, ITEM_ROTATE = 6;
static const float ITEM_PICKUP_DIST = 1.6f;
static const int   ITEM_PX = 32;
static const int   ITEM_SPEEDUP_SECONDS = 5;
static const int   ITEM_REWIND_SECONDS = 5;
static const int   ITEM_PADDLESPEED_SECONDS = 10;
static const int   ITEM_REVERSE_SECONDS = 10;
static const int   ITEM_ROTATE_SECONDS = 30;
static const float ITEM_SPEEDUP_MULT = 1.8f;
static const float ITEM_PADDLESPEED_MULT = 1.8f;
static const float ITEM_SPAWN_MIN_SEC = 3.0f;
static const float ITEM_SPAWN_MAX_SEC = 6.0f;

// Shared ball freeze/ease sequence timings (reverse, rotate, rewind items)
static const float BALL_FREEZE_SECONDS = 5.0f; // reverse/rotate: ball frozen this long
static const float BALL_EASE_SECONDS = 5.0f; // all three: ease back to speed over this long
static const float REVERSE_INDICATOR_SECONDS = 1.7f; // "REVERSED!" banner shown this long
static const float REWIND_PLAYBACK_SECONDS = 5.0f; // reverse-playback scrub duration

static float easeInOut(float x) { if (x < 0) x = 0; if (x > 1) x = 1; return x * x * (3.0f - 2.0f * x); }

// Maximum ball speed (field units per tick, before the match's time-based speedup),
// so repeated paddle hits / items can never make it fast enough to tunnel through a
// paddle or the field boundary in a single simulation step.
static const float MAX_BALL_SPEED = 1.0f;
static void clampBallSpeed(float& vx, float& vy) {
    float sp = std::sqrt(vx * vx + vy * vy);
    if (sp > MAX_BALL_SPEED && sp > 0.0001f) {
        float k = MAX_BALL_SPEED / sp;
        vx *= k; vy *= k;
    }
}

static const char* itemNameFor(uint8_t t) {
    switch (t) {
    case ITEM_SPEEDUP:     return "ball speed";
    case ITEM_REWIND:      return "rewind";
    case ITEM_BOUNCE:      return "bounce";
    case ITEM_PADDLESPEED: return "paddle speed";
    case ITEM_REVERSE:     return "reversed controls";
    case ITEM_ROTATE:      return "rotate";
    default:                return "?";
    }
}

// 32x32 1-bit icon textures (1 = black pixel, 0 = transparent/background),
// converted from the uploaded PNGs: each uint32_t is one row, MSB = leftmost pixel.
static const uint32_t ICON_SPEEDUP[32] = { 0x00000000u,0x7FFE7FFEu,0x7FFC3FFEu,0x7FF99FFEu,0x7FF3CFFEu,0x7FE667FEu,0x7FCC33FEu,0x7F9999FEu,0x7F33CCFEu,0x7E66667Eu,0x7CCC333Eu,0x7999999Eu,0x7333CCCEu,0x6667E666u,0x4CCFF332u,0x199FF998u,0x333FFCCCu,0x667FFE66u,0x4CFFFF32u,0x19FFFF98u,0x33FFFFCCu,0x67FFFFE6u,0x4FFFFFF2u,0x1FFFFFF8u,0x3FFFFFFCu,0x70846D0Eu,0x77B5AD6Eu,0x7085AD0Eu,0x7EBDAD7Eu,0x70BC737Eu,0x7FFFFFFEu,0x00000000u };
static const uint32_t ICON_REWIND[32] = { 0x00000000u,0x7FFFFFFEu,0x7FFFFFFEu,0x7E0C15D0u,0x7EFF7C96u,0x7EFF7556u,0x020F7552u,0x03EF75D6u,0x7FEF75D6u,0x7E0F75D0u,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7E0C15D0u,0x7EFF7C96u,0x7EFF7556u,0x020F7552u,0x03EF75D6u,0x7FEF75D6u,0x7E0F75D0u,0x7FFFFFFEu,0x7FFFFFFEu,0x7E0C15D0u,0x7EFF7C96u,0x7EFF7556u,0x020F7552u,0x03EF75D6u,0x7FEF75D6u,0x7E0F75D0u,0x7FFFFFFEu,0x7FFFFFFEu,0x00000000u };
static const uint32_t ICON_BOUNCE[32] = { 0x00000000u,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7000000Eu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x00000000u };
static const uint32_t ICON_PADDLESPD[32] = { 0x00000000u,0x7FFFFFFEu,0x477FFFC2u,0x46BFFFDAu,0x45DFFFC2u,0x477FFFDEu,0x46BFFFDEu,0x45DFFFFEu,0x477FFFC6u,0x46BFFFDAu,0x45DFFFDAu,0x7FFFFFDAu,0x4108FFC6u,0x5F6B7FFEu,0x5F6B7FC6u,0x410B7FDAu,0x7D7B7FDAu,0x7D7B7FDAu,0x4178FFC6u,0x7FFFFFFEu,0x5A1FFFDEu,0x5ADFFFDEu,0x5A1FFFDEu,0x5AFFFFC2u,0x42FFFFFEu,0x7FFFFFC2u,0x7FFFFFDEu,0x7FFFFFC6u,0x7FFFFFDEu,0x7FFFFFC2u,0x7FFFFFFEu,0x00000000u };
static const uint32_t ICON_REVERSE[32] = { 0x00000000u,0x7FFFFFFEu,0x4F7FFFFEu,0x4F7FFFFEu,0x4F7FFFFEu,0x4D5FFFFEu,0x4E3FFFFEu,0x4F7FFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x4615118Eu,0x5AF576BEu,0x4235318Eu,0x56F575EEu,0x5A1B168Eu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x4F7FFFFEu,0x4E3FFFFEu,0x4D5FFFFEu,0x4F7FFFFEu,0x4F7FFFFEu,0x4F7FFFFEu,0x7FFFFFFEu,0x00000000u };
static const uint32_t ICON_ROTATE[32] = { 0x00000000u,0x7FFFFFFEu,0x403B83FEu,0x5FBDBBFEu,0x5FA0BBFEu,0x5FBDBBFEu,0x403BBBFEu,0x7FFFBBFEu,0x7FFFBBFEu,0x7FFF83FEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x41041FFEu,0x5DDF7FFEu,0x41DF7FFEu,0x4FDF7FFEu,0x57DF7FFEu,0x59DF7FFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x7FFFFFFEu,0x00000000u };

static const uint32_t* iconBitsFor(uint8_t t) {
    switch (t) {
    case ITEM_SPEEDUP:     return ICON_SPEEDUP;
    case ITEM_REWIND:      return ICON_REWIND;
    case ITEM_BOUNCE:      return ICON_BOUNCE;
    case ITEM_PADDLESPEED: return ICON_PADDLESPD;
    case ITEM_REVERSE:     return ICON_REVERSE;
    case ITEM_ROTATE:      return ICON_ROTATE;
    default:                return nullptr;
    }
}

// ---------------- Network protocol ----------------
#pragma pack(push, 1)
struct InputMsg {
    int8_t dir; // gameplay: -1/0/1 movement. lobby/rematch/point-break phase: 0/1 vote.
};
struct StateMsg {
    float   ballX, ballY;
    float   p1Y, p2Y;      // p1 = host (left/top), p2 = joiner (right/bottom)
    int32_t score1, score2;
    uint8_t phase;         // 0 = lobby, 1 = playing, 2 = post-game vote, 3 = point-break vote
    uint8_t hostVote, clientVote;
    int32_t secondsLeft;   // match clock (phase 1) or vote countdown (phase 0/2/3)
    uint8_t restarting;    // pulses 1 on the tick a new round begins
    uint8_t itemActive;
    uint8_t itemType;
    float   itemX, itemY;
    uint8_t hostFastLeft, clientFastLeft; // seconds left of paddle-speed buff
    uint8_t hostRevLeft, clientRevLeft;   // seconds left of reversed controls
    uint8_t ballBoostLeft;                // seconds left of ball-speed item
    uint8_t rotateLeft;                   // seconds left of screen-rotate effect
    uint8_t ballSeqKind;                  // 0 none, 1 reverse, 2 rotate, 3 rewind
    uint8_t ballSeqAffected;              // 0 none, 1 host, 2 client (kind==1 only)
    float   ballSeqElapsed;               // seconds since the current ball sequence began
};
#pragma pack(pop)

static bool sendAll(SOCKET s, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int n = send(s, buf + sent, len - sent, 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        sent += n;
    }
    return true;
}
static bool recvAll(SOCKET s, char* buf, int len) {
    int got = 0;
    while (got < len) {
        int n = recv(s, buf + got, len - got, 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        got += n;
    }
    return true;
}
static bool sendStruct(SOCKET s, const void* p, int sz) { return sendAll(s, (const char*)p, sz); }
static bool recvStruct(SOCKET s, void* p, int sz) { return recvAll(s, (char*)p, sz); }

static void clampPaddle(float& y) {
    if (y < 0) y = 0;
    if (y > FIELD_H - PADDLE_H) y = (float)(FIELD_H - PADDLE_H);
}

static std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> out;
    std::stringstream ss(s);
    std::string line;
    while (std::getline(ss, line)) out.push_back(line);
    if (out.empty()) out.push_back("");
    return out;
}

// ---------------- Application state ----------------
enum class AppState {
    MENU,
    HOST_INPUT_PORT,
    HOST_WAITING,
    JOIN_INPUT_IP,
    JOIN_INPUT_PORT,
    JOIN_CONNECTING,
    PLAYING, // covers lobby (phase 0), active play (phase 1), post-game vote (phase 2), point-break (phase 3)
    ERRORMSG
};

static AppState g_state = AppState::MENU;
static std::string g_inputBuffer;
static std::string g_joinIp;
static std::string g_errorMsg;

static std::atomic<bool> g_keyUp{ false };
static std::atomic<bool> g_keyDown{ false };

static std::atomic<bool> g_cancelRequested{ false }; // cancel while waiting/connecting
static std::atomic<bool> g_abortMatch{ false };       // user pressed ESC mid-session
static std::atomic<bool> g_workerFinished{ false };
static std::atomic<bool> g_matchStarted{ false };
static std::atomic<int>  g_localRematchVote{ 0 };     // reused for lobby-ready, rematch AND point-break votes
static std::thread g_workerThread;

struct RenderState {
    std::mutex mtx;
    float ballX = FIELD_W / 2.0f, ballY = FIELD_H / 2.0f;
    float p1Y = FIELD_H / 2.0f - PADDLE_H / 2.0f;
    float p2Y = FIELD_H / 2.0f - PADDLE_H / 2.0f;
    int   score1 = 0, score2 = 0;
    bool  gameOver = false;
    bool  isHost = false;
    int   phase = 0; // 0=lobby,1=playing,2=postgame,3=point-break
    int   secondsLeft = MATCH_SECONDS;
    int   rematchVotes = 0;
    std::string status;
    int   itemActive = 0;
    int   itemType = 0;
    float itemX = 0, itemY = 0;
    int   hostFastLeft = 0, clientFastLeft = 0;
    int   hostRevLeft = 0, clientRevLeft = 0;
    int   ballBoostLeft = 0;
    int   rotateLeft = 0;
    int   ballSeqKind = 0;
    int   ballSeqAffected = 0;
    float ballSeqElapsed = 0;
    bool  isCpuMode = false;
};
static RenderState g_render;

// ---------------- Networking workers ----------------
static void hostWorker(int port) {
    g_matchStarted = false;
    {
        std::lock_guard<std::mutex> lk(g_render.mtx);
        g_render.status = "Creating socket...";
    }
    SOCKET listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock == INVALID_SOCKET) { g_errorMsg = "socket() failed."; g_workerFinished = true; return; }

    int opt = 1;
    setsockopt(listenSock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons((u_short)port);

    if (bind(listenSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) {
        g_errorMsg = "bind() failed - the port may already be in use.";
        closesocket(listenSock);
        g_workerFinished = true;
        return;
    }
    if (listen(listenSock, 1) == SOCKET_ERROR) {
        g_errorMsg = "listen() failed.";
        closesocket(listenSock);
        g_workerFinished = true;
        return;
    }

    u_long nb = 1;
    ioctlsocket(listenSock, FIONBIO, &nb);

    {
        std::lock_guard<std::mutex> lk(g_render.mtx);
        g_render.status = "Listening on port " + std::to_string(port) + "\n"
            "Waiting for an opponent to join...\n"
            "Share your IP address and this port with them.";
    }

    SOCKET client = INVALID_SOCKET;
    while (!g_cancelRequested) {
        fd_set rfds; FD_ZERO(&rfds); FD_SET(listenSock, &rfds);
        timeval tv{ 0, 200000 };
        int r = select(0, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            client = accept(listenSock, nullptr, nullptr);
            if (client != INVALID_SOCKET) break;
        }
    }
    closesocket(listenSock);

    if (client == INVALID_SOCKET) { // cancelled
        g_workerFinished = true;
        return;
    }

    u_long bl = 0;
    ioctlsocket(client, FIONBIO, &bl);
    int one = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

    {
        std::lock_guard<std::mutex> lk(g_render.mtx);
        g_render.isHost = true;
        g_render.isCpuMode = false;
        g_render.status = "You are the LEFT paddle (host).";
        g_render.phase = 0;
    }
    g_matchStarted = true;

    std::atomic<bool> rxAlive{ true };
    std::atomic<int>  peerVal{ 0 }; // movement dir during play; vote (0/1) otherwise
    std::thread rx([&]() {
        InputMsg m;
        while (rxAlive) {
            if (!recvStruct(client, &m, sizeof(m))) { rxAlive = false; break; }
            peerVal = m.dir;
        }
        });

    std::mt19937 rng{ std::random_device{}() };
    auto randRange = [&](float lo, float hi) { std::uniform_real_distribution<float> d(lo, hi); return d(rng); };
    auto randItemType = [&]() -> uint8_t { std::uniform_int_distribution<int> d(1, 6); return (uint8_t)d(rng); };

    auto sendEmptyState = [&](uint8_t phase, int hostVote, int clientVote, int secLeft, bool restarting) -> bool {
        StateMsg st{};
        {
            std::lock_guard<std::mutex> lk(g_render.mtx);
            st.ballX = g_render.ballX; st.ballY = g_render.ballY;
            st.p1Y = g_render.p1Y; st.p2Y = g_render.p2Y;
            st.score1 = g_render.score1; st.score2 = g_render.score2;
            // Forward the real ongoing attribute timers (rotate especially) so the
            // client's layout doesn't snap back to landscape during a vote screen
            // just because this packet type used to hardcode them to zero.
            st.hostFastLeft = (uint8_t)g_render.hostFastLeft; st.clientFastLeft = (uint8_t)g_render.clientFastLeft;
            st.hostRevLeft = (uint8_t)g_render.hostRevLeft; st.clientRevLeft = (uint8_t)g_render.clientRevLeft;
            st.ballBoostLeft = (uint8_t)g_render.ballBoostLeft; st.rotateLeft = (uint8_t)g_render.rotateLeft;
        }
        st.phase = phase; st.hostVote = (uint8_t)hostVote; st.clientVote = (uint8_t)clientVote;
        st.secondsLeft = secLeft; st.restarting = (uint8_t)(restarting ? 1 : 0);
        st.itemActive = 0; st.itemType = 0; st.itemX = 0; st.itemY = 0;
        st.ballSeqKind = 0; st.ballSeqAffected = 0; st.ballSeqElapsed = 0;
        return sendStruct(client, &st, sizeof(st));
        };

    // ---- LOBBY: wait for both players to press START ----
    {
        std::lock_guard<std::mutex> lk(g_render.mtx);
        g_render.phase = 0;
        g_render.rematchVotes = 0;
    }
    g_localRematchVote = 0;
    peerVal = 0;
    bool lobbyOk = false;
    while (rxAlive && !g_abortMatch) {
        int hostReady = g_localRematchVote.load();
        int clientReady = (peerVal.load() == 1) ? 1 : 0;
        { std::lock_guard<std::mutex> lk(g_render.mtx); g_render.rematchVotes = hostReady + clientReady; }
        if (!sendEmptyState(0, hostReady, clientReady, 0, false)) { rxAlive = false; break; }
        if (hostReady && clientReady) { lobbyOk = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
    }
    if (!lobbyOk || !rxAlive || g_abortMatch) {
        rxAlive = false;
        shutdown(client, SD_BOTH);
        closesocket(client);
        if (rx.joinable()) rx.join();
        g_workerFinished = true;
        return;
    }

    bool keepGoing = true;
    while (keepGoing && rxAlive && !g_abortMatch) {
        // ---- reset round state ----
        {
            std::lock_guard<std::mutex> lk(g_render.mtx);
            g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
            g_render.p1Y = FIELD_H / 2.0f - PADDLE_H / 2.0f;
            g_render.p2Y = g_render.p1Y;
            g_render.score1 = 0; g_render.score2 = 0;
            g_render.gameOver = false;
            g_render.phase = 1;
            g_render.rematchVotes = 0;
        }
        g_localRematchVote = 0;

        float ballVX = BALL_SPD_INIT, ballVY = BALL_SPD_INIT * 0.6f;
        bool over = false;
        auto matchStart = std::chrono::steady_clock::now();

        bool itemActive = false;
        uint8_t itemType = 0;
        float itemX = 0, itemY = 0;
        auto itemNextSpawn = matchStart + std::chrono::milliseconds((int)(randRange(ITEM_SPAWN_MIN_SEC, ITEM_SPAWN_MAX_SEC) * 1000));
        int lastHitBy = 0; // 0=none, 1=host, 2=client

        auto hostFastUntil = matchStart, clientFastUntil = matchStart;
        auto hostRevUntil = matchStart, clientRevUntil = matchStart;
        auto rotateUntil = matchStart;
        auto ballBoostUntil = matchStart;
        bool prevRotActive = false;
        auto lastTickTime = matchStart;

        // ---- shared ball freeze/ease sequence state ----
        int ballSeqKind = 0;          // 0 none, 1 reverse, 2 rotate, 3 rewind
        int ballSeqAffected = 0;      // for kind==1
        std::chrono::steady_clock::time_point ballSeqStart = matchStart;

        struct Snap { float t, bx, by, bvx, bvy, p1, p2; int s1, s2; };
        std::deque<Snap> history;

        // ---- gameplay loop ----
        while (rxAlive && !over && !g_abortMatch) {
            auto now = std::chrono::steady_clock::now();
            float elapsedSec = std::chrono::duration<float>(now - matchStart).count();
            if (elapsedSec < 0) elapsedSec = 0;
            if (elapsedSec > (float)MATCH_SECONDS) elapsedSec = (float)MATCH_SECONDS;
            float timeScale = 1.0f + (SPEED_MAX_MULT - 1.0f) * (elapsedSec / (float)MATCH_SECONDS);

            bool hostFast = now < hostFastUntil;
            bool clientFast = now < clientFastUntil;
            bool hostRevActive = now < hostRevUntil;
            bool clientRevActive = now < clientRevUntil;
            bool rotActive = now < rotateUntil;
            bool boostActive = now < ballBoostUntil;

            auto secsLeftOf = [&](std::chrono::steady_clock::time_point until, bool active) -> int {
                if (!active) return 0;
                int v = (int)std::ceil(std::chrono::duration<float>(until - now).count());
                if (v < 0) v = 0;
                return v;
                };
            int hostFastSec = secsLeftOf(hostFastUntil, hostFast);
            int clientFastSec = secsLeftOf(clientFastUntil, clientFast);
            int hostRevSec = secsLeftOf(hostRevUntil, hostRevActive);
            int clientRevSec = secsLeftOf(clientRevUntil, clientRevActive);
            int rotSec = secsLeftOf(rotateUntil, rotActive);
            if (rotSec > ITEM_ROTATE_SECONDS) rotSec = ITEM_ROTATE_SECONDS;
            int boostSec = secsLeftOf(ballBoostUntil, boostActive);

            // Rotation just ended naturally (not from a fresh pickup): give the ball
            // the same freeze-then-ease treatment as when it started. The ball's real
            // velocity is left untouched; only movement is paused/ramped (see below).
            if (prevRotActive && !rotActive && ballSeqKind == 0) {
                ballSeqKind = 2; ballSeqAffected = 0; ballSeqStart = now;
            }
            prevRotActive = rotActive;

            // Pause the attribute timers (reversed controls / paddle speed / rotate /
            // ball-speed-boost) while a ball freeze-or-ease sequence is in progress, so
            // their duration only "spends" once the ball is back to full speed.
            if (ballSeqKind != 0) {
                float dt = std::chrono::duration<float>(now - lastTickTime).count();
                if (dt < 0) dt = 0;
                if (dt > 0.25f) dt = 0.25f; // guard against huge gaps (e.g. debugger pause)
                auto shift = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(dt));
                if (hostRevActive)   hostRevUntil += shift;
                if (clientRevActive) clientRevUntil += shift;
                if (hostFast)        hostFastUntil += shift;
                if (clientFast)      clientFastUntil += shift;
                if (rotActive)       rotateUntil += shift;
                if (boostActive)     ballBoostUntil += shift;
            }
            lastTickTime = now;

            float speedMult = timeScale * (boostActive ? ITEM_SPEEDUP_MULT : 1.0f);

            int rawDir = (g_keyDown ? 1 : 0) - (g_keyUp ? 1 : 0);
            int myDir = hostRevActive ? -rawDir : rawDir;
            int rawPeer = peerVal.load();
            int peerDir = clientRevActive ? -rawPeer : rawPeer;
            float p1Spd = PADDLE_SPD * (hostFast ? ITEM_PADDLESPEED_MULT : 1.0f);
            float p2Spd = PADDLE_SPD * (clientFast ? ITEM_PADDLESPEED_MULT : 1.0f);

            // ---- ball sequence: freeze first, then ramp the DISPLACEMENT (never the
            // stored velocity itself) back up to full over BALL_EASE_SECONDS. This way
            // any wall/paddle bounces that happen during the ramp still work normally -
            // they were being erased every tick by the old "overwrite velocity" approach.
            // All three item kinds (reverse/rotate/rewind) share this exact same
            // freeze-then-ease shape; rewind's only difference is an instant position/
            // paddle/score/clock jump applied once, at pickup time (see below). ----
            bool ballLive = true;      // whether normal movement/collision runs this tick
            float seqRampMult = 1.0f;   // 0..1 extra multiplier on displacement while easing
            float seqElapsedOut = 0;
            if (ballSeqKind != 0) {
                float seqElapsed = std::chrono::duration<float>(now - ballSeqStart).count();
                seqElapsedOut = seqElapsed;
                if (seqElapsed < BALL_FREEZE_SECONDS) {
                    ballLive = false;
                }
                else {
                    float p2 = (seqElapsed - BALL_FREEZE_SECONDS) / BALL_EASE_SECONDS;
                    bool finishing = (p2 >= 1.0f);
                    if (finishing) p2 = 1.0f;
                    seqRampMult = easeInOut(p2);
                    if (finishing) ballSeqKind = 0;
                }
            }

            float bx, by, p1, p2; int s1, s2; int secLeft;
            bool justScored = false;
            {
                std::lock_guard<std::mutex> lk(g_render.mtx);
                g_render.p1Y += myDir * p1Spd;
                g_render.p2Y += peerDir * p2Spd;
                clampPaddle(g_render.p1Y);
                clampPaddle(g_render.p2Y);

                if (ballLive) {
                    float prevBallX = g_render.ballX;
                    g_render.ballX += ballVX * speedMult * seqRampMult;
                    g_render.ballY += ballVY * speedMult * seqRampMult;
                    if (g_render.ballY <= BALL_WALL_MARGIN || g_render.ballY >= FIELD_H - BALL_WALL_MARGIN) ballVY = -ballVY;

                    // Swept (crossing) collision so a fast ball can never skip over a
                    // paddle's collision plane in a single tick. The plane matches the
                    // paddle's drawn edge exactly (see drawGameScreen), so hits always
                    // look right regardless of screen resolution.
                    if (ballVX < 0 && prevBallX > PADDLE_ZONE && g_render.ballX <= PADDLE_ZONE &&
                        g_render.ballY >= g_render.p1Y - 1 && g_render.ballY <= g_render.p1Y + PADDLE_H) {
                        ballVX = -ballVX * HIT_SPEEDUP; ballVY *= HIT_SPEEDUP;
                        clampBallSpeed(ballVX, ballVY);
                        g_render.ballX = PADDLE_ZONE; lastHitBy = 1;
                    }
                    if (ballVX > 0 && prevBallX < FIELD_W - PADDLE_ZONE && g_render.ballX >= FIELD_W - PADDLE_ZONE &&
                        g_render.ballY >= g_render.p2Y - 1 && g_render.ballY <= g_render.p2Y + PADDLE_H) {
                        ballVX = -ballVX * HIT_SPEEDUP; ballVY *= HIT_SPEEDUP;
                        clampBallSpeed(ballVX, ballVY);
                        g_render.ballX = FIELD_W - PADDLE_ZONE; lastHitBy = 2;
                    }

                    // ---- item collision ----
                    if (itemActive) {
                        float dx = g_render.ballX - itemX, dy = g_render.ballY - itemY;
                        if (dx * dx + dy * dy <= ITEM_PICKUP_DIST * ITEM_PICKUP_DIST) {
                            switch (itemType) {
                            case ITEM_SPEEDUP:
                                ballBoostUntil = now + std::chrono::seconds(ITEM_SPEEDUP_SECONDS);
                                break;
                            case ITEM_BOUNCE:
                                ballVX = -ballVX;
                                break;
                            case ITEM_PADDLESPEED:
                                if (lastHitBy == 1) hostFastUntil = now + std::chrono::seconds(ITEM_PADDLESPEED_SECONDS);
                                else if (lastHitBy == 2) clientFastUntil = now + std::chrono::seconds(ITEM_PADDLESPEED_SECONDS);
                                break;
                            case ITEM_REVERSE:
                                if (lastHitBy == 1 || lastHitBy == 2) {
                                    bool already = (lastHitBy == 1) ? hostRevActive : clientRevActive;
                                    auto base = already ? (lastHitBy == 1 ? hostRevUntil : clientRevUntil) : now;
                                    if (lastHitBy == 1) hostRevUntil = base + std::chrono::seconds(ITEM_REVERSE_SECONDS);
                                    else clientRevUntil = base + std::chrono::seconds(ITEM_REVERSE_SECONDS);
                                    // Only freeze the ball on the FIRST pickup (effect was not
                                    // active yet). Picking up another one while it's still
                                    // active just extends the duration above - it must never
                                    // re-freeze the ball, even if the earlier freeze/ease has
                                    // already fully finished.
                                    if (!already) {
                                        ballSeqKind = 1; ballSeqAffected = lastHitBy; ballSeqStart = now;
                                    }
                                    // real velocity is left untouched; freeze/ramp only affects
                                    // whether/how much it's applied to position (see above)
                                }
                                break;
                            case ITEM_ROTATE: {
                                bool already = rotActive;
                                rotateUntil = (already ? rotateUntil : now) + std::chrono::seconds(ITEM_ROTATE_SECONDS);
                                if (!already) {
                                    ballSeqKind = 2; ballSeqAffected = 0; ballSeqStart = now;
                                }
                                break;
                            }
                            case ITEM_REWIND: {
                                if (ballSeqKind == 3) {
                                    // already mid rewind-freeze/ease; don't stop the ball again
                                    break;
                                }
                                float target = elapsedSec - (float)ITEM_REWIND_SECONDS;
                                const Snap* pick = nullptr;
                                for (auto& sN : history) { if (sN.t <= target) pick = &sN; else break; }
                                ballSeqKind = 3; ballSeqAffected = 0; ballSeqStart = now;
                                if (pick) {
                                    // instant jump back in time; the ball then freezes and
                                    // eases back to speed exactly like the other two items
                                    g_render.ballX = pick->bx; g_render.ballY = pick->by;
                                    g_render.p1Y = pick->p1; g_render.p2Y = pick->p2;
                                    g_render.score1 = pick->s1; g_render.score2 = pick->s2;
                                    ballVX = pick->bvx; ballVY = pick->bvy;
                                    clampBallSpeed(ballVX, ballVY);
                                    matchStart = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(pick->t));
                                }
                                break;
                            }
                            default: break;
                            }
                            itemActive = false;
                            itemNextSpawn = now + std::chrono::milliseconds((int)(randRange(ITEM_SPAWN_MIN_SEC, ITEM_SPAWN_MAX_SEC) * 1000));
                        }
                    }

                    if (g_render.ballX < 0) {
                        g_render.score2++; justScored = true;
                        g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
                        ballVX = BALL_SPD_INIT; ballVY = BALL_SPD_INIT * 0.6f; lastHitBy = 0;
                        ballSeqKind = 0;
                    }
                    else if (g_render.ballX > FIELD_W) {
                        g_render.score1++; justScored = true;
                        g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
                        ballVX = -BALL_SPD_INIT; ballVY = BALL_SPD_INIT * 0.6f; lastHitBy = 0;
                        ballSeqKind = 0;
                    }
                }

                if (!itemActive && now >= itemNextSpawn) {
                    itemActive = true;
                    itemType = randItemType();
                    itemX = randRange(10.0f, (float)FIELD_W - 10.0f);
                    itemY = randRange(2.0f, (float)FIELD_H - 2.0f);
                }

                secLeft = MATCH_SECONDS - (int)elapsedSec;
                if (secLeft < 0) secLeft = 0;
                g_render.secondsLeft = secLeft;

                if (g_render.score1 >= WIN_SCORE || g_render.score2 >= WIN_SCORE || elapsedSec >= (float)MATCH_SECONDS) {
                    g_render.gameOver = true; over = true;
                }

                g_render.itemActive = itemActive ? 1 : 0;
                g_render.itemType = itemType; g_render.itemX = itemX; g_render.itemY = itemY;
                g_render.hostFastLeft = hostFastSec; g_render.clientFastLeft = clientFastSec;
                g_render.hostRevLeft = hostRevSec; g_render.clientRevLeft = clientRevSec;
                g_render.rotateLeft = rotSec;
                g_render.ballSeqKind = ballSeqKind; g_render.ballSeqAffected = ballSeqAffected;
                g_render.ballSeqElapsed = (ballSeqKind != 0) ? seqElapsedOut : 0;

                bx = g_render.ballX; by = g_render.ballY; p1 = g_render.p1Y; p2 = g_render.p2Y;
                s1 = g_render.score1; s2 = g_render.score2;
            }

            history.push_back(Snap{ elapsedSec, bx, by, ballVX, ballVY, p1, p2, s1, s2 });
            while (!history.empty() && history.front().t < elapsedSec - 7.0f) history.pop_front();

            StateMsg st{};
            st.ballX = bx; st.ballY = by; st.p1Y = p1; st.p2Y = p2;
            st.score1 = s1; st.score2 = s2;
            st.phase = 1; st.hostVote = 0; st.clientVote = 0; st.secondsLeft = secLeft; st.restarting = 0;
            st.itemActive = itemActive ? 1 : 0; st.itemType = itemType; st.itemX = itemX; st.itemY = itemY;
            st.hostFastLeft = (uint8_t)hostFastSec; st.clientFastLeft = (uint8_t)clientFastSec;
            st.hostRevLeft = (uint8_t)hostRevSec; st.clientRevLeft = (uint8_t)clientRevSec;
            st.ballBoostLeft = (uint8_t)boostSec; st.rotateLeft = (uint8_t)rotSec;
            st.ballSeqKind = (uint8_t)ballSeqKind; st.ballSeqAffected = (uint8_t)ballSeqAffected;
            st.ballSeqElapsed = (ballSeqKind != 0) ? seqElapsedOut : 0.0f;
            if (!sendStruct(client, &st, sizeof(st))) { rxAlive = false; break; }

            if (justScored && !over) {
                // ---- point break: wait for both players before the next serve ----
                { std::lock_guard<std::mutex> lk(g_render.mtx); g_render.phase = 3; g_render.rematchVotes = 0; }
                g_localRematchVote = 0;
                peerVal = 0;
                bool ready = false;
                while (rxAlive && !g_abortMatch) {
                    int hostReady = g_localRematchVote.load();
                    int clientReady = (peerVal.load() == 1) ? 1 : 0;
                    { std::lock_guard<std::mutex> lk(g_render.mtx); g_render.rematchVotes = hostReady + clientReady; }
                    if (!sendEmptyState(3, hostReady, clientReady, 0, false)) { rxAlive = false; break; }
                    if (hostReady && clientReady) { ready = true; break; }
                    std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
                }
                if (rxAlive && !g_abortMatch && ready) {
                    std::lock_guard<std::mutex> lk(g_render.mtx);
                    g_render.phase = 1;
                }
                g_localRematchVote = 0;
            }

            auto elapsedT = std::chrono::steady_clock::now() - now;
            auto sleepMs = TICK_MS - std::chrono::duration_cast<std::chrono::milliseconds>(elapsedT).count();
            if (sleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }

        if (!rxAlive || g_abortMatch) { keepGoing = false; break; }

        // ---- post-game vote ----
        {
            std::lock_guard<std::mutex> lk(g_render.mtx);
            g_render.phase = 2;
            g_render.rematchVotes = 0;
            g_render.itemActive = 0;
            g_render.hostFastLeft = 0; g_render.clientFastLeft = 0;
            g_render.hostRevLeft = 0; g_render.clientRevLeft = 0;
            g_render.ballBoostLeft = 0; g_render.rotateLeft = 0;
            g_render.ballSeqKind = 0;
        }
        g_localRematchVote = 0;
        peerVal = 0;
        auto voteStart = std::chrono::steady_clock::now();
        bool restarting = false;
        while (rxAlive && !g_abortMatch) {
            int hostVote = g_localRematchVote.load();
            int clientVote = (peerVal.load() == 1) ? 1 : 0;
            int votes = (hostVote == 1 ? 1 : 0) + clientVote;
            float elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - voteStart).count();
            int secLeft = REMATCH_SECONDS - (int)elapsed;
            if (secLeft < 0) secLeft = 0;
            bool bothYes = (hostVote == 1 && clientVote == 1);

            { std::lock_guard<std::mutex> lk(g_render.mtx); g_render.rematchVotes = votes; g_render.secondsLeft = secLeft; }
            if (!sendEmptyState(2, hostVote, clientVote, secLeft, bothYes)) { rxAlive = false; break; }

            if (bothYes) { restarting = true; break; }
            if (elapsed >= (float)REMATCH_SECONDS) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
        }
        keepGoing = restarting && rxAlive && !g_abortMatch;
    }

    rxAlive = false;
    shutdown(client, SD_BOTH);
    closesocket(client);
    if (rx.joinable()) rx.join();
    g_workerFinished = true;
}

static void cpuWorker() {
    {
        std::lock_guard<std::mutex> lk(g_render.mtx);
        g_render.isHost = true;
        g_render.isCpuMode = true;
        g_render.status = "You are the LEFT paddle. Playing against the CPU.";
        g_render.phase = 1;
    }
    g_matchStarted = true;

    std::mt19937 rng{ std::random_device{}() };
    auto randRange = [&](float lo, float hi) { std::uniform_real_distribution<float> d(lo, hi); return d(rng); };
    auto randItemType = [&]() -> uint8_t { std::uniform_int_distribution<int> d(1, 6); return (uint8_t)d(rng); };

    bool keepGoing = true;
    while (keepGoing && !g_abortMatch) {
        // ---- reset round state ----
        {
            std::lock_guard<std::mutex> lk(g_render.mtx);
            g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
            g_render.p1Y = FIELD_H / 2.0f - PADDLE_H / 2.0f;
            g_render.p2Y = g_render.p1Y;
            g_render.score1 = 0; g_render.score2 = 0;
            g_render.gameOver = false;
            g_render.phase = 1;
        }

        float ballVX = BALL_SPD_INIT, ballVY = BALL_SPD_INIT * 0.6f;
        bool over = false;
        auto matchStart = std::chrono::steady_clock::now();

        bool itemActive = false;
        uint8_t itemType = 0;
        float itemX = 0, itemY = 0;
        auto itemNextSpawn = matchStart + std::chrono::milliseconds((int)(randRange(ITEM_SPAWN_MIN_SEC, ITEM_SPAWN_MAX_SEC) * 1000));
        int lastHitBy = 0; // 0=none, 1=host(you), 2=client(CPU)

        auto hostFastUntil = matchStart, clientFastUntil = matchStart;
        auto hostRevUntil = matchStart, clientRevUntil = matchStart;
        auto rotateUntil = matchStart;
        auto ballBoostUntil = matchStart;
        bool prevRotActive = false;
        auto lastTickTime = matchStart;

        int ballSeqKind = 0;
        int ballSeqAffected = 0;
        std::chrono::steady_clock::time_point ballSeqStart = matchStart;

        struct Snap { float t, bx, by, bvx, bvy, p1, p2; int s1, s2; };
        std::deque<Snap> history;

        float cpuTargetY = FIELD_H / 2.0f - PADDLE_H / 2.0f;
        float cpuVelY = 0.0f; // the CPU paddle's own smoothed velocity, for human-like acceleration

        // ---- gameplay loop ----
        while (!over && !g_abortMatch) {
            auto now = std::chrono::steady_clock::now();
            float elapsedSec = std::chrono::duration<float>(now - matchStart).count();
            if (elapsedSec < 0) elapsedSec = 0;
            if (elapsedSec > (float)MATCH_SECONDS) elapsedSec = (float)MATCH_SECONDS;
            float timeScale = 1.0f + (SPEED_MAX_MULT - 1.0f) * (elapsedSec / (float)MATCH_SECONDS);

            bool hostFast = now < hostFastUntil;
            bool clientFast = now < clientFastUntil;
            bool hostRevActive = now < hostRevUntil;
            bool clientRevActive = now < clientRevUntil;
            bool rotActive = now < rotateUntil;
            bool boostActive = now < ballBoostUntil;

            auto secsLeftOf = [&](std::chrono::steady_clock::time_point until, bool active) -> int {
                if (!active) return 0;
                int v = (int)std::ceil(std::chrono::duration<float>(until - now).count());
                if (v < 0) v = 0;
                return v;
                };
            int hostFastSec = secsLeftOf(hostFastUntil, hostFast);
            int clientFastSec = secsLeftOf(clientFastUntil, clientFast);
            int hostRevSec = secsLeftOf(hostRevUntil, hostRevActive);
            int clientRevSec = secsLeftOf(clientRevUntil, clientRevActive);
            int rotSec = secsLeftOf(rotateUntil, rotActive);
            if (rotSec > ITEM_ROTATE_SECONDS) rotSec = ITEM_ROTATE_SECONDS;

            if (prevRotActive && !rotActive && ballSeqKind == 0) {
                ballSeqKind = 2; ballSeqAffected = 0; ballSeqStart = now;
            }
            prevRotActive = rotActive;

            if (ballSeqKind != 0) {
                float dt = std::chrono::duration<float>(now - lastTickTime).count();
                if (dt < 0) dt = 0;
                if (dt > 0.25f) dt = 0.25f;
                auto shift = std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(dt));
                if (hostRevActive)   hostRevUntil += shift;
                if (clientRevActive) clientRevUntil += shift;
                if (hostFast)        hostFastUntil += shift;
                if (clientFast)      clientFastUntil += shift;
                if (rotActive)       rotateUntil += shift;
                if (boostActive)     ballBoostUntil += shift;
            }
            lastTickTime = now;

            float speedMult = timeScale * (boostActive ? ITEM_SPEEDUP_MULT : 1.0f);

            int rawDir = (g_keyDown ? 1 : 0) - (g_keyUp ? 1 : 0);
            int myDir = hostRevActive ? -rawDir : rawDir;
            float p1Spd = PADDLE_SPD * (hostFast ? ITEM_PADDLESPEED_MULT : 1.0f);

            // ---- CPU AI: only actively track the ball while it's heading this way
            // (bvx > 0). Predict where it will cross the paddle's plane, including
            // any wall bounces along the way, and always aim for that spot - even if
            // there isn't time to fully get there. Movement is smoothed (its own
            // velocity eases toward a target speed) instead of snapping to full
            // speed every tick, for a more human, less jerky feel. ----
            {
                float curBallX, curBallY;
                { std::lock_guard<std::mutex> lk(g_render.mtx); curBallX = g_render.ballX; curBallY = g_render.ballY; }
                float effVX = ballVX * speedMult, effVY = ballVY * speedMult;
                if (effVX > 0.0001f) {
                    float dx = (FIELD_W - PADDLE_ZONE) - curBallX;
                    if (dx < 0) dx = 0;
                    float t = dx / effVX;
                    float rawY = curBallY + effVY * t;
                    float loY = BALL_WALL_MARGIN, hiY = (float)FIELD_H - BALL_WALL_MARGIN;
                    float range = hiY - loY;
                    float period = 2.0f * range;
                    float rel = rawY - loY;
                    float m = std::fmod(rel, period);
                    if (m < 0) m += period;
                    float folded = (m > range) ? (period - m) : m;
                    float landingY = loY + folded;
                    cpuTargetY = landingY - PADDLE_H / 2.0f;
                    if (cpuTargetY < 0) cpuTargetY = 0;
                    if (cpuTargetY > FIELD_H - PADDLE_H) cpuTargetY = (float)(FIELD_H - PADDLE_H);
                }
            }
            bool ballComingAtCpu = ballVX > 0.0001f;
            float p2MaxSpeed = PADDLE_SPD * 1.15f * (clientFast ? ITEM_PADDLESPEED_MULT : 1.0f);
            float p2MaxAccel = p2MaxSpeed * 0.30f;

            bool ballLive = true;
            float seqRampMult = 1.0f;
            float seqElapsedOut = 0;
            if (ballSeqKind != 0) {
                float seqElapsed = std::chrono::duration<float>(now - ballSeqStart).count();
                seqElapsedOut = seqElapsed;
                if (seqElapsed < BALL_FREEZE_SECONDS) {
                    ballLive = false;
                }
                else {
                    float p2 = (seqElapsed - BALL_FREEZE_SECONDS) / BALL_EASE_SECONDS;
                    bool finishing = (p2 >= 1.0f);
                    if (finishing) p2 = 1.0f;
                    seqRampMult = easeInOut(p2);
                    if (finishing) ballSeqKind = 0;
                }
            }

            float bx, by, p1, p2; int s1, s2; int secLeft;
            bool justScored = false;
            {
                std::lock_guard<std::mutex> lk(g_render.mtx);
                g_render.p1Y += myDir * p1Spd;
                clampPaddle(g_render.p1Y);

                // Smoothed pursuit: ease this tick's velocity toward a desired speed
                // (proportional to distance from the target, capped) instead of
                // jumping straight to max speed - much less jittery to watch.
                float desiredVel = 0.0f;
                if (ballComingAtCpu) {
                    float diff = cpuTargetY - g_render.p2Y;
                    desiredVel = diff * 0.3f;
                    if (desiredVel > p2MaxSpeed) desiredVel = p2MaxSpeed;
                    if (desiredVel < -p2MaxSpeed) desiredVel = -p2MaxSpeed;
                }
                float dv = desiredVel - cpuVelY;
                if (dv > p2MaxAccel) dv = p2MaxAccel;
                if (dv < -p2MaxAccel) dv = -p2MaxAccel;
                cpuVelY += dv;
                g_render.p2Y += cpuVelY;
                clampPaddle(g_render.p2Y);

                if (ballLive) {
                    float prevBallX = g_render.ballX;
                    g_render.ballX += ballVX * speedMult * seqRampMult;
                    g_render.ballY += ballVY * speedMult * seqRampMult;
                    if (g_render.ballY <= BALL_WALL_MARGIN || g_render.ballY >= FIELD_H - BALL_WALL_MARGIN) ballVY = -ballVY;

                    if (ballVX < 0 && prevBallX > PADDLE_ZONE && g_render.ballX <= PADDLE_ZONE &&
                        g_render.ballY >= g_render.p1Y - 1 && g_render.ballY <= g_render.p1Y + PADDLE_H) {
                        ballVX = -ballVX * HIT_SPEEDUP; ballVY *= HIT_SPEEDUP;
                        clampBallSpeed(ballVX, ballVY);
                        g_render.ballX = PADDLE_ZONE; lastHitBy = 1;
                    }
                    if (ballVX > 0 && prevBallX < FIELD_W - PADDLE_ZONE && g_render.ballX >= FIELD_W - PADDLE_ZONE &&
                        g_render.ballY >= g_render.p2Y - 1 && g_render.ballY <= g_render.p2Y + PADDLE_H) {
                        ballVX = -ballVX * HIT_SPEEDUP; ballVY *= HIT_SPEEDUP;
                        clampBallSpeed(ballVX, ballVY);
                        g_render.ballX = FIELD_W - PADDLE_ZONE; lastHitBy = 2;
                    }

                    if (itemActive) {
                        float dx = g_render.ballX - itemX, dy = g_render.ballY - itemY;
                        if (dx * dx + dy * dy <= ITEM_PICKUP_DIST * ITEM_PICKUP_DIST) {
                            switch (itemType) {
                            case ITEM_SPEEDUP:
                                ballBoostUntil = now + std::chrono::seconds(ITEM_SPEEDUP_SECONDS);
                                break;
                            case ITEM_BOUNCE:
                                ballVX = -ballVX;
                                break;
                            case ITEM_PADDLESPEED:
                                if (lastHitBy == 1) hostFastUntil = now + std::chrono::seconds(ITEM_PADDLESPEED_SECONDS);
                                else if (lastHitBy == 2) clientFastUntil = now + std::chrono::seconds(ITEM_PADDLESPEED_SECONDS);
                                break;
                            case ITEM_REVERSE:
                                if (lastHitBy == 1 || lastHitBy == 2) {
                                    bool already = (lastHitBy == 1) ? hostRevActive : clientRevActive;
                                    auto base = already ? (lastHitBy == 1 ? hostRevUntil : clientRevUntil) : now;
                                    if (lastHitBy == 1) hostRevUntil = base + std::chrono::seconds(ITEM_REVERSE_SECONDS);
                                    else clientRevUntil = base + std::chrono::seconds(ITEM_REVERSE_SECONDS);
                                    if (!already) {
                                        ballSeqKind = 1; ballSeqAffected = lastHitBy; ballSeqStart = now;
                                    }
                                }
                                break;
                            case ITEM_ROTATE: {
                                bool already = rotActive;
                                rotateUntil = (already ? rotateUntil : now) + std::chrono::seconds(ITEM_ROTATE_SECONDS);
                                if (!already) {
                                    ballSeqKind = 2; ballSeqAffected = 0; ballSeqStart = now;
                                }
                                break;
                            }
                            case ITEM_REWIND: {
                                if (ballSeqKind == 3) break;
                                float target = elapsedSec - (float)ITEM_REWIND_SECONDS;
                                const Snap* pick = nullptr;
                                for (auto& sN : history) { if (sN.t <= target) pick = &sN; else break; }
                                ballSeqKind = 3; ballSeqAffected = 0; ballSeqStart = now;
                                if (pick) {
                                    g_render.ballX = pick->bx; g_render.ballY = pick->by;
                                    g_render.p1Y = pick->p1; g_render.p2Y = pick->p2;
                                    g_render.score1 = pick->s1; g_render.score2 = pick->s2;
                                    ballVX = pick->bvx; ballVY = pick->bvy;
                                    clampBallSpeed(ballVX, ballVY);
                                    matchStart = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(pick->t));
                                }
                                break;
                            }
                            default: break;
                            }
                            itemActive = false;
                            itemNextSpawn = now + std::chrono::milliseconds((int)(randRange(ITEM_SPAWN_MIN_SEC, ITEM_SPAWN_MAX_SEC) * 1000));
                        }
                    }

                    if (g_render.ballX < 0) {
                        g_render.score2++; justScored = true;
                        g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
                        ballVX = BALL_SPD_INIT; ballVY = BALL_SPD_INIT * 0.6f; lastHitBy = 0;
                        ballSeqKind = 0;
                    }
                    else if (g_render.ballX > FIELD_W) {
                        g_render.score1++; justScored = true;
                        g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
                        ballVX = -BALL_SPD_INIT; ballVY = BALL_SPD_INIT * 0.6f; lastHitBy = 0;
                        ballSeqKind = 0;
                    }
                }

                if (!itemActive && now >= itemNextSpawn) {
                    itemActive = true;
                    itemType = randItemType();
                    itemX = randRange(10.0f, (float)FIELD_W - 10.0f);
                    itemY = randRange(2.0f, (float)FIELD_H - 2.0f);
                }

                secLeft = MATCH_SECONDS - (int)elapsedSec;
                if (secLeft < 0) secLeft = 0;
                g_render.secondsLeft = secLeft;

                if (g_render.score1 >= WIN_SCORE || g_render.score2 >= WIN_SCORE || elapsedSec >= (float)MATCH_SECONDS) {
                    g_render.gameOver = true; over = true;
                }

                g_render.itemActive = itemActive ? 1 : 0;
                g_render.itemType = itemType; g_render.itemX = itemX; g_render.itemY = itemY;
                g_render.hostFastLeft = hostFastSec; g_render.clientFastLeft = clientFastSec;
                g_render.hostRevLeft = hostRevSec; g_render.clientRevLeft = clientRevSec;
                g_render.rotateLeft = rotSec;
                g_render.ballSeqKind = ballSeqKind; g_render.ballSeqAffected = ballSeqAffected;
                g_render.ballSeqElapsed = (ballSeqKind != 0) ? seqElapsedOut : 0;

                bx = g_render.ballX; by = g_render.ballY; p1 = g_render.p1Y; p2 = g_render.p2Y;
                s1 = g_render.score1; s2 = g_render.score2;
            }
            (void)justScored; // no point-break pause in CPU mode - the rally just continues

            history.push_back(Snap{ elapsedSec, bx, by, ballVX, ballVY, p1, p2, s1, s2 });
            while (!history.empty() && history.front().t < elapsedSec - 7.0f) history.pop_front();

            auto elapsedT = std::chrono::steady_clock::now() - now;
            auto sleepMs = TICK_MS - std::chrono::duration_cast<std::chrono::milliseconds>(elapsedT).count();
            if (sleepMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
        }

        if (g_abortMatch) { keepGoing = false; break; }

        // ---- post-game: single player, just wait for ENTER (restart) or ESC (menu) ----
        {
            std::lock_guard<std::mutex> lk(g_render.mtx);
            g_render.phase = 2;
            g_render.rematchVotes = 0;
            g_render.itemActive = 0;
            g_render.hostFastLeft = 0; g_render.clientFastLeft = 0;
            g_render.hostRevLeft = 0; g_render.clientRevLeft = 0;
            g_render.ballBoostLeft = 0; g_render.rotateLeft = 0;
            g_render.ballSeqKind = 0;
        }
        g_localRematchVote = 0;
        bool restarting = false;
        while (!g_abortMatch) {
            if (g_localRematchVote.load() == 1) { restarting = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
        }
        keepGoing = restarting && !g_abortMatch;
    }

    g_workerFinished = true;
}

static void joinWorker(std::string ip, int port) {
    g_matchStarted = false;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { g_errorMsg = "socket() failed."; g_workerFinished = true; return; }

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);
    addrinfo hints{}; addrinfo* res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ip.c_str(), portStr, &hints, &res) != 0 || !res) {
        g_errorMsg = "Could not resolve that address.";
        closesocket(s);
        g_workerFinished = true;
        return;
    }

    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, res->ai_addr, (int)res->ai_addrlen); // expected to return WOULDBLOCK
    freeaddrinfo(res);

    bool connected = false;
    auto start = std::chrono::steady_clock::now();
    while (!g_cancelRequested) {
        fd_set wfds, efds;
        FD_ZERO(&wfds); FD_SET(s, &wfds);
        FD_ZERO(&efds); FD_SET(s, &efds);
        timeval tv{ 0, 200000 };
        int r = select(0, nullptr, &wfds, &efds, &tv);
        if (r > 0) {
            if (FD_ISSET(s, &efds)) break;
            if (FD_ISSET(s, &wfds)) { connected = true; break; }
        }
        if (std::chrono::steady_clock::now() - start > std::chrono::seconds(8)) break;
    }

    if (!connected) {
        closesocket(s);
        if (!g_cancelRequested) g_errorMsg = "Could not connect. Check the address/port and firewall/port-forwarding settings.";
        g_workerFinished = true;
        return;
    }

    u_long bl = 0;
    ioctlsocket(s, FIONBIO, &bl);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));

    {
        std::lock_guard<std::mutex> lk(g_render.mtx);
        g_render.isHost = false;
        g_render.isCpuMode = false;
        g_render.status = "You are the RIGHT paddle.";
        g_render.phase = 0;
    }
    g_matchStarted = true;
    g_localRematchVote = 0;

    std::atomic<bool> alive{ true };
    std::atomic<int>  lastPhase{ 0 };
    std::thread rx([&]() {
        StateMsg st;
        int prevPhase = 0;
        while (alive) {
            if (!recvStruct(s, &st, sizeof(st))) { alive = false; break; }
            {
                std::lock_guard<std::mutex> lk(g_render.mtx);
                g_render.ballX = st.ballX; g_render.ballY = st.ballY;
                g_render.p1Y = st.p1Y; g_render.p2Y = st.p2Y;
                g_render.score1 = st.score1; g_render.score2 = st.score2;
                g_render.phase = st.phase;
                g_render.secondsLeft = st.secondsLeft;
                g_render.rematchVotes = (st.hostVote ? 1 : 0) + (st.clientVote ? 1 : 0);
                g_render.itemActive = st.itemActive; g_render.itemType = st.itemType;
                g_render.itemX = st.itemX; g_render.itemY = st.itemY;
                g_render.hostFastLeft = st.hostFastLeft; g_render.clientFastLeft = st.clientFastLeft;
                g_render.hostRevLeft = st.hostRevLeft; g_render.clientRevLeft = st.clientRevLeft;
                g_render.ballBoostLeft = st.ballBoostLeft; g_render.rotateLeft = st.rotateLeft;
                g_render.ballSeqKind = st.ballSeqKind; g_render.ballSeqAffected = st.ballSeqAffected;
                g_render.ballSeqElapsed = st.ballSeqElapsed;
            }
            lastPhase = st.phase;
            if ((prevPhase != 0 && st.phase == 0) || (prevPhase == 2 && st.phase == 1) || (prevPhase == 1 && st.phase == 3)) {
                g_localRematchVote = 0; // fresh lobby, a new round began, or a new point-break started -> clear local vote
            }
            prevPhase = st.phase;
        }
        });

    while (alive && !g_abortMatch) {
        int phase = lastPhase.load();
        int8_t toSend;
        if (phase == 1) toSend = (int8_t)((g_keyDown ? 1 : 0) - (g_keyUp ? 1 : 0));
        else toSend = (int8_t)(g_localRematchVote.load() == 1 ? 1 : 0); // lobby/rematch/point-break vote
        InputMsg im{ toSend };
        if (!sendStruct(s, &im, sizeof(im))) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));
    }

    alive = false;
    shutdown(s, SD_BOTH);
    closesocket(s);
    if (rx.joinable()) rx.join();
    g_workerFinished = true;
}

// ---------------- Text-mode screens (menu / input / status) ----------------
struct BtnRect { int row = -1, colStart = 0, colEnd = 0; };
static BtnRect g_hostBtn, g_joinBtn, g_cpuBtn, g_quitBtn;

static std::vector<std::string> buildMenuLines() {
    std::vector<std::string> L;
    L.push_back("========================================================");
    L.push_back("                 C O N S O L E   P O N G");
    L.push_back("========================================================");
    L.push_back("");
    L.push_back(" No dedicated server needed - one player HOSTs the game,");
    L.push_back(" the other player JOINs using the host's IP and port.");
    L.push_back(" Or just play solo against the CPU.");
    L.push_back("");

    int row = (int)L.size();
    std::string btnLine = "   ";
    auto addBtn = [&](const std::string& text, BtnRect& r) {
        r.row = row; r.colStart = (int)btnLine.size(); btnLine += text; r.colEnd = (int)btnLine.size();
        };
    addBtn("[ 1: HOST ]", g_hostBtn);
    btnLine += "   ";
    addBtn("[ 2: JOIN ]", g_joinBtn);
    btnLine += "   ";
    addBtn("[ 3: CPU ]", g_cpuBtn);
    btnLine += "   ";
    addBtn("[ 4: QUIT ]", g_quitBtn);
    L.push_back(btnLine);
    L.push_back("");
    L.push_back(" Click a button above, or press 1 / 2 / 3 / 4 on your keyboard.");
    return L;
}
static std::vector<std::string> buildHostPortInputLines() {
    std::vector<std::string> L;
    L.push_back("=== HOST A GAME ===");
    L.push_back("");
    L.push_back(" Enter the port to listen on (default 5000):");
    L.push_back("");
    L.push_back(" > " + g_inputBuffer + "_");
    L.push_back("");
    L.push_back(" [ENTER] Start listening      [ESC] Back to menu");
    return L;
}
static std::vector<std::string> buildJoinIpInputLines() {
    std::vector<std::string> L;
    L.push_back("=== JOIN A GAME (step 1/2) ===");
    L.push_back("");
    L.push_back(" Enter the host's IP address or hostname:");
    L.push_back(" (Ctrl+V to paste)");
    L.push_back("");
    L.push_back(" > " + g_inputBuffer + "_");
    L.push_back("");
    L.push_back(" [ENTER] Next      [ESC] Back to menu");
    return L;
}
static std::vector<std::string> buildJoinPortInputLines() {
    std::vector<std::string> L;
    L.push_back("=== JOIN A GAME (step 2/2) ===");
    L.push_back("");
    L.push_back(" Host: " + g_joinIp);
    L.push_back(" Enter the port (default 5000):");
    L.push_back("");
    L.push_back(" > " + g_inputBuffer + "_");
    L.push_back("");
    L.push_back(" [ENTER] Connect      [ESC] Back to menu");
    return L;
}
static std::vector<std::string> buildWaitingLines() {
    std::vector<std::string> L;
    L.push_back("=== WAITING FOR OPPONENT ===");
    L.push_back("");
    std::string status;
    { std::lock_guard<std::mutex> lk(g_render.mtx); status = g_render.status; }
    for (auto& l : splitLines(status)) L.push_back(" " + l);
    L.push_back("");
    L.push_back(" [ESC] Cancel");
    return L;
}
static std::vector<std::string> buildConnectingLines() {
    std::vector<std::string> L;
    L.push_back("=== CONNECTING ===");
    L.push_back("");
    L.push_back(" Connecting to " + g_joinIp + " ...");
    L.push_back("");
    L.push_back(" [ESC] Cancel");
    return L;
}
static std::vector<std::string> buildErrorLines() {
    std::vector<std::string> L;
    L.push_back("=== ERROR ===");
    L.push_back("");
    for (auto& l : splitLines(g_errorMsg)) L.push_back(" " + l);
    L.push_back("");
    L.push_back(" [ENTER] Back to menu");
    return L;
}
static std::vector<std::string> buildFrameLines() {
    switch (g_state) {
    case AppState::MENU:            return buildMenuLines();
    case AppState::HOST_INPUT_PORT: return buildHostPortInputLines();
    case AppState::HOST_WAITING:    return buildWaitingLines();
    case AppState::JOIN_INPUT_IP:   return buildJoinIpInputLines();
    case AppState::JOIN_INPUT_PORT: return buildJoinPortInputLines();
    case AppState::JOIN_CONNECTING: return buildConnectingLines();
    case AppState::PLAYING:         return {}; // drawn by drawGameScreen() instead
    case AppState::ERRORMSG:        return buildErrorLines();
    }
    return {};
}

// ---------------- Graphical game screen (white blocks, smooth motion) ----------------
// Layout is computed at runtime from the actual screen resolution (see computeLayout()).
static int SCREEN_W = 1920, SCREEN_H = 1080;
static int GAME_ORIGIN_X, GAME_ORIGIN_Y, GAME_W_PX, GAME_H_PX;
static float SCALE_X, SCALE_Y;
static int GAME_ORIGIN_X_P, GAME_ORIGIN_Y_P, GAME_W_PX_P, GAME_H_PX_P;
static float SCALE_X_P, SCALE_Y_P;
static const int PADDLE_PX_W = 16;
static const int BALL_PX = 16;

static void computeLayout() {
    GAME_W_PX = (int)(SCREEN_W * 0.55f);
    GAME_H_PX = (int)(SCREEN_H * 0.68f);
    GAME_ORIGIN_X = (SCREEN_W - GAME_W_PX) / 2;
    GAME_ORIGIN_Y = (int)(SCREEN_H * 0.16f);
    SCALE_X = (float)GAME_W_PX / FIELD_W;
    SCALE_Y = (float)GAME_H_PX / FIELD_H;

    GAME_W_PX_P = (int)(SCREEN_W * 0.28f);
    GAME_H_PX_P = (int)(SCREEN_H * 0.76f);
    GAME_ORIGIN_X_P = (SCREEN_W - GAME_W_PX_P) / 2;
    GAME_ORIGIN_Y_P = (int)(SCREEN_H * 0.10f);
    SCALE_X_P = (float)GAME_H_PX_P / FIELD_W; // width-axis -> vertical extent
    SCALE_Y_P = (float)GAME_W_PX_P / FIELD_H; // height-axis -> horizontal extent
}

static RECT lerpRect(const RECT& a, const RECT& b, float t) {
    RECT r;
    r.left = a.left + (LONG)((b.left - a.left) * t);
    r.top = a.top + (LONG)((b.top - a.top) * t);
    r.right = a.right + (LONG)((b.right - a.right) * t);
    r.bottom = a.bottom + (LONG)((b.bottom - a.bottom) * t);
    return r;
}

// morphT: 0 = landscape layout, 1 = portrait layout (already eased by the caller)
static void drawGameScreen(HDC memDC, HFONT font, int lineH, float morphT) {
    float bx, by, p1, p2; int s1, s2, phase, secondsLeft, votes; bool isHost;
    int itemActive, itemType; float itemX, itemY;
    int hostFastLeft, clientFastLeft, hostRevLeft, clientRevLeft, rotateLeft, ballBoostLeft;
    int ballSeqKind, ballSeqAffected; float ballSeqElapsed;
    bool isCpuMode;
    std::string status;
    {
        std::lock_guard<std::mutex> lk(g_render.mtx);
        bx = g_render.ballX; by = g_render.ballY;
        p1 = g_render.p1Y; p2 = g_render.p2Y;
        s1 = g_render.score1; s2 = g_render.score2;
        phase = g_render.phase; secondsLeft = g_render.secondsLeft; votes = g_render.rematchVotes;
        isHost = g_render.isHost; status = g_render.status;
        itemActive = g_render.itemActive; itemType = g_render.itemType;
        itemX = g_render.itemX; itemY = g_render.itemY;
        hostFastLeft = g_render.hostFastLeft; clientFastLeft = g_render.clientFastLeft;
        hostRevLeft = g_render.hostRevLeft; clientRevLeft = g_render.clientRevLeft;
        rotateLeft = g_render.rotateLeft; ballBoostLeft = g_render.ballBoostLeft;
        ballSeqKind = g_render.ballSeqKind; ballSeqAffected = g_render.ballSeqAffected;
        ballSeqElapsed = g_render.ballSeqElapsed;
        isCpuMode = g_render.isCpuMode;
    }

    HFONT oldFont = (HFONT)SelectObject(memDC, font);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    HBRUSH whiteBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HBRUSH itemBrush = CreateSolidBrush(RGB(255, 210, 60));

    int ox = GAME_ORIGIN_X + (int)((GAME_ORIGIN_X_P - GAME_ORIGIN_X) * morphT);
    int oy = GAME_ORIGIN_Y + (int)((GAME_ORIGIN_Y_P - GAME_ORIGIN_Y) * morphT);
    int gw = GAME_W_PX + (int)((GAME_W_PX_P - GAME_W_PX) * morphT);
    int gh = GAME_H_PX + (int)((GAME_H_PX_P - GAME_H_PX) * morphT);

    auto mapPt = [&](float fx, float fy, int& outX, int& outY) {
        int xl = GAME_ORIGIN_X + (int)(fx * SCALE_X), yl = GAME_ORIGIN_Y + (int)(fy * SCALE_Y);
        int xp = GAME_ORIGIN_X_P + (int)(fy * SCALE_Y_P), yp = GAME_ORIGIN_Y_P + (int)(fx * SCALE_X_P);
        outX = xl + (int)((xp - xl) * morphT);
        outY = yl + (int)((yp - yl) * morphT);
        };

    char scoreBuf[128];
    if (phase == 1 || phase == 3) {
        int mm = secondsLeft / 60, ss = secondsLeft % 60;
        snprintf(scoreBuf, sizeof(scoreBuf), "SCORE  %d : %d      Time %d:%02d", s1, s2, mm, ss);
    }
    else {
        snprintf(scoreBuf, sizeof(scoreBuf), "SCORE  %d : %d", s1, s2);
    }
    {
        SIZE sz; GetTextExtentPoint32A(memDC, scoreBuf, (int)strlen(scoreBuf), &sz);
        int scoreX = ox + gw / 2 - sz.cx / 2;
        int scoreY = oy - sz.cy - 14; // just above the field, centered over it
        if (scoreX < 8) scoreX = 8;
        if (scoreY < 8) scoreY = 8;
        TextOutA(memDC, scoreX, scoreY, scoreBuf, (int)strlen(scoreBuf));
    }

    RECT field{ ox, oy, ox + gw, oy + gh };
    FrameRect(memDC, &field, whiteBrush);

    // center dashed line: landscape-style (vertical) fading into portrait-style (horizontal)
    if (morphT < 0.5f) {
        int cx = ox + gw / 2;
        for (int y = oy; y < oy + gh; y += 20) { RECT d{ cx - 2, y, cx + 2, y + 10 }; FillRect(memDC, &d, whiteBrush); }
    }
    else {
        int cy = oy + gh / 2;
        for (int x = ox; x < ox + gw; x += 20) { RECT d{ x, cy - 2, x + 10, cy + 2 }; FillRect(memDC, &d, whiteBrush); }
    }

    // Paddle rects: the edge that faces the field center sits exactly on the
    // PADDLE_ZONE collision plane (converted through the same scale factors used
    // for the ball), so what you see always matches where the ball actually bounces.
    // Your own paddle is drawn in a bright accent color so it's always obvious
    // which one you're moving; the opponent's stays plain white.
    HBRUSH myBrush = CreateSolidBrush(RGB(70, 220, 255));
    int p1FrontL = GAME_ORIGIN_X + (int)(PADDLE_ZONE * SCALE_X);
    RECT p1L{ p1FrontL - PADDLE_PX_W, GAME_ORIGIN_Y + (int)(p1 * SCALE_Y), p1FrontL, GAME_ORIGIN_Y + (int)((p1 + PADDLE_H) * SCALE_Y) };
    int p1FrontP = GAME_ORIGIN_Y_P + (int)(PADDLE_ZONE * SCALE_X_P);
    RECT p1P{ GAME_ORIGIN_X_P + (int)(p1 * SCALE_Y_P), p1FrontP - PADDLE_PX_W, GAME_ORIGIN_X_P + (int)((p1 + PADDLE_H) * SCALE_Y_P), p1FrontP };
    RECT p1R = lerpRect(p1L, p1P, morphT);
    FillRect(memDC, &p1R, isHost ? myBrush : whiteBrush);

    int p2FrontL = GAME_ORIGIN_X + (int)((FIELD_W - PADDLE_ZONE) * SCALE_X);
    RECT p2L{ p2FrontL, GAME_ORIGIN_Y + (int)(p2 * SCALE_Y), p2FrontL + PADDLE_PX_W, GAME_ORIGIN_Y + (int)((p2 + PADDLE_H) * SCALE_Y) };
    int p2FrontP = GAME_ORIGIN_Y_P + (int)((FIELD_W - PADDLE_ZONE) * SCALE_X_P);
    RECT p2P{ GAME_ORIGIN_X_P + (int)(p2 * SCALE_Y_P), p2FrontP, GAME_ORIGIN_X_P + (int)((p2 + PADDLE_H) * SCALE_Y_P), p2FrontP + PADDLE_PX_W };
    RECT p2R = lerpRect(p2L, p2P, morphT);
    FillRect(memDC, &p2R, isHost ? whiteBrush : myBrush);
    DeleteObject(myBrush);

    if (phase == 1) {
        int bcx, bcy; mapPt(bx, by, bcx, bcy);
        RECT ballR{ bcx - BALL_PX / 2, bcy - BALL_PX / 2, bcx + BALL_PX / 2, bcy + BALL_PX / 2 };
        FillRect(memDC, &ballR, whiteBrush);

        if (itemActive) {
            int icx, icy; mapPt(itemX, itemY, icx, icy);
            RECT bgR{ icx - ITEM_PX / 2 - 3, icy - ITEM_PX / 2 - 3, icx + ITEM_PX / 2 + 3, icy + ITEM_PX / 2 + 3 };
            FillRect(memDC, &bgR, itemBrush);
            const uint32_t* bits = iconBitsFor((uint8_t)itemType);
            if (bits) {
                HBRUSH blackBrush2 = (HBRUSH)GetStockObject(BLACK_BRUSH);
                int ox0 = icx - ITEM_PX / 2, oy0 = icy - ITEM_PX / 2;
                for (int yy = 0; yy < 32; yy++) {
                    uint32_t row = bits[yy];
                    if (row == 0) continue;
                    int runStart = -1;
                    for (int xx = 0; xx <= 32; xx++) {
                        bool on = (xx < 32) && (row & (1u << (31 - xx)));
                        if (on && runStart < 0) runStart = xx;
                        else if (!on && runStart >= 0) {
                            RECT px{ ox0 + runStart, oy0 + yy, ox0 + xx, oy0 + yy + 1 };
                            FillRect(memDC, &px, blackBrush2);
                            runStart = -1;
                        }
                    }
                }
            }
        }

        // "REVERSED!" banner, shown only on the affected player's screen for the first
        // REVERSE_INDICATOR_SECONDS of the sequence.
        if (ballSeqKind == 1 && ballSeqElapsed < REVERSE_INDICATOR_SECONDS) {
            bool amAffected = (isHost && ballSeqAffected == 1) || (!isHost && ballSeqAffected == 2);
            if (amAffected) {
                const char* msg = "*** YOUR CONTROLS ARE REVERSED! ***";
                SIZE sz; GetTextExtentPoint32A(memDC, msg, (int)strlen(msg), &sz);
                int tx = ox + gw / 2 - sz.cx / 2;
                int ty = oy + gh / 2 - sz.cy / 2;
                RECT bg{ tx - 14, ty - 8, tx + sz.cx + 14, ty + sz.cy + 8 };
                HBRUSH warnBrush = CreateSolidBrush(RGB(200, 40, 40));
                FillRect(memDC, &bg, warnBrush);
                DeleteObject(warnBrush);
                TextOutA(memDC, tx, ty, msg, (int)strlen(msg));
            }
        }
    }

    int textY = oy + gh + 16;
    if (phase == 0) {
        TextOutA(memDC, 16, textY, status.c_str(), (int)status.size());
        char line[160];
        snprintf(line, sizeof(line), "Ready? (%d/2) - press ENTER to start", votes);
        TextOutA(memDC, 16, textY + lineH, line, (int)strlen(line));
        const char* legend = "Items: ball speed / rewind 5s / bounce / paddle speed / reversed controls / rotate";
        TextOutA(memDC, 16, textY + lineH * 2, legend, (int)strlen(legend));
    }
    else if (phase == 2) {
        bool tie = (s1 == s2);
        bool iWin = isHost ? (s1 > s2) : (s2 > s1);
        const char* msg = tie ? "*** TIME'S UP - TIE GAME ***" : (iWin ? "*** YOU WIN! ***" : "*** YOU LOSE! ***");
        TextOutA(memDC, 16, textY, msg, (int)strlen(msg));
        char line2[128];
        if (isCpuMode) {
            snprintf(line2, sizeof(line2), "Press ENTER to play again      [ESC] Back to menu");
        }
        else {
            bool localVoted = (g_localRematchVote.load() == 1);
            if (localVoted)
                snprintf(line2, sizeof(line2), "Play again? (%d/2) - waiting for opponent... %ds left", votes, secondsLeft);
            else
                snprintf(line2, sizeof(line2), "Play again? (%d/2) - press ENTER   %ds left", votes, secondsLeft);
        }
        TextOutA(memDC, 16, textY + lineH, line2, (int)strlen(line2));
    }
    else if (phase == 3) {
        char line1[64];
        snprintf(line1, sizeof(line1), "POINT!  SCORE %d : %d", s1, s2);
        TextOutA(memDC, 16, textY, line1, (int)strlen(line1));
        bool localVoted = (g_localRematchVote.load() == 1);
        char line2b[128];
        if (localVoted)
            snprintf(line2b, sizeof(line2b), "Next point: ready (%d/2) - waiting for opponent...", votes);
        else
            snprintf(line2b, sizeof(line2b), "Next point: ready? (%d/2) - press ENTER", votes);
        TextOutA(memDC, 16, textY + lineH, line2b, (int)strlen(line2b));
    }
    else {
        char msg2[96];
        if (isCpuMode) snprintf(msg2, sizeof(msg2), "Move: W/S, Up/Down, A/D or Left/Right   vs CPU   [ESC] Quit");
        else snprintf(msg2, sizeof(msg2), "Move: W/S, Up/Down, A/D or Left/Right      [ESC] Quit match");
        TextOutA(memDC, 16, textY, msg2, (int)strlen(msg2));
        int myFast = isHost ? hostFastLeft : clientFastLeft;
        int myRev = isHost ? hostRevLeft : clientRevLeft;
        char eff[160]; int effLen = 0; eff[0] = '\0';
        if (myFast > 0)        effLen += snprintf(eff + effLen, sizeof(eff) - effLen, "Fast paddle(%ds) ", myFast);
        if (myRev > 0)          effLen += snprintf(eff + effLen, sizeof(eff) - effLen, "REVERSED!(%ds) ", myRev);
        if (rotateLeft > 0)     effLen += snprintf(eff + effLen, sizeof(eff) - effLen, "Rotated(%ds) ", rotateLeft);
        if (ballBoostLeft > 0)  effLen += snprintf(eff + effLen, sizeof(eff) - effLen, "Ball boost(%ds) ", ballBoostLeft);
        if (effLen > 0) TextOutA(memDC, 16, textY + lineH, eff, effLen);
    }

    DeleteObject(itemBrush);
    SelectObject(memDC, oldFont);
}

// ---------------- Win32 boilerplate ----------------
static const int PAD_X = 10, PAD_Y = 10;
static const UINT_PTR ID_TIMER = 1;
static const int ROT_MORPH_MS = 600; // content-morph animation duration (eased)

static HFONT g_font = nullptr;
static int g_charW = 10, g_charH = 18;

// Local content-morph state for the rotate item (no window rotation, just an
// eased blend between the landscape and portrait layouts, drawn every frame).
static bool g_rotSettled = false;     // current settled orientation
static bool g_rotMorphing = false;
static bool g_rotMorphTarget = false;
static std::chrono::steady_clock::time_point g_rotMorphStart;
static float g_rotMorphT = 0.0f;      // 0=landscape, 1=portrait (already eased)

static void goToHostInput() { g_state = AppState::HOST_INPUT_PORT; g_inputBuffer.clear(); }
static void goToJoinInput() { g_state = AppState::JOIN_INPUT_IP; g_inputBuffer.clear(); }

static void startHostMatch(int port) {
    g_workerFinished = false; g_matchStarted = false;
    g_cancelRequested = false; g_abortMatch = false; g_errorMsg.clear();
    g_localRematchVote = 0;
    if (g_workerThread.joinable()) g_workerThread.join();
    g_workerThread = std::thread(hostWorker, port);
    g_state = AppState::HOST_WAITING;
}
static void startJoinMatch(const std::string& ip, int port) {
    g_workerFinished = false; g_matchStarted = false;
    g_cancelRequested = false; g_abortMatch = false; g_errorMsg.clear();
    g_localRematchVote = 0;
    if (g_workerThread.joinable()) g_workerThread.join();
    g_workerThread = std::thread(joinWorker, ip, port);
    g_state = AppState::JOIN_CONNECTING;
}
static void startCpuMatch() {
    g_workerFinished = false; g_matchStarted = false;
    g_cancelRequested = false; g_abortMatch = false; g_errorMsg.clear();
    g_localRematchVote = 0;
    if (g_workerThread.joinable()) g_workerThread.join();
    g_workerThread = std::thread(cpuWorker);
    g_state = AppState::PLAYING; // no waiting/lobby needed for a local CPU match
}


static bool isAllowedInputChar(AppState st, char c) {
    if (st == AppState::HOST_INPUT_PORT || st == AppState::JOIN_INPUT_PORT) return isdigit((unsigned char)c) != 0;
    if (st == AppState::JOIN_INPUT_IP) return c >= 32 && c < 127; // any printable ASCII (hostnames, IPv6, etc.)
    return false;
}
static size_t maxLenFor(AppState st) {
    if (st == AppState::JOIN_INPUT_IP) return 128;
    return 5;
}
static void pasteClipboardInto(HWND hwnd, std::string& buf) {
    if (!OpenClipboard(hwnd)) return;
    HANDLE h = GetClipboardData(CF_TEXT);
    if (h) {
        char* p = (char*)GlobalLock(h);
        if (p) {
            size_t maxLen = maxLenFor(g_state);
            for (const char* c = p; *c && buf.size() < maxLen; ++c) {
                if (isAllowedInputChar(g_state, *c)) buf += *c;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
}

// Only a small part of the fullscreen window ever has anything drawn on it, so
// clearing/blitting the whole screen every frame was wasteful. This returns a
// bounding box comfortably covering whatever the current screen could draw.
static RECT computeContentRect() {
    RECT r;
    if (g_state == AppState::PLAYING) {
        // Score/status/footer text is always drawn near the left edge (x ~= 16-24),
        // independent of the (horizontally centered) game field, so the left/top
        // bounds must stay anchored to the screen edge rather than the field.
        int left = 0;
        int top = 0;
        int rightL = GAME_ORIGIN_X + GAME_W_PX, rightP = GAME_ORIGIN_X_P + GAME_W_PX_P;
        int right = (rightL > rightP ? rightL : rightP) + 40;
        int botL = GAME_ORIGIN_Y + GAME_H_PX, botP = GAME_ORIGIN_Y_P + GAME_H_PX_P;
        int bottom = (botL > botP ? botL : botP) + 150; // room for footer lines below the field
        if (right > SCREEN_W) right = SCREEN_W;
        if (bottom > SCREEN_H) bottom = SCREEN_H;
        r = RECT{ left, top, right, bottom };
    }
    else {
        int right = PAD_X + 70 * g_charW;
        int bottom = PAD_Y + 30 * g_charH;
        if (right > SCREEN_W) right = SCREEN_W;
        if (bottom > SCREEN_H) bottom = SCREEN_H;
        r = RECT{ 0, 0, right, bottom };
    }
    return r;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, ID_TIMER, TICK_MS, nullptr);
        return 0;

    case WM_TIMER: {
        if (g_state == AppState::PLAYING) {
            int rotSecs;
            { std::lock_guard<std::mutex> lk(g_render.mtx); rotSecs = g_render.rotateLeft; }
            bool wantPortrait = rotSecs > 0;
            if (!g_rotMorphing && wantPortrait != g_rotSettled) {
                g_rotMorphing = true;
                g_rotMorphTarget = wantPortrait;
                g_rotMorphStart = std::chrono::steady_clock::now();
            }
            if (g_rotMorphing) {
                float raw = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_rotMorphStart).count() * 1000.0f / ROT_MORPH_MS;
                bool done = raw >= 1.0f;
                if (done) raw = 1.0f;
                float eased = easeInOut(raw);
                g_rotMorphT = g_rotMorphTarget ? eased : (1.0f - eased);
                if (done) { g_rotMorphing = false; g_rotSettled = g_rotMorphTarget; }
            }
        }
        else if (g_rotSettled || g_rotMorphing) {
            g_rotSettled = false; g_rotMorphing = false; g_rotMorphT = 0.0f;
        }

        switch (g_state) {
        case AppState::HOST_WAITING:
        case AppState::JOIN_CONNECTING:
            if (g_matchStarted) {
                g_state = AppState::PLAYING;
            }
            else if (g_workerFinished) {
                if (g_workerThread.joinable()) g_workerThread.join();
                g_state = g_errorMsg.empty() ? AppState::MENU : AppState::ERRORMSG;
            }
            break;
        case AppState::PLAYING:
            if (g_workerFinished) {
                if (g_workerThread.joinable()) g_workerThread.join();
                bool over;
                { std::lock_guard<std::mutex> lk(g_render.mtx); over = g_render.gameOver; }
                if (g_abortMatch) {
                    g_state = AppState::MENU;
                }
                else if (!over) {
                    if (g_errorMsg.empty()) g_errorMsg = "Connection lost.";
                    g_state = AppState::ERRORMSG;
                }
                else {
                    g_state = AppState::MENU;
                }
            }
            break;
        default: break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }

    case WM_LBUTTONDOWN:
        if (g_state == AppState::MENU) {
            int x = LOWORD(lParam), y = HIWORD(lParam);
            int col = (x - PAD_X) / g_charW;
            int row = (y - PAD_Y) / g_charH;
            if (row == g_hostBtn.row && col >= g_hostBtn.colStart && col < g_hostBtn.colEnd) goToHostInput();
            else if (row == g_joinBtn.row && col >= g_joinBtn.colStart && col < g_joinBtn.colEnd) goToJoinInput();
            else if (row == g_cpuBtn.row && col >= g_cpuBtn.colStart && col < g_cpuBtn.colEnd) startCpuMatch();
            else if (row == g_quitBtn.row && col >= g_quitBtn.colStart && col < g_quitBtn.colEnd) DestroyWindow(hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;

    case WM_CHAR:
        if (g_state == AppState::HOST_INPUT_PORT || g_state == AppState::JOIN_INPUT_PORT || g_state == AppState::JOIN_INPUT_IP) {
            if (wParam == 8) { if (!g_inputBuffer.empty()) g_inputBuffer.pop_back(); }
            else if (wParam >= 32 && wParam < 127 && isAllowedInputChar(g_state, (char)wParam) && g_inputBuffer.size() < maxLenFor(g_state)) {
                g_inputBuffer += (char)wParam;
            }
        }
        else if (g_state == AppState::MENU) {
            if (wParam == '1') goToHostInput();
            else if (wParam == '2') goToJoinInput();
            else if (wParam == '3') startCpuMatch();
            else if (wParam == '4') DestroyWindow(hwnd);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_KEYDOWN:
        switch (wParam) {
        case VK_UP: case 'W': case VK_LEFT: case 'A': g_keyUp = true; break;
        case VK_DOWN: case 'S': case VK_RIGHT: case 'D': g_keyDown = true; break;
        case 'V':
            if (GetKeyState(VK_CONTROL) & 0x8000) {
                if (g_state == AppState::HOST_INPUT_PORT || g_state == AppState::JOIN_INPUT_PORT || g_state == AppState::JOIN_INPUT_IP) {
                    pasteClipboardInto(hwnd, g_inputBuffer);
                }
            }
            break;
        case VK_RETURN:
            if (g_state == AppState::HOST_INPUT_PORT) {
                int port = g_inputBuffer.empty() ? 5000 : atoi(g_inputBuffer.c_str());
                if (port <= 0 || port > 65535) port = 5000;
                startHostMatch(port);
            }
            else if (g_state == AppState::JOIN_INPUT_IP) {
                if (!g_inputBuffer.empty()) { g_joinIp = g_inputBuffer; g_inputBuffer.clear(); g_state = AppState::JOIN_INPUT_PORT; }
            }
            else if (g_state == AppState::JOIN_INPUT_PORT) {
                int port = g_inputBuffer.empty() ? 5000 : atoi(g_inputBuffer.c_str());
                if (port <= 0 || port > 65535) port = 5000;
                startJoinMatch(g_joinIp, port);
            }
            else if (g_state == AppState::ERRORMSG) {
                g_state = AppState::MENU;
            }
            else if (g_state == AppState::PLAYING) {
                int phase;
                { std::lock_guard<std::mutex> lk(g_render.mtx); phase = g_render.phase; }
                if (phase == 0 || phase == 2 || phase == 3) g_localRematchVote = 1;
            }
            break;
        case VK_ESCAPE:
            if (g_state == AppState::HOST_INPUT_PORT || g_state == AppState::JOIN_INPUT_IP || g_state == AppState::JOIN_INPUT_PORT) {
                g_inputBuffer.clear(); g_state = AppState::MENU;
            }
            else if (g_state == AppState::HOST_WAITING || g_state == AppState::JOIN_CONNECTING) {
                g_cancelRequested = true;
            }
            else if (g_state == AppState::PLAYING) {
                g_abortMatch = true;
            }
            else if (g_state == AppState::ERRORMSG) {
                g_state = AppState::MENU;
            }
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;

    case WM_KEYUP:
        switch (wParam) {
        case VK_UP: case 'W': case VK_LEFT: case 'A': g_keyUp = false; break;
        case VK_DOWN: case 'S': case VK_RIGHT: case 'D': g_keyDown = false; break;
        }
        return 0;

    case WM_ERASEBKGND:
        return 1; // we paint the whole background ourselves

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);

        // Reuse one off-screen buffer for the whole app's lifetime instead of
        // creating/destroying a full-screen bitmap every frame (was a source of lag).
        static HDC memDC = nullptr;
        static HBITMAP bmp = nullptr;
        static int bufW = 0, bufH = 0;
        static RECT prevContentRect{ 0, 0, 0, 0 };
        if (!memDC || bufW != rc.right || bufH != rc.bottom) {
            if (bmp) DeleteObject(bmp);
            if (memDC) DeleteDC(memDC);
            memDC = CreateCompatibleDC(hdc);
            bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
            SelectObject(memDC, bmp);
            bufW = rc.right; bufH = rc.bottom;
            HBRUSH allBlack = CreateSolidBrush(RGB(0, 0, 0));
            FillRect(memDC, &rc, allBlack);
            DeleteObject(allBlack);
            prevContentRect = RECT{ 0, 0, 0, 0 };
            // Paint the whole screen black right away - otherwise only our small
            // content rect ever gets blitted and whatever was on screen before
            // (desktop, previous window, etc.) stays visible around it.
            BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        }

        // Only clear/redraw/blit the small region content actually occupies (plus
        // the previous frame's region, so switching screens never leaves stale
        // pixels behind) instead of the entire fullscreen buffer every frame.
        RECT content = computeContentRect();
        RECT clearRect = content;
        if (prevContentRect.right > prevContentRect.left) {
            if (prevContentRect.left < clearRect.left) clearRect.left = prevContentRect.left;
            if (prevContentRect.top < clearRect.top) clearRect.top = prevContentRect.top;
            if (prevContentRect.right > clearRect.right) clearRect.right = prevContentRect.right;
            if (prevContentRect.bottom > clearRect.bottom) clearRect.bottom = prevContentRect.bottom;
        }
        prevContentRect = content;

        HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &clearRect, blackBrush);
        DeleteObject(blackBrush);

        if (g_state == AppState::PLAYING) {
            drawGameScreen(memDC, g_font, g_charH, g_rotMorphT);
        }
        else {
            HFONT oldFont = (HFONT)SelectObject(memDC, g_font);
            SetBkMode(memDC, TRANSPARENT);
            SetTextColor(memDC, RGB(60, 230, 100)); // retro terminal green

            auto lines = buildFrameLines();
            for (size_t i = 0; i < lines.size(); i++) {
                TextOutA(memDC, PAD_X, PAD_Y + (int)i * g_charH, lines[i].c_str(), (int)lines[i].size());
            }

            SelectObject(memDC, oldFont);
        }
        BitBlt(hdc, clearRect.left, clearRect.top, clearRect.right - clearRect.left, clearRect.bottom - clearRect.top,
            memDC, clearRect.left, clearRect.top, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        g_cancelRequested = true;
        g_abortMatch = true;
        if (g_workerThread.joinable()) g_workerThread.join();
        KillTimer(hwnd, ID_TIMER);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wParam, lParam);
}

int APIENTRY WinMain(_In_ HINSTANCE hInstance, _In_opt_ HINSTANCE, _In_ LPSTR, _In_ int nCmdShow) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxA(nullptr, "WSAStartup failed.", "Console Pong", MB_ICONERROR);
        return 1;
    }

    SCREEN_W = GetSystemMetrics(SM_CXSCREEN);
    SCREEN_H = GetSystemMetrics(SM_CYSCREEN);
    if (SCREEN_W <= 0) SCREEN_W = 1920;
    if (SCREEN_H <= 0) SCREEN_H = 1080;
    computeLayout();

    const char* CLASS_NAME = "ConsolePongWindowClass";
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512)); // IDC_ARROW, forced ANSI
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    g_font = CreateFontA(
        -18, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        ANSI_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        FIXED_PITCH | FF_MODERN, "Consolas");

    HDC tmpDC = GetDC(nullptr);
    HFONT oldF = (HFONT)SelectObject(tmpDC, g_font);
    TEXTMETRICA tm;
    GetTextMetricsA(tmpDC, &tm);
    g_charW = tm.tmAveCharWidth;
    g_charH = tm.tmHeight + tm.tmExternalLeading;
    SelectObject(tmpDC, oldF);
    ReleaseDC(nullptr, tmpDC);

    // Always fullscreen, borderless.
    HWND hwnd = CreateWindowA(
        CLASS_NAME, "Console Pong", WS_POPUP | WS_VISIBLE,
        0, 0, SCREEN_W, SCREEN_H,
        nullptr, nullptr, hInstance, nullptr);

    buildMenuLines(); // populate button hit-rects before the first click can occur

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    SetForegroundWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_font) DeleteObject(g_font);
    WSACleanup();
    return (int)msg.wParam;
}
