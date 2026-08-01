// descriptor.js - the parsed introspection model for `describe`.
//
// A port of tools/skdev/descriptor.py. This is what makes a web terminal worth more than a serial
// monitor: `describe` reports the engine's own control surface - every parameter with its real range
// and deck scope, every config with its enum labels, the query vocabulary, and a capability mask - so
// the UI can be GENERATED rather than hard-coded. A slider per advertised param, buttons for the pads
// the engine implements, and nothing at all for the enum entries this engine ignores.
//
// Wire format (one item per line, terminated by a bare `end`, which the caller strips):
//
//     descr engine=<name> version=<ver> masked=<0|1>
//     param  <name> <deck|global> <lo>..<hi>
//     config <name> [scope] <int>:<label> ...
//     query  <name> <deck|global> [kind] [int:label ...]
//     caps   0x<hex>
//     end
//
// Unknown tags are ignored rather than rejected, so firmware that adds a line type does not break an
// older page.

/** @typedef {{name: string, scope: string, lo: number, hi: number}} ParamDesc */
/** @typedef {{name: string, values: Map<number,string>}} ConfigDesc */
/** @typedef {{name: string, scope: string, kind: string, values: Map<number,string>}} QueryDesc */

/**
 * Parse a describe block.
 * @param {string[]} lines  log lines already filtered out, `end` not required to be present
 */
export function parseDescribe(lines) {
  const d = {
    engine: '',
    version: '',
    // masked=1 means the engine declared which ids it actually implements. With masked=0 the
    // descriptor is the whole ParamId enum, so a generated surface would show controls the engine
    // ignores - worth flagging in the UI rather than silently rendering dead sliders.
    masked: false,
    /** @type {Map<string,ParamDesc>} */ params: new Map(),
    /** @type {Map<string,ConfigDesc>} */ configs: new Map(),
    /** @type {Map<string,QueryDesc>} */ queries: new Map(),
    caps: 0,
  };
  for (const line of lines) {
    const tok = line.trim().split(/\s+/).filter(Boolean);
    if (!tok.length) continue;
    switch (tok[0]) {
      case 'descr': {
        const kv = new Map(tok.slice(1).filter((t) => t.includes('=')).map((t) => {
          const i = t.indexOf('=');
          return [t.slice(0, i), t.slice(i + 1)];
        }));
        d.engine = kv.get('engine') || '';
        d.version = kv.get('version') || '';
        d.masked = kv.get('masked') === '1';
        break;
      }
      case 'param': { // param <name> <scope> <lo>..<hi>
        if (tok.length < 4) break;
        const [lo, hi] = tok[3].split('..').map(Number);
        d.params.set(tok[1], { name: tok[1], scope: tok[2], lo, hi });
        break;
      }
      case 'config': { // config <name> [scope] i:label i:label ...
        if (tok.length < 2) break;
        d.configs.set(tok[1], { name: tok[1], values: enumValues(tok.slice(2)) });
        break;
      }
      case 'query': { // query <name> <scope> [kind] [i:label ...]
        if (tok.length < 2) break;
        d.queries.set(tok[1], {
          name: tok[1],
          scope: tok.length > 2 ? tok[2] : 'global',
          kind: tok.length > 3 ? tok[3] : 'text',
          values: enumValues(tok.slice(4)),
        });
        break;
      }
      case 'caps':
        if (tok.length > 1) d.caps = parseInt(tok[1], 16) || 0;
        break;
      default:
        break; // forward-compatible: a tag this page does not know is not an error
    }
  }
  return d;
}

/** `0:slice 1:reel 2:drift` -> Map. Non-`int:label` tokens (an optional scope) are skipped. */
function enumValues(tokens) {
  const out = new Map();
  for (const t of tokens) {
    const i = t.indexOf(':');
    if (i < 0) continue;
    const k = Number(t.slice(0, i));
    if (Number.isInteger(k)) out.set(k, t.slice(i + 1));
  }
  return out;
}

/**
 * The command vocabulary a descriptor implies, for console completion. Mirrors what tools/skterm.py
 * completes against, so the two front-ends offer the same words.
 */
export function vocabulary(desc) {
  const words = new Set([
    'set', 'get', 'param', 'config', 'query', 'cv', 'gate', 'midi', 'pad', 'seq', 'fx',
    'reset', 'preset', 'caps', 'mode', 'describe', 'help', 'note', 'msg', 'transport',
    'test', 'run', 'save', 'load', 'play', 'rec', 'stop', 'clear', 'trig', 'arm', 'disarm',
    'flux', 'grit', 'lock', 'gritmode', 'voct', 'mix', 'size', 'xfade', 'cpu', 'A', 'B',
  ]);
  for (const k of desc.params.keys()) words.add(k);
  for (const k of desc.configs.keys()) words.add(k);
  for (const k of desc.queries.keys()) words.add(k);
  return [...words].sort();
}

/**
 * Parse the `query usb` reply - a flag soup like
 * `boot=1 region=2 clkcfg=1 hsi48=1 usbsel=0 usb33den=1 usb33rdy=1 phy=1 pullup=1` - into ordered
 * pairs, so the UI can render the UsbDiag bring-up snapshot as a table rather than a wall of text.
 */
export function parseUsbDiag(reply) {
  return reply.trim().split(/\s+/).filter((t) => t.includes('=')).map((t) => {
    const i = t.indexOf('=');
    return { key: t.slice(0, i), value: t.slice(i + 1) };
  });
}
