#!/usr/bin/env node
'use strict';

const fs = require('fs');
const path = require('path');
const library = require('./sound_library');

const soundsDir = path.join(__dirname, 'sounds');
const [action, first, second] = process.argv.slice(2);

try {
  if (action === 'list') {
    console.log(JSON.stringify(library.listSounds(soundsDir).map((entry) => entry.id)));
  } else if (action === 'import' || action === 'replace') {
    const manifest = action === 'import' ? first : second;
    const files = JSON.parse(fs.readFileSync(manifest, 'utf8'));
    const added = library.importSounds({ files, soundsDir, replaceId: action === 'replace' ? first : '' });
    console.log(`Added: ${added.map((entry) => entry.id).join(', ')}`);
  } else if (action === 'remove') {
    const removed = library.removeSound({ id: first, soundsDir });
    console.log(`Removed: ${removed.id}`);
  } else {
    throw new Error('Usage: sound_command.js <list|import|replace|remove> [arguments]');
  }
} catch (err) {
  console.error(err.message);
  process.exit(1);
}
