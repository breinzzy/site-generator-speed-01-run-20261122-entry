// Native driver for the site-generator entry.
// The harness runs `node /bot/main.ts`; all work (JSON parse, build,
// JSON serialize) happens in the static native binary `sitegen` beside
// this file, so this driver only spawns it and waits.
// The chmod is needed because a GitHub web upload (or clone) may strip
// the executable bit from `sitegen`; without it spawnSync fails EACCES.
import { spawnSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { chmodSync } from 'node:fs';

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const dir = dirname(fileURLToPath(import.meta.url));
const bin = join(dir, 'sitegen');
try { chmodSync(bin, 0o755); } catch {}
const r = spawnSync(bin, [INPUT_DIR, OUTPUT_DIR], { stdio: 'inherit' });
if (r.error) throw r.error;
if (r.status !== 0) process.exit(r.status ?? 1);
