'use strict';

/* KAST graphics library — private motion-graphics files in relay/graphics/.
 * Mirrors sound_library.js: safe ids, atomic copies, 12 active slots, and
 * never overwriting user content. Each graphic can carry an optional
 * binding (.bindings.json, private): a synchronized sound id and how long
 * the overlay stays visible before the relay hides it again. */

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const MAX_GRAPHICS = 12;
const EXTENSIONS = ['.mp4', '.webm', '.mov', '.gif', '.png', '.jpg', '.jpeg'];
const DEFAULT_DURATION_MS = 5000;
const BINDINGS_FILE = '.bindings.json';

function safeId(name) {
  const base = path.basename(String(name), path.extname(String(name)));
  const id = base.replace(/[,:|]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 20).trim();
  if (!id) throw new Error('Graphic name must contain letters or numbers');
  return id;
}

function listGraphics(graphicsDir) {
  try {
    return fs.readdirSync(graphicsDir)
      .filter((file) => EXTENSIONS.includes(path.extname(file).toLowerCase()))
      .map((file) => ({ id: safeId(file), file: path.join(graphicsDir, file) }))
      .sort((a, b) => a.id.toLowerCase().localeCompare(b.id.toLowerCase()));
  } catch {
    return [];
  }
}

function uniqueId(requested, existing) {
  const used = new Set(existing.map((entry) => entry.id.toLowerCase()));
  if (!used.has(requested.toLowerCase())) return requested;
  for (let n = 2; ; n++) {
    const suffix = ` ${n}`;
    const candidate = requested.slice(0, 20 - suffix.length) + suffix;
    if (!used.has(candidate.toLowerCase())) return candidate;
  }
}

function copyAtomic(source, graphicsDir, id, extension) {
  fs.mkdirSync(graphicsDir, { recursive: true });
  const destination = path.join(graphicsDir, id + extension.toLowerCase());
  const temporary = path.join(graphicsDir, `.import-${crypto.randomUUID()}.tmp`);
  fs.copyFileSync(source, temporary, fs.constants.COPYFILE_EXCL);
  fs.renameSync(temporary, destination);
  return destination;
}

function importGraphics({ files, graphicsDir, replaceId = '' }) {
  if (!Array.isArray(files) || files.length === 0) throw new Error('Choose at least one graphic file');
  for (const file of files) {
    if (!EXTENSIONS.includes(path.extname(file).toLowerCase())) {
      throw new Error(`Unsupported graphic '${path.basename(file)}'; use MP4, WebM, MOV, GIF, PNG, or JPG`);
    }
    if (!fs.existsSync(file)) throw new Error(`Graphic file not found: ${file}`);
  }
  const existing = listGraphics(graphicsDir);
  if (replaceId && !existing.some((entry) => entry.id.toLowerCase() === replaceId.toLowerCase())) {
    throw new Error(`Graphic '${replaceId}' is not in the library`);
  }
  if (!replaceId && existing.length + files.length > MAX_GRAPHICS) {
    throw new Error(`All ${MAX_GRAPHICS} graphic slots would be full; replace or remove a graphic first`);
  }
  if (replaceId && files.length !== 1) throw new Error('Replace one graphic at a time');

  const added = [];
  const reserved = replaceId ? existing.filter((entry) => entry.id.toLowerCase() !== replaceId.toLowerCase()) : existing;
  for (const file of files) {
    const id = uniqueId(safeId(path.basename(file)), reserved);
    const destination = copyAtomic(file, graphicsDir, id, path.extname(file));
    reserved.push({ id, file: destination });
    added.push({ id, file: destination });
  }
  if (replaceId) removeGraphic({ id: replaceId, graphicsDir });
  return added;
}

function removeGraphic({ id, graphicsDir }) {
  const hit = listGraphics(graphicsDir).find((entry) => entry.id.toLowerCase() === String(id).toLowerCase());
  if (!hit) throw new Error(`Graphic '${id}' is not in the library`);
  fs.unlinkSync(hit.file);
  const bindings = readBindings(graphicsDir);
  if (delete bindings[hit.id]) writeBindings(graphicsDir, bindings);
  return hit;
}

/* --- optional per-graphic behavior: synchronized sound + visible time --- */

function bindingsPath(graphicsDir) {
  return path.join(graphicsDir, BINDINGS_FILE);
}

function readBindings(graphicsDir) {
  try {
    const parsed = JSON.parse(fs.readFileSync(bindingsPath(graphicsDir), 'utf8'));
    return parsed && typeof parsed === 'object' ? parsed : {};
  } catch {
    return {};
  }
}

function writeBindings(graphicsDir, bindings) {
  fs.mkdirSync(graphicsDir, { recursive: true });
  fs.writeFileSync(bindingsPath(graphicsDir), JSON.stringify(bindings, null, 2) + '\n');
}

function bindingFor(graphicsDir, id) {
  const raw = readBindings(graphicsDir)[id] || {};
  const durationMs = Number(raw.durationMs);
  return {
    sound: typeof raw.sound === 'string' && raw.sound ? raw.sound : null,
    durationMs: Number.isFinite(durationMs) && durationMs >= 500 && durationMs <= 60000
      ? Math.round(durationMs) : DEFAULT_DURATION_MS,
  };
}

function setBinding(graphicsDir, id, { sound, durationMs } = {}) {
  if (!listGraphics(graphicsDir).some((entry) => entry.id.toLowerCase() === String(id).toLowerCase())) {
    throw new Error(`Graphic '${id}' is not in the library`);
  }
  const bindings = readBindings(graphicsDir);
  const exact = listGraphics(graphicsDir).find((entry) => entry.id.toLowerCase() === String(id).toLowerCase()).id;
  const prior = bindings[exact] && typeof bindings[exact] === 'object' ? bindings[exact] : {};
  bindings[exact] = {
    /* merge: a bind-sound call must not wipe a previously set duration */
    ...prior,
    sound: sound ? String(sound).slice(0, 20) : null,
    ...(durationMs !== undefined ? { durationMs: Math.round(Number(durationMs)) } : {}),
  };
  writeBindings(graphicsDir, bindings);
  return bindingFor(graphicsDir, exact);
}

module.exports = {
  MAX_GRAPHICS, EXTENSIONS, DEFAULT_DURATION_MS,
  safeId, listGraphics, importGraphics, removeGraphic,
  bindingFor, setBinding, readBindings,
};
