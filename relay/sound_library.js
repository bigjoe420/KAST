'use strict';

const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const MAX_SOUNDS = 12;
const EXTENSIONS = ['.mp3', '.wav', '.ogg', '.m4a'];

function safeId(name) {
  const base = path.basename(String(name), path.extname(String(name)));
  const id = base.replace(/[,:|]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 20).trim();
  if (!id) throw new Error('Sound name must contain letters or numbers');
  return id;
}

function parseSoundManifest(text) {
  return JSON.parse(String(text).replace(/^\uFEFF/, ''));
}

function listSounds(soundsDir) {
  try {
    return fs.readdirSync(soundsDir)
      .filter((file) => EXTENSIONS.includes(path.extname(file).toLowerCase()))
      .map((file) => ({ id: safeId(file), file: path.join(soundsDir, file) }))
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

function copyAtomic(source, soundsDir, id, extension) {
  fs.mkdirSync(soundsDir, { recursive: true });
  const destination = path.join(soundsDir, id + extension.toLowerCase());
  const temporary = path.join(soundsDir, `.import-${crypto.randomUUID()}.tmp`);
  fs.copyFileSync(source, temporary, fs.constants.COPYFILE_EXCL);
  fs.renameSync(temporary, destination);
  return destination;
}

function installStarter({ starterDir, soundsDir }) {
  fs.mkdirSync(soundsDir, { recursive: true });
  const marker = path.join(soundsDir, '.starter-installed.json');
  if (fs.existsSync(marker)) return [];

  const installed = [];
  for (const starter of listSounds(starterDir)) {
    const existing = listSounds(soundsDir);
    if (existing.some((entry) => entry.id.toLowerCase() === starter.id.toLowerCase())) continue;
    copyAtomic(starter.file, soundsDir, starter.id, path.extname(starter.file));
    installed.push(starter.id);
  }
  fs.writeFileSync(marker, JSON.stringify({ version: 1 }) + '\n');
  return installed;
}

function importSounds({ files, soundsDir, replaceId = '' }) {
  if (!Array.isArray(files) || files.length === 0) throw new Error('Choose at least one sound file');
  for (const file of files) {
    if (!EXTENSIONS.includes(path.extname(file).toLowerCase())) {
      throw new Error(`Unsupported sound '${path.basename(file)}'; use MP3, WAV, OGG, or M4A`);
    }
    if (!fs.existsSync(file)) throw new Error(`Sound file not found: ${file}`);
  }
  let existing = listSounds(soundsDir);
  if (replaceId && !existing.some((entry) => entry.id.toLowerCase() === replaceId.toLowerCase())) {
    throw new Error(`Sound '${replaceId}' is not in the library`);
  }
  if (!replaceId && existing.length + files.length > MAX_SOUNDS) {
    throw new Error(`All ${MAX_SOUNDS} sound slots would be full; replace or remove a sound first`);
  }
  if (replaceId && files.length !== 1) throw new Error('Replace one sound at a time');

  const added = [];
  const reserved = replaceId ? existing.filter((entry) => entry.id.toLowerCase() !== replaceId.toLowerCase()) : existing;
  for (const file of files) {
    const id = uniqueId(safeId(path.basename(file)), reserved);
    const destination = copyAtomic(file, soundsDir, id, path.extname(file));
    reserved.push({ id, file: destination });
    added.push({ id, file: destination });
  }
  if (replaceId) removeSound({ id: replaceId, soundsDir });
  return added;
}

function removeSound({ id, soundsDir }) {
  const hit = listSounds(soundsDir).find((entry) => entry.id.toLowerCase() === String(id).toLowerCase());
  if (!hit) throw new Error(`Sound '${id}' is not in the library`);
  fs.unlinkSync(hit.file);
  return hit;
}

module.exports = { MAX_SOUNDS, EXTENSIONS, safeId, listSounds, installStarter, importSounds, removeSound, parseSoundManifest };
