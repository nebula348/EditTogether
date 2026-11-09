# EditTogether

A Geometry Dash mod for editing a level with other people at the same time. It works on the same Wi-Fi and,
since v0.6, over the internet. Free, MIT licensed, made by nebula348.

Android ARM64 only. Needs Geometry Dash 2.2081 and Geode 5.11.2. Mod ID: `edittogether`.

## What it does

One player hosts a room and the others join. The host approves each new player, and from then on everything
anyone does in the editor (placing, moving, rotating, scaling, triggers, colors, groups, level settings) shows
up on everyone else's screen. When someone playtests, the others see their icon move through the level.

Two ways to connect, usable at the same time:

- **Same Wi-Fi:** Find Rooms lists the rooms nearby, or use Connect by IP. The phones talk directly over TCP.
- **Online:** the host ticks "Online" when creating the room and gets an 8-letter code. Others use
  Find Rooms, then Join Online, and type the code. Traffic goes through [Apinator](https://apinator.io),
  a hosted WebSocket service, so nobody has to open ports or be on the same network.

## Using it

1. Log in to your Geometry Dash account. Your username is your name in rooms.
2. Open a level in the editor and tap the EditTogether button, next to the trash button.
3. Host: Create Room, pick the max number of players (2 to 100) and an optional password. A red badge on the
   button shows pending join requests; accept or reject them in Current Session.
4. Everyone else: Find Rooms (or Join Online) and press Join. You'll be asked whether to save your current level
   first. Nothing is saved without a yes. The room opens as a new local level called `ET <room name>`.
5. Selecting an object locks it, and others see your name above it.
6. If you drop and reconnect, you get back in without a new approval.

The host can switch players between Editor and Viewer, release their locks, or remove them.

## How online play works

Apinator only relays messages, it isn't peer to peer. Each room gets two kinds of private channels:

- `private-et-<CODE>-l`, a lobby where a joiner announces a random public id;
- `private-et-<CODE>-p<id>`, one channel per player that only the host and that player can subscribe to.

To subscribe to a private channel you need a signature made with the Apinator app secret, so the secret can't live
in the mod. A small Cloudflare Worker (`server/`) holds it and signs only what the rules allow: the lobby for anyone,
a player's channel for the host or for whoever knows the secret behind that id. Someone who only knows the room
code can sit in the lobby but can't read anyone's edits.

The host still decides who gets in. Passwords, approvals, roles and locks are checked by the host, same as on the
LAN. The game protocol is identical in both modes; online it just runs over a chunked, numbered link on top of the
channel. Network code runs on its own thread, so it never blocks the game.

### Setting up your own relay

You need a free Apinator app and a free Cloudflare account.

```
cd server
npm install
npx wrangler login
npx wrangler secret put APINATOR_SECRET
npx wrangler deploy
```

Put your app key and cluster in `server/wrangler.toml`, and the URL that `deploy` prints in the `relay-url`
setting of `mod.json` (players can also change it in the mod settings). The secret never goes in the repo.
See `.env.example` for the values involved.

The free Apinator plan has limits (500 connections, 500,000 messages a day, shared by everyone using the same app).
The mod sends player positions at 10 Hz online instead of 30 Hz to stay well under them, but I haven't measured
how Apinator counts messages delivered to several subscribers.

## Installing

Copy `edittogether.geode` into the Geode mods folder and restart the game:

```
adb push edittogether.geode /storage/emulated/0/Android/media/com.geode.launcher/game/geode/mods/
```

If you have the old `lean.edittogether.geode`, delete it. The mod was renamed and old settings don't carry over.

## Building

You need the Geode SDK 5.11.2 (`GEODE_SDK` set), the Geode CLI and Android NDK r29. Older NDKs fail because Geode 5
needs Clang 19 and a libc++ with `std::atomic_ref`.

```
geode build -p android64 --ndk <path to ndk r29> --config Release
```

mbedTLS 3.6.2 is downloaded by CMake (the hash is pinned) and linked statically for TLS.

## Tests

The tests don't need Geode or the game:

```
cmake -S tests -B build-tests
cmake --build build-tests
build-tests/core_tests
build-tests/net_tests
build-tests/relay_tests
```

- `core_tests`: protocol, validation, join and approval, roles, locks, ordering, checksums, snapshots, reconnection.
- `net_tests`: LAN sockets, discovery, a full session over TCP.
- `relay_tests`: JSON, WebSocket framing, and a whole host and player session over an in-memory relay that follows
  Apinator's rules, including eavesdroppers, wrong passwords and reconnection.
- `relay_live`: the same flow through the real Apinator and a running auth service
  (`ET_RELAY_URL=<url> build-tests/relay_live`). This one needs network access.
- `cd server && npm test`: the Worker's rules.

`scripts/test-android.ps1` builds and runs the suites on a connected phone. `tools/et_bot.cpp` is a scripted
second player for testing with one phone.

## Layout

```
src/core      protocol, host and client logic (no Geode)
src/net       LAN sockets and the online stack (TLS, WebSocket, HTTP, Apinator client, relay rooms)
src/session   Session (network, cores, editor) and EditorSync (the Geode side)
src/mod       hooks and the editor button
src/ui        popups
server        the Cloudflare Worker
```

## Known problems

- Online play depends on a shared free plan and on the distance to Apinator's `us` servers (about 150 ms from Argentina).
- A removed player can keep listening on their own channel until the room closes. There is no end-to-end encryption.
- Edits made while disconnected are thrown away when the room state loads after reconnecting.
- Undoing something another player changed in the meantime can duplicate the object.
- Leaving the editor ends your session (the host's room closes), unless you go to play the room's level.
- Changing level settings reloads the editor on the other phones, at most once every 10 seconds.
- Very big levels (50k+ objects) are slow to join.
- The Worker signs lobby channels for any valid-looking code, so someone could use up the Apinator quota.
  Add rate limiting on the Worker if you publish it widely.

## License

MIT, see `LICENSE`. Uses mbedTLS (Apache-2.0).
