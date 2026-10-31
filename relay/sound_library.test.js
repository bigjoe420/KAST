'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const { safeId, listSounds, installStarter, importSounds, removeSound, parseSoundManifest } = require('./sound_library');

function fixture(t) {
  const root = path.join(__dirname, `test-output-${process.pid}-${Math.random().toString(16).slice(2)}.tmp`);
  const soundsDir = path.join(root, 'sounds');
  const starterDir = path.join(root, 'starter');
  const incoming = path.join(root, 'incoming');
  for (const dir of [soundsDir, starterDir, incoming]) fs.mkdirSync(dir, { recursive: true });
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  return { root, soundsDir, starterDir, incoming };
}

function audio(dir, name, contents = 'sound') {
  const file = path.join(dir, name);
  fs.writeFileSync(file, contents);
  return file;
}

test('safeId produces bounded wire-safe labels', () => {
  assert.equal(safeId('  My: Sound, Really Long Name.mp3 '), 'My Sound Really Long');
  assert.throws(() => safeId(':::'), /name/i);
});

test('sound manifests accept Windows PowerShell UTF-8 BOM', () => {
  assert.deepEqual(parseSoundManifest('\uFEFF["KAST Bench One.mp3"]'), ['KAST Bench One.mp3']);
});

test('starter installation fills missing sounds without overwriting user audio', (t) => {
  const f = fixture(t);
  audio(f.starterDir, 'ding.mp3', 'starter');
  audio(f.starterDir, 'boo.mp3', 'starter');
  audio(f.soundsDir, 'ding.mp3', 'mine');
  const installed = installStarter({ starterDir: f.starterDir, soundsDir: f.soundsDir });
  assert.deepEqual(installed, ['boo']);
  assert.equal(fs.readFileSync(path.join(f.soundsDir, 'ding.mp3'), 'utf8'), 'mine');
  assert.equal(listSounds(f.soundsDir).length, 2);
});

test('starter installation is first-run only so removals survive restart', (t) => {
  const f = fixture(t);
  audio(f.starterDir, 'ding.mp3', 'starter');
  assert.deepEqual(installStarter({ starterDir: f.starterDir, soundsDir: f.soundsDir }), ['ding']);
  removeSound({ id: 'ding', soundsDir: f.soundsDir });
  assert.deepEqual(installStarter({ starterDir: f.starterDir, soundsDir: f.soundsDir }), []);
  assert.equal(listSounds(f.soundsDir).length, 0);
});

test('multi-import copies supported files and makes duplicate ids unique', (t) => {
  const f = fixture(t);
  audio(f.soundsDir, 'ding.mp3');
  const one = audio(f.incoming, 'ding.mp3', 'one');
  const two = audio(f.incoming, 'ding.wav', 'two');
  const added = importSounds({ files: [one, two], soundsDir: f.soundsDir });
  assert.deepEqual(added.map((x) => x.id), ['ding 2', 'ding 3']);
  assert.equal(listSounds(f.soundsDir).length, 3);
  assert.equal(fs.readFileSync(one, 'utf8'), 'one');
});

test('import rejects unsupported files and more than twelve active sounds', (t) => {
  const f = fixture(t);
  const bad = audio(f.incoming, 'notes.txt');
  assert.throws(() => importSounds({ files: [bad], soundsDir: f.soundsDir }), /supported/);
  for (let i = 0; i < 12; i++) audio(f.soundsDir, `sound${i}.mp3`);
  const extra = audio(f.incoming, 'extra.mp3');
  assert.throws(() => importSounds({ files: [extra], soundsDir: f.soundsDir }), /12/);
  assert.equal(listSounds(f.soundsDir).length, 12);
});

test('replace keeps the slot count and remove deletes only the selected id', (t) => {
  const f = fixture(t);
  audio(f.soundsDir, 'ding.mp3', 'old');
  audio(f.soundsDir, 'boo.mp3', 'keep');
  const incoming = audio(f.incoming, 'new effect.mp3', 'new');
  const replaced = importSounds({ files: [incoming], soundsDir: f.soundsDir, replaceId: 'ding' });
  assert.equal(replaced[0].id, 'new effect');
  assert.deepEqual(listSounds(f.soundsDir).map((x) => x.id).sort(), ['boo', 'new effect']);
  removeSound({ id: 'boo', soundsDir: f.soundsDir });
  assert.deepEqual(listSounds(f.soundsDir).map((x) => x.id), ['new effect']);
});
