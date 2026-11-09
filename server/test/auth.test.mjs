import assert from "node:assert/strict";
import crypto from "node:crypto";
import worker from "../src/index.js";

const env = { APINATOR_SECRET: "test-secret", APINATOR_KEY: "app_test", APINATOR_CLUSTER: "us" };
const call = async (path, body, method = "POST") => {
  const r = await worker.fetch(new Request("https://x.test" + path, { method, body: body === undefined ? undefined : JSON.stringify(body) }), env);
  return [r.status, await r.json()];
};
const hmac = (m) => crypto.createHmac("sha256", env.APINATOR_SECRET).update(m).digest("hex");
let passed = 0;
const test = async (name, fn) => { await fn(); passed++; console.log("ok  " + name); };

await test("config is public and has no secret", async () => {
  const [s, b] = await call("/v1/config", undefined, "GET");
  assert.equal(s, 200);
  assert.deepEqual(b, { key: "app_test", cluster: "us", version: 1 });
});

const [, room] = await call("/v1/room", {});
const code = room.code;
const jsecret = crypto.randomBytes(16).toString("hex");
const jid = crypto.createHash("sha256").update(jsecret).digest("hex").slice(0, 32);
const lobby = `private-et-${code}-l`;
const mine = `private-et-${code}-p${jid}`;

await test("room codes are 8 chars from the safe alphabet and unique", async () => {
  assert.match(code, /^[A-HJ-NP-Z2-9]{8}$/);
  const [, other] = await call("/v1/room", {});
  assert.notEqual(other.code, code);
  assert.equal(room.hostToken, hmac("host|" + code));
});

await test("lobby needs no proof; signature is HMAC-SHA256 hex of socket_id:channel", async () => {
  const [s, b] = await call("/v1/auth", { socket_id: "node.1", channel: lobby });
  assert.equal(s, 200);
  assert.equal(b.auth[lobby], "app_test:" + hmac("node.1:" + lobby));
});

await test("joiner channel: owner and host allowed, everyone else refused", async () => {
  assert.equal((await call("/v1/auth", { socket_id: "n.1", channel: mine, jsecret }))[0], 200);
  assert.equal((await call("/v1/auth", { socket_id: "n.1", channel: mine, hostToken: room.hostToken }))[0], 200);
  assert.equal((await call("/v1/auth", { socket_id: "n.1", channel: mine }))[0], 403);
  const spy = crypto.randomBytes(16).toString("hex");
  assert.equal((await call("/v1/auth", { socket_id: "n.1", channel: mine, jsecret: spy }))[0], 403);
  const [, otherRoom] = await call("/v1/room", {});
  assert.equal((await call("/v1/auth", { socket_id: "n.1", channel: mine, hostToken: otherRoom.hostToken }))[0], 403);
});

await test("batch auth returns every channel or fails as a whole", async () => {
  const [s, b] = await call("/v1/auth", { socket_id: "n.2", channels: [lobby, mine], jsecret });
  assert.equal(s, 200);
  assert.deepEqual(Object.keys(b.auth).sort(), [lobby, mine].sort());
  const [s2] = await call("/v1/auth", { socket_id: "n.2", channels: [lobby, mine] });
  assert.equal(s2, 403);
});

await test("rejects malformed channels, socket ids and bodies", async () => {
  for (const channel of ["presence-et-" + code + "-l", "private-other", `private-et-${code}-x`, `private-et-abc-l`, 5])
    assert.notEqual((await call("/v1/auth", { socket_id: "n.1", channel }))[0], 200, String(channel));
  assert.equal((await call("/v1/auth", { socket_id: "bad id!", channel: lobby }))[0], 400);
  assert.equal((await call("/v1/auth", { channels: new Array(65).fill(lobby), socket_id: "n.1" }))[0], 400);
  const r = await worker.fetch(new Request("https://x.test/v1/auth", { method: "POST", body: "{not json" }), env);
  assert.equal(r.status, 400);
  assert.equal((await call("/nope", {}))[0], 404);
});

console.log(`${passed} tests passed`);
