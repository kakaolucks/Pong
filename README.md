# Console Pong

A console-style online Pong game built with C++17 and the native Windows API.

Despite its console-inspired appearance, this is **not a console application**. It runs as a native Win32 GUI application with a graphical game window.

## Features

* Console-style terminal interface
* Native Win32 GUI
* Online multiplayer over TCP
* Host and Join modes
* Authoritative host-side game simulation
* Smooth 60 FPS gameplay
* 5-minute matches
* First to 11 points wins
* Ball speed gradually increases during a match
* Multiple gameplay items and temporary effects
* Screen rotation effect
* Rewind effect
* Rematch system
* Keyboard controls
* Connection and error handling

## Game Modes

### Host

The host creates the game and listens for an incoming TCP connection.

The host controls the left paddle.

### Join

The second player connects to the host using an IP address or hostname and port.

The joining player controls the right paddle.

The game does not require a dedicated server. The host process acts as the authoritative game server while also playing the game.

## Controls

| Key                          | Action                     |
| ---------------------------- | -------------------------- |
| `W` / `A`                    | Move paddle up             |
| `S` / `D`                    | Move paddle down           |
| `Arrow Up` / `Arrow Left`    | Move paddle up             |
| `Arrow Down` / `Arrow Right` | Move paddle down           |
| `Enter`                      | Ready / Continue / Rematch |
| `ESC`                        | Cancel / Return / Abort    |

The menu can also be controlled with:

```text
1 - Host
2 - Join
3 - Quit
```

## Match Rules

A match lasts up to 5 minutes.

The match ends when:

* A player reaches 11 points, or
* The 5-minute timer expires.

If the timer expires, the player with the higher score wins. A match can also end in a tie.

After scoring, there is a short point-break phase before the next serve.

Both players must press `Enter` to continue.

After the match ends, both players have 15 seconds to vote for a rematch.

If both players accept, another round starts using the existing connection.

## Items

Random items appear on the field during gameplay.

### Speed Up

Temporarily increases the ball's speed.

Duration: 5 seconds

### Rewind

Rewinds the game state by up to approximately 5 seconds.

The game stores several seconds of previous game states to implement this effect.

### Bounce

Reverses the ball's horizontal movement.

### Paddle Speed

Temporarily increases the paddle speed of the player who last hit the ball.

Duration: 10 seconds

### Reverse

Temporarily reverses the controls of the player who last hit the ball.

Duration: 10 seconds

### Rotate

Temporarily rotates the game screen.

The rotation includes an animated transition and changes the game field into a portrait-oriented layout.

Duration: 30 seconds

## Ball Speed

The ball gradually becomes faster as the match progresses.

Its time-based speed can increase up to 3 times the initial speed.

Every successful paddle hit also increases the ball speed slightly.

After a point is scored, the ball speed is reset for the next serve.

## Networking

The game uses TCP networking through Winsock2.

The host:

1. Creates a TCP listening socket.
2. Waits for one player to connect.
3. Runs the authoritative game simulation.
4. Sends game state updates to the client.

The client:

1. Connects to the host.
2. Sends player input.
3. Receives authoritative game state updates.

`TCP_NODELAY` is enabled to reduce unnecessary network latency.

Only one client can connect to a host at a time.

## Playing Over the Internet

For players on different networks, the host may need to allow the selected port through the Windows Firewall and configure port forwarding on their router.

The default port is:

```text
5000
```

When connecting, the client needs the host's IP address or hostname and the same port number.

For LAN play, the host's local IP address can be used.

## Interface

The game uses a console-inspired interface with:

* Black backgrounds
* Green terminal-style text
* Consolas font
* Text-based menus
* Keyboard-driven navigation

The actual game uses a graphical Win32 rendering area with smooth paddle and ball movement.

The default game window is approximately:

```text
960 x 540
```

## Technical Details

### Language

```text
C++17
```

### APIs and Libraries

* Windows Win32 API
* Winsock2
* GDI
* GDI+
* User32
* Multithreading
* TCP/IP

### Architecture

The project is implemented as a native Windows executable using `WinMain`.

The host acts as both:

* The game server
* A local game client

The client receives authoritative state from the host instead of independently simulating the complete game state.

## Building

### MinGW / GCC

The source file can be compiled with:

```bash
g++ -O2 -std=c++17 -mwindows -o pong.exe pong.cpp -lgdi32 -luser32 -lws2_32
```

The `-mwindows` option is important because the application uses the Windows GUI subsystem and `WinMain` instead of a console `main()` entry point.

Depending on the MinGW environment, GDI+ may also need to be explicitly linked:

```bash
-lgdiplus
```

For example:

```bash
g++ -O2 -std=c++17 -mwindows -o pong.exe pong.cpp -lgdi32 -lgdiplus -luser32 -lws2_32
```

## Requirements

* Windows
* C++17-compatible compiler
* MinGW-w64 or another Windows C++ compiler
* Winsock2
* GDI / GDI+
* Network connection for multiplayer

## Notes

This project is designed to look and feel like a classic console-style application while using a native graphical Windows window underneath.

It is therefore a **Win32 GUI application, not a Windows console application**.

The console-style interface is part of the game's visual design rather than its underlying application subsystem.
