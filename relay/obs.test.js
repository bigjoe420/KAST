'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { WebSocketServer } = require('ws');
const { ObsClient } = require('./obs');

async function fakeObs({ password = '', onRequest } = {}) {
  const wss = new WebSocketServer({ port: 0, host: '127.0.0.1' });
  await new Promise((resolve) => wss.once('listening', resolve));
  const sockets = new Set();
  const requests = [];
  let connections = 0;
  wss.on('connection', (ws) => {
    connections++;
    sockets.add(ws);
    ws.on('close', () => sockets.delete(ws));
    ws.send(JSON.stringify({ op: 0, d: {
      obsStudioVersion: 'test', obsWebSocketVersion: '5.0.0', rpcVersion: 1,
      ...(password ? { authentication: { challenge: 'challenge', salt: 'salt' } } : {}),
    } }));
    ws.on('message', (raw) => {
      const msg = JSON.parse(raw.toString());
      if (msg.op === 1) {
        const crypto = require('crypto');
        const secret = crypto.createHash('sha256').update(password + 'salt').digest('base64');
        const expected = crypto.createHash('sha256').update(secret + 'challenge').digest('base64');
        if (password && msg.d.authentication !== expected) {
          ws.close(4009, 'bad password');
        } else {
          ws.send(JSON.stringify({ op: 2, d: { negotiatedRpcVersion: 1 } }));
        }
      } else if (msg.op === 6) {
        requests.push(msg.d);
        if (onRequest) {
          onRequest(msg.d, ws);
        } else {
          ws.send(JSON.stringify({ op: 7, d: {
            requestType: msg.d.requestType, requestId: msg.d.requestId,
            requestStatus: { result: true, code: 100 }, responseData: { ok: true },
          } }));
        }
      }
    });
  });
  return {
    url: `ws://127.0.0.1:${wss.address().port}`,
    requests,
    connections: () => connections,
    drop: () => { for (const ws of sockets) ws.terminate(); },
    close: () => new Promise((resolve) => {
      for (const ws of sockets) ws.terminate();
      wss.close(resolve);
    }),
  };
}

test('authenticates and resolves a request response', async (t) => {
  const obs = await fakeObs({ password: 'private' });
  t.after(() => obs.close());
  const client = new ObsClient({ url: obs.url, password: 'private', reconnect: false });
  t.after(() => client.close());
  await client.connect();
  assert.deepEqual(await client.request('GetVersion'), { ok: true });
  assert.equal(obs.requests[0].requestType, 'GetVersion');
});

test('reports bad credentials without reconnecting', async (t) => {
  const obs = await fakeObs({ password: 'right' });
  t.after(() => obs.close());
  const client = new ObsClient({ url: obs.url, password: 'wrong', reconnectDelayMs: 5 });
  t.after(() => client.close());
  const states = [];
  client.on('state', (state) => states.push(state));
  await assert.rejects(client.connect(), /authentication failed/i);
  await new Promise((resolve) => setTimeout(resolve, 30));
  assert.equal(client.state, 'auth_failed');
  assert.equal(states.filter((s) => s.status === 'connecting').length, 1);
});

test('rejects a request that exceeds its timeout', async (t) => {
  const obs = await fakeObs({ onRequest: () => {} });
  t.after(() => obs.close());
  const client = new ObsClient({ url: obs.url, requestTimeoutMs: 20, reconnect: false });
  t.after(() => client.close());
  await client.connect();
  await assert.rejects(client.request('GetVersion'), /timed out/i);
});

test('rejects a failed OBS request with its status comment', async (t) => {
  const obs = await fakeObs({ onRequest: (request, ws) => {
    ws.send(JSON.stringify({ op: 7, d: {
      requestType: request.requestType, requestId: request.requestId,
      requestStatus: { result: false, code: 600, comment: 'not found' },
    } }));
  } });
  t.after(() => obs.close());
  const client = new ObsClient({ url: obs.url, reconnect: false });
  t.after(() => client.close());
  await client.connect();
  await assert.rejects(client.request('RemoveScene'), (err) => {
    assert.equal(err.code, 600);
    assert.match(err.message, /not found/);
    return true;
  });
});

test('reconnects automatically after an established session drops', async (t) => {
  const obs = await fakeObs();
  t.after(() => obs.close());
  const client = new ObsClient({ url: obs.url, reconnectDelayMs: 10 });
  t.after(() => client.close());
  await client.start();
  await new Promise((resolve) => setTimeout(resolve, 20));
  assert.equal(obs.connections(), 1);
  obs.drop();
  await new Promise((resolve) => setTimeout(resolve, 80));
  assert.equal(client.state, 'connected');
  assert.equal(obs.connections(), 2);
});
