'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const { ObsGfx, sourceName } = require('./obs_gfx');

const capabilities = [
  'GetVersion', 'GetSceneList', 'CreateScene', 'GetInputList', 'CreateInput',
  'GetCurrentProgramScene', 'GetSceneItemId', 'CreateSceneItem',
  'SetSceneItemEnabled', 'TriggerMediaInputAction', 'RemoveInput', 'RemoveScene',
];

class FakeObs {
  constructor() {
    this.scenes = ['Main', 'Other'];
    this.current = 'Main';
    this.inputs = [];
    this.links = new Set();
    this.itemIds = new Map();     /* "KAST GFX/<source>" -> id */
    this.enabled = new Map();     /* itemId -> bool */
    this.restarts = [];
    this.calls = [];
    this.nextId = 1;
  }

  async request(type, data = {}) {
    this.calls.push({ type, data });
    if (type === 'GetVersion') return { availableRequests: capabilities };
    if (type === 'GetSceneList') return { scenes: this.scenes.map((sceneName) => ({ sceneName })) };
    if (type === 'CreateScene') { this.scenes.push(data.sceneName); return {}; }
    if (type === 'GetInputList') return { inputs: this.inputs.map((inputName) => ({ inputName })) };
    if (type === 'CreateInput') {
      this.inputs.push(data.inputName);
      const id = this.nextId++;
      this.itemIds.set(`${data.sceneName}/${data.inputName}`, id);
      this.enabled.set(id, data.sceneItemEnabled !== false);
      return {};
    }
    if (type === 'GetCurrentProgramScene') return { sceneName: this.current };
    if (type === 'GetSceneItemId') {
      const key = data.sceneName === undefined ? null : `${data.sceneName}/${data.sourceName}`;
      /* nesting links (source is a scene) and item ids share the map */
      if (this.itemIds.has(key)) return { sceneItemId: this.itemIds.get(key) };
      if (this.links.has(key)) return { sceneItemId: 999 };
      const err = new Error('not found'); err.code = 600; throw err;
    }
    if (type === 'CreateSceneItem') {
      this.links.add(`${data.sceneName}/${data.sourceName}`);
      return { sceneItemId: 999 };
    }
    if (type === 'SetSceneItemEnabled') { this.enabled.set(data.sceneItemId, data.sceneItemEnabled); return {}; }
    if (type === 'TriggerMediaInputAction') { this.restarts.push(data.inputName); return {}; }
    if (type === 'RemoveInput') {
      this.inputs = this.inputs.filter((x) => x !== data.inputName);
      return {};
    }
    if (type === 'RemoveScene') { this.scenes = this.scenes.filter((x) => x !== data.sceneName); return {}; }
    return {};
  }

  itemIdOf(source) { return this.itemIds.get(`KAST GFX/${source}`); }
  isEnabled(source) { return this.enabled.get(this.itemIdOf(source)); }
}

const graphics = [
  { id: 'gg pop', file: 'C:\\media\\gg pop.webm' },
  { id: 'alert buzz', file: 'C:\\media\\alert.gif' },
];

function makeGfx(client, opts = {}) {
  return new ObsGfx({
    client,
    listGraphics: () => graphics,
    bindingFor: () => ({ sound: null, durationMs: 60 }),
    logger: { log() {} },
    ...opts,
  });
}

test('setup creates only managed items, hidden, in the KAST GFX scene', async () => {
  const client = new FakeObs();
  const gfx = makeGfx(client);
  await gfx.setup();
  assert.deepEqual(client.scenes, ['Main', 'Other', 'KAST GFX']);
  assert.deepEqual(client.inputs, ['KAST GFX - gg pop', 'KAST GFX - alert buzz']);
  const creates = client.calls.filter((c) => c.type === 'CreateInput');
  assert.equal(creates.length, 2);
  assert.equal(creates[0].data.inputSettings.local_file, graphics[0].file);
  assert.equal(creates[0].data.sceneItemEnabled, false);
  assert(client.links.has('Main/KAST GFX'));
});

test('play reveals, restarts, and hides the graphic after its duration', async () => {
  const client = new FakeObs();
  const gfx = makeGfx(client);
  await gfx.play('gg pop');
  assert.equal(client.isEnabled(sourceName('gg pop')), true);
  assert.deepEqual(client.restarts, [sourceName('gg pop')]);
  await new Promise((resolve) => setTimeout(resolve, 120));
  assert.equal(client.isEnabled(sourceName('gg pop')), false);
});

test('overlay follows the user to a new active scene', async () => {
  const client = new FakeObs();
  const gfx = makeGfx(client);
  await gfx.play('gg pop');
  client.current = 'Other';
  await gfx.linkActiveScene();
  assert(client.links.has('Other/KAST GFX'));
});

test('repeated taps reset the hide timer and never duplicate sources', async () => {
  const client = new FakeObs();
  const gfx = makeGfx(client);
  await gfx.play('gg pop');
  await new Promise((resolve) => setTimeout(resolve, 40));
  await gfx.play('gg pop');
  assert.deepEqual(client.inputs, ['KAST GFX - gg pop', 'KAST GFX - alert buzz']);
  await new Promise((resolve) => setTimeout(resolve, 120));
  assert.equal(client.isEnabled(sourceName('gg pop')), false);
  assert.deepEqual(client.restarts, [sourceName('gg pop'), sourceName('gg pop')]);
});

test('playing an unknown graphic fails without calling OBS', async () => {
  const client = new FakeObs();
  const gfx = makeGfx(client);
  await assert.rejects(gfx.play('missing'), /not in the graphics catalog/);
  assert.equal(client.calls.length, 0);
});

test('setup reconciles stale managed sources without touching user inputs', async () => {
  const client = new FakeObs();
  client.inputs = ['KAST GFX - old clip', 'My Overlay'];
  const gfx = makeGfx(client);
  await gfx.setup();
  assert.deepEqual(client.inputs, ['My Overlay', 'KAST GFX - gg pop', 'KAST GFX - alert buzz']);
});

test('cleanup removes only exact KAST-owned graphics items and scene', async () => {
  const client = new FakeObs();
  client.inputs = ['KAST GFX - gg pop', 'KAST GFX backup', 'My Overlay'];
  const gfx = makeGfx(client);
  await gfx.remove();
  assert.deepEqual(client.inputs, ['KAST GFX backup', 'My Overlay']);
  assert.deepEqual(client.scenes, ['Main', 'Other']);
});

test('release detaches only the selected KAST graphic before a file change', async () => {
  const client = new FakeObs();
  const gfx = makeGfx(client);
  await gfx.setup();
  await gfx.release('gg pop');
  assert.deepEqual(client.inputs, ['KAST GFX - alert buzz']);
  await gfx.release('gg pop');   /* idempotent */
  assert.deepEqual(client.inputs, ['KAST GFX - alert buzz']);
});

test('missing required OBS capabilities produce a clear setup error', async () => {
  const client = new FakeObs();
  const original = client.request.bind(client);
  client.request = async (type, data) => {
    if (type === 'GetVersion') return { availableRequests: ['GetVersion'] };
    return original(type, data);
  };
  const gfx = makeGfx(client);
  await assert.rejects(gfx.setup(), /does not support required OBS requests/);
  assert(!client.scenes.includes('KAST GFX'));
});

test('source names are bounded and prefixed', () => {
  assert.equal(sourceName('gg pop'), 'KAST GFX - gg pop');
  assert(sourceName('x'.repeat(100)).length <= 64);
});
