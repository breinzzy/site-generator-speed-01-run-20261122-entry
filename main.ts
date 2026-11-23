// Native driver for the site-generator entry.
// The harness runs `node /bot/main.ts`; all work (JSON parse, build,
// JSON serialize) happens in the static native binary `sitegen` beside
// this file, so this driver only spawns it and waits.
//
// Why the copy: the grader sandbox mounts /bot read-only and GitHub's web
// upload strips the exec bit, so the shipped `sitegen` cannot be executed
// in place (round 3 failed run_completes with exit code 1). The driver
// therefore copies the binary to a writable temp dir, sets the exec bit
// there, and runs that copy. If the temp copy/spawn fails (e.g. /tmp is
// noexec), it falls back to running the shipped binary in place.
import { spawnSync } from 'node:child_process';
import { join, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { copyFileSync, chmodSync } from 'node:fs';
import { tmpdir } from 'node:os';

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

const dir = dirname(fileURLToPath(import.meta.url));
const src = join(dir, 'sitegen');

function run(bin) {
  return spawnSync(bin, [INPUT_DIR, OUTPUT_DIR], { stdio: 'inherit' });
}

// Try 1: temp copy (writable dir, exec bit set by us).
try {
  const dest = join(tmpdir(), `sitegen-${process.pid}`);
  copyFileSync(src, dest);
  chmodSync(dest, 0o755);
  const r1 = run(dest);
  if (!r1.error) {
    if (r1.status !== 0) process.exit(r1.status ?? 1);
    process.exit(0);
  }
} catch {}

// Try 2: shipped binary in place (only if the temp copy/spawn failed).
try { chmodSync(src, 0o755); } catch {}
const r2 = run(src);
if (r2.error) throw r2.error;
if (r2.status !== 0) process.exit(r2.status ?? 1);
