# Relay auth service

A small Cloudflare Worker. It's the only thing that knows the Apinator app secret. The mod asks it for a room code
and for signatures that let it subscribe to Apinator's private channels.

| Endpoint | What it does |
|---|---|
| `GET /v1/config` | returns the public app key and cluster |
| `POST /v1/room` | creates a room: `{code, hostToken}`, where `hostToken = HMAC(secret, "host|" + code)` |
| `POST /v1/auth` | takes `{socket_id, channel or channels, hostToken or jsecret}` and returns `{auth: {channel: "key:hmac"}}` |

The rules are in `src/index.js`. The lobby `private-et-<CODE>-l` is open to anyone. A player channel
`private-et-<CODE>-p<jid>` is only signed for the host (`hostToken`) or for someone who knows the `jsecret` whose
SHA-256 prefix is `jid`. The signature is `hex(HMAC-SHA256(secret, socket_id + ":" + channel))`, which is what
Apinator expects.

```
npm install
npm test
npx wrangler dev        # local; put APINATOR_SECRET in .dev.vars (see .dev.vars.example)
npx wrangler login
npx wrangler secret put APINATOR_SECRET
npx wrangler deploy
```

`APINATOR_KEY` and `APINATOR_CLUSTER` are public and live in `wrangler.toml`. The secret stays out of the repo.
