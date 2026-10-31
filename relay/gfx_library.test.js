'use strict';

const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('fs');
const path = require('path');
const {
  safeId, listGraphics, importGraphics, removeGraphic,
  bindingFor, setBinding, DEFAULT_DURATION_MS,
} = require('./gfx_library');

function fixture(t) {
  const root = path.join(__dirname, `test-gfx-${process.pid}-${Math.random().toString(16).slice(2)}.tmp`);
  const graphicsDir = path.join(root, 'graphics');
  const incoming = path.join(root, 'incoming');
  for (const dir of [graphicsDir, incoming]) fs.mkdirSync(dir, { recursive: true });
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  return { root, graphicsDir, incoming };
}

function media(dir, name, contents = 'gfx') {
  const file = path.join(dir, name);
  fs.writeFileSync(file, contents);
  return file;
}

test('safeId produces bounded wire-safe labels', () => {
  assert.equal(safeId('  My: Clip, Really Long Name.webm '), 'My Clip Really Long');
  assert.throws(() => safeId(':::'), /name/i);
});

test('listGraphics ignores non-media files like the private bindings file', (t) => {
  const f = fixture(t);
  media(f.graphicsDir, 'gg.webm');
  media(f.graphicsDir, 'pop.png');
  fs.writeFileSync(path.join(f.graphicsDir, '.bindings.json'), '{}');
  fs.writeFileSync(path.join(f.graphicsDir, 'notes.txt'), 'x');
  assert.deepEqual(listGraphics(f.graphicsDir).map((x) => x.id), ['gg', 'pop']);
});

test('multi-import copies supported files and makes duplicate ids unique', (t) => {
  const f = fixture(t);
  media(f.graphicsDir, 'gg.webm');
  const one = media(f.incoming, 'gg.mp4', 'one');
  const two = media(f.incoming, 'gg.webm', 'two');
  const added = importGraphics({ files: [one, two], graphicsDir: f.graphicsDir });
  assert.deepEqual(added.map((x) => x.id), ['gg 2', 'gg 3']);
  assert.equal(fs.readFileSync(one, 'utf8'), 'one');
});

test('import rejects unsupported files and more than twelve active graphics', (t) => {
  const f = fixture(t);
  const bad = media(f.incoming, 'notes.txt');
  assert.throws(() => importGraphics({ files: [bad], graphicsDir: f.graphicsDir }), /supported/);
  for (let i = 0; i < 12; i++) media(f.graphicsDir, `gfx${i}.webm`);
  const extra = media(f.incoming, 'extra.webm');
  assert.throws(() => importGraphics({ files: [extra], graphicsDir: f.graphicsDir }), /12/);
});

test('replace keeps the slot count and remove deletes only the selected id', (t) => {
  const f = fixture(t);
  media(f.graphicsDir, 'gg.webm', 'old');
  media(f.graphicsDir, 'pop.png', 'keep');
  const incoming = media(f.incoming, 'new clip.webm', 'new');
  const replaced = importGraphics({ files: [incoming], graphicsDir: f.graphicsDir, replaceId: 'gg' });
  assert.equal(replaced[0].id, 'new clip');
  assert.deepEqual(listGraphics(f.graphicsDir).map((x) => x.id).sort(), ['new clip', 'pop']);
  removeGraphic({ id: 'pop', graphicsDir: f.graphicsDir });
  assert.deepEqual(listGraphics(f.graphicsDir).map((x) => x.id), ['new clip']);
});

test('bindings default sensibly and persist sound plus duration', (t) => {
  const f = fixture(t);
  media(f.graphicsDir, 'gg.webm');
  assert.deepEqual(bindingFor(f.graphicsDir, 'gg'), { sound: null, durationMs: DEFAULT_DURATION_MS });
  setBinding(f.graphicsDir, 'gg', { sound: 'airhorn', durationMs: 2500 });
  assert.deepEqual(bindingFor(f.graphicsDir, 'gg'), { sound: 'airhorn', durationMs: 2500 });
  setBinding(f.graphicsDir, 'gg', { sound: null });
  assert.equal(bindingFor(f.graphicsDir, 'gg').sound, null);
  assert.equal(bindingFor(f.graphicsDir, 'gg').durationMs, 2500);
});

test('out-of-range durations fall back to the default at read time', (t) => {
  const f = fixture(t);
  media(f.graphicsDir, 'gg.webm');
  setBinding(f.graphicsDir, 'gg', { durationMs: 10 });
  assert.equal(bindingFor(f.graphicsDir, 'gg').durationMs, DEFAULT_DURATION_MS);
});

test('setBinding refuses unknown graphics and remove drops the binding', (t) => {
  const f = fixture(t);
  media(f.graphicsDir, 'gg.webm');
  assert.throws(() => setBinding(f.graphicsDir, 'missing', { sound: 'ding' }), /not in the library/);
  setBinding(f.graphicsDir, 'gg', { sound: 'ding' });
  removeGraphic({ id: 'gg', graphicsDir: f.graphicsDir });
  assert.deepEqual(bindingFor(f.graphicsDir, 'gg'), { sound: null, durationMs: DEFAULT_DURATION_MS });
});
