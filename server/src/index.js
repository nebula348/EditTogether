// Auth service for the EditTogether relay (Cloudflare Worker). It is the only place that knows the Apinator
// app secret; the mod asks it to sign channel subscriptions.
//
//   private-et-<CODE>-l        lobby: joiners announce their public id (jid)
//   private-et-<CODE>-p<jid>   one channel per player, for the host and that player only
//
// jid = first 16 bytes of SHA-256(jsecret). A player proves it owns a jid by sending jsecret, which never goes
// through the lobby. The host proves itself with hostToken = HMAC(secret, "host|" + code).

const enc = new TextEncoder();
const CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
const CHANNEL = /^private-et-([A-Z2-9]{8})-(l|p[0-9a-f]{32})$/;
const HEX = (bytes) => [...new Uint8Array(bytes)].map((b) => b.toString(16).padStart(2, "0")).join("");

async function hmacHex(secret, message) {
  const key = await crypto.subtle.importKey("raw", enc.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  return HEX(await crypto.subtle.sign("HMAC", key, enc.encode(message)));
}

async function sha256Hex(text) {
  return HEX(await crypto.subtle.digest("SHA-256", enc.encode(text)));
}

function sameString(a, b) {
  if (a.length !== b.length) return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

const json = (status, body) =>
  new Response(JSON.stringify(body), { status, headers: { "content-type": "application/json" } });

async function readBody(request) {
  const text = await request.text();
  if (text.length > 4096) return null;
  try {
    const value = JSON.parse(text || "{}");
    return value && typeof value === "object" ? value : null;
  } catch {
    return null;
  }
}

function newCode() {
  const bytes = crypto.getRandomValues(new Uint8Array(8));
  return [...bytes].map((b) => CODE_ALPHABET[b % CODE_ALPHABET.length]).join("");
}

// Which channel this caller may subscribe to; returns an error string, or "" when allowed.
async function authorize(env, channel, body) {
  const m = CHANNEL.exec(channel);
  if (!m) return "unknown channel";
  const [, code, kind] = m;
  if (kind === "l") return "";
  const hostToken = typeof body.hostToken === "string" ? body.hostToken : "";
  if (hostToken && sameString(hostToken, await hmacHex(env.APINATOR_SECRET, "host|" + code))) return "";
  const jsecret = typeof body.jsecret === "string" ? body.jsecret : "";
  if (/^[0-9a-f]{32}$/.test(jsecret) && sameString((await sha256Hex(jsecret)).slice(0, 32), kind.slice(1))) return "";
  return "not allowed";
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);

    if (request.method === "GET" && url.pathname === "/v1/config") {
      return json(200, { key: env.APINATOR_KEY, cluster: env.APINATOR_CLUSTER, version: 1 });
    }

    if (request.method !== "POST") return json(405, { error: "method not allowed" });
    const body = await readBody(request);
    if (!body) return json(400, { error: "bad request" });

    if (url.pathname === "/v1/room") {
      const code = newCode();
      return json(200, { code, hostToken: await hmacHex(env.APINATOR_SECRET, "host|" + code) });
    }

    if (url.pathname === "/v1/auth") {
      const socketId = typeof body.socket_id === "string" ? body.socket_id : "";
      if (!/^[\w.\-]{1,64}$/.test(socketId)) return json(400, { error: "bad socket_id" });
      const channels = Array.isArray(body.channels) ? body.channels : [body.channel];
      if (channels.length < 1 || channels.length > 64) return json(400, { error: "bad channels" });
      const auth = {};
      for (const channel of channels) {
        if (typeof channel !== "string") return json(400, { error: "bad channel" });
        const denied = await authorize(env, channel, body);
        if (denied) return json(403, { error: denied, channel });
        auth[channel] = env.APINATOR_KEY + ":" + (await hmacHex(env.APINATOR_SECRET, socketId + ":" + channel));
      }
      return json(200, { auth });
    }

    return json(404, { error: "not found" });
  },
};
