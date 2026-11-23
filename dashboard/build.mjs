// Crumple Zone: bundle the dashboard front-end with esbuild.
// Copyright (C) 2026 chinesepowered
// SPDX-License-Identifier: AGPL-3.0-only
import { build } from 'esbuild';
import fs from 'node:fs';

fs.mkdirSync('dist/web', { recursive: true });
await build({
  entryPoints: ['src/web/main.ts'],
  bundle: true,
  format: 'iife',
  target: 'es2022',
  outfile: 'dist/web/main.js',
  sourcemap: true,
  minify: false,
});
fs.copyFileSync('src/web/index.html', 'dist/web/index.html');
fs.copyFileSync('src/web/styles.css', 'dist/web/styles.css');
fs.copyFileSync('node_modules/@xterm/xterm/css/xterm.css', 'dist/web/xterm.css');
console.log('built dist/web');
