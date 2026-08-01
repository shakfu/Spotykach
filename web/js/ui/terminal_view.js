// terminal_view.js - the WebSerial terminal.
//
// A serial monitor would be a weekend's work and worth very little. What makes this worth building is
// that the protocol is SELF-DESCRIBING: `describe` returns the platform tables and the engine's
// liveness mask, so the control surface is GENERATED - a slider per advertised param with its real
// range, buttons for the pads this engine implements, and nothing at all for the 24-id enum entries it
// ignores. That is the same descriptor tools/test_generic.py drives its hardware sweep from, so the
// semantics are already proven.
//
// One thing to be honest about up front, and the page says so rather than letting a user discover it:
// released firmware has no terminal. scripts/build_release.py runs `make ENGINE=<e> SPK_VERSION=<v>`
// and nothing else - no TERMINAL=1 - so every binary in dist/ lacks the command channel. Until that
// firmware decision is made, this tab serves people who build their own images.

import { el, $, clear, confirmDestructive, showError } from './dom.js';
import * as serial from '../terminal/serial.js';
import { Device } from '../terminal/device.js';
import { parseDescribe, vocabulary, parseUsbDiag } from '../terminal/descriptor.js';
import { isDestructive, CommandError, Timeout } from '../terminal/framing.js';

const CPU_HISTORY = 240; // samples kept in the plot

export function mountTerminal(root, ctx) {
  let device = null;
  let descriptor = null;
  let cpuTimer = null;
  const cpuHistory = [];

  const log = el('div', { class: 'console' });
  const status = el('div', { class: 'status' });
  const surface = el('div', { class: 'surface' });
  const cpuPanel = el('div', { class: 'cpu' });
  const usbPanel = el('div', { class: 'usb' });
  const canvas = el('canvas', { class: 'plot', width: 720, height: 160 });

  const input = el('input', {
    type: 'text',
    class: 'cmdline',
    placeholder: 'type a command, e.g. query cpu   (Tab completes, Up recalls)',
    autocomplete: 'off',
    spellcheck: false,
    disabled: true,
  });

  const history = [];
  let historyPos = 0;

  function write(text, cls = '') {
    const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
    log.append(el('div', { class: `line ${cls}` }, text));
    while (log.childElementCount > 500) log.firstElementChild.remove();
    if (atBottom) log.scrollTop = log.scrollHeight;
  }

  // --- connection -----------------------------------------------------------

  async function connect() {
    try {
      const transport = await serial.requestPort();
      device = new Device(transport, { logSink: (l) => write(l, 'log') });
      status.textContent = `connected (${transport.info()})`;
      connectBtn.textContent = 'Disconnect';
      input.disabled = false;
      input.focus();
      write('connected', 'meta');
      await refreshDescribe();
      await refreshUsb();
      startCpu();
    } catch (e) {
      if (e.name === 'NotFoundError') return; // the user closed the port chooser
      showError(usbPanel, e);
      status.textContent = '';
    }
  }

  async function disconnect() {
    stopCpu();
    try {
      await device?.close();
    } catch {
      /* the port may already be gone */
    }
    device = null;
    descriptor = null;
    clear(surface);
    clear(usbPanel);
    input.disabled = true;
    connectBtn.textContent = 'Connect';
    status.textContent = 'disconnected';
    write('disconnected', 'meta');
  }

  const connectBtn = el('button', {
    class: 'primary',
    onclick: () => (device ? disconnect() : connect()),
  }, 'Connect');

  // --- command line ---------------------------------------------------------

  async function send(line, { quiet = false } = {}) {
    if (!device) return null;
    if (isDestructive(line) && !confirmDestructive(`Send "${line}"?`)) {
      write(`cancelled: ${line}`, 'meta');
      return null;
    }
    if (!quiet) write(`> ${line}`, 'sent');
    try {
      const reply = await device.cmd(line);
      if (!quiet) write(reply === '' ? 'ok' : `ok ${reply}`, 'ok');
      return reply;
    } catch (e) {
      if (!quiet) {
        write(e instanceof CommandError ? `err ${e.reason}`
          : e instanceof Timeout ? 'timeout - no reply' : String(e), 'err');
      }
      return null;
    }
  }

  input.addEventListener('keydown', async (e) => {
    if (e.key === 'Enter') {
      const line = input.value.trim();
      if (!line) return;
      history.push(line);
      historyPos = history.length;
      input.value = '';
      await send(line);
      if (/^(config|set param|reset|preset)\b/.test(line)) refreshSurface();
    } else if (e.key === 'ArrowUp') {
      e.preventDefault();
      if (historyPos > 0) input.value = history[--historyPos] ?? '';
    } else if (e.key === 'ArrowDown') {
      e.preventDefault();
      historyPos = Math.min(historyPos + 1, history.length);
      input.value = history[historyPos] ?? '';
    } else if (e.key === 'Tab') {
      e.preventDefault();
      complete();
    }
  });

  /** Completion from `describe`'s own vocabulary, as tools/skterm.py does against the same list. */
  function complete() {
    if (!descriptor) return;
    const words = vocabulary(descriptor);
    const parts = input.value.split(' ');
    const prefix = parts.at(-1);
    if (!prefix) return;
    const hits = words.filter((w) => w.startsWith(prefix));
    if (!hits.length) return;
    if (hits.length === 1) {
      parts[parts.length - 1] = hits[0];
      input.value = `${parts.join(' ')} `;
    } else {
      write(hits.join('  '), 'meta');
    }
  }

  // --- generated control surface --------------------------------------------

  async function refreshDescribe() {
    clear(surface);
    const lines = await device.describeLines();
    descriptor = parseDescribe(lines);
    renderSurface();
  }

  function renderSurface() {
    clear(surface);
    if (!descriptor) return;
    surface.append(el('h3', {}, `${descriptor.engine || 'engine'} `,
      el('span', { class: 'muted' }, descriptor.version)));

    if (!descriptor.masked) {
      // masked=0 means the engine never declared which ids it implements, so the descriptor is the
      // whole ParamId enum and most of these controls do nothing. Saying so beats rendering 24 dead
      // sliders and letting the user conclude the device is broken.
      surface.append(el('div', { class: 'callout warn' },
        el('strong', {}, 'masked=0: '),
        'this build does not declare which parameters it implements, so the list below is the whole '
        + 'enum. Some of these controls will have no effect.'));
    }

    const decks = ['A', 'B'];
    if (descriptor.params.size) {
      const grid = el('div', { class: 'grid' });
      for (const p of descriptor.params.values()) {
        for (const deck of p.scope === 'deck' ? decks : ['A']) {
          grid.append(paramRow(p, deck));
        }
      }
      surface.append(el('h4', {}, 'Parameters'), grid);
    }

    if (descriptor.configs.size) {
      const grid = el('div', { class: 'grid' });
      for (const c of descriptor.configs.values()) {
        const sel = el('select', {
          onchange: () => send(`config ${c.name} A ${sel.value}`),
        }, [...c.values.entries()].map(([v, label]) => el('option', { value: String(v) }, `${v} - ${label}`)));
        grid.append(el('div', { class: 'row' }, el('label', {}, c.name), sel));
      }
      surface.append(el('h4', {}, 'Configs'), grid);
    }

    // Actions are hand-listed rather than generated: the descriptor cannot express arity or whether a
    // verb is safe to call, which is exactly why docs/dev/terminal-target-b.md tags entries by safety
    // rather than by category. Anything destructive goes through confirmDestructive in send().
    const actions = el('div', { class: 'actions' });
    for (const deck of decks) {
      for (const [label, cmd] of [
        [`gate ${deck}`, `gate ${deck}`],
        [`play ${deck}`, `pad play ${deck}`],
        [`rec ${deck}`, `pad rec ${deck}`],
        [`stop ${deck}`, `pad stop ${deck}`],
        [`clear ${deck}`, `pad clear ${deck}`],
      ]) {
        actions.append(el('button', {
          class: isDestructive(cmd) ? 'danger' : '',
          onclick: () => send(cmd),
        }, label));
      }
    }
    surface.append(el('h4', {}, 'Actions'), actions);

    if (descriptor.queries.size) {
      const list = el('div', { class: 'grid' });
      for (const q of descriptor.queries.values()) {
        const value = el('span', { class: 'mono value' }, '-');
        list.append(el('div', { class: 'row' },
          el('label', {}, q.name),
          el('button', {
            onclick: async () => {
              const r = await send(`query ${q.name} ${q.scope === 'deck' ? 'A' : ''}`.trim(), { quiet: true });
              value.textContent = r == null ? 'err' : label(q, r);
            },
          }, 'read'),
          value));
      }
      surface.append(el('h4', {}, 'Queries'),
        el('p', { class: 'muted note' },
          'Every query here is safe to call in any order - the two that are not (`fit`, which takes an '
          + 'argument, and `reseed`, which self-clears when read) are deliberately absent from the '
          + 'descriptor.'),
        list);
    }
  }

  const label = (q, raw) => {
    if (q.kind === 'enum' && q.values.has(Number(raw))) return `${raw} (${q.values.get(Number(raw))})`;
    if (q.kind === 'bool') return raw === '1' ? 'yes' : 'no';
    return raw || 'ok';
  };

  function paramRow(p, deck) {
    const out = el('span', { class: 'mono value' }, '-');
    const slider = el('input', {
      type: 'range',
      min: String(p.lo),
      max: String(p.hi),
      step: String((p.hi - p.lo) / 1000),
      value: String((p.lo + p.hi) / 2),
    });
    const push = async () => {
      out.textContent = Number(slider.value).toPrecision(4);
      await send(`set param ${p.name} ${deck} ${slider.value}`, { quiet: true });
    };
    slider.addEventListener('input', () => {
      out.textContent = Number(slider.value).toPrecision(4);
    });
    slider.addEventListener('change', push);
    return el('div', { class: 'row' },
      el('label', {}, `${p.name}${p.scope === 'deck' ? ` ${deck}` : ''}`),
      slider,
      out,
      el('button', {
        class: 'link',
        onclick: async () => {
          const v = await send(`get param ${p.name} ${deck}`, { quiet: true });
          if (v != null) {
            slider.value = v;
            out.textContent = Number(v).toPrecision(4);
          }
        },
      }, 'read'));
  }

  const refreshSurface = () => descriptor && renderSurface();

  // --- CPU meter ------------------------------------------------------------
  //
  // The reason this is a plot and not three numbers: the P2 bench workflow in TODO.md is currently
  // "read the numbers repeatedly and notice whether max is still climbing". A rising max is the signal
  // that matters, and a plot answers convergence at a glance - which is precisely the question that
  // mattered for pstretch at 8192.

  function startCpu() {
    stopCpu();
    cpuTimer = setInterval(pollCpu, 500);
    pollCpu();
  }

  function stopCpu() {
    if (cpuTimer) clearInterval(cpuTimer);
    cpuTimer = null;
  }

  async function pollCpu() {
    if (!device) return;
    try {
      const { avg, min, max } = await device.cpu();
      cpuHistory.push(avg);
      while (cpuHistory.length > CPU_HISTORY) cpuHistory.shift();
      $('.cpu-readout', cpuPanel).textContent =
        `now ${avg.toFixed(1)}%   min ${min.toFixed(1)}%   max ${max.toFixed(1)}%`;
      drawPlot(max);
    } catch {
      // A build without TERMINAL=1 answers `err unknown-verb`; stop hammering it.
      stopCpu();
      $('.cpu-readout', cpuPanel).textContent = 'not available on this build';
    }
  }

  function drawPlot(max) {
    const ctx2 = canvas.getContext('2d');
    const { width: w, height: h } = canvas;
    const style = getComputedStyle(document.body);
    ctx2.clearRect(0, 0, w, h);
    const ceiling = Math.max(100, Math.ceil((max || 0) / 25) * 25);

    ctx2.strokeStyle = style.getPropertyValue('--grid') || '#333';
    ctx2.lineWidth = 1;
    ctx2.font = '10px ui-monospace, monospace';
    ctx2.fillStyle = style.getPropertyValue('--muted') || '#888';
    for (let pct = 0; pct <= ceiling; pct += 25) {
      const y = h - (pct / ceiling) * (h - 12) - 6;
      ctx2.beginPath();
      ctx2.moveTo(28, y);
      ctx2.lineTo(w, y);
      ctx2.stroke();
      ctx2.fillText(`${pct}%`, 2, y + 3);
    }

    if (cpuHistory.length > 1) {
      ctx2.strokeStyle = style.getPropertyValue('--accent') || '#4ea1ff';
      ctx2.lineWidth = 2;
      ctx2.beginPath();
      cpuHistory.forEach((v, i) => {
        const x = 28 + (i / (CPU_HISTORY - 1)) * (w - 30);
        const y = h - (v / ceiling) * (h - 12) - 6;
        if (i === 0) ctx2.moveTo(x, y);
        else ctx2.lineTo(x, y);
      });
      ctx2.stroke();
    }
    if (max != null) {
      const y = h - (max / ceiling) * (h - 12) - 6;
      ctx2.strokeStyle = style.getPropertyValue('--danger') || '#e05252';
      ctx2.setLineDash([4, 3]);
      ctx2.beginPath();
      ctx2.moveTo(28, y);
      ctx2.lineTo(w, y);
      ctx2.stroke();
      ctx2.setLineDash([]);
    }
  }

  // --- usb bring-up snapshot ------------------------------------------------

  async function refreshUsb() {
    clear(usbPanel);
    const reply = await send('query usb', { quiet: true });
    if (reply == null) {
      usbPanel.append(el('p', { class: 'muted' },
        '`query usb` is not available on this build (it needs USBDIAG=1).'));
      return;
    }
    const rows = parseUsbDiag(reply);
    usbPanel.append(el('table', { class: 'layout' },
      el('thead', {}, el('tr', {}, el('th', {}, 'Field'), el('th', {}, 'Value'))),
      el('tbody', {}, rows.map((r) => el('tr', {},
        el('td', { class: 'mono' }, r.key),
        el('td', { class: 'mono' }, r.value))))));
  }

  // --- layout ---------------------------------------------------------------

  cpuPanel.append(
    el('div', { class: 'controls' },
      el('span', { class: 'cpu-readout mono' }, '-'),
      el('button', {
        onclick: async () => {
          // `reset cpu` only clears the meter's min/max extremes - it touches no params, so it is not
          // in the destructive list even though it shares the `reset` verb.
          await send('reset cpu');
          cpuHistory.length = 0;
        },
      }, 'reset cpu'),
      el('button', { onclick: () => (cpuTimer ? stopCpu() : startCpu()) }, 'start / stop polling')),
    canvas,
    el('p', { class: 'muted note' },
      'min and max are extremes since the last reset, not a rolling window. The sequence a measurement '
      + 'wants is: reset, drive the engine, then watch whether max stops climbing.'));

  root.append(
    el('div', { class: 'callout warn' },
      el('strong', {}, 'Released firmware has no terminal. '),
      'scripts/build_release.py never passes TERMINAL=1, so every binary in dist/ lacks the command '
      + 'channel and this tab will find nothing to talk to. It works against a build you make yourself '
      + 'with ',
      el('code', {}, 'make ENGINE=<engine> TERMINAL=1'),
      '. Note USB MIDI is lost on the QSPI engines (chuck, csound, mosc), which claim the same OTG core.'),
    el('div', { class: 'controls' }, connectBtn, status),
    !serial.supported() && el('div', { class: 'callout' },
      el('strong', {}, 'This browser has no WebSerial. '),
      'Talking to hardware needs Chrome or Edge. Unlike the card tabs there is no fallback for this '
      + 'one - there is no zip-shaped substitute for a serial port.'),
    el('h3', {}, 'Console'),
    log,
    input,
    el('h3', {}, 'CPU load'),
    cpuPanel,
    el('h3', {}, 'Control surface'),
    el('p', { class: 'muted note' },
      'Generated from the device\'s own `describe` reply - every control below is one this build '
      + 'actually advertises. Destructive verbs ask first.'),
    surface,
    el('h3', {}, 'USB bring-up'),
    usbPanel,
  );

  drawPlot(null);
  return { dispose: disconnect };
}
