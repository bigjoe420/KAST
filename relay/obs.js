'use strict';

const crypto = require('crypto');
const { EventEmitter } = require('events');
const WebSocket = require('ws');

function authentication(password, challenge, salt) {
  const secret = crypto.createHash('sha256').update(password + salt).digest('base64');
  return crypto.createHash('sha256').update(secret + challenge).digest('base64');
}

class ObsClient extends EventEmitter {
  constructor({ url = 'ws://127.0.0.1:4455', password = '', requestTimeoutMs = 5000,
                reconnect = true, reconnectDelayMs = 3000 } = {}) {
    super();
    this.url = url;
    this.password = password;
    this.requestTimeoutMs = requestTimeoutMs;
    this.reconnect = reconnect;
    this.reconnectDelayMs = reconnectDelayMs;
    this.state = 'disconnected';
    this.detail = '';
    this.ws = null;
    this.pending = new Map();
    this.connectPromise = null;
    this.connectReject = null;
    this.retryTimer = null;
    this.helloTimer = null;
    this.auto = false;
    this.closing = false;
    this.blockReconnect = false;
  }

  setState(state, detail = '') {
    if (state === this.state && detail === this.detail) return;
    this.state = state;
    this.detail = detail;
    this.emit('state', { status: state, detail });
  }

  connect() {
    if (this.state === 'connected') return Promise.resolve();
    if (this.connectPromise) return this.connectPromise;
    if (this.state === 'auth_failed' || this.state === 'invalidated') {
      return Promise.reject(new Error(this.detail));
    }
    this.closing = false;
    this.blockReconnect = false;
    this.setState('connecting');
    this.connectPromise = new Promise((resolve, reject) => {
      this.connectReject = reject;
      const ws = new WebSocket(this.url);
      this.ws = ws;
      this.helloTimer = setTimeout(() => {
        ws.terminate();
        reject(new Error('OBS hello timed out'));
        this.connectPromise = null;
      }, this.requestTimeoutMs);
      ws.once('open', () => this.setState('identifying'));
      ws.on('message', (raw) => {
        let msg;
        try { msg = JSON.parse(raw.toString()); } catch { return; }
        if (msg.op === 0) {
          clearTimeout(this.helloTimer);
          const auth = msg.d && msg.d.authentication;
          if (auth && !this.password) {
            ws.close();
            reject(new Error('OBS requires a password'));
            this.connectPromise = null;
            return;
          }
          ws.send(JSON.stringify({ op: 1, d: {
            rpcVersion: 1, eventSubscriptions: 0,
            ...(auth ? { authentication: authentication(this.password, auth.challenge, auth.salt) } : {}),
          } }));
        } else if (msg.op === 2) {
          this.setState('connected');
          const done = this.connectResolve;
          this.connectPromise = null;
          this.connectReject = null;
          if (done) done();
          resolve();
        } else if (msg.op === 7) {
          const pending = this.pending.get(msg.d.requestId);
          if (!pending) return;
          this.pending.delete(msg.d.requestId);
          clearTimeout(pending.timer);
          if (msg.d.requestStatus && msg.d.requestStatus.result) {
            pending.resolve(msg.d.responseData || {});
          } else {
            const status = msg.d.requestStatus || {};
            const err = new Error(`OBS ${pending.type} failed (${status.code || 'unknown'}): ${status.comment || 'request rejected'}`);
            err.code = status.code;
            pending.reject(err);
          }
        }
      });
      ws.on('error', (err) => {
        if (this.connectPromise) this.setState('connecting', err.message);
      });
      ws.on('close', (code, reason) => {
        clearTimeout(this.helloTimer);
        this.ws = null;
        const text = reason.toString();
        for (const [id, pending] of this.pending) {
          clearTimeout(pending.timer);
          pending.reject(new Error(`OBS disconnected during ${pending.type}`));
          this.pending.delete(id);
        }
        if (this.connectReject) {
          let err;
          if (code === 4009) {
            this.blockReconnect = true;
            this.setState('auth_failed', 'OBS authentication failed; check the password');
            err = new Error('OBS authentication failed; check the password');
          } else if (code === 4011) {
            this.blockReconnect = true;
            this.setState('invalidated', 'OBS invalidated this session; reconnect manually');
            err = new Error(this.detail);
          } else {
            this.setState('disconnected', text || `OBS socket closed (${code})`);
            err = new Error(this.detail);
          }
          this.connectReject = null;
          this.connectPromise = null;
          reject(err);
        } else if (!this.closing) {
          this.setState('disconnected', text || `OBS socket closed (${code})`);
        }
        if (this.auto && !this.closing && !this.blockReconnect) this.scheduleReconnect();
      });
    });
    return this.connectPromise;
  }

  start() {
    this.auto = true;
    return this.connect().catch(() => {});
  }

  scheduleReconnect() {
    if (this.retryTimer || this.closing || this.blockReconnect) return;
    this.retryTimer = setTimeout(() => {
      this.retryTimer = null;
      this.connect().catch(() => {});
    }, this.reconnectDelayMs);
  }

  request(requestType, requestData = {}) {
    if (this.state !== 'connected' || !this.ws) {
      return Promise.reject(new Error(`OBS is not connected (${this.detail || this.state})`));
    }
    const requestId = crypto.randomUUID();
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error(`OBS ${requestType} timed out after ${this.requestTimeoutMs} ms`));
      }, this.requestTimeoutMs);
      this.pending.set(requestId, { resolve, reject, timer, type: requestType });
      this.ws.send(JSON.stringify({ op: 6, d: { requestType, requestId, requestData } }));
    });
  }

  close() {
    this.closing = true;
    this.auto = false;
    clearTimeout(this.retryTimer);
    clearTimeout(this.helloTimer);
    for (const [id, pending] of this.pending) {
      clearTimeout(pending.timer);
      pending.reject(new Error('OBS client closed'));
      this.pending.delete(id);
    }
    if (this.ws) this.ws.close();
    this.ws = null;
    this.setState('disconnected');
  }
}

module.exports = { ObsClient, authentication };
