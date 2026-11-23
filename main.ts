// makefun-site 1.0.0, the house entry, make.fun's own reference program: the harness runs `node /bot/main.ts` (node22). It reads every case from /input/cases.json (a JSON
// array of {id, input}), answers each one alone with run() from solve.js, and writes /output/answers.json: a JSON
// array of the answers, one per case and in the cases' order. An entry keeps this driver and replaces solve.js with a
// faster module whose run(input) returns the same answer for every input. Outside a container MAKEFUN_INPUT_DIR and MAKEFUN_OUTPUT_DIR stand in for /input and /output.

import { readFileSync, writeFileSync } from 'node:fs';
import { join } from 'node:path';
import { setFlagsFromString } from 'node:v8';

// V8's incremental marking off, set before solve.js is imported so all of its work runs under it (the rules: the house
// baseline runs with --no-incremental-marking, and an entry may tune V8's flags too).
// A 64 MiB young generation (--max-semi-space-size=64) cuts scavenge frequency
// for this entry's short-lived strings/arrays (measured ~12% faster cold locally).
// --turbo-filter restricts TurboFan to the hot functions only (named template
// fns tpl_* plus the markdown/render helpers), so the cold run skips compiling
// the many cold helpers; measured ~20% faster cold locally.
setFlagsFromString('--no-incremental-marking --max-semi-space-size=64 --turbo-filter=inlineInto|renderMarkdown|build|escapeHtml|escHtml|wordsSimple|wordsOf|parseFrontMatter|render|tpl_|lk|truthy|textOf|__constInc|__objInc|findUnescaped|findSingleStar|isEscapedAt|slugify|uniqueSlug');

/** solve.js, imported by its URL (a .js module carries no types) and typed by its contract. */
const { run } = (await import(new URL('./solve.js', import.meta.url).href)) as { run: (input: unknown) => unknown };

const INPUT_DIR = process.env.MAKEFUN_INPUT_DIR ?? '/input';
const OUTPUT_DIR = process.env.MAKEFUN_OUTPUT_DIR ?? '/output';

// Entry driver (kept from the house baseline's main.ts): the harness runs
// `node /bot/main.ts` (node22). solve.js beside it is this entry's fast
// drop-in for makefun-site 1.0.0 — byte-exact on every case, single file, no
// dependencies. This driver only reads the cases, answers each one alone
// with run(), and writes the answers in order; all of the speed is in solve.js.

const cases = JSON.parse(readFileSync(join(INPUT_DIR, 'cases.json'), 'utf8')) as { id: string; input: unknown }[];
const answers: unknown[] = [];
for (const c of cases) answers.push(run(c.input));
if (answers.length !== cases.length) throw new Error('answers do not cover every case');
writeFileSync(join(OUTPUT_DIR, 'answers.json'), JSON.stringify(answers));
