// ==========================================================
//  Console-style Online Pong  (Win32 GUI, NOT a console app)
//  - Black background, retro-style text screens for menus,
//    but the actual gameplay is drawn as solid white blocks
//    with smooth motion.
//  - No dedicated server required: the HOST's own process
//    listens on a TCP port and acts as the server for the
//    session; the other player JOINs by typing (or pasting)
//    the host's IP address / hostname and port.
//  - After joining, both players see a LOBBY and must press
//    START before the round begins.
//  - Ball speed increases gradually over the 5-minute match,
//    plus a small kick on every paddle hit (resets on score).
//  - Random items appear on the field: ball speed boost,
//    rewind, a bounce pad, paddle speed boost, reversed
//    controls, and a screen-rotate effect (with animation).
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
#define NOMINMAX
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>
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
#pragma comment(lib, "gdiplus.lib")

// ---------------- Game constants ----------------
static const int   FIELD_W = 60;
static const int   FIELD_H = 20;
static const int   PADDLE_H = 4;
static const float PADDLE_SPD = 0.45f;
static const float BALL_SPD_INIT = 0.3f;
static const int   WIN_SCORE = 11;
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
    int8_t dir; // gameplay: -1/0/1 movement. lobby/rematch phase: 0/1 vote.
};
struct StateMsg {
    float   ballX, ballY;
    float   p1Y, p2Y;      // p1 = host (left/top), p2 = joiner (right/bottom)
    int32_t score1, score2;
    uint8_t phase;         // 0 = lobby, 1 = playing, 2 = post-game vote
    uint8_t hostVote, clientVote;
    int32_t secondsLeft;   // match clock (phase 1) or vote countdown (phase 0/2)
    uint8_t restarting;    // pulses 1 on the tick a new round begins
    uint8_t itemActive;
    uint8_t itemType;
    float   itemX, itemY;
    uint8_t hostFastLeft, clientFastLeft; // seconds left of paddle-speed buff
    uint8_t hostRevLeft, clientRevLeft;   // seconds left of reversed controls
    uint8_t ballBoostLeft;                // seconds left of ball-speed item
    uint8_t rotateLeft;                   // seconds left of screen-rotate effect
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
    PLAYING, // covers lobby (phase 0), active play (phase 1) and post-game vote (phase 2)
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
static std::atomic<int>  g_localRematchVote{ 0 };     // reused for lobby-ready AND rematch votes
static std::thread g_workerThread;

struct RenderState {
    std::mutex mtx;
    float ballX = FIELD_W / 2.0f, ballY = FIELD_H / 2.0f;
    float p1Y = FIELD_H / 2.0f - PADDLE_H / 2.0f;
    float p2Y = FIELD_H / 2.0f - PADDLE_H / 2.0f;
    int   score1 = 0, score2 = 0;
    bool  gameOver = false;
    bool  isHost = false;
    int   phase = 0; // 0=lobby,1=playing,2=postgame
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
        g_render.status = "You are the LEFT paddle (host).";
        g_render.phase = 0;
    }
    g_matchStarted = true;

    std::atomic<bool> rxAlive{ true };
    std::atomic<int>  peerVal{ 0 }; // movement dir during play; lobby/rematch vote (0/1) otherwise
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
        }
        st.phase = phase; st.hostVote = (uint8_t)hostVote; st.clientVote = (uint8_t)clientVote;
        st.secondsLeft = secLeft; st.restarting = (uint8_t)(restarting ? 1 : 0);
        st.itemActive = 0; st.itemType = 0; st.itemX = 0; st.itemY = 0;
        st.hostFastLeft = 0; st.clientFastLeft = 0; st.hostRevLeft = 0; st.clientRevLeft = 0;
        st.ballBoostLeft = 0; st.rotateLeft = 0;
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
        auto ballBoostUntil = matchStart, rotateUntil = matchStart;

        struct Snap { float t, bx, by, bvx, bvy, p1, p2; int s1, s2; };
        std::deque<Snap> history;

        // ---- gameplay loop ----
        while (rxAlive && !over && !g_abortMatch) {
            auto now = std::chrono::steady_clock::now();
            float elapsedSec = std::chrono::duration<float>(now - matchStart).count();
            if (elapsedSec < 0) elapsedSec = 0;
            if (elapsedSec > (float)MATCH_SECONDS) elapsedSec = (float)MATCH_SECONDS;
            float timeScale = 1.0f + (SPEED_MAX_MULT - 1.0f) * (elapsedSec / (float)MATCH_SECONDS);

            bool boostActive = now < ballBoostUntil;
            bool hostFast = now < hostFastUntil;
            bool clientFast = now < clientFastUntil;
            bool hostRevActive = now < hostRevUntil;
            bool clientRevActive = now < clientRevUntil;
            bool rotActive = now < rotateUntil;

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
            int boostSec = secsLeftOf(ballBoostUntil, boostActive);
            int rotSec = secsLeftOf(rotateUntil, rotActive);
            if (rotSec > ITEM_ROTATE_SECONDS) rotSec = ITEM_ROTATE_SECONDS;

            float speedMult = timeScale * (boostActive ? ITEM_SPEEDUP_MULT : 1.0f);
            int rawDir = (g_keyDown ? 1 : 0) - (g_keyUp ? 1 : 0);
            int myDir = hostRevActive ? -rawDir : rawDir;
            int rawPeer = peerVal.load();
            int peerDir = clientRevActive ? -rawPeer : rawPeer;
            float p1Spd = PADDLE_SPD * (hostFast ? ITEM_PADDLESPEED_MULT : 1.0f);
            float p2Spd = PADDLE_SPD * (clientFast ? ITEM_PADDLESPEED_MULT : 1.0f);

            float bx, by, p1, p2; int s1, s2; int secLeft;
            bool justScored = false;
            {
                std::lock_guard<std::mutex> lk(g_render.mtx);
                g_render.p1Y += myDir * p1Spd;
                g_render.p2Y += peerDir * p2Spd;
                clampPaddle(g_render.p1Y);
                clampPaddle(g_render.p2Y);

                g_render.ballX += ballVX * speedMult;
                g_render.ballY += ballVY * speedMult;
                if (g_render.ballY <= 0 || g_render.ballY >= FIELD_H - 1) ballVY = -ballVY;

                if (g_render.ballX <= 2 && g_render.ballY >= g_render.p1Y - 1 && g_render.ballY <= g_render.p1Y + PADDLE_H) {
                    ballVX = -ballVX * HIT_SPEEDUP; ballVY *= HIT_SPEEDUP; g_render.ballX = 2; lastHitBy = 1;
                }
                if (g_render.ballX >= FIELD_W - 3 && g_render.ballY >= g_render.p2Y - 1 && g_render.ballY <= g_render.p2Y + PADDLE_H) {
                    ballVX = -ballVX * HIT_SPEEDUP; ballVY *= HIT_SPEEDUP; g_render.ballX = (float)(FIELD_W - 3); lastHitBy = 2;
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
                            if (lastHitBy == 1) hostRevUntil = now + std::chrono::seconds(ITEM_REVERSE_SECONDS);
                            else if (lastHitBy == 2) clientRevUntil = now + std::chrono::seconds(ITEM_REVERSE_SECONDS);
                            break;
                        case ITEM_ROTATE:
                            rotateUntil = now + std::chrono::seconds(ITEM_ROTATE_SECONDS);
                            break;
                        case ITEM_REWIND: {
                            float target = elapsedSec - (float)ITEM_REWIND_SECONDS;
                            const Snap* pick = nullptr;
                            for (auto& sN : history) { if (sN.t <= target) pick = &sN; else break; }
                            if (pick) {
                                g_render.ballX = pick->bx; g_render.ballY = pick->by;
                                ballVX = pick->bvx; ballVY = pick->bvy;
                                g_render.p1Y = pick->p1; g_render.p2Y = pick->p2;
                                g_render.score1 = pick->s1; g_render.score2 = pick->s2;
                                matchStart = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<float>(pick->t));
                                elapsedSec = pick->t;
                            }
                            break;
                        }
                        default: break;
                        }
                        itemActive = false;
                        itemNextSpawn = now + std::chrono::milliseconds((int)(randRange(ITEM_SPAWN_MIN_SEC, ITEM_SPAWN_MAX_SEC) * 1000));
                    }
                }
                if (!itemActive && now >= itemNextSpawn) {
                    itemActive = true;
                    itemType = randItemType();
                    itemX = randRange(10.0f, (float)FIELD_W - 10.0f);
                    itemY = randRange(2.0f, (float)FIELD_H - 2.0f);
                }

                if (g_render.ballX < 0) {
                    g_render.score2++; justScored = true;
                    g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
                    ballVX = BALL_SPD_INIT; ballVY = BALL_SPD_INIT * 0.6f; lastHitBy = 0;
                }
                else if (g_render.ballX > FIELD_W) {
                    g_render.score1++; justScored = true;
                    g_render.ballX = FIELD_W / 2.0f; g_render.ballY = FIELD_H / 2.0f;
                    ballVX = -BALL_SPD_INIT; ballVY = BALL_SPD_INIT * 0.6f; lastHitBy = 0;
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
                g_render.ballBoostLeft = boostSec; g_render.rotateLeft = rotSec;

                bx = g_render.ballX; by = g_render.ballY; p1 = g_render.p1Y; p2 = g_render.p2Y;
                s1 = g_render.score1; s2 = g_render.score2;
            }

            history.push_back(Snap{ elapsedSec, bx, by, ballVX, ballVY, p1, p2, s1, s2 });
            while (!history.empty() && history.front().t < elapsedSec - 6.0f) history.pop_front();

            StateMsg st{};
            st.ballX = bx; st.ballY = by; st.p1Y = p1; st.p2Y = p2;
            st.score1 = s1; st.score2 = s2;
            st.phase = 1; st.hostVote = 0; st.clientVote = 0; st.secondsLeft = secLeft; st.restarting = 0;
            st.itemActive = itemActive ? 1 : 0; st.itemType = itemType; st.itemX = itemX; st.itemY = itemY;
            st.hostFastLeft = (uint8_t)hostFastSec; st.clientFastLeft = (uint8_t)clientFastSec;
            st.hostRevLeft = (uint8_t)hostRevSec; st.clientRevLeft = (uint8_t)clientRevSec;
            st.ballBoostLeft = (uint8_t)boostSec; st.rotateLeft = (uint8_t)rotSec;
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
        else toSend = (int8_t)(g_localRematchVote.load() == 1 ? 1 : 0); // lobby-ready / rematch-vote
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
static BtnRect g_hostBtn, g_joinBtn, g_quitBtn;

static std::vector<std::string> buildMenuLines() {
    std::vector<std::string> L;
    L.push_back("========================================================");
    L.push_back("                 C O N S O L E   P O N G");
    L.push_back("========================================================");
    L.push_back("");
    L.push_back(" No dedicated server needed - one player HOSTs the game,");
    L.push_back(" the other player JOINs using the host's IP and port.");
    L.push_back("");

    int row = (int)L.size();
    std::string btnLine = "   ";
    auto addBtn = [&](const std::string& text, BtnRect& r) {
        r.row = row; r.colStart = (int)btnLine.size(); btnLine += text; r.colEnd = (int)btnLine.size();
        };
    addBtn("[ 1: HOST ]", g_hostBtn);
    btnLine += "      ";
    addBtn("[ 2: JOIN ]", g_joinBtn);
    btnLine += "      ";
    addBtn("[ 3: QUIT ]", g_quitBtn);
    L.push_back(btnLine);
    L.push_back("");
    L.push_back(" Click a button above, or press 1 / 2 / 3 on your keyboard.");
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
static const int GAME_ORIGIN_X = 180;
static const int GAME_ORIGIN_Y = 80;
static const int GAME_W_PX = 600;
static const int GAME_H_PX = 400;
static const float SCALE_X = (float)GAME_W_PX / FIELD_W;
static const float SCALE_Y = (float)GAME_H_PX / FIELD_H;

static const int GAME_ORIGIN_X_P = 70;
static const int GAME_ORIGIN_Y_P = 100;
static const int GAME_W_PX_P = 400;
static const int GAME_H_PX_P = 700;
static const float SCALE_X_P = (float)GAME_H_PX_P / FIELD_W; // width-axis -> vertical extent
static const float SCALE_Y_P = (float)GAME_W_PX_P / FIELD_H; // height-axis -> horizontal extent

static const int PADDLE_PX_W = 14;
static const int BALL_PX = 14;

static void drawGameScreen(HDC memDC, HFONT font, int lineH, bool layoutRotated) {
    float bx, by, p1, p2; int s1, s2, phase, secondsLeft, votes; bool isHost;
    int itemActive, itemType; float itemX, itemY;
    int hostFastLeft, clientFastLeft, hostRevLeft, clientRevLeft, ballBoostLeft, rotateLeft;
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
        ballBoostLeft = g_render.ballBoostLeft; rotateLeft = g_render.rotateLeft;
    }

    HFONT oldFont = (HFONT)SelectObject(memDC, font);
    SetBkMode(memDC, TRANSPARENT);
    SetTextColor(memDC, RGB(255, 255, 255));
    HBRUSH whiteBrush = (HBRUSH)GetStockObject(WHITE_BRUSH);
    HBRUSH itemBrush = CreateSolidBrush(RGB(255, 210, 60));

    int ox, oy, gw, gh;
    if (!layoutRotated) { ox = GAME_ORIGIN_X; oy = GAME_ORIGIN_Y; gw = GAME_W_PX; gh = GAME_H_PX; }
    else { ox = GAME_ORIGIN_X_P; oy = GAME_ORIGIN_Y_P; gw = GAME_W_PX_P; gh = GAME_H_PX_P; }

    auto mapPt = [&](float fx, float fy, int& outX, int& outY) {
        if (!layoutRotated) { outX = ox + (int)(fx * SCALE_X); outY = oy + (int)(fy * SCALE_Y); }
        else { outX = ox + (int)(fy * SCALE_Y_P); outY = oy + (int)(fx * SCALE_X_P); }
        };

    char scoreBuf[128];
    if (phase == 1) {
        int mm = secondsLeft / 60, ss = secondsLeft % 60;
        snprintf(scoreBuf, sizeof(scoreBuf), "SCORE  %d : %d      Time %d:%02d", s1, s2, mm, ss);
    }
    else {
        snprintf(scoreBuf, sizeof(scoreBuf), "SCORE  %d : %d", s1, s2);
    }
    TextOutA(memDC, 16, 12, scoreBuf, (int)strlen(scoreBuf));

    RECT field{ ox, oy, ox + gw, oy + gh };
    FrameRect(memDC, &field, whiteBrush);

    if (!layoutRotated) {
        int cx = ox + gw / 2;
        for (int y = oy; y < oy + gh; y += 20) { RECT d{ cx - 2, y, cx + 2, y + 10 }; FillRect(memDC, &d, whiteBrush); }
        RECT leftP{ ox + 6, oy + (int)(p1 * SCALE_Y), ox + 6 + PADDLE_PX_W, oy + (int)((p1 + PADDLE_H) * SCALE_Y) };
        FillRect(memDC, &leftP, whiteBrush);
        RECT rightP{ ox + gw - 6 - PADDLE_PX_W, oy + (int)(p2 * SCALE_Y), ox + gw - 6, oy + (int)((p2 + PADDLE_H) * SCALE_Y) };
        FillRect(memDC, &rightP, whiteBrush);
    }
    else {
        int cy = oy + gh / 2;
        for (int x = ox; x < ox + gw; x += 20) { RECT d{ x, cy - 2, x + 10, cy + 2 }; FillRect(memDC, &d, whiteBrush); }
        RECT topP{ ox + (int)(p1 * SCALE_Y_P), oy + 6, ox + (int)((p1 + PADDLE_H) * SCALE_Y_P), oy + 6 + PADDLE_PX_W };
        FillRect(memDC, &topP, whiteBrush);
        RECT botP{ ox + (int)(p2 * SCALE_Y_P), oy + gh - 6 - PADDLE_PX_W, ox + (int)((p2 + PADDLE_H) * SCALE_Y_P), oy + gh - 6 };
        FillRect(memDC, &botP, whiteBrush);
    }

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
        bool localVoted = (g_localRematchVote.load() == 1);
        char line2[128];
        if (localVoted)
            snprintf(line2, sizeof(line2), "Play again? (%d/2) - waiting for opponent... %ds left", votes, secondsLeft);
        else
            snprintf(line2, sizeof(line2), "Play again? (%d/2) - press ENTER   %ds left", votes, secondsLeft);
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
        const char* msg2 = "Move: W/S, Up/Down, A/D or Left/Right      [ESC] Quit match";
        TextOutA(memDC, 16, textY, msg2, (int)strlen(msg2));
        std::string eff;
        int myFast = isHost ? hostFastLeft : clientFastLeft;
        int myRev = isHost ? hostRevLeft : clientRevLeft;
        if (myFast > 0) eff += "Fast paddle(" + std::to_string(myFast) + "s) ";
        if (myRev > 0)  eff += "REVERSED!(" + std::to_string(myRev) + "s) ";
        if (ballBoostLeft > 0) eff += "Ball boost(" + std::to_string(ballBoostLeft) + "s) ";
        if (rotateLeft > 0)    eff += "Rotated(" + std::to_string(rotateLeft) + "s) ";
        if (!eff.empty()) TextOutA(memDC, 16, textY + lineH, eff.c_str(), (int)eff.size());
    }

    DeleteObject(itemBrush);
    SelectObject(memDC, oldFont);
}

// ---------------- Win32 boilerplate ----------------
static const int CLIENT_W = 960; // 16:9 window (landscape)
static const int CLIENT_H = 540;
static const int PAD_X = 10, PAD_Y = 10;
static const UINT_PTR ID_TIMER = 1;
static const int ROTATE_ANIM_MS = 500;

static HFONT g_font = nullptr;
static int g_charW = 10, g_charH = 18;
static DWORD g_baseStyle = 0, g_baseExStyle = 0; // the normal bordered window style

// ---- Rotate-item transition: temporarily becomes a borderless layered
// window so the whole window can visibly spin, like a physical rotation. ----
static bool  g_rotated = false;             // settled orientation (false=landscape, true=portrait)
static bool  g_rotTransitionActive = false;
static bool  g_rotEntering = false;         // true = becoming portrait, false = becoming landscape
static std::chrono::steady_clock::time_point g_rotTransStart;
static int   g_rotCenterX = 0, g_rotCenterY = 0; // screen-space center kept fixed while spinning
static int   g_rotSrcW = 0, g_rotSrcH = 0;
static Gdiplus::Bitmap* g_rotSourceBmp = nullptr;

static int    g_rotCanvasSize = 0;
static HBITMAP g_layerDib = nullptr;
static void* g_layerBits = nullptr;
static HDC     g_layerMemDC = nullptr;
static Gdiplus::Bitmap* g_layerGdiBmp = nullptr;

static void drawGameScreen(HDC memDC, HFONT font, int lineH, bool layoutRotated); // fwd decl

static void ensureLayerSurface() {
    if (g_layerDib) return;
    g_rotCanvasSize = (int)std::ceil(std::sqrt((double)CLIENT_W * CLIENT_W + (double)CLIENT_H * CLIENT_H)) + 20;
    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = g_rotCanvasSize;
    bmi.bmiHeader.biHeight = -g_rotCanvasSize; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    HDC screenDC = GetDC(nullptr);
    g_layerDib = CreateDIBSection(screenDC, &bmi, DIB_RGB_COLORS, &g_layerBits, nullptr, 0);
    g_layerMemDC = CreateCompatibleDC(screenDC);
    SelectObject(g_layerMemDC, g_layerDib);
    ReleaseDC(nullptr, screenDC);
    g_layerGdiBmp = new Gdiplus::Bitmap(g_rotCanvasSize, g_rotCanvasSize, g_rotCanvasSize * 4,
        PixelFormat32bppPARGB, (BYTE*)g_layerBits);
}

static Gdiplus::Bitmap* captureCurrentFrame(int w, int h, bool layoutRotated) {
    HDC screenDC = GetDC(nullptr);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP bmp = CreateCompatibleBitmap(screenDC, w, h);
    HBITMAP old = (HBITMAP)SelectObject(memDC, bmp);
    RECT rc{ 0, 0, w, h };
    HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
    FillRect(memDC, &rc, blackBrush);
    DeleteObject(blackBrush);
    drawGameScreen(memDC, g_font, g_charH, layoutRotated);
    Gdiplus::Bitmap* gb = Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr);
    SelectObject(memDC, old);
    DeleteObject(bmp);
    DeleteDC(memDC);
    ReleaseDC(nullptr, screenDC);
    return gb;
}

static void beginRotationTransition(HWND hwnd, bool entering) {
    ensureLayerSurface();

    RECT wr; GetWindowRect(hwnd, &wr);
    g_rotCenterX = (wr.left + wr.right) / 2;
    g_rotCenterY = (wr.top + wr.bottom) / 2;

    bool srcRotated = g_rotated; // current (pre-transition) orientation
    g_rotSrcW = srcRotated ? CLIENT_H : CLIENT_W;
    g_rotSrcH = srcRotated ? CLIENT_W : CLIENT_H;

    if (g_rotSourceBmp) { delete g_rotSourceBmp; g_rotSourceBmp = nullptr; }
    g_rotSourceBmp = captureCurrentFrame(g_rotSrcW, g_rotSrcH, srcRotated);

    SetWindowLongPtrA(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, g_baseExStyle | WS_EX_LAYERED);
    SetWindowPos(hwnd, HWND_TOP, g_rotCenterX - g_rotCanvasSize / 2, g_rotCenterY - g_rotCanvasSize / 2,
        g_rotCanvasSize, g_rotCanvasSize, SWP_FRAMECHANGED | SWP_SHOWWINDOW);

    g_rotEntering = entering;
    g_rotTransitionActive = true;
    g_rotTransStart = std::chrono::steady_clock::now();
}

static void endRotationTransition(HWND hwnd) {
    g_rotTransitionActive = false;
    g_rotated = g_rotEntering;
    if (g_rotSourceBmp) { delete g_rotSourceBmp; g_rotSourceBmp = nullptr; }

    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, g_baseExStyle);
    SetWindowLongPtrA(hwnd, GWL_STYLE, g_baseStyle);

    int cw = g_rotated ? CLIENT_H : CLIENT_W;
    int ch = g_rotated ? CLIENT_W : CLIENT_H;
    RECT wr{ 0, 0, cw, ch };
    AdjustWindowRect(&wr, g_baseStyle, FALSE);
    int outerW = wr.right - wr.left, outerH = wr.bottom - wr.top;
    SetWindowPos(hwnd, HWND_TOP, g_rotCenterX - outerW / 2, g_rotCenterY - outerH / 2, outerW, outerH,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, FALSE);
}

static void stepRotationTransition(HWND hwnd) {
    float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_rotTransStart).count() * 1000.0f / ROTATE_ANIM_MS;
    bool done = false;
    if (t >= 1.0f) { t = 1.0f; done = true; }
    float angle = 90.0f * t;

    Gdiplus::Graphics g(g_layerGdiBmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));
    g.SetCompositingMode(Gdiplus::CompositingModeSourceOver);
    g.TranslateTransform((Gdiplus::REAL)g_rotCanvasSize / 2.0f, (Gdiplus::REAL)g_rotCanvasSize / 2.0f);
    g.RotateTransform(angle);
    g.TranslateTransform(-(Gdiplus::REAL)g_rotSrcW / 2.0f, -(Gdiplus::REAL)g_rotSrcH / 2.0f);
    if (g_rotSourceBmp) g.DrawImage(g_rotSourceBmp, 0, 0, g_rotSrcW, g_rotSrcH);

    POINT ptSrc{ 0, 0 };
    SIZE sz{ g_rotCanvasSize, g_rotCanvasSize };
    POINT ptDst{ g_rotCenterX - g_rotCanvasSize / 2, g_rotCenterY - g_rotCanvasSize / 2 };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    HDC screenDC = GetDC(nullptr);
    UpdateLayeredWindow(hwnd, screenDC, &ptDst, &sz, g_layerMemDC, &ptSrc, 0, &bf, ULW_ALPHA);
    ReleaseDC(nullptr, screenDC);

    if (done) endRotationTransition(hwnd);
}

static void cancelRotationForced(HWND hwnd) {
    if (g_rotSourceBmp) { delete g_rotSourceBmp; g_rotSourceBmp = nullptr; }
    g_rotTransitionActive = false;
    g_rotated = false;

    SetWindowLongPtrA(hwnd, GWL_EXSTYLE, g_baseExStyle);
    SetWindowLongPtrA(hwnd, GWL_STYLE, g_baseStyle);

    RECT wr{ 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&wr, g_baseStyle, FALSE);
    int outerW = wr.right - wr.left, outerH = wr.bottom - wr.top;
    int cx = g_rotCenterX != 0 ? g_rotCenterX : (outerW / 2 + 100);
    int cy = g_rotCenterY != 0 ? g_rotCenterY : (outerH / 2 + 100);
    SetWindowPos(hwnd, HWND_TOP, cx - outerW / 2, cy - outerH / 2, outerW, outerH,
        SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    InvalidateRect(hwnd, nullptr, FALSE);
}

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
            if (!g_rotTransitionActive && wantPortrait != g_rotated) {
                beginRotationTransition(hwnd, wantPortrait);
            }
            if (g_rotTransitionActive) {
                stepRotationTransition(hwnd);
            }
        }
        else if (g_rotated || g_rotTransitionActive) {
            cancelRotationForced(hwnd);
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
            else if (wParam == '3') DestroyWindow(hwnd);
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

        if (g_rotTransitionActive) {
            // The layered popup window is painted via UpdateLayeredWindow instead.
            EndPaint(hwnd, &ps);
            return 0;
        }

        RECT rc; GetClientRect(hwnd, &rc);

        HDC memDC = CreateCompatibleDC(hdc);
        HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
        HBITMAP oldBmp = (HBITMAP)SelectObject(memDC, bmp);

        HBRUSH blackBrush = CreateSolidBrush(RGB(0, 0, 0));
        FillRect(memDC, &rc, blackBrush);
        DeleteObject(blackBrush);

        if (g_state == AppState::PLAYING) {
            drawGameScreen(memDC, g_font, g_charH, g_rotated);
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
        BitBlt(hdc, 0, 0, rc.right, rc.bottom, memDC, 0, 0, SRCCOPY);
        SelectObject(memDC, oldBmp);
        DeleteObject(bmp);
        DeleteDC(memDC);
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

    const char* CLASS_NAME = "ConsolePongWindowClass";
    WNDCLASSA wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorA(nullptr, MAKEINTRESOURCEA(32512)); // IDC_ARROW, forced ANSI
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);

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

    g_baseStyle = (WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME & ~WS_MAXIMIZEBOX);
    g_baseExStyle = 0;
    RECT wr{ 0, 0, CLIENT_W, CLIENT_H };
    AdjustWindowRect(&wr, g_baseStyle, FALSE);

    HWND hwnd = CreateWindowA(
        CLASS_NAME, "Console Pong", g_baseStyle,
        CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top,
        nullptr, nullptr, hInstance, nullptr);

    buildMenuLines(); // populate button hit-rects before the first click can occur

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_rotSourceBmp) { delete g_rotSourceBmp; g_rotSourceBmp = nullptr; }
    if (g_layerGdiBmp) { delete g_layerGdiBmp; g_layerGdiBmp = nullptr; }
    if (g_layerMemDC) { DeleteDC(g_layerMemDC); g_layerMemDC = nullptr; }
    if (g_layerDib) { DeleteObject(g_layerDib); g_layerDib = nullptr; }
    Gdiplus::GdiplusShutdown(gdipToken);

    if (g_font) DeleteObject(g_font);
    WSACleanup();
    return (int)msg.wParam;
}