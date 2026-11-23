// Fast drop-in for makefun-site 1.0.0 — byte-exact reimplementation, optimized.
// Single file, no dependencies. Every rule mirrors vendor/makefun-site exactly;
// only the implementation is faster (chunked escaping, ASCII fast paths, fused
// markdown lex+render, per-case caches, no token objects).

const byCodeUnits = (a, b) => (a < b ? -1 : a > b ? 1 : 0);
const UNI_WS = /[\u0085\u00A0\u1680\u2000-\u200A\u2028\u2029\u202F\u205F\u3000\uFEFF]/;
const MONTHS = ['January', 'February', 'March', 'April', 'May', 'June', 'July', 'August', 'September', 'October', 'November', 'December'];
const RESERVED = ['posts', 'tags', 'page'];

// ---------------------------------------------------------------- text

const ESC_RE = /[&<>"']/;
// Fused textOf + escapeHtml for template {{var}} output: one call instead of
// two, with the string fast path (no escaping needed) inlined.
function escHtml(v) {
  if (typeof v === 'string') {
    if (!ESC_RE.test(v)) return v;
    let out = '';
    let start = 0;
    for (let i = 0; i < v.length; i++) {
      const c = v[i];
      let e;
      if (c === '&') e = '&amp;';
      else if (c === '<') e = '&lt;';
      else if (c === '>') e = '&gt;';
      else if (c === '"') e = '&quot;';
      else if (c === "'") e = '&#39;';
      else continue;
      out += v.slice(start, i) + e;
      start = i + 1;
    }
    return out + v.slice(start);
  }
  return escapeHtml(textOf(v));
}

function escapeHtml(s) {
  if (!ESC_RE.test(s)) return s;
  let out = '';
  let start = 0;
  for (let i = 0; i < s.length; i++) {
    const c = s[i];
    let e;
    if (c === '&') e = '&amp;';
    else if (c === '<') e = '&lt;';
    else if (c === '>') e = '&gt;';
    else if (c === '"') e = '&quot;';
    else if (c === "'") e = '&#39;';
    else continue;
    out += s.slice(start, i) + e;
    start = i + 1;
  }
  return out + s.slice(start);
}

function longDate(date) {
  return `${Number(date.slice(8, 10))} ${MONTHS[Number(date.slice(5, 7)) - 1]} ${date.slice(0, 4)}`;
}

// Per-case slug cache (created fresh inside run()).
let slugCache = null;

const SLUG_CHARS = [];
for (let i = 0; i < 128; i++) {
  if (i >= 65 && i <= 90) SLUG_CHARS.push(String.fromCharCode(i + 32));
  else if ((i >= 97 && i <= 122) || (i >= 48 && i <= 57)) SLUG_CHARS.push(String.fromCharCode(i));
  else SLUG_CHARS.push('');
}

function slugifySlow(text) {
  const plain = text.normalize('NFKD').replace(/\p{M}/gu, '');
  let out = '';
  let gap = false;
  for (const ch of plain) {
    let keep = null;
    if (ch >= 'A' && ch <= 'Z') keep = ch.toLowerCase();
    else if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) keep = ch;
    if (keep === null) { gap = out.length > 0; continue; }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  if (out.length > 64) out = out.slice(0, 64);
  while (out.endsWith('-')) out = out.slice(0, -1);
  return out === '' ? 'untitled' : out;
}

function slugify(text) {
  const hit = slugCache.get(text);
  if (hit !== undefined) return hit;
  let out = '';
  let gap = false;
  for (let i = 0; i < text.length; i++) {
    const ch = text.charCodeAt(i);
    if (ch > 127) { const slow = slugifySlow(text); slugCache.set(text, slow); return slow; }
    const keep = SLUG_CHARS[ch];
    if (keep === '') { gap = out.length > 0; continue; }
    if (gap) out += '-';
    gap = false;
    out += keep;
  }
  if (out.length > 64) out = out.slice(0, 64);
  while (out.endsWith('-')) out = out.slice(0, -1);
  if (out === '') out = 'untitled';
  slugCache.set(text, out);
  return out;
}

function uniqueSlug(slug, taken) {
  let out = slug;
  for (let n = 2; taken.has(out); n++) out = `${slug}-${n}`;
  taken.add(out);
  return out;
}

function isWsCode(c) {
  return c === 32 || c === 9 || c === 10 || c === 13 || c === 12 || c === 11 || c === 0x85 || c === 0xa0 || c === 0x1680 ||
    (c >= 0x2000 && c <= 0x200a) || c === 0x2028 || c === 0x2029 || c === 0x202f || c === 0x205f || c === 0x3000 || c === 0xfeff;
}

function wordsSimple(text) {
  let n = 0;
  let inW = false;
  for (let i = 0; i < text.length; i++) {
    const c = text.charCodeAt(i);
    if (c <= 32) inW = false;
    else if (!inW) { inW = true; n++; }
  }
  return n;
}

function wordsFull(text) {
  let n = 0;
  let inWord = false;
  for (let i = 0; i < text.length; i++) {
    if (isWsCode(text.charCodeAt(i))) inWord = false;
    else if (!inWord) { inWord = true; n++; }
  }
  return n;
}

function wordsOf(text) {
  // Bodies are ASCII-whitespace separated in practice; fall back to the full
  // Unicode-whitespace scan only when such a character is actually present.
  return UNI_WS.test(text) ? wordsFull(text) : wordsSimple(text);
}

// ---------------------------------------------------------------- front matter

const KEYS = ['title', 'date', 'tags', 'draft', 'layout', 'slug', 'summary'];

function daysIn(y, m) {
  if (m === 2) return (y % 4 === 0 && y % 100 !== 0) || y % 400 === 0 ? 29 : 28;
  return m === 4 || m === 6 || m === 9 || m === 11 ? 30 : 31;
}

function isDigit(c) { return c >= 48 && c <= 57; }

function stringOf(raw) {
  if (raw.charCodeAt(0) === 34) { // '"'
    if (raw.length < 2 || raw.charCodeAt(raw.length - 1) !== 34) return { error: 'a quoted string is not closed' };
    let out = '';
    for (let i = 1; i < raw.length - 1; i++) {
      const c = raw[i];
      if (c === '\\') {
        const n = raw[i + 1];
        if ((n !== '"' && n !== '\\') || i + 1 === raw.length - 1) return { error: 'a quoted string holds an escape other than \\" or \\\\' };
        out += n;
        i++;
      } else if (c === '"') return { error: 'a quoted string holds a stray quote' };
      else out += c;
    }
    return { value: out };
  }
  if (raw.charCodeAt(0) === 91) return { error: 'a bare string opens on [' }; // '['
  return { value: raw };
}

function valueOf(key, raw) {
  switch (key) {
    case 'title':
    case 'summary': {
      const s = stringOf(raw);
      return 'error' in s ? { error: `key "${key}": ${s.error}` } : s;
    }
    case 'date': {
      let ok = raw.length === 10 && raw.charCodeAt(4) === 45 && raw.charCodeAt(7) === 45;
      if (ok) for (let i = 0; i < 10; i++) if (i !== 4 && i !== 7 && !isDigit(raw.charCodeAt(i))) { ok = false; break; }
      const y = ok ? Number(raw.slice(0, 4)) : 0;
      const mo = ok ? Number(raw.slice(5, 7)) : 0;
      const d = ok ? Number(raw.slice(8, 10)) : 0;
      if (!ok || y < 1970 || y > 2099 || mo < 1 || mo > 12 || d < 1 || d > daysIn(y, mo)) return { error: 'key "date": not a date from 1970-01-01 to 2099-12-31 (YYYY-MM-DD)' };
      return { value: raw };
    }
    case 'tags': {
      if (raw.charCodeAt(0) !== 91 || raw.charCodeAt(raw.length - 1) !== 93) return { error: 'key "tags": not a list [a, b]' };
      const inner = raw.slice(1, -1);
      if (inner.trim() === '') return { value: [] };
      const tags = inner.split(',');
      for (let i = 0; i < tags.length; i++) {
        tags[i] = tags[i].trim();
        const t = tags[i];
        if (t === '') return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
        for (let j = 0; j < t.length; j++) {
          const c = t[j];
          if (c === '"' || c === '[' || c === ']') return { error: 'key "tags": an empty tag, or one holding " [ or ]' };
        }
      }
      return { value: tags };
    }
    case 'draft':
      return raw === 'true' ? { value: true } : raw === 'false' ? { value: false } : { error: 'key "draft": true or false' };
    case 'layout': {
      if (raw.length === 0) return { error: 'key "layout": not a template name' };
      for (let i = 0; i < raw.length; i++) {
        const c = raw.charCodeAt(i);
        if (!((c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 45)) return { error: 'key "layout": not a template name' };
      }
      return { value: raw };
    }
    case 'slug': {
      if (raw.length === 0 || raw.length > 64) return { error: 'key "slug": not a slug' };
      let prevDash = true;
      for (let i = 0; i < raw.length; i++) {
        const c = raw.charCodeAt(i);
        if (c === 45) { if (prevDash) return { error: 'key "slug": not a slug' }; prevDash = true; }
        else if ((c >= 97 && c <= 122) || (c >= 48 && c <= 57)) prevDash = false;
        else return { error: 'key "slug": not a slug' };
      }
      if (prevDash) return { error: 'key "slug": not a slug' };
      return { value: raw };
    }
    default:
      return { error: `unknown key "${key}"` };
  }
}

// Parse front matter from text (line ends already "\n"). Returns {data, body} or {error}.
function parseFrontMatter(text) {
  if (text !== '---' && !text.startsWith('---\n')) return { error: 'no front matter' };
  // Find closing line: first line after line 1 that is exactly '---'.
  let pos = 4; // just past '---\n' (or end)
  let closeLineStart = -1;
  let lineNo = 1; // 0-based index of the line starting at pos
  if (text === '---') return { error: 'the front matter is not closed' };
  while (pos < text.length) {
    const nl = text.indexOf('\n', pos);
    const lineEnd = nl === -1 ? text.length : nl;
    if (lineEnd - pos === 3 && text[pos] === '-' && text[pos + 1] === '-' && text[pos + 2] === '-') { closeLineStart = pos; break; }
    pos = lineEnd + 1;
    lineNo++;
    if (nl === -1) break;
  }
  if (closeLineStart === -1) return { error: 'the front matter is not closed' };
  const data = {};
  // Lines between opening (line 0) and closing line.
  let p = 4;
  let idx = 1; // line index (0-based) of the line starting at p
  while (p < closeLineStart) {
    const nl = text.indexOf('\n', p);
    const lineEnd = nl === -1 ? text.length : nl;
    const line = text.slice(p, lineEnd);
    // blank (spaces/tabs only) or comment ('#' first)
    let blank = true;
    for (let i = 0; i < line.length; i++) { const c = line.charCodeAt(i); if (c !== 32 && c !== 9) { blank = false; break; } }
    if (!blank && line.charCodeAt(0) !== 35) {
      // must be "key: value": key [a-z_]+, ':', then end or ' ' + rest
      let k = 0;
      while (k < line.length) { const c = line.charCodeAt(k); if ((c >= 97 && c <= 122) || c === 95) k++; else break; }
      if (k === 0 || k >= line.length || line.charCodeAt(k) !== 58) return { error: `line ${idx + 1} is not "key: value"` };
      const key = line.slice(0, k);
      let raw;
      if (k + 1 === line.length) raw = '';
      else if (line.charCodeAt(k + 1) === 32) raw = line.slice(k + 2);
      else return { error: `line ${idx + 1} is not "key: value"` };
      // drop trailing spaces/tabs
      let e = raw.length;
      while (e > 0) { const c = raw.charCodeAt(e - 1); if (c === 32 || c === 9) e--; else break; }
      raw = raw.slice(0, e);
      if (!KEYS.includes(key)) return { error: `unknown key "${key}"` };
      if (Object.hasOwn(data, key)) return { error: `key "${key}" is given twice` };
      if (raw === '') return { error: `key "${key}" has no value` };
      const v = valueOf(key, raw);
      if ('error' in v) return v;
      data[key] = v.value;
    }
    p = lineEnd + 1;
    idx++;
  }
  if (!Object.hasOwn(data, 'title')) return { error: 'no title' };
  // Body: everything after the closing line (and its '\n' if present).
  const closeNl = text.indexOf('\n', closeLineStart);
  const body = closeNl === -1 ? '' : text.slice(closeNl + 1);
  return { data, body };
}

// ---------------------------------------------------------------- markdown

const ESCAPABLE = '\\`*_[]()#+-.!>';
function isEscapable(c) { return ESCAPABLE.indexOf(c) !== -1; }

function isEscapedAt(s, p) {
  // A char is escaped when preceded by an odd run of backslashes: the run
  // pairs up left to right, so an odd count leaves one backslash to escape p.
  let k = 0;
  let q = p - 1;
  while (q >= 0 && s[q] === '\\') { k++; q--; }
  return (k % 2) === 1;
}

function findUnescaped(s, what, from, to) {
  const wlen = what.length;
  let j = s.indexOf(what, from);
  while (j !== -1 && j + wlen <= to) {
    if (!isEscapedAt(s, j)) return j;
    j = s.indexOf(what, j + 1);
  }
  return -1;
}

function findSingleStar(s, from, to) {
  let j = s.indexOf('*', from);
  while (j !== -1 && j < to) {
    // Escape check first: an escaped '*' must never be read as half of a
    // '**' pair (its backslash was consumed by the scanner before it).
    if (isEscapedAt(s, j)) { j = s.indexOf('*', j + 1); continue; }
    if (s[j + 1] === '*') { j = s.indexOf('*', j + 2); continue; }
    return j;
  }
  return -1;
}

function isSpaceTab(c) { return c === 32 || c === 9; }

// Trimmed string for src[a,b) (spaces/tabs only, like the reference trimLine).
function trimSlice(src, a, b) {
  while (a < b && isSpaceTab(src.charCodeAt(a))) a++;
  while (b > a && isSpaceTab(src.charCodeAt(b - 1))) b--;
  return src.slice(a, b);
}

function isBlankR(src, a, b) {
  for (let i = a; i < b; i++) { const c = src.charCodeAt(i); if (c !== 32 && c !== 9) return false; }
  return true;
}

// Fence open: '```' + lang [A-Za-z0-9_+-]* + spaces/tabs only. Returns lang or null.
function fenceLangR(src, a, b) {
  if (src.charCodeAt(a) !== 96 || src.charCodeAt(a + 1) !== 96 || src.charCodeAt(a + 2) !== 96) return null;
  let j = a + 3;
  while (j < b) {
    const c = src.charCodeAt(j);
    if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 95 || c === 43 || c === 45) j++;
    else break;
  }
  const lang = src.slice(a + 3, j);
  while (j < b && isSpaceTab(src.charCodeAt(j))) j++;
  return j === b ? lang : null;
}

function isFenceCloseR(src, a, b) {
  if (src.charCodeAt(a) !== 96 || src.charCodeAt(a + 1) !== 96 || src.charCodeAt(a + 2) !== 96) return false;
  for (let j = a + 3; j < b; j++) if (!isSpaceTab(src.charCodeAt(j))) return false;
  return true;
}

// Heading: 1-6 '#', space/tab, text (trailing spaces/tabs stripped, never empty).
function headingOfR(src, a, b) {
  let n = 0;
  while (a + n < b && src.charCodeAt(a + n) === 35) n++;
  if (n === 0 || n > 6) return null;
  if (a + n >= b || !isSpaceTab(src.charCodeAt(a + n))) return null;
  let j = a + n;
  while (j < b && isSpaceTab(src.charCodeAt(j))) j++;
  let e = b;
  while (e > j && isSpaceTab(src.charCodeAt(e - 1))) e--;
  const text = src.slice(j, e);
  return text === '' ? null : { depth: n, text };
}

function isHrR(src, a, b) {
  if (b - a < 3) return false;
  const c = src.charCodeAt(a);
  if (c !== 45 && c !== 42) return false;
  if (src.charCodeAt(a + 1) !== c || src.charCodeAt(a + 2) !== c) return false;
  for (let j = a + 3; j < b; j++) if (!isSpaceTab(src.charCodeAt(j))) return false;
  return true;
}

function isContinuedR(src, a, b) {
  let sp = 0;
  while (a + sp < b && src.charCodeAt(a + sp) === 32) sp++;
  return sp >= 2 && a + sp < b && !isWsCode(src.charCodeAt(a + sp));
}

/** Inline render of `s` to a string; env.link(target) resolves a link target. */
function inline(s, env) {
  const out = [];
  inlineInto(s, env, out, 0, s.length);
  return out.join('');
}

/** Inline-render s[lo,hi) into `out` (no intermediate strings for nesting). */
const NEXT_SPECIAL = /[\\`*\[]/g;

function inlineInto(s, env, out, lo, hi) {
  let i = lo;
  let seg = lo;
  while (i < hi) {
    NEXT_SPECIAL.lastIndex = i;
    const mt = NEXT_SPECIAL.exec(s);
    if (mt === null || mt.index >= hi) break;
    const j = mt.index;
    const ch = s[j];
    if (ch === '\\') {
      if (j + 1 < hi && isEscapable(s[j + 1])) {
        if (j > seg) out.push(escapeHtml(s.slice(seg, j)));
        out.push(escapeHtml(s[j + 1]));
        i = j + 2; seg = i;
      } else i = j + 1;
      continue;
    }
    if (ch === '`') {
      const k = s.indexOf('`', j + 1);
      if (k !== -1 && k < hi && k > j + 1) {
        if (j > seg) out.push(escapeHtml(s.slice(seg, j)));
        out.push('<code>', escapeHtml(s.slice(j + 1, k)), '</code>');
        i = k + 1; seg = i;
      } else i = j + 1;
      continue;
    }
    if (ch === '*') {
      if (j + 1 < hi && s[j + 1] === '*') {
        const k = findUnescaped(s, '**', j + 2, hi);
        if (k > j + 2) {
          if (j > seg) out.push(escapeHtml(s.slice(seg, j)));
          out.push('<strong>');
          inlineInto(s, env, out, j + 2, k);
          out.push('</strong>');
          i = k + 2; seg = i;
        } else i = j + 2;
        continue;
      }
      const k = findSingleStar(s, j + 1, hi);
      if (k > j + 1) {
        if (j > seg) out.push(escapeHtml(s.slice(seg, j)));
        out.push('<em>');
        inlineInto(s, env, out, j + 1, k);
        out.push('</em>');
        i = k + 1; seg = i;
      } else i = j + 1;
      continue;
    }
    // '['
    const k = findUnescaped(s, ']', j + 1, hi);
    let m = -1;
    if (k !== -1 && k + 1 < hi && s[k + 1] === '(') { const mm = s.indexOf(')', k + 2); if (mm !== -1 && mm < hi) m = mm; }
    const target = m === -1 ? '' : s.slice(k + 2, m);
    if (target !== '' && !/\s/.test(target)) {
      if (j > seg) out.push(escapeHtml(s.slice(seg, j)));
      out.push('<a href="', escapeHtml(env.link(target)), '">');
      inlineInto(s, env, out, j + 1, k);
      out.push('</a>');
      i = m + 1; seg = i;
    } else i = j + 1;
  }
  if (seg < hi) out.push(escapeHtml(s.slice(seg, hi)));
}

/** Render src: {html, toc, excerpt}. Fused lex+render, one out array. */

/** Render src: {html, toc, excerpt}. Range-based (no split): lines are
 *  (start,end) pairs into src, and inline text is rendered straight off src
 *  whenever no line needs trimming, so no paragraph/list text string is
 *  built in the common case. */
function renderMarkdown(src, env) {
  // Line starts (numbers only): line k is src[starts[k], lineB(k)).
  const starts = [0];
  let p = 0;
  while (true) {
    const nl = src.indexOf('\n', p);
    if (nl === -1) break;
    starts.push(nl + 1);
    p = nl + 1;
  }
  const n = starts.length;
  const lineB = k => (k + 1 < n ? starts[k + 1] - 1 : src.length);
  const out = [];
  const toc = [];
  const ids = new Set();
  let excerpt = null;
  let i = 0;

  // Inline-render the trimmed lines (ranges into src) joined by '\n'.
  // When nothing needs trimming the text is one src slice, so no string is
  // built at all; only a line that really needs trimming pays for a join.
  const inlineText = (ranges) => {
    if (ranges.length === 1) {
      let a = ranges[0][0], b = ranges[0][1];
      while (a < b && isSpaceTab(src.charCodeAt(a))) a++;
      while (b > a && isSpaceTab(src.charCodeAt(b - 1))) b--;
      inlineInto(src, env, out, a, b);
      return;
    }
    let needsTrim = false;
    for (let k = 0; k < ranges.length; k++) {
      const a = ranges[k][0], b = ranges[k][1];
      if (a < b && (isSpaceTab(src.charCodeAt(a)) || isSpaceTab(src.charCodeAt(b - 1)))) { needsTrim = true; break; }
    }
    if (!needsTrim) inlineInto(src, env, out, ranges[0][0], ranges[ranges.length - 1][1]);
    else {
      const parts = [];
      for (let k = 0; k < ranges.length; k++) parts.push(trimSlice(src, ranges[k][0], ranges[k][1]));
      const text = parts.join('\n');
      inlineInto(text, env, out, 0, text.length);
    }
  };

  // A list starting at lines[i] (isOrdered, start, first item range).
  const doList = (isOrdered, start, firstA, firstB) => {
    const items = [[[firstA, firstB]]];
    i++;
    while (i < n) {
      const a = starts[i], b = lineB(i);
      let mA = -1, mB = -1;
      if (isOrdered) {
        let j = a;
        while (j < b && src.charCodeAt(j) >= 48 && src.charCodeAt(j) <= 57) j++;
        if (j - a >= 1 && j - a <= 9 && src.charCodeAt(j) === 46 && src.charCodeAt(j + 1) === 32) { mA = j + 2; mB = b; }
      } else if ((src.charCodeAt(a) === 45 || src.charCodeAt(a) === 42) && src.charCodeAt(a + 1) === 32) { mA = a + 2; mB = b; }
      if (mA !== -1) { items.push([[mA, mB]]); i++; }
      else if (isContinuedR(src, a, b)) { items[items.length - 1].push([a, b]); i++; }
      else break;
    }
    const tag = isOrdered ? 'ol' : 'ul';
    out.push(isOrdered && start !== 1 ? `<ol start="${start}">` : `<${tag}>`, '\n');
    for (let k = 0; k < items.length; k++) {
      out.push('<li>');
      inlineText(items[k]);
      out.push('</li>\n');
    }
    out.push(`</${tag}>\n`);
  };

  while (i < n) {
    const a = starts[i], b = lineB(i);
    if (a === b) { i++; continue; }
    const c0 = src.charCodeAt(a);
    if (c0 === 32 || c0 === 9) {
      if (isBlankR(src, a, b)) { i++; continue; }
      // indented non-blank: a paragraph (no block starts indented)
    } else if (c0 === 96) { // '`'
      const lang = fenceLangR(src, a, b);
      if (lang !== null) {
        const cls = lang === '' ? '' : ` class="language-${escapeHtml(lang)}"`;
        out.push(`<pre><code${cls}>`);
        i++;
        const fs = i;
        while (i < n) {
          if (isFenceCloseR(src, starts[i], lineB(i))) break;
          i++;
        }
        // One escape over the whole body: escaping never touches '\n', so it
        // equals escaping each line and adding '\n' after each of them.
        if (i > fs) out.push(escapeHtml(src.slice(starts[fs], i < n ? starts[i] - 1 : src.length)), '\n');
        i++;
        out.push('</code></pre>\n');
        continue;
      }
    } else if (c0 === 35) { // '#'
      const heading = headingOfR(src, a, b);
      if (heading !== null) {
        const id = uniqueSlug(slugify(heading.text), ids);
        out.push(`<h${heading.depth} id="${id}">`);
        const mark = out.length;
        inlineInto(heading.text, env, out, 0, heading.text.length);
        const html = out.slice(mark).join('');
        out.push(`</h${heading.depth}>\n`);
        if (heading.depth === 2 || heading.depth === 3) toc.push({ level: heading.depth, id, html });
        i++;
        continue;
      }
    } else if (c0 === 45 || c0 === 42) { // '-' or '*'
      if (isHrR(src, a, b)) { out.push('<hr>\n'); i++; continue; }
      if (src.charCodeAt(a + 1) === 32) { doList(false, 1, a + 2, b); continue; }
    } else if (c0 === 62) { // '>'
      const body = [];
      while (i < n) {
        const la = starts[i], lb = lineB(i);
        if (src.charCodeAt(la) !== 62) break;
        body.push([src.charCodeAt(la + 1) === 32 ? la + 2 : la + 1, lb]);
        i++;
      }
      const kept = [];
      for (let k = 0; k < body.length; k++) { const t = trimSlice(src, body[k][0], body[k][1]); if (t !== '') kept.push(t); }
      if (kept.length === 0) out.push('<blockquote>\n</blockquote>\n');
      else { const text = kept.length === 1 ? kept[0] : kept.join('\n'); out.push('<blockquote>\n<p>'); inlineInto(text, env, out, 0, text.length); out.push('</p>\n</blockquote>\n'); }
      continue;
    } else if (c0 >= 48 && c0 <= 57) { // digit: maybe an ordered list
      let j = a;
      while (j < b && src.charCodeAt(j) >= 48 && src.charCodeAt(j) <= 57) j++;
      if (j - a >= 1 && j - a <= 9 && src.charCodeAt(j) === 46 && src.charCodeAt(j + 1) === 32) {
        doList(true, Number(src.slice(a, j)), j + 2, b);
        continue;
      }
    }
    // paragraph: continuation lines are those that do not end a paragraph
    // (blank / fence / heading / hr) -- the endsParagraph test, inlined.
    const body = [[a, b]];
    i++;
    while (i < n) {
      const la = starts[i], lb = lineB(i);
      if (la === lb) break;
      const c1 = src.charCodeAt(la);
      if (c1 === 32 || c1 === 9) { if (isBlankR(src, la, lb)) break; }
      else if (c1 === 96) { if (fenceLangR(src, la, lb) !== null) break; }
      else if (c1 === 35) { if (headingOfR(src, la, lb) !== null) break; }
      else if (c1 === 45 || c1 === 42) { if (isHrR(src, la, lb)) break; }
      body.push([la, lb]);
      i++;
    }
    out.push('<p>');
    const mark = out.length;
    inlineText(body);
    if (excerpt === null) excerpt = out.slice(mark).join('');
    out.push('</p>\n');
  }
  return { html: out.join(''), toc, excerpt: excerpt ?? '' };
}

// ---------------------------------------------------------------- templates

const INCLUDE_DEPTH = 8;
const PATH_RE = /^[A-Za-z_][A-Za-z0-9_]*(?:\.[A-Za-z_][A-Za-z0-9_]*)*$/;
const FOR_RE = /^for ([A-Za-z_][A-Za-z0-9_]*) in (\S+)$/;
const IF_RE = /^if (not )?(\S+)$/;
const INCLUDE_RE = /^include "([a-z0-9-]+)"$/;

class TemplateError extends Error {}

function pathOf(name, text) {
  if (!PATH_RE.test(text)) throw new TemplateError(`template "${name}": ${JSON.stringify(text)} is not a path`);
  return text.split('.');
}

function tokenize(name, src) {
  const tokens = [];
  let text = '';
  let i = 0;
  while (i < src.length) {
    const open = src.indexOf('{', i);
    if (open === -1 || open === src.length - 1) { text += src.slice(i); break; }
    const next = src[open + 1];
    if (next !== '{' && next !== '%') { text += src.slice(i, open + 1); i = open + 1; continue; }
    text += src.slice(i, open);
    if (next === '{') {
      const raw = src[open + 2] === '{';
      const close = src.indexOf(raw ? '}}}' : '}}', open + (raw ? 3 : 2));
      if (close === -1) throw new TemplateError(`template "${name}": a ${raw ? '{{{' : '{{'} is not closed`);
      if (text !== '') tokens.push({ t: 'text', text });
      text = '';
      tokens.push({ t: raw ? 'raw' : 'var', path: pathOf(name, src.slice(open + (raw ? 3 : 2), close).trim()) });
      i = close + (raw ? 3 : 2);
      continue;
    }
    const close = src.indexOf('%}', open + 2);
    if (close === -1) throw new TemplateError(`template "${name}": a {% is not closed`);
    const end = close + 2;
    const lineStart = src.lastIndexOf('\n', open - 1) + 1;
    const nl = src.indexOf('\n', end);
    const lineEnd = nl === -1 ? src.length : nl;
    const standalone = /^[ \t]*$/.test(src.slice(lineStart, open)) && /^[ \t]*$/.test(src.slice(end, lineEnd));
    if (standalone) text = text.slice(0, text.length - (open - lineStart));
    if (text !== '') tokens.push({ t: 'text', text });
    text = '';
    tokens.push({ t: 'tag', tag: src.slice(open + 2, close).trim() });
    i = standalone ? (nl === -1 ? src.length : nl + 1) : end;
  }
  if (text !== '') tokens.push({ t: 'text', text });
  return tokens;
}

function parseTemplate(name, src) {
  const root = { kind: 'root', body: [] };
  const stack = [root];
  let body = root.body;
  for (const tok of tokenize(name, src)) {
    if (tok.t === 'text') body.push({ kind: 'text', text: tok.text });
    else if (tok.t === 'var' || tok.t === 'raw') body.push({ kind: tok.t, path: tok.path });
    else {
      const tag = tok.tag;
      const top = stack[stack.length - 1];
      let m;
      if ((m = FOR_RE.exec(tag)) !== null) {
        const node = { kind: 'for', name: m[1], path: pathOf(name, m[2]), body: [] };
        body.push(node);
        stack.push(node);
        body = node.body;
      } else if ((m = IF_RE.exec(tag)) !== null) {
        const node = { kind: 'if', not: m[1] !== undefined, path: pathOf(name, m[2]), then: [], else: null };
        body.push(node);
        stack.push(node);
        body = node.then;
      } else if (tag === 'else') {
        if (top.kind !== 'if' || top.else !== null) throw new TemplateError(`template "${name}": an else outside an if`);
        top.else = [];
        body = top.else;
      } else if (tag === 'endfor' || tag === 'endif') {
        if (top.kind !== (tag === 'endfor' ? 'for' : 'if')) throw new TemplateError(`template "${name}": an ${tag} with no open ${tag.slice(3)}`);
        stack.pop();
        const parent = stack[stack.length - 1];
        body = parent.kind === 'root' ? parent.body : parent.kind === 'for' ? parent.body : parent.else ?? parent.then;
      } else if ((m = INCLUDE_RE.exec(tag)) !== null) body.push({ kind: 'include', name: m[1] });
      else throw new TemplateError(`template "${name}": {% ${tag} %} is not a tag`);
    }
  }
  if (stack.length > 1) throw new TemplateError(`template "${name}": a ${stack[stack.length - 1].kind} is not closed`);
  return root.body;
}

// Scope is a prototype chain of plain objects: the top scope holds site/tags/
// recent plus the render's vars, each for-loop adds one link (its element and
// `loop`). A first path name reads straight off the chain; later names are
// own properties of the value so far, exactly like the reference lookup.
function lookup(S, path) {
  let v = S[path[0]];
  for (let k = 1; k < path.length; k++) {
    if (v === null || typeof v !== 'object' || !Object.hasOwn(v, path[k])) return undefined;
    v = v[path[k]];
  }
  return v;
}

/** One later path segment: an own property of an object value, else undefined. */
function lk(v, key) {
  return v !== null && typeof v === 'object' && Object.hasOwn(v, key) ? v[key] : undefined;
}

function textOf(v) {
  if (typeof v === 'string') return v;
  if (typeof v === 'number') return String(v);
  if (typeof v === 'boolean') return v ? 'true' : 'false';
  if (Array.isArray(v)) return v.map(textOf).join(', ');
  return '';
}

function truthy(v) {
  if (Array.isArray(v)) return v.length > 0;
  return v !== undefined && v !== null && v !== false && v !== '' && v !== 0;
}

function renderNodes(nodes, S, ctx, depth, out) {
  for (let ni = 0; ni < nodes.length; ni++) {
    const n = nodes[ni];
    switch (n.kind) {
      case 'text': out.push(n.text); break;
      case 'var': out.push(escapeHtml(textOf(lookup(S, n.path)))); break;
      case 'raw': out.push(textOf(lookup(S, n.path))); break;
      case 'if': {
        const v = truthy(lookup(S, n.path));
        const branch = v !== n.not ? n.then : n.else;
        if (branch !== null) renderNodes(branch, S, ctx, depth, out);
        break;
      }
      case 'for': {
        const xs = lookup(S, n.path);
        if (!Array.isArray(xs)) break;
        for (let k = 0; k < xs.length; k++) {
          const S2 = Object.create(S);
          S2[n.name] = xs[k];
          S2.loop = { index: k + 1, first: k === 0, last: k === xs.length - 1, length: xs.length };
          renderNodes(n.body, S2, ctx, depth, out);
        }
        break;
      }
      case 'include': {
        const t = ctx.templates.get(n.name);
        if (t !== undefined && depth < INCLUDE_DEPTH) t(out, S, ctx, depth + 1);
        break;
      }
    }
  }
}

// ---- compiled templates: a parsed template becomes one JS function that
// appends straight to `out`, with direct scope-chain reads. Same semantics
// as renderNodes above; if compilation ever fails, the tree walker is used.

let __uid = 0;

function exprOf(path, scopeVar) {
  let e = `${scopeVar}[${JSON.stringify(path[0])}]`;
  for (let k = 1; k < path.length; k++) e = `lk(${e}, ${JSON.stringify(path[k])})`;
  return e;
}

function genNodes(nodes, scopeVar, out, deps, inFor) {
  for (const n of nodes) {
    if (n.kind === 'text') out.push(`out.push(${JSON.stringify(n.text)});`);
    else if (n.kind === 'var') out.push(`out.push(escHtml(${exprOf(n.path, scopeVar)}));`);
    else if (n.kind === 'raw') out.push(`out.push(textOf(${exprOf(n.path, scopeVar)}));`);
    else if (n.kind === 'if') {
      const cond = n.not ? `!truthy(${exprOf(n.path, scopeVar)})` : `truthy(${exprOf(n.path, scopeVar)})`;
      out.push(`if (${cond}) {`);
      genNodes(n.then, scopeVar, out, deps, inFor);
      if (n.else !== null) { out.push('} else {'); genNodes(n.else, scopeVar, out, deps, inFor); }
      out.push('}');
    } else if (n.kind === 'for') {
      const id = __uid++;
      const sv = `__s${id}`;
      out.push(`{ const __xs${id} = ${exprOf(n.path, scopeVar)}; if (Array.isArray(__xs${id})) { for (let __k${id} = 0; __k${id} < __xs${id}.length; __k${id}++) { const ${sv} = Object.create(${scopeVar}); ${sv}[${JSON.stringify(n.name)}] = __xs${id}[__k${id}]; ${sv}.loop = { index: __k${id} + 1, first: __k${id} === 0, last: __k${id} === __xs${id}.length - 1, length: __xs${id}.length };`);
      genNodes(n.body, sv, out, deps, true);
      out.push('} } }');
    } else if (n.kind === 'include') {
      // Memoize the include when its output depends only on the case globals
      // (constant per case) or on globals plus one scope value (keyed by it).
      const need = deps ? deps.get(n.name) : undefined;
      let mode = 0; // 0 direct, 1 const, 2 single-key
      let keyName = '';
      if (need !== undefined) {
        mode = 1;
        for (const d of need) {
          if (d === 'site' || d === 'tags' || d === 'recent') continue;
          if (d === 'loop') { if (inFor) { mode = 0; break; } continue; }
          if (keyName === '') keyName = d;
          else { mode = 0; break; }
        }
        if (mode === 1 && keyName !== '') mode = 2;
      }
      if (mode === 1) out.push(`if (depth < INCLUDE_DEPTH) out.push(__constInc(${JSON.stringify(n.name)}, ${scopeVar}, ctx, depth));`);
      else if (mode === 2) out.push(`if (depth < INCLUDE_DEPTH) out.push(__objInc(${JSON.stringify(n.name)}, ${scopeVar}[${JSON.stringify(keyName)}], ${scopeVar}, ctx, depth));`);
      else {
        const id = __uid++;
        out.push(`{ const __t${id} = ctx.templates.get(${JSON.stringify(n.name)}); if (__t${id} !== undefined && depth < INCLUDE_DEPTH) __t${id}(out, ${scopeVar}, ctx, depth + 1); }`);
      }
    }
  }
}

// First names a template needs from its caller's scope (transitively, minus
// names its own fors bind). Over-approximation only costs cache hits.
function collectDeps(nodes, bound, deps, out) {
  for (const n of nodes) {
    if (n.kind === 'var' || n.kind === 'raw' || n.kind === 'if') {
      if (!bound.has(n.path[0])) out.add(n.path[0]);
    } else if (n.kind === 'for') {
      if (!bound.has(n.path[0])) out.add(n.path[0]);
      const b2 = new Set(bound); b2.add(n.name); b2.add('loop');
      collectDeps(n.body, b2, deps, out);
    } else if (n.kind === 'include') {
      const d = deps.get(n.name);
      if (d !== undefined) for (const x of d) if (!bound.has(x)) out.add(x);
    }
  }
}

function computeDeps(parsed) {
  const deps = new Map();
  for (const name of parsed.keys()) deps.set(name, new Set());
  let changed = true;
  let iter = 0;
  while (changed && iter++ < 20) {
    changed = false;
    for (const [name, nodes] of parsed) {
      const set = deps.get(name);
      const before = set.size;
      collectDeps(nodes, new Set(), deps, set);
      if (set.size !== before) changed = true;
    }
  }
  return deps;
}

// Memoized include renders (per case, in ctx).
function __constInc(name, S, ctx, depth) {
  let byDepth = ctx.constCache.get(name);
  if (byDepth === undefined) { byDepth = new Map(); ctx.constCache.set(name, byDepth); }
  let s = byDepth.get(depth);
  if (s === undefined) {
    const o = [];
    const fn = ctx.templates.get(name);
    if (fn !== undefined) fn(o, S, ctx, depth + 1);
    s = o.join('');
    byDepth.set(depth, s);
  }
  return s;
}

function __objInc(name, key, S, ctx, depth) {
  let byKey = ctx.objCache.get(key);
  if (byKey === undefined) { byKey = new Map(); ctx.objCache.set(key, byKey); }
  let byName = byKey.get(name);
  if (byName === undefined) { byName = new Map(); byKey.set(name, byName); }
  let s = byName.get(depth);
  if (s === undefined) {
    const o = [];
    const fn = ctx.templates.get(name);
    if (fn !== undefined) fn(o, S, ctx, depth + 1);
    s = o.join('');
    byName.set(depth, s);
  }
  return s;
}

/** Compile parsed `nodes` to (out, S, ctx, depth) => void, or null. */
function compileTemplate(name, nodes, deps) {
  const parts = [];
  genNodes(nodes, 'S', parts, deps, false);
  const fnName = 'tpl_' + name.replace(/[^A-Za-z0-9_$]/g, '_');
  try {
    const factory = new Function('textOf', 'escapeHtml', 'escHtml', 'truthy', 'lk', 'INCLUDE_DEPTH', '__constInc', '__objInc',
      `return function ${fnName}(out, S, ctx, depth) {\n${parts.join('\n')}\n};`);
    return factory(textOf, escapeHtml, escHtml, truthy, lk, INCLUDE_DEPTH, __constInc, __objInc);
  } catch (e) {
    return null;
  }
}

// ---------------------------------------------------------------- build

const REPORT_FILE = 'build-report.txt';

function resolvePath(dir, rel) {
  const parts = dir === '' ? [] : dir.split('/');
  for (const seg of rel.split('/')) {
    if (seg === '' || seg === '.') continue;
    if (seg === '..') {
      if (parts.length === 0) return null;
      parts.pop();
    } else parts.push(seg);
  }
  return parts.join('/');
}

const dirOf = (path) => (path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : '');

function build(input) {
  slugCache = new Map();
  const templates = new Map();
  const report = [];
  const parsed = new Map();
  const templateNames = Object.keys(input.templates).sort(byCodeUnits);
  for (let ti = 0; ti < templateNames.length; ti++) {
    const name = templateNames[ti];
    try {
      parsed.set(name, parseTemplate(name, input.templates[name]));
    }
    catch (err) {
      if (!(err instanceof TemplateError)) throw err;
      report.push(err.message);
    }
  }
  const deps = computeDeps(parsed);
  for (const [name, nodes] of parsed) {
    const fn = compileTemplate(name, nodes, deps);
    templates.set(name, fn !== null ? fn : ((out, S, ctx, depth) => renderNodes(nodes, S, ctx, depth, out)));
  }
  const base = input.site.base_url.replace(/\/+$/, '');
  const b = {
    input, base, templates,
    sources: [], statics: [], bySource: new Map(),
    broken: new Set(), report, posts: [], tags: [], output: {},
  };

  // 1. sources, sorted by path; .md -> source (\r normalized), else static
  const files = [...input.files].sort((x, y) => byCodeUnits(x.path, y.path));
  for (let fi = 0; fi < files.length; fi++) {
    const f = files[fi];
    if (f.path.endsWith('.md')) {
      const text = f.content.indexOf('\r') === -1 ? f.content : f.content.replace(/\r\n?/g, '\n');
      b.sources.push({ path: f.path, text });
    } else b.statics.push({ path: f.path, content: f.content });
  }

  // 2. front matter
  const kept = [];
  for (let si = 0; si < b.sources.length; si++) {
    const s = b.sources[si];
    const fm = parseFrontMatter(s.text);
    if ('error' in fm) { b.report.push(`refused ${s.path}: ${fm.error}`); continue; }
    s.data = fm.data;
    s.body = fm.body;
    s.post = s.path.startsWith('posts/');
    let own;
    for (let d = 0; d < RESERVED.length; d++) {
      const dd = RESERVED[d];
      if (dd !== 'posts' && s.path.startsWith(`${dd}/`)) { own = dd; break; }
    }
    if (own !== undefined) { b.report.push(`refused ${s.path}: ${own}/ is the generator's own`); continue; }
    if (s.post && !Object.hasOwn(s.data, 'date')) { b.report.push(`refused ${s.path}: a post needs a date`); continue; }
    const layout = s.data.layout ?? (s.post ? 'post' : 'page');
    if (!b.templates.has(layout)) { b.report.push(`refused ${s.path}: layout "${layout}" is no template that parses`); continue; }
    s.layout = layout;
    if (s.data.draft === true) { b.report.push(`draft ${s.path}`); continue; }
    kept.push(s);
  }
  b.sources = kept;

  // 3. permalinks
  const taken = new Map();
  for (let si = 0; si < b.sources.length; si++) {
    const s = b.sources[si];
    const dir = s.post ? 'posts' : dirOf(s.path);
    let set = taken.get(dir);
    if (set === undefined) { set = new Set(dir === '' ? RESERVED : []); taken.set(dir, set); }
    const slug = uniqueSlug(s.data.slug ?? slugify(s.data.title), set);
    s.slug = slug;
    s.outDir = dir === '' ? slug : `${dir}/${slug}`;
    s.url = `${b.base}/${s.outDir}/`;
    b.bySource.set(s.path, s);
  }

  // 4. markdown + link resolution (link results cached per case)
  const staticPaths = new Set(b.statics.map((f) => f.path));
  const linkCaches = new Map();
  for (let si = 0; si < b.sources.length; si++) {
    const s = b.sources[si];
    const dir = dirOf(s.path);
    let linkCache = linkCaches.get(dir);
    if (linkCache === undefined) { linkCache = new Map(); linkCaches.set(dir, linkCache); }
    const env = {
      link: (target) => {
        const hit = linkCache.get(target);
        if (hit !== undefined) {
          if (hit === null) b.broken.add(`broken-link ${s.path} -> ${target}`);
          return hit === null ? target : hit;
        }
        let out;
        let scheme = false;
        const c0 = target.charCodeAt(0);
        if ((c0 >= 65 && c0 <= 90) || (c0 >= 97 && c0 <= 122)) {
          let j = 1;
          while (j < target.length) {
            const c = target.charCodeAt(j);
            if ((c >= 65 && c <= 90) || (c >= 97 && c <= 122) || (c >= 48 && c <= 57) || c === 43 || c === 45 || c === 46) j++;
            else break;
          }
          scheme = j < target.length && target.charCodeAt(j) === 58;
        }
        if (scheme || target.startsWith('/') || target.startsWith('#')) out = target;
        else {
          const hash = target.indexOf('#');
          const rel = hash === -1 ? target : target.slice(0, hash);
          const frag = hash === -1 ? '' : target.slice(hash);
          const path = rel === '' ? null : resolvePath(dir, rel);
          if (path !== null && b.bySource.has(path)) out = `${b.bySource.get(path).url}${frag}`;
          else if (path !== null && staticPaths.has(path)) out = `${b.base}/${path}${frag}`;
          else { b.broken.add(`broken-link ${s.path} -> ${target}`); linkCache.set(target, null); return target; }
        }
        linkCache.set(target, out);
        return out;
      },
    };
    const md = renderMarkdown(s.body, env);
    s.html = md.html;
    s.toc = md.toc;
    s.excerpt = md.excerpt;
  }

  // 5. collections
  const posts = b.sources.filter((s) => s.post);
  posts.sort((x, y) => byCodeUnits(y.data.date, x.data.date) || byCodeUnits(x.slug, y.slug));
  b.posts = posts;
  for (let k = 0; k < posts.length; k++) {
    posts[k].newer = k === 0 ? null : posts[k - 1];
    posts[k].older = k === posts.length - 1 ? null : posts[k + 1];
  }

  // 6. tags
  const bySlug = new Map();
  const ownTags = (s) => {
    const seen = new Set();
    const out = [];
    const names = s.data.tags ?? [];
    for (let i = 0; i < names.length; i++) {
      const name = names[i];
      const slug = slugify(name);
      if (!seen.has(slug)) { seen.add(slug); out.push(name); }
    }
    return out;
  };
  for (let pi = 0; pi < b.posts.length; pi++) {
    const p = b.posts[pi];
    p.tagRefs = [];
    const names = ownTags(p);
    for (let i = 0; i < names.length; i++) {
      const name = names[i];
      const slug = slugify(name);
      let tag = bySlug.get(slug);
      if (tag === undefined) { tag = { name, slug, url: `${b.base}/tags/${slug}/`, posts: [] }; bySlug.set(slug, tag); }
      tag.posts.push(p);
      p.tagRefs.push(tag);
    }
  }
  for (let si = 0; si < b.sources.length; si++) {
    const s = b.sources[si];
    if (s.post) continue;
    const names = ownTags(s);
    s.tagRefs = [];
    for (let i = 0; i < names.length; i++) {
      const name = names[i];
      const slug = slugify(name);
      s.tagRefs.push(bySlug.get(slug) ?? { name, slug, url: '', posts: [] });
    }
  }
  b.tags = [...bySlug.values()].sort((x, y) => y.posts.length - x.posts.length || byCodeUnits(x.slug, y.slug));

  // 7. layouts — the source object itself serves as its card/page: attach the
  // template-visible fields once, instead of building card/page objects.
  const linkOf = (s) => (s === null ? null : { title: s.data.title, url: s.url, date: s.data.date ?? '', date_long: s.data.date === undefined ? '' : longDate(s.data.date) });
  for (let si = 0; si < b.sources.length; si++) {
    const s = b.sources[si];
    s.title = s.data.title;
    s.date = s.data.date ?? '';
    s.date_long = s.data.date === undefined ? '' : longDate(s.data.date);
    s.summary = s.data.summary ?? '';
    s.reading_minutes = Math.max(1, Math.ceil(wordsOf(s.body) / 200));
    s.tags = s.tagRefs.map((t) => ({ name: t.name, slug: t.slug, url: t.url }));
    s.content = s.html;
    s.is_post = s.post;
    if (s.post) { s.newer = linkOf(s.newer); s.older = linkOf(s.older); }
    else { s.newer = null; s.older = null; }
  }
  const tagOf = (t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length, posts: t.posts });
  const site = { title: b.input.site.title, base_url: b.base };
  const allTags = b.tags.map((t) => ({ name: t.name, slug: t.slug, url: t.url, count: t.posts.length }));
  const recent = b.posts.slice(0, b.input.site.recent);
  const ctx = { templates: b.templates, constCache: new Map(), objCache: new Map() };
  const S = Object.create(null);
  S.site = site; S.tags = allTags; S.recent = recent;
  const render = (name) => {
    const fn = b.templates.get(name);
    if (fn === undefined) return null;
    const out = [];
    fn(out, S, ctx, 0);
    return out.join('');
  };
  for (let si = 0; si < b.sources.length; si++) {
    const s = b.sources[si];
    S.page = s; S.posts = undefined; S.pager = undefined; S.tag = undefined;
    b.output[`${s.outDir}/index.html`] = render(s.layout);
  }
  const per = b.input.site.per_page;
  const pages = Math.max(1, Math.ceil(b.posts.length / per));
  const indexUrl = (k) => (k === 1 ? `${b.base}/` : `${b.base}/page/${k}/`);
  for (let k = 1; k <= pages; k++) {
    S.page = undefined;
    S.posts = b.posts.slice((k - 1) * per, k * per);
    S.pager = { number: k, total: pages, url: indexUrl(k), newer_url: k === 1 ? '' : indexUrl(k - 1), older_url: k === pages ? '' : indexUrl(k + 1) };
    S.tag = undefined;
    const html = render('index');
    if (html !== null) b.output[k === 1 ? 'index.html' : `page/${k}/index.html`] = html;
  }
  for (let ti = 0; ti < b.tags.length; ti++) {
    const t = b.tags[ti];
    S.page = undefined; S.posts = undefined; S.pager = undefined; S.tag = tagOf(t);
    const html = render('tag');
    if (html !== null) b.output[`tags/${t.slug}/index.html`] = html;
  }
  S.page = undefined; S.posts = undefined; S.pager = undefined; S.tag = undefined;
  const tagsHtml = render('tags');
  if (tagsHtml !== null) b.output['tags/index.html'] = tagsHtml;
  for (let fi = 0; fi < b.statics.length; fi++) {
    const f = b.statics[fi];
    if (Object.hasOwn(b.output, f.path)) b.report.push(`shadowed ${f.path}: a built page has its path`);
    else b.output[f.path] = f.content;
  }

  // 8. report
  const lines = [...b.report, ...b.broken].sort(byCodeUnits);
  const pageCount = b.sources.filter((s) => !s.post).length;
  lines.push(`built ${pageCount} pages, ${b.posts.length} posts, ${b.tags.length} tags and ${b.statics.length} static files`);
  b.output[REPORT_FILE] = `${lines.join('\n')}\n`;
  return b.output;
}

export function run(input) {
  return build(input);
}
