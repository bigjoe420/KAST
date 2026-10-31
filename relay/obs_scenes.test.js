'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { ObsScenes, isUserScene, wireName } = require('./obs_scenes');

class FakeObs {
  constructor(scenes = ['Main', 'Just Chatting', 'BRB', 'KAST SFX', 'KAST GFX']) {
    this.scenes = scenes;
    this.current = 'Main';
    this.calls = [];
  }

  async request(type, data = {}) {
    this.calls.push({ type, data });
    if (type === 'GetSceneList') return { scenes: this.scenes.map((sceneName) => ({ sceneName })) };
    if (type === 'GetCurrentProgramScene') return { sceneName: this.current };
    if (type === 'SetCurrentProgramScene') {
      if (!this.scenes.includes(data.sceneName)) {
        const err = new Error('no such scene'); err.code = 600; throw err;
      }
      this.current = data.sceneName;
      return {};
    }
    return {};
  }
}

function tempSelection() {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), 'kast-scenes-'));
  return path.join(dir, 'scene_tiles.json');
}

test('user-scene filter excludes every KAST-managed name', () => {
  assert.equal(isUserScene('Main'), true);
  assert.equal(isUserScene('KAST SFX'), false);
  assert.equal(isUserScene('KAST GFX'), false);
  assert.equal(isUserScene('KAST SFX - ding'), false);
  assert.equal(isUserScene('KAST GFX - gg pop'), false);
});

test('catalog holds selected scenes that still exist, with the live scene', async () => {
  const selectionPath = tempSelection();
  fs.writeFileSync(selectionPath, JSON.stringify(['BRB', 'Main', 'Gone']));
  const scenes = new ObsScenes({ client: new FakeObs(), selectionPath });
  const catalog = await scenes.catalog();
  assert.deepEqual(catalog.scenes, ['BRB', 'Main']);
  assert.equal(catalog.current, 'Main');
});

test('missing or corrupt selection file means no tiles, never a crash', async () => {
  const selectionPath = tempSelection();
  fs.writeFileSync(selectionPath, '{not json');
  const scenes = new ObsScenes({ client: new FakeObs(), selectionPath });
  assert.deepEqual((await scenes.catalog()).scenes, []);
  fs.unlinkSync(selectionPath);
  assert.deepEqual((await scenes.catalog()).scenes, []);
});

test('writeSelection bounds tiles and strips managed names', () => {
  const scenes = new ObsScenes({ client: null, selectionPath: tempSelection() });
  const many = Array.from({ length: 20 }, (_, i) => `Scene ${i}`);
  const saved = scenes.writeSelection(['KAST SFX', ...many]);
  assert.equal(saved.length, 12);
  assert(!saved.includes('KAST SFX'));
});

test('switchTo changes only the program scene, by wire or real name', async () => {
  const client = new FakeObs(['My, Scene', 'Main']);
  const selectionPath = tempSelection();
  fs.writeFileSync(selectionPath, JSON.stringify(['My, Scene', 'Main']));
  const scenes = new ObsScenes({ client, selectionPath });
  await scenes.switchTo(wireName('My, Scene'));   /* device sends wire names */
  assert.equal(client.current, 'My, Scene');
  await scenes.switchTo('Main');
  assert.equal(client.current, 'Main');
  const sets = client.calls.filter((c) => c.type === 'SetCurrentProgramScene');
  assert.equal(sets.length, 2);
});

test('switchTo refuses unselected and unknown scenes without calling OBS', async () => {
  const client = new FakeObs();
  const selectionPath = tempSelection();
  fs.writeFileSync(selectionPath, JSON.stringify(['Main']));
  const scenes = new ObsScenes({ client, selectionPath });
  await assert.rejects(scenes.switchTo('BRB'), /not selected as a box tile/);
  await assert.rejects(scenes.switchTo('Nowhere'), /not a KAST tile scene/);
  assert(!client.calls.some((c) => c.type === 'SetCurrentProgramScene'));
});

test('KAST never issues rename, create, or remove requests', async () => {
  const client = new FakeObs();
  const selectionPath = tempSelection();
  fs.writeFileSync(selectionPath, JSON.stringify(['Main', 'BRB']));
  const scenes = new ObsScenes({ client, selectionPath });
  await scenes.catalog();
  await scenes.switchTo('BRB');
  const forbidden = ['CreateScene', 'RemoveScene', 'SetSceneName', 'RemoveInput'];
  assert(!client.calls.some((c) => forbidden.includes(c.type)));
});
