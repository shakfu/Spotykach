// terminal_view.ts - the WebSerial terminal.
//
// A serial monitor would be a weekend's work and worth very little. What makes this worth building is
// that the protocol is SELF-DESCRIBING: `describe` returns the platform tables and the engine's
// liveness mask, so the control surface is GENERATED - a slider per advertised param with its real
// range, buttons for the pads this engine implements, and nothing at all for the 24-id enum entries it
// ignores. That is the same descriptor tools/test_generic.py drives its hardware sweep from, so the
// semantics are already proven.
//
// One thing to be honest about up front, and the page says so rather than letting a user discover it:
// released firmware has no terminal, so this tab serves people who build their own images.

import { TerminalModel, type ConsoleLine } from '../app/terminal_model.ts';
import { isDestructive, vocabulary, type ParamDesc, type QueryDesc } from '../core/protocol.ts';
import { webSerial } from '../platform/serial.ts';
import { browserClock } from '../platform/clock.ts';
import { append, aside, clear, confirmDestructive, el } from './dom.ts';
import { drawCpuPlot } from './cpu_plot.ts';
import type { ViewContext } from './context.ts';

export function mountTerminal(root: HTMLElement, _ctx: ViewContext): void {
  const model = new TerminalModel({
    serial: webSerial, clock: browserClock, confirm: confirmDestructive,
  });

  const log = el('div', { class: 'console' });
  const status = el('div', { class: 'status' });
  const surface = el('div', { class: 'surface' });
  const cpuPanel = el('div', { class: 'cpu' });
  const usbPanel = el('div', { class: 'usb' });
  const canvas = el('canvas', { class: 'plot', width: 720, height: 160 });
  const readout = el('span', { class: 'cpu-readout mono' }, '-');

  const input = el('input', {
    type: 'text',
    class: 'cmdline',
    placeholder: 'type a command, e.g. query cpu   (Tab completes, Up recalls)',
    autocomplete: 'off',
    disabled: true,
  });

  const history: string[] = [];
  let historyPos = 0;
  let renderedLines = 0;

  const connectBtn = el('button', {
    class: 'primary',
    onclick: () => (model.store.get().connected ? model.disconnect() : model.connect()),
  }, 'Connect');

  // Hidden until the filtered chooser comes back empty-handed, so the normal path stays one button.
  const allPortsBtn = el('button', {
    hidden: true,
    onclick: () => model.connect({ filtered: false }),
  }, 'List every serial port');

  // --- console --------------------------------------------------------------

  function renderConsole(lines: readonly ConsoleLine[]): void {
    // Append-only: re-rendering five hundred lines on every reply would drop the scroll position and
    // burn the frame budget on a screen that is mostly idle.
    if (lines.length < renderedLines) {
      clear(log);
      renderedLines = 0;
    }
    const atBottom = log.scrollTop + log.clientHeight >= log.scrollHeight - 4;
    for (const l of lines.slice(renderedLines)) {
      log.append(el('div', { class: `line ${l.kind}` }, l.text));
    }
    renderedLines = lines.length;
    while (log.childElementCount > 500) log.firstElementChild?.remove();
    if (atBottom) log.scrollTop = log.scrollHeight;
  }

  input.addEventListener('keydown', async (e: KeyboardEvent) => {
    if (e.key === 'Enter') {
      const line = input.value.trim();
      if (!line) return;
      history.push(line);
      historyPos = history.length;
      input.value = '';
      await model.send(line);
      if (/^(config|set param|reset|preset)\b/.test(line)) renderSurface();
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
  function complete(): void {
    const descriptor = model.store.get().descriptor;
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
      model.write(hits.join('  '), 'meta');
    }
  }

  // --- generated control surface --------------------------------------------

  const queryLabel = (q: QueryDesc, raw: string): string => {
    if (q.kind === 'enum' && q.values.has(Number(raw))) return `${raw} (${q.values.get(Number(raw))})`;
    if (q.kind === 'bool') return raw === '1' ? 'yes' : 'no';
    return raw || 'ok';
  };

  function paramRow(p: ParamDesc, deck: string): HTMLElement {
    const out = el('span', { class: 'mono value' }, '-');
    const slider = el('input', {
      type: 'range',
      min: String(p.lo),
      max: String(p.hi),
      step: String((p.hi - p.lo) / 1000),
      value: String((p.lo + p.hi) / 2),
    });
    slider.addEventListener('input', () => {
      out.textContent = Number(slider.value).toPrecision(4);
    });
    slider.addEventListener('change', async () => {
      out.textContent = Number(slider.value).toPrecision(4);
      await model.send(`set param ${p.name} ${deck} ${slider.value}`, { quiet: true });
    });
    return el('div', { class: 'row' },
      el('label', {}, `${p.name}${p.scope === 'deck' ? ` ${deck}` : ''}`),
      slider,
      out,
      el('button', {
        class: 'link',
        onclick: async () => {
          const v = await model.send(`get param ${p.name} ${deck}`, { quiet: true });
          if (v != null) {
            slider.value = v;
            out.textContent = Number(v).toPrecision(4);
          }
        },
      }, 'read'));
  }

  function renderSurface(): void {
    clear(surface);
    const descriptor = model.store.get().descriptor;
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
      const grid = el('div', { class: 'grid gap-1' });
      for (const p of descriptor.params.values()) {
        for (const deck of p.scope === 'deck' ? decks : ['A']) grid.append(paramRow(p, deck));
      }
      surface.append(el('h4', {}, 'Parameters'), grid);
    }

    if (descriptor.configs.size) {
      const grid = el('div', { class: 'grid gap-1' });
      for (const c of descriptor.configs.values()) {
        const sel = el('select', { onchange: () => model.send(`config ${c.name} A ${sel.value}`) },
          [...c.values.entries()].map(([v, lbl]) => el('option', { value: String(v) }, `${v} - ${lbl}`)));
        grid.append(el('div', { class: 'row' }, el('label', {}, c.name), sel));
      }
      surface.append(el('h4', {}, 'Configs'), grid);
    }

    // Actions are hand-listed rather than generated: the descriptor cannot express arity or whether a
    // verb is safe to call, which is exactly why docs/dev/terminal-target-b.md tags entries by safety
    // rather than by category. Anything destructive is confirmed inside the model's send().
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
          onclick: () => model.send(cmd),
        }, label));
      }
    }
    surface.append(el('h4', {}, 'Actions'), actions);

    if (descriptor.queries.size) {
      const list = el('div', { class: 'grid gap-1' });
      for (const q of descriptor.queries.values()) {
        const value = el('span', { class: 'mono value' }, '-');
        list.append(el('div', { class: 'row' },
          el('label', {}, q.name),
          el('button', {
            onclick: async () => {
              const r = await model.send(`query ${q.name} ${q.scope === 'deck' ? 'A' : ''}`.trim(),
                { quiet: true });
              value.textContent = r == null ? 'err' : queryLabel(q, r);
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

  // --- render on state change -----------------------------------------------

  let lastDescriptor: unknown = null;
  model.store.subscribe((s) => {
    status.textContent = s.error ? s.error : s.status;
    connectBtn.textContent = s.connected ? 'Disconnect' : 'Connect';
    allPortsBtn.hidden = !s.offerAllPorts;
    input.disabled = !s.connected;
    renderConsole(s.lines);

    if (s.descriptor !== lastDescriptor) {
      lastDescriptor = s.descriptor;
      renderSurface();
    }

    readout.textContent = !s.cpuAvailable ? 'not available on this build'
      : s.cpu ? `now ${s.cpu.avg.toFixed(1)}%   min ${s.cpu.min.toFixed(1)}%   max ${s.cpu.max.toFixed(1)}%`
        : '-';
    drawCpuPlot(canvas, s.cpuHistory, s.cpu?.max ?? null);

    clear(usbPanel);
    if (!s.usbAvailable) {
      usbPanel.append(el('p', { class: 'muted' },
        '`query usb` is not available on this build (it needs USBDIAG=1).'));
    } else if (s.usb.length) {
      usbPanel.append(el('table', { class: 'layout' },
        el('thead', {}, el('tr', {}, el('th', {}, 'Field'), el('th', {}, 'Value'))),
        el('tbody', {}, s.usb.map((r) => el('tr', {},
          el('td', { class: 'mono' }, r.key),
          el('td', { class: 'mono' }, r.value))))));
    }
  });

  cpuPanel.append(
    el('div', { class: 'controls' },
      readout,
      el('button', {
        // `reset cpu` only clears the meter's min/max extremes - it touches no params, so it is not
        // in the destructive list even though it shares the `reset` verb.
        onclick: () => model.resetCpu(),
      }, 'reset cpu'),
      el('button', { onclick: () => model.togglePolling() }, 'start / stop polling')),
    canvas,
    el('p', { class: 'muted note' },
      'min and max are extremes since the last reset, not a rolling window. The sequence a measurement '
      + 'wants is: reset, drive the engine, then watch whether max stops climbing.'));

  // `append` rather than root.append: the WebSerial notice is conditional, and this helper is what
  // drops a `false` child instead of stringifying it.
  append(root, [
    el('div', { class: 'callout warn' },
      el('strong', {}, 'Released firmware has no terminal. '),
      'Needs a build you make yourself: ',
      el('code', {}, 'make ENGINE=<engine> TERMINAL=1'),
      '.'),
    el('div', { class: 'controls' }, connectBtn, allPortsBtn, status),
    !model.supported() && el('div', { class: 'callout' },
      el('strong', {}, 'This browser has no WebSerial. '),
      'Talking to hardware needs Chrome or Edge - and unlike the card tabs there is no fallback here, '
      + 'because there is no zip-shaped substitute for a serial port.'),
    el('h3', {}, 'Console'),
    log,
    input,
    el('h3', {}, 'CPU load'),
    cpuPanel,
    el('h3', {}, 'Control surface'),
    surface,
    el('h3', {}, 'USB bring-up'),
    usbPanel,
    aside('Why released firmware has no terminal, and what it costs',
      el('p', {},
        'scripts/build_release.py never passes TERMINAL=1, so every binary in dist/ lacks the command '
        + 'channel and this tab finds nothing to talk to. Shipping terminal-enabled releases is an open '
        + 'firmware decision: it costs ~19-25 KB of SRAM_EXEC everywhere, and on the QSPI engines '
        + '(chuck, csound, mosc) it costs USB MIDI, which claims the same OTG core.'),
      el('p', {},
        'The control surface above is generated from the device\'s own `describe` reply - every control '
        + 'is one this build actually advertises, and nothing appears for the enum entries it ignores. '
        + 'Destructive verbs ask before firing.')),
  ]);
}
