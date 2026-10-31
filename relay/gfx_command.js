#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const library = require('./gfx_library');

const graphicsDir = path.join(__dirname, 'graphics');
const [action, first, second] = process.argv.slice(2);

try {
  if (action === 'list') {
    const entries = library.listGraphics(graphicsDir).map((entry) => {
      const binding = library.bindingFor(graphicsDir, entry.id);
      return { id: entry.id, sound: binding.sound, durationMs: binding.durationMs };
    });
    console.log(JSON.stringify(entries));
  } else if (action === 'import' || action === 'replace') {
    const manifest = action === 'import' ? first : second;
    /* Windows PowerShell 5.1 writes UTF-8 with a BOM — strip one leading BOM */
    const files = JSON.parse(String(fs.readFileSync(manifest, 'utf8')).replace(/^\uFEFF/, ''));
    const added = library.importGraphics({ files, graphicsDir, replaceId: action === 'replace' ? first : '' });
    console.log(`Added: ${added.map((entry) => entry.id).join(', ')}`);
  } else if (action === 'remove') {
    const removed = library.removeGraphic({ id: first, graphicsDir });
    console.log(`Removed: ${removed.id}`);
  } else if (action === 'bind') {
    /* bind <graphicId> <soundId|none> [durationMs] */
    const binding = library.setBinding(graphicsDir, first, {
      sound: second && second !== 'none' ? second : null,
      durationMs: process.argv[5] !== undefined ? Number(process.argv[5]) : undefined,
    });
    console.log(`Bound: ${first} -> ${binding.sound || 'no sound'}, visible ${binding.durationMs} ms`);
  } else {
    throw new Error('Usage: gfx_command.js <list|import|replace|remove|bind> [arguments]');
  }
} catch (err) {
  console.error(err.message);
  process.exit(1);
}
