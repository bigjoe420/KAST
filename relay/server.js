#!/usr/bin/env node
/*
 * KAST PC relay — local bridge between the ESP32 terminal and the outside
 * world (Kick chat now, OBS in Phase 4). Runs on the streaming PC.
 *
 * Today (skeleton):
 *   - accepts WebSocket connections from the device
 *   - speaks a small JSON line protocol (hello / ping-pong / chat frames)
 *   - --simulate-chat: bench mode, pushes a fake chat line every 5 s so
 *     the device receive path can be verified without Kick credentials
 *
 * Next: Kick API auth (read/send chat). Credentials live in relay/.env
 * (gitignored) and never on the device.
 *
 * Usage: node server.js [--simulate-chat]
 */
'use strict';

const fs = require('fs');
const path = require('path');
const dgram = require('dgram');
const { WebSocketServer } = require('ws');

/* Config: relay/.env (gitignored) overrides the process environment. */
const env_path = path.join(__dirname, '.env');
if (fs.existsSync(env_path)) {
  for (const line of fs.readFileSync(env_path, 'utf8').split(/\r?\n/)) {
    const m = line.match(/^\s*([A-Z0-9_]+)\s*=\s*(.+?)\s*$/);
    if (m && !process.env[m[1]]) {
      process.env[m[1]] = m[2];
    }
  }
}

const PORT = Number(process.env.KAST_RELAY_PORT || 8765);
const SIMULATE = process.argv.includes('--simulate-chat');
let sim_id = 0;   /* simulate-mode chat bounce counter (module scope) */

/* --- persistent log --------------------------------------------------- *
 * The tray appends our output to relay/tray.log. Earlier the tray did
 * this by reading our stdout pipe, but every PowerShell pipe-drain design
 * failed in practice (Peek blocked the UI thread; async DataReceived
 * events crash with "no Runspace available" because they fire on
 * threadpool threads). Simplest robust design: we write our own log.
 * The tray just watches the file, which is all it ever needed. */
{
  const log_stream = fs.createWriteStream(path.join(__dirname, 'tray.log'), { flags: 'a' });
  const tee = (orig) => (...a) => {
    const s = a.map(String).join(' ');
    try { log_stream.write(s + '\n'); } catch { /* disk full etc. — stay up */ }
    orig(s);
  };
  console.log = tee(console.log.bind(console));
  console.error = tee(console.error.bind(console));
}

/* --- Kick API wiring (OAuth 2.1 + PKCE; secrets from relay/.env) ------ *
 * Token flow: relay prints an authorize URL once, the owner approves in
 * the browser, the localhost callback captures the code, tokens persist
 * in relay/.kick-tokens.json (gitignored) and refresh automatically.
 * Device sends stay untouched: chat.send/chat.mod frames keep the same
 * wire shape; the relay just stops being a black hole. */
const crypto = require('crypto');
const http = require('http');

const KICK_CLIENT_ID = process.env.KICK_CLIENT_ID || '';
const KICK_CLIENT_SECRET = process.env.KICK_CLIENT_SECRET || '';
const KICK_REDIRECT_URI = process.env.KICK_REDIRECT_URI || 'http://localhost:8766/callback';
const KICK_TIMEOUT_MIN = Number(process.env.KAST_TIMEOUT_MINUTES || 10);
const KICK_SCOPES = [
  'user:read', 'channel:read', 'chat:write', 'events:subscribe',
  'moderation:ban', 'moderation:chat_message:manage',
].join(' ');
const KICK_API = 'https://api.kick.com/public/v1';
const TOKENS_PATH = path.join(__dirname, '.kick-tokens.json');
const CALLBACK_PORT = Number((KICK_REDIRECT_URI.match(/:(\d+)/) || [0, 8766])[1]);

let kickTokens = null;   /* {access_token, refresh_token, expires_at, broadcaster_user_id} */
try {
  if (fs.existsSync(TOKENS_PATH)) {
    kickTokens = JSON.parse(fs.readFileSync(TOKENS_PATH, 'utf8'));
  }
} catch (err) {
  console.log(`[relay] token file unreadable (${err.message}) — re-auth needed`);
  kickTokens = null;
}

function kickSaveTokens() {
  try {
    fs.writeFileSync(TOKENS_PATH, JSON.stringify(kickTokens, null, 2));
  } catch (err) {
    console.log(`[relay] token save failed: ${err.message}`);
  }
}

function kickReady() {
  return kickTokens != null && kickTokens.access_token;
}

async function kickTokenRequest(form) {
  const res = await fetch('https://id.kick.com/oauth/token', {
    method: 'POST',
    headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
    body: new URLSearchParams(form),
  });
  const j = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error(`token endpoint ${res.status}: ${JSON.stringify(j).slice(0, 200)}`);
  }
  return j;
}

let kickPendingAuth = null;

function kickAuthUrl() {
  const verifier = crypto.randomBytes(48).toString('base64url');
  const challenge = crypto.createHash('sha256').update(verifier).digest('base64url');
  kickPendingAuth = {
    verifier,
    state: crypto.randomBytes(16).toString('base64url'),
  };
  const u = new URL('https://id.kick.com/oauth/authorize');
  u.searchParams.set('response_type', 'code');
  u.searchParams.set('client_id', KICK_CLIENT_ID);
  u.searchParams.set('redirect_uri', KICK_REDIRECT_URI);
  u.searchParams.set('scope', KICK_SCOPES);
  u.searchParams.set('code_challenge', challenge);
  u.searchParams.set('code_challenge_method', 'S256');
  u.searchParams.set('state', kickPendingAuth.state);
  return u.toString();
}

/* localhost callback — catches the browser redirect, exchanges the code.
   Port conflicts must not kill the relay: with tokens already saved the
   callback is dead weight anyway. */
if (KICK_CLIENT_ID) {
  const callback = http.createServer((req, res) => {
    const u = new URL(req.url, KICK_REDIRECT_URI);
    if (u.pathname !== '/callback' || !kickPendingAuth) {
      res.writeHead(404); res.end('nothing here'); return;
    }
    const code = u.searchParams.get('code');
    const state = u.searchParams.get('state');
    if (!code || state !== kickPendingAuth.state) {
      res.writeHead(400); res.end('KAST: bad or stale auth callback — retry from the relay log.'); return;
    }
    (async () => {
      const j = await kickTokenRequest({
        grant_type: 'authorization_code',
        client_id: KICK_CLIENT_ID,
        client_secret: KICK_CLIENT_SECRET,
        redirect_uri: KICK_REDIRECT_URI,
        code_verifier: kickPendingAuth.verifier,
        code,
      });
      kickTokens = {
        access_token: j.access_token,
        refresh_token: j.refresh_token,
        expires_at: Date.now() + (j.expires_in || 3600) * 1000 - 60000,
        broadcaster_user_id: null,   /* resolved lazily on first call */
      };
      kickPendingAuth = null;
      kickSaveTokens();
      console.log('[relay] Kick authorized — tokens saved, chat send is live');
      kickChatStart();
      res.writeHead(200, { 'Content-Type': 'text/plain' });
      res.end('KAST relay authorized on Kick. You can close this tab.');
    })().catch((err) => {
      res.writeHead(500); res.end(`KAST auth failed: ${err.message}`);
    });
  });
  callback.on('error', (err) => {
    console.log(`[relay] OAuth callback unavailable (${err.message}) — fine when already authorized`);
  });
  callback.listen(CALLBACK_PORT, () => {
    console.log(`[relay] OAuth callback listening on ${KICK_REDIRECT_URI}`);
  });
}

async function kickRefresh() {
  const j = await kickTokenRequest({
    grant_type: 'refresh_token',
    client_id: KICK_CLIENT_ID,
    client_secret: KICK_CLIENT_SECRET,
    refresh_token: kickTokens.refresh_token,
  });
  kickTokens.access_token = j.access_token;
  if (j.refresh_token) {   /* refresh tokens rotate */
    kickTokens.refresh_token = j.refresh_token;
  }
  kickTokens.expires_at = Date.now() + (j.expires_in || 3600) * 1000 - 60000;
  kickSaveTokens();
}

async function kickEnsureToken() {
  if (!kickTokens || !kickTokens.refresh_token) {
    throw new Error('not authorized — open the auth URL from the relay log');
  }
  if (Date.now() > kickTokens.expires_at) {
    await kickRefresh();
  }
}

async function kickBroadcasterId() {
  if (kickTokens.broadcaster_user_id) {
    return kickTokens.broadcaster_user_id;
  }
  const j = await kickApi('/users');
  const id = j?.data?.[0]?.user_id ?? j?.data?.user_id ?? j?.user_id;
  if (id == null) {
    throw new Error(`broadcaster id not in /users response: ${JSON.stringify(j).slice(0, 160)}`);
  }
  kickTokens.broadcaster_user_id = id;
  kickSaveTokens();
  return id;
}

async function kickApi(pathname, opts = {}) {
  await kickEnsureToken();
  const res = await fetch(KICK_API + pathname, {
    method: opts.method || 'GET',
    headers: {
      Authorization: `Bearer ${kickTokens.access_token}`,
      ...(opts.body ? { 'Content-Type': 'application/json' } : {}),
    },
    body: opts.body,
  });
  if (res.status === 204) {
    return {};
  }
  const j = await res.json().catch(() => ({}));
  if (!res.ok) {
    throw new Error(`${opts.method || 'GET'} ${pathname} -> ${res.status}: ${JSON.stringify(j).slice(0, 200)}`);
  }
  return j;
}

async function kickUserIdByName(name) {
  const j = await kickApi(`/users?name=${encodeURIComponent(name)}`);
  const id = j?.data?.[0]?.user_id ?? j?.data?.user_id;
  if (id == null) {
    throw new Error(`user "${name}" not found: ${JSON.stringify(j).slice(0, 160)}`);
  }
  return id;
}

async function kickSendChat(text) {
  const broadcaster = await kickBroadcasterId();
  const j = await kickApi('/chat', {
    method: 'POST',
    body: JSON.stringify({ broadcaster_user_id: broadcaster, content: String(text).slice(0, 500), type: 'user' }),
  });
  return j?.data?.message_id;
}

async function kickMod(action, id, user) {
  const broadcaster = await kickBroadcasterId();
  if (action === 'delete') {
    await kickApi(`/chat/${encodeURIComponent(id)}`, { method: 'DELETE' });
    return;
  }
  if (action !== 'ban' && action !== 'timeout') {
    throw new Error(`unknown mod action ${action}`);
  }
  const user_id = await kickUserIdByName(user);
  await kickApi('/moderation/bans', {
    method: 'POST',
    body: JSON.stringify({
      broadcaster_user_id: broadcaster,
      user_id,
      ...(action === 'timeout' ? { duration: KICK_TIMEOUT_MIN } : {}),
    }),
  });
}

/* --- Kick live chat READ (site history polling; unofficial, read-only) -- *
 * Kick's official event transport is webhook-only and demands a public
 * URL — the wrong shape for a local-first box. The realtime socket
 * (websockets.kick.com, Centrifugo) sits behind Cloudflare TLS
 * fingerprinting that rejects non-browser clients, and the old Pusher
 * socket no longer accepts site keys. The site's own chat history
 * endpoint GET /api/v2/channels/<id>/messages is public and pollable
 * (curl.exe passes the fingerprint check; Node's fetch does not), so the
 * relay polls every couple of seconds and rebroadcasts new messages to
 * the device in the exact frame shape the simulator uses — including
 * the sender's own messages, which paint under the real username exactly
 * like Kick's own chat does.
 * [emote:ID:Name] tokens become :Name: text tokens; image tiles ride the
 * existing emote.req path. */
const { execFile } = require('child_process');

const KICK_POLL_MS = 2500;
const KICK_POLL_BACKOFF_MS = 10000;
let kickChatLive = false;
let kickChatStarting = false;
const kickSeen = new Set();     /* message ids already handled (bounded) */

function kickNormalizeText(content) {
  return String(content).replace(/\[emote:\d+:([^\]]+)\]/g, ':$1:');
}

async function kickChannelSlug() {
  const j = await kickApi('/users');
  const u = j?.data?.[0] ?? j?.data ?? {};
  const slug = u.slug ?? u.name ?? u.username ?? u.user_name;
  if (!slug) {
    throw new Error(`slug not found in /users: ${JSON.stringify(j).slice(0, 160)}`);
  }
  return slug;
}

function kickChannelInfo(slug) {
  /* One curl -> channel id + chatroom id. Kick's Cloudflare blocks
     non-browser TLS fingerprints; curl.exe ships with Windows 10+ so no
     path gymnastics needed. */
  return new Promise((resolve, reject) => {
    execFile('curl.exe', ['-sS', '-m', '15', `https://kick.com/api/v2/channels/${encodeURIComponent(slug)}`],
             (err, stdout) => {
               if (err) return reject(err);
               try {
                 const j = JSON.parse(stdout);
                 const channelId = j?.id;
                 const roomId = j?.chatroom?.id;
                 if (channelId == null || roomId == null) {
                   throw new Error(`no ids in response: ${stdout.slice(0, 120)}`);
                 }
                 resolve({ channelId, roomId });
               } catch (e) {
                 reject(e);
               }
             });
  });
}

function kickChatStart() {
  if (kickChatLive || kickChatStarting) return;
  kickChatStarting = true;
  (async () => {
    const slug = await kickChannelSlug();
    const { channelId, roomId } = await kickChannelInfo(slug);
    kickChatStarting = false;
    kickChatLive = false;
    kickSeen.clear();
    kickChatPoll(channelId, roomId, slug);
  })().catch((err) => {
    kickChatStarting = false;
    console.log(`[relay] kick chat read unavailable: ${err.message}`);
  });
}

function kickChatPoll(channelId, roomId, slug) {
  execFile('curl.exe', ['-sS', '-m', '15',
                        `https://kick.com/api/v2/channels/${channelId}/messages`],
           (err, stdout) => {
    let delay = KICK_POLL_MS;
    if (err) {
      if (kickChatLive) {
        console.log(`[relay] kick chat poll failed: ${err.message}`);
      }
      kickChatLive = false;
      delay = KICK_POLL_BACKOFF_MS;
    } else {
      try {
        const msgs = JSON.parse(stdout)?.data?.messages ?? [];
        if (!kickChatLive) {
          /* first successful poll: seed the seen set so history is not
             replayed onto the glass; go live from this point on */
          for (const m of msgs) kickSeen.add(String(m.id));
          kickChatLive = true;
          console.log(`[relay] kick chat read live (@${slug}, room ${roomId})`);
        } else {
          const fresh = msgs
            .filter((m) => m && m.id != null && !kickSeen.has(String(m.id)))
            .sort((a, b) => String(a.created_at).localeCompare(String(b.created_at)));
          for (const m of fresh) {
            kickSeen.add(String(m.id));
            if (m.type !== 'message') continue;
            /* own sends ride back through the poll and SHOULD paint on
               the glass under the sender's real name, like Kick's own
               chat does — suppressing them made glass-typed messages
               vanish (sim bounce is gated off when live, so nothing
               else rendered them; owner-reported 2026-10-27) */
            const text = kickNormalizeText(m.content).slice(0, 300);
            broadcast({ type: 'chat', user: (m.sender?.username ?? '?').slice(0, 24),
                        id: `kick-${m.id}`.slice(0, 40),
                        emotes: scanEmotes(text), text, ts: Date.now() });
          }
          if (kickSeen.size > 600) {
            /* bound the set: rebuild from the just-fetched window; older
               ids are history and can never arrive as "new" again */
            kickSeen.clear();
            for (const m of msgs) kickSeen.add(String(m.id));
          }
        }
      } catch (e) {
        if (kickChatLive) {
          console.log(`[relay] kick chat poll parse: ${e.message}`);
        }
        kickChatLive = false;
        delay = KICK_POLL_BACKOFF_MS;
      }
    }
    setTimeout(() => kickChatPoll(channelId, roomId, slug), delay);
  });
}

/* Bench emote set. Device-side protocol carries emotes as raw RGB565
   (24x24) base64 in {"type":"emote","code","data"} frames — the relay owns
   all image conversion. With Kick wired, the relay would fetch CDN PNGs,
   decode them (pngjs), and repack to RGB565; the simulator synthesizes
   deterministic tiles instead so no npm deps are needed on the bench. */
const EMOTE_CODES = ['kick', 'hype', 'gg'];

/* Codes present in text as :code: tokens, in order, max 3 unique. */
function scanEmotes(text) {
  const found = [];
  for (const code of EMOTE_CODES) {
    if (text.includes(`:${code}:`) && !found.includes(code)) {
      found.push(code);
    }
  }
  return found.join(',');
}

function synthEmote(code) {
  const W = 24, H = 24;
  const buf = Buffer.alloc(W * H * 2);
  let h = 5381;
  for (const c of code) {
    h = ((h * 33) ^ c.charCodeAt(0)) & 0xffff;
  }
  const fill = 0x0400 | ((h & 0x1f) << 11) | ((h & 0x7) << 8); /* dark, hashed */
  const border = 0x07e0;   /* pure green frame */
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const v = (x === 0 || y === 0 || x === W - 1 || y === H - 1) ? border : fill;
      buf.writeUInt16LE(v, (y * W + x) * 2);
    }
  }
  return buf;
}

/* --- Soundboard: PC speakers over the relay --------------------------- *
 * The device page carries no audio decode path; a tap sends
 * {"type":"sfx.play","id"} and the relay plays the matching file from
 * relay/sounds/ on the PC's speakers. The catalog rides
 * {"type":"sfx.catalog","sounds":"id:Name,..."} — comma/colon pairs, not
 * a JSON array, so the device parser stays string-only. Names are sliced
 * to 12 chars so a full 12-sound catalog stays under the device's
 * 512-byte small-frame limit. Sound files are personal content:
 * relay/sounds/ is gitignored; drop mp3/wav/ogg/m4a files in and they
 * appear on the glass automatically. */

const SFX_DIR = path.join(__dirname, 'sounds');
const SFX_EXT = ['.mp3', '.wav', '.ogg', '.m4a'];

function sfxList() {
  try {
    return fs.readdirSync(SFX_DIR)
      .filter((f) => SFX_EXT.includes(path.extname(f).toLowerCase()))
      .map((f) => path.basename(f, path.extname(f)))
      .sort((a, b) => a.toLowerCase().localeCompare(b.toLowerCase()));
  } catch {
    return [];
  }
}

/* The wire is comma/colon delimited — those characters can't ride it. */
function sfxWire(s, max) {
  return String(s).replace(/[,:|]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, max);
}

function sfxCatalogFrame() {
  const sounds = sfxList().slice(0, 12)
    .map((id) => `${sfxWire(id, 20)}:${sfxWire(id, 12)}`);
  return JSON.stringify({ type: 'sfx.catalog', sounds: sounds.join(',') });
}

function sfxPlay(id) {
  const file = sfxFindFile(id);
  if (!file) {
    console.log(`[relay] sfx.play '${id}' — no matching file in sounds/`);
    return;
  }
  const wav = path.extname(file).toLowerCase() === '.wav';
  const args = ['-NoProfile', '-NonInteractive', '-WindowStyle', 'Hidden'];
  if (wav) {
    const q = file.replace(/'/g, "''");
    args.push('-Command', `(New-Object System.Media.SoundPlayer '${q}').PlaySync()`);
  } else {
    /* WMPlayer can sit at buffering=100 forever for these local MP3s.
       MCI uses the same built-in Windows codecs and reports real playback
       progress, so the helper stays alive only until the track stops. */
    args.push('-File', path.join(__dirname, 'play_sfx.ps1'), '-Path', file);
  }
  execFile('powershell.exe', args,
           (err) => {
             if (err) {
               console.log(`[relay] sfx.play '${path.basename(file)}' failed: ${err.message}`);
             } else {
               console.log(`[relay] sfx.play '${path.basename(file)}'`);
             }
           });
}

/* --- SFX audio delivery: stream the mp3 to the device speaker -------- *
 * The device asks with {"type":"sfx.audio.req","id"} right after its
 * sfx.play; the relay answers with a paced run of
 * {"type":"sfx.data","seq","total","end","data"} frames — base64 chunks
 * sized to stay inside the device's 4 KB big-frame reassembly window.
 * 640 KB caps a track at roughly 40 s at typical effect bitrates. */

const SFX_CHUNK_RAW = 2900;   /* ~3.9 KB base64 per frame, under the 4 KB limit */
const SFX_CHUNK_GAP_MS = 70;  /* pacing: the device appends on the ws task */

function sfxFindFile(id) {
  const safe = sfxWire(id, 20).toLowerCase();
  if (!safe) return null;
  try {
    const hit = fs.readdirSync(SFX_DIR)
      .find((f) => sfxWire(path.basename(f, path.extname(f)), 20).toLowerCase() === safe);
    return hit ? path.join(SFX_DIR, hit) : null;
  } catch {
    return null;
  }
}

function sfxStreamAudio(id, ws) {
  const file = sfxFindFile(id);
  if (!file) {
    console.log(`[relay] sfx.audio.req '${id}' — no matching file in sounds/`);
    return;
  }
  let buf;
  try {
    buf = fs.readFileSync(file);
  } catch (err) {
    console.log(`[relay] sfx.audio.req read failed: ${err.message}`);
    return;
  }
  if (buf.length > 640 * 1024) {
    console.log(`[relay] sfx.audio.req '${path.basename(file)}' too big ` +
                `(${buf.length} bytes > 640 KB) — device stream skipped`);
    return;
  }
  const name = path.basename(file);
  console.log(`[relay] sfx.audio.req '${name}' — streaming ${buf.length} bytes ` +
              `in ${Math.ceil(buf.length / SFX_CHUNK_RAW)} chunks`);
  for (let seq = 0, off = 0; off < buf.length || seq === 0; seq++) {
    const chunk = buf.subarray(off, off + SFX_CHUNK_RAW);
    const end = off + chunk.length >= buf.length ? 1 : 0;
    const frame = JSON.stringify({
      type: 'sfx.data', id: sfxWire(id, 20), seq,
      total: buf.length, end,
      data: chunk.toString('base64'),
    });
    /* capture seq/off NOW — the setTimeout callback must not see the
       loop variables of a later iteration */
    const send = () => {
      if (ws.readyState === ws.OPEN) {
        ws.send(frame);
      }
    };
    if (end) {
      setTimeout(send, seq * SFX_CHUNK_GAP_MS);
      break;
    }
    setTimeout(send, seq * SFX_CHUNK_GAP_MS);
    off += chunk.length;
  }
}

/* --- discovery beacon (D-15: zero IP typing) ------------------------- *
 * UDP broadcast, not mDNS: port 5353 is often held by other apps on a
 * streaming PC (dev tools, Bonjour, Chrome), which blocks rebinding it.
 * A tiny beacon needs no inbound firewall hole on this side: the PC only
 * SENDS; the device only LISTENS on UDP 4210 and learns the relay's IP
 * from the packet source. (ROADMAP sanctions "mDNS/broadcast".) */
const BEACON_PORT = 4210;
const beacon = dgram.createSocket('udp4');
beacon.on('error', (err) => {
  console.log(`[relay] discovery beacon unavailable: ${err.message} — device falls back to saved endpoint`);
});
beacon.bind(() => {
  beacon.setBroadcast(true);
  const msg = Buffer.from(`KAST_RELAY_V1 ${PORT}`);
  const shout = () => {
    beacon.send(msg, BEACON_PORT, '255.255.255.255', (err) => {
      if (err && err.code !== 'EMSGSIZE') {
        console.log(`[relay] beacon send failed: ${err.message}`);
      }
    });
  };
  shout();
  setInterval(shout, 2000);
  console.log(`[relay] discovery beacon on UDP ${BEACON_PORT} ("KAST_RELAY_V1 ${PORT}")`);
});

const wss = new WebSocketServer({ port: PORT });
const clients = new Set();

function broadcast(obj) {
  const line = JSON.stringify(obj);
  for (const ws of clients) {
    if (ws.readyState === ws.OPEN) {
      ws.send(line);
    }
  }
}

wss.on('connection', (ws, req) => {
  clients.add(ws);
  console.log(`[relay] device connected (${req.socket.remoteAddress}), ${clients.size} client(s)`);
  if (ws.readyState === ws.OPEN) {
    ws.send(JSON.stringify({ type: 'hello', app: 'kast-relay', version: '0.1.0' }));
    ws.send(sfxCatalogFrame());
  }

  ws.on('message', (data) => {
    let msg;
    try {
      msg = JSON.parse(data.toString());
    } catch {
      console.log(`[relay] non-JSON frame dropped: ${data.toString().slice(0, 80)}`);
      return;
    }
    switch (msg.type) {
      case 'ping':
        ws.send(JSON.stringify({ type: 'pong', t: Date.now() }));
        break;
      case 'dev.hello':
        /* Device boot telemetry: the previous reset reason survives in NVS. */
        console.log(`[relay] device boot: reset=${msg.reset}`);
        break;
      case 'chat.send':
        if (kickReady()) {
          kickSendChat(msg.text).then((mid) => {
            console.log(`[relay] kick chat.send ok (message_id ${mid})`);
          }).catch((err) => {
            console.log(`[relay] kick chat.send failed: ${err.message}`);
          });
        } else {
          console.log(`[relay] chat.send (Kick not authorized — open the auth URL): ${msg.text}`);
        }
        if (SIMULATE && !kickChatLive) {
          /* bench closed loop: bounce the sent line back as chat so the
             device's full render path is visible even with Kick off.
             Suppressed once real chat read is live — the genuine echo
             arrives through the history poll with the real username */
          const text = String(msg.text).slice(0, 300);
          broadcast({ type: 'chat', user: 'you', id: `sim-${++sim_id}`,
                      emotes: scanEmotes(text),
                      text, ts: Date.now() });
        }
        break;
      case 'sfx.play':
        /* device tapped a soundboard tile */
        sfxPlay(msg.id);
        break;
      case 'sfx.audio.req':
        /* device wants the mp3 for its own speaker (monitor path) */
        sfxStreamAudio(msg.id, ws);
        break;
      case 'sfx.catalog':
        /* device re-requests (e.g. after new files land on the PC) */
        ws.send(sfxCatalogFrame());
        break;
      case 'emote.req':
        /* device wants an emote tile — synthesize in simulate; with Kick
           wired this fetches + converts the CDN image instead */
        console.log(`[relay] emote.req ${msg.code}`);
        if (EMOTE_CODES.includes(msg.code)) {
          ws.send(JSON.stringify({ type: 'emote', code: msg.code,
                                   data: synthEmote(msg.code).toString('base64') }));
        }
        break;
      case 'chat.mod':
        console.log(`[relay] chat.mod ${msg.action} id=${msg.id} user=${msg.user}`);
        if (kickReady()) {
          kickMod(String(msg.action), String(msg.id || ''), String(msg.user || '')).then(() => {
            broadcast({ type: 'mod.ack', action: String(msg.action).slice(0, 16),
                        id: String(msg.id || ''), ts: Date.now() });
          }).catch((err) => {
            console.log(`[relay] kick chat.mod failed: ${err.message}`);
          });
        } else if (SIMULATE) {
          /* bench loop: acknowledge so the device can show the confirm */
          broadcast({ type: 'mod.ack', action: String(msg.action).slice(0, 16),
                      id: String(msg.id || ''), ts: Date.now() });
        }
        break;
      default:
        console.log(`[relay] rx: ${data.toString().slice(0, 120)}`);
    }
  });

  ws.on('close', () => {
    clients.delete(ws);
    console.log(`[relay] device disconnected, ${clients.size} client(s)`);
  });
  ws.on('error', (err) => console.log(`[relay] socket error: ${err.message}`));
});

if (SIMULATE) {
  const names = ['benchbot', 'glass_check', 'night_owl', 'streamfan'];
  const lines = [
    'relay link is :gg: alive',
    'typing :kick: from the bench test',
    'the :kick: towers are :hype: spinning',
    'if you can read this, the path works :hype:',
  ];
  setInterval(() => {
    if (clients.size === 0 || kickChatLive) {
      /* real chat read replaces the fake feed — don't interleave */
      return;
    }
    const text = lines[Math.floor(Math.random() * lines.length)];
    broadcast({
      type: 'chat',
      user: names[Math.floor(Math.random() * names.length)],
      id: `sim-${++sim_id}`,
      emotes: scanEmotes(text),
      text,
      ts: Date.now(),
    });
  }, 5000);
  console.log('[relay] chat simulator on (5s interval)');
}

console.log(`[relay] KAST relay listening on ws://0.0.0.0:${PORT}`);
if (KICK_CLIENT_ID && !kickReady()) {
  console.log(`[relay] Kick: authorize once by opening this URL in a browser:`);
  console.log(`[relay]   ${kickAuthUrl()}`);
  console.log('[relay] (approving it saves tokens; relay restarts stay authorized)');
} else if (kickReady()) {
  console.log('[relay] Kick authorized — chat send + moderation are live');
  kickChatStart();
}
process.on('SIGINT', () => {
  console.log('\n[relay] bye');
  process.exit(0);
});
