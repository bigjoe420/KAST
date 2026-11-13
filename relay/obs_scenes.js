'use strict';

/* KAST Scene Controller — the user picks which of THEIR OBS scenes appear
 * as box tiles; tapping a tile switches the OBS program scene.
 *
 * Hard rules (owner scope): KAST never creates, renames, or deletes user
 * scenes, and its own managed scenes (KAST SFX / KAST GFX) never appear as
 * tiles. The selection lives in a private JSON file; tiles whose scene was
 * renamed or deleted in OBS simply drop off the catalog. */

const fs = require('fs');
const path = require('path');

const MAX_TILES = 12;
const KAST_SCENES = new Set(['KAST SFX', 'KAST GFX']);

function isUserScene(name) {
  return !KAST_SCENES.has(name) &&
    !name.startsWith('KAST SFX - ') &&
    !name.startsWith('KAST GFX - ');
}

class ObsScenes {
  constructor({ client, selectionPath, logger = console } = {}) {
    this.client = client;
    this.selectionPath = selectionPath;
    this.logger = logger;
  }

  readSelection() {
    try {
      const parsed = JSON.parse(fs.readFileSync(this.selectionPath, 'utf8'));
      if (!Array.isArray(parsed)) return [];
      return parsed.map(String).filter(isUserScene).slice(0, MAX_TILES);
    } catch {
      return [];
    }
  }

  writeSelection(names) {
    if (!Array.isArray(names)) throw new Error('Scene selection must be a list of names');
    const clean = names.map(String).filter(isUserScene).slice(0, MAX_TILES);
    fs.mkdirSync(path.dirname(this.selectionPath), { recursive: true });
    fs.writeFileSync(this.selectionPath, JSON.stringify(clean, null, 2) + '\n');
    return clean;
  }

  /* Every user scene in OBS, in STUDIO-UI order (top of the scene list
     first). GetSceneList returns the array bottom-to-top — the reverse of
     what the user sees — so flip it. Every consumer (Companion picker,
     box tile order) matches the OBS UI this way. */
  async listUserScenes() {
    const { scenes } = await this.client.request('GetSceneList');
    return scenes.map((scene) => scene.sceneName).filter(isUserScene).reverse();
  }

  async currentScene() {
    const current = await this.client.request('GetCurrentProgramScene');
    return current.sceneName || current.currentProgramSceneName || '';
  }

  /* The box tile catalog: selected scenes that still exist in OBS, ordered
     by the studio UI (top of the OBS list = top tile). The selection file
     is a SET — its stored order is legacy-array order from before the
     GetSceneList reversal fix, so never display in file order. */
  async catalog() {
    const ordered = await this.listUserScenes();
    const selected = new Set(this.readSelection());
    const tiles = ordered.filter((name) => selected.has(name));
    return { scenes: tiles, current: await this.currentScene() };
  }

  /* Wire names strip , : | (the device catalog is comma-delimited); a tap
     sends back the wire name, so match against both forms. */
  async switchTo(name) {
    const existing = await this.listUserScenes();
    const match = existing.find((scene) => scene === name) ||
                  existing.find((scene) => wireName(scene) === name);
    if (!match) {
      throw new Error(`Scene '${name}' is not a KAST tile scene`);
    }
    if (!this.readSelection().includes(match)) {
      throw new Error(`Scene '${match}' is not selected as a box tile`);
    }
    await this.client.request('SetCurrentProgramScene', { sceneName: match });
    return match;
  }
}

function wireName(name) {
  return String(name).replace(/[,:|]/g, ' ').replace(/\s+/g, ' ').trim().slice(0, 24);
}

module.exports = { ObsScenes, isUserScene, wireName, MAX_TILES };
