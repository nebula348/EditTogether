# Changelog

## v0.6.0
- Online rooms through Apinator. Tick "Online" when creating a room, share the code, and others join from Find Rooms with Join Online. Same-Wi-Fi play works as before.
- A small Cloudflare Worker (`server/`) signs channel subscriptions so the Apinator secret stays out of the mod.
- Renamed the mod: ID `edittogether`, developer `nebula348`. Delete the old `lean.edittogether.geode`.
- Removed the Globed and UPnP options.
- You're now asked whether to save your level before joining a room.
- Smoother player movement: positions are timestamped and interpolated.
- Faster reconnection with the saved token, and the session lists keep their scroll position.
- Clearer message when a room runs a different version of the mod.
