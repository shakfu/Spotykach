// engine.test.ts - the per-engine page: the model, and the generated documentation it renders.
//
// Two halves, both testable without a browser. The model is a fetch behind a port, so "what does the
// page do while loading, on a bad link, or when the network fails" are ordinary assertions. The
// generated fragments are files on disk, so the safety property that lets them be injected with
// innerHTML is checkable directly rather than argued about.

import { readFileSync, readdirSync, statSync } from 'node:fs';
import { join } from 'node:path';

import { suite, test, ok, eq, layoutData, engineData } from './harness.ts';
import { makeLayout } from '../src/core/layout.ts';
import { makeCatalogue } from '../src/core/engines.ts';
import { EngineModel } from '../src/app/engine_model.ts';
import type { DocSource } from '../src/core/ports.ts';

suite('engine');

const WEB = new URL('..', import.meta.url).pathname;
const layout = makeLayout(layoutData());
const catalogue = makeCatalogue(engineData(), layout);

const fakeDocs = (body = '<p>hello</p>'): DocSource & { asked: string[] } => {
  const asked: string[] = [];
  return {
    asked,
    async fetchPage(path) {
      asked.push(path);
      return body;
    },
  };
};

// --- the model ------------------------------------------------------------------------------------

test('showing an engine loads its page and keeps its card format', async () => {
  const docs = fakeDocs();
  const m = new EngineModel(catalogue, docs);
  await m.show('tape');
  const s = m.store.get();
  eq(s.entry!.doc.name, 'tape');
  eq(s.entry!.bank!.engine, 'tape', 'the card bank travels with the page');
  eq(s.html, '<p>hello</p>');
  eq(docs.asked, ['engines/tape.html']);
});

test('an engine that reads no card still has a page', async () => {
  const m = new EngineModel(catalogue, fakeDocs());
  await m.show('delay');
  eq(m.store.get().entry!.bank, null);
  ok(m.store.get().html, 'the documentation is the point, card or no card');
});

test('a mistyped engine link names the engine it could not find', async () => {
  // The name usually came from a URL somebody else sent, so "not found" without it is useless.
  const m = new EngineModel(catalogue, fakeDocs());
  await m.show('bogus');
  ok(m.store.get().error!.includes('bogus'), m.store.get().error!);
  eq(m.store.get().entry, null);
});

test('a failed fetch is reported rather than leaving a blank page', async () => {
  const docs: DocSource = { fetchPage: async () => { throw new Error('HTTP 404'); } };
  const m = new EngineModel(catalogue, docs);
  await m.show('radio');
  eq(m.store.get().loading, false);
  ok(m.store.get().error!.includes('404'));
  ok(m.store.get().entry, 'the heading and format still show - only the prose failed');
});

test('a page is fetched once, however often it is opened', async () => {
  const docs = fakeDocs();
  const m = new EngineModel(catalogue, docs);
  await m.show('bard');
  await m.show('shuttle');
  await m.show('bard');
  eq(docs.asked, ['engines/bard.html', 'engines/shuttle.html'], 'the second bard came from cache');
});

test('a slow page cannot land under a different engine', async () => {
  // The failure this prevents: click bard, click shuttle before it lands, and read bard's manual
  // under shuttle's heading.
  let release: (v: string) => void = () => {};
  const slow: DocSource = {
    fetchPage: (path) => (path.includes('bard')
      ? new Promise<string>((r) => { release = r; })
      : Promise.resolve('<p>shuttle</p>')),
  };
  const m = new EngineModel(catalogue, slow);
  const pending = m.show('bard');
  await m.show('shuttle');
  release('<p>bard</p>');
  await pending;
  eq(m.store.get().entry!.doc.name, 'shuttle');
  eq(m.store.get().html, '<p>shuttle</p>', 'the late reply was discarded');
});

// --- the generated fragments ----------------------------------------------------------------------

const pages = readdirSync(join(WEB, 'engines')).filter((f) => f.endsWith('.html'));

test('every documented engine has a rendered page', () => {
  const documented = catalogue.entries.filter((e) => e.doc.page);
  eq(documented.length, pages.length);
  for (const e of documented) {
    ok(statSync(join(WEB, e.doc.page)).size > 0, `${e.doc.name}: empty page`);
  }
});

test('no raw tag survives from a source document', () => {
  // This is the property that lets the view use innerHTML at all. The generator escapes every piece
  // of source text before inserting a tag, so a `<script>` in a doc arrives as visible text.
  for (const f of pages) {
    const html = readFileSync(join(WEB, 'engines', f), 'utf8');
    ok(!/<\s*(script|iframe|object|embed|style)\b/i.test(html), `${f}: a dangerous element`);
    // Inside TAGS only. Scanning the whole document for `on\w+=` matches prose - "DoubleMono = left"
    // contains "ono =" - and a test that cries wolf on documentation is worse than no test.
    for (const tag of html.matchAll(/<[a-z][^>]*>/gi)) {
      ok(!/\son\w+\s*=/i.test(tag[0]), `${f}: event handler attribute in ${tag[0].slice(0, 60)}`);
      ok(!/javascript:/i.test(tag[0]), `${f}: javascript: URL in ${tag[0].slice(0, 60)}`);
    }
  }
});

test('a diagram resolves from the PAGE, not from the fragment', () => {
  // The bug this pins, which shipped and which the first version of this test could not see: these
  // fragments are fetched and injected into web/index.html, and a browser resolves relative URLs in
  // injected markup against the DOCUMENT's URL - not against the file the markup came from. So a
  // `media/x.svg` written relative to the fragment is requested as web/media/x.svg and 404s.
  //
  // The old assertion joined WEB + 'engines' + src, resolving it the way the fragment sees it, so it
  // agreed with the bug. Resolving against WEB is what the browser actually does.
  let seen = 0;
  for (const f of pages) {
    const html = readFileSync(join(WEB, 'engines', f), 'utf8');
    for (const m of html.matchAll(/<img src="([^"]+)"/g)) {
      seen++;
      ok(!m[1].startsWith('../'), `${f}: ${m[1]} points outside web/`);
      ok(!/^(https?:|\/)/.test(m[1]), `${f}: ${m[1]} should be a local, page-relative path`);
      ok(statSync(join(WEB, m[1])).size > 0,
        `${f}: ${m[1]} does not exist relative to index.html - the browser will 404`);
    }
  }
  ok(seen > 10, `only ${seen} diagrams found; the engine docs reference far more`);
});

test('a link to a sibling engine stays in the app', () => {
  // Leaving the tool to read about another engine of the same tool is a round trip for nothing.
  const all = pages.map((f) => readFileSync(join(WEB, 'engines', f), 'utf8')).join('\n');
  ok(all.includes('href="#engine/'), 'no sibling-engine link was rewritten');
  ok(!/href="\.\.\/engines\/\w+\.md"/.test(all), 'a raw sibling .md link survived');
});

test('links to docs that are not shipped go to the repository', () => {
  const all = pages.map((f) => readFileSync(join(WEB, 'engines', f), 'utf8')).join('\n');
  ok(all.includes('https://github.com/shakfu/sk-engines/blob/main/docs/'), 'no doc link was rewritten');
  ok(!/href="\.\.\/dev\//.test(all), 'a relative ../dev/ link survived and would 404');
});

test('no markdown survives into a page', () => {
  // Outside code blocks, an unconverted construct is a bug the reader sees as literal punctuation.
  for (const f of pages) {
    const html = readFileSync(join(WEB, 'engines', f), 'utf8')
      .replace(/<pre>[\s\S]*?<\/pre>/g, '')
      .replace(/<code>[\s\S]*?<\/code>/g, '');
    ok(!/\*\*/.test(html), `${f}: literal ** left in the output`);
    ok(!/^\s*\|/m.test(html), `${f}: an unconverted table row`);
    ok(!/\[[^\]]+\]\([^)]+\)/.test(html), `${f}: an unconverted link`);
  }
});

test('every diagram image carries its real dimensions', () => {
  // D2 emits `<svg viewBox="...">` with no width/height, which inside an <img> is an intrinsic RATIO
  // and no intrinsic SIZE - the reason the diagrams were invisible at first. The exporter reads the
  // viewBox and puts the numbers on the tag, which fixes it at the source AND is what lets the viewer
  // offer a true "actual size": natural size has no meaning until the natural size is known.
  let checked = 0;
  for (const f of pages) {
    const html = readFileSync(join(WEB, 'engines', f), 'utf8');
    for (const m of html.matchAll(/<img src="(engines\/media\/[^"]+)"[^>]*>/g)) {
      const tag = m[0];
      const w = Number(tag.match(/\swidth="(\d+)"/)?.[1]);
      const h = Number(tag.match(/\sheight="(\d+)"/)?.[1]);
      ok(w > 100 && h > 100, `${f}: ${m[1]} has no usable dimensions (${tag})`);
      // Cross-check against the file itself, so a wrong number is caught rather than any number.
      const box = readFileSync(join(WEB, m[1]), 'utf8').slice(0, 2000)
        .match(/viewBox="\s*[-\d.]+\s+[-\d.]+\s+([\d.]+)\s+([\d.]+)/);
      ok(box, `${m[1]}: no viewBox to check against`);
      eq(w, Math.trunc(Number(box![1])), `${m[1]}: width disagrees with its viewBox`);
      eq(h, Math.trunc(Number(box![2])), `${m[1]}: height disagrees with its viewBox`);
      checked++;
    }
  }
  ok(checked > 10, `only ${checked} diagrams checked`);
});

test('a diagram is openable at its own size', () => {
  // At any width this page can offer, a 4000-unit-wide control diagram scales to about a third and
  // its labels stop being readable - visible but useless. The inline copy is a thumbnail; the link
  // is how it is actually read.
  let linked = 0;
  for (const f of pages) {
    const html = readFileSync(join(WEB, 'engines', f), 'utf8');
    for (const fig of html.matchAll(/<figure>([\s\S]*?)<\/figure>/g)) {
      const body = fig[1];
      const src = body.match(/<img src="([^"]+)"/)?.[1];
      if (!src) continue;
      linked++;
      ok(body.includes(`<a href="${src}"`), `${f}: the diagram is not linked to itself`);
      ok(/<figcaption>/.test(body), `${f}: no caption saying where the readable copy is`);
    }
  }
  ok(linked > 10, `only ${linked} linked diagrams`);
});

test('the reading measure is on the prose, not on the column', () => {
  // Capping .engine-doc capped the diagrams and the wide control tables with it. A paragraph wants
  // 88 characters; a diagram wants every pixel there is.
  //
  // Asserted against the BUILT stylesheet, and that is the point of this version. Written against the
  // source it passed while emitting the wrong number: `max-w-prose` reads like the right utility but
  // Tailwind ships it hardcoded to 65ch, and a same-named `--container-prose` token does not override
  // it - it is shadowed, silently. Only the compiled value catches that.
  const css = readFileSync(new URL('../dist/app.css', import.meta.url), 'utf8');
  ok(/\.engine-doc\{[^}]*max-width:\s*none/.test(css), 'the doc column must not be capped');
  const prose = css.match(/\.engine-doc>p[^{]*\{([^}]*)\}/);
  ok(prose, 'the prose elements have a rule at all');
  const width = prose![1].match(/max-width:\s*([^;}]+)/)?.[1] ?? '';
  const resolved = width.startsWith('var(')
    ? css.match(new RegExp(`${width.slice(4, -1)}:\\s*([^;}]+)`))?.[1] ?? ''
    : width;
  eq(resolved.trim(), '88ch', 'the doc measure must be 88ch, not Tailwind\'s 65ch prose default');
});

test('a diagram opens in the viewer, and still works without it', () => {
  // The generated markup wraps each diagram in a link to the file. That link is the answer when
  // scripting is unavailable; the viewer intercepts it as an enhancement rather than replacing it,
  // which is why the href stays a real path and not a `#`.
  const view = readFileSync(new URL('../src/ui/engine_view.ts', import.meta.url), 'utf8');
  ok(view.includes("closest?.('figure a')"), 'clicks on a figure are delegated');
  ok(view.includes('e.preventDefault()'), 'and the navigation is intercepted');
  ok(view.includes('lightbox.open('), 'to open the viewer instead');

  const box = readFileSync(new URL('../src/ui/lightbox.ts', import.meta.url), 'utf8');
  ok(box.includes("el('dialog'"), 'built on <dialog>: Escape and the focus trap come from the platform');
  ok(box.includes('showModal'), 'opened modally');
  ok(/e\.target === dialog/.test(box), 'clicking the backdrop closes it');
  ok(box.includes('Actual size') && box.includes('Fit to window'),
    'both views are offered - Fit shows the shape, Actual shows what the labels say');
});

test('every diagram offers a print-ready PDF', () => {
  // A control surface is a thing people print and pin next to the hardware; the SVG is for the
  // screen. The link lives in the caption as well as the viewer, so it works with scripting off.
  let offered = 0;
  for (const f of pages) {
    const html = readFileSync(join(WEB, 'engines', f), 'utf8');
    for (const fig of html.matchAll(/<figure>([\s\S]*?)<\/figure>/g)) {
      const src = fig[1].match(/<img src="([^"]+)"/)?.[1];
      if (!src) continue;
      const pdf = fig[1].match(/href="([^"]+\.pdf)"\s+download/)?.[1];
      ok(pdf, `${f}: ${src} has no PDF offered`);
      eq(pdf, src.replace(/\.svg$/, '.pdf'), `${f}: the PDF is not this diagram's`);
      ok(statSync(join(WEB, pdf!)).size > 5000, `${f}: ${pdf} is missing or suspiciously small`);
      offered++;
    }
  }
  ok(offered > 10, `only ${offered} PDFs offered`);
});

test('the PDF is vector text, not a picture of a diagram', () => {
  // The point of a PDF here is printing, and a rasterised one prints badly and cannot be searched.
  // d2's own PDF export drives a headless Chromium it downloads at runtime - which 404s - so these
  // come from librsvg instead, and this asserts that swap did not cost the text.
  const pdf = readFileSync(join(WEB, 'engines/media/pstretch-controls.pdf'));
  const head = pdf.toString('latin1');
  ok(head.startsWith('%PDF-'), 'not a PDF at all');
  const fonts = head.split('/Font').length - 1;
  ok(fonts > 5, `only ${fonts} font objects - the labels may have been rasterised`);
});

test('the caption download link is not swallowed by the viewer', () => {
  // Both links live inside the same <figure>. Intercepting every one of them would turn "download
  // PDF" into "open the picture again".
  const view = readFileSync(new URL('../src/ui/engine_view.ts', import.meta.url), 'utf8');
  ok(view.includes('!link.querySelector(\'img\')'),
    'the click handler must only intercept the link wrapping the diagram');
});
