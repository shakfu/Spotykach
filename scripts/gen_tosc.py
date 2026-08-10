#!/usr/bin/env python3
"""Generate TouchOSC layouts (`.tosc`) for the spotykach OSC address space.

`docs/dev/terminal-osc.md` designs an OSC codec for the terminal channel whose whole point is that
**the device becomes a node in a Max/Pd/TouchOSC rig**. This script is the TouchOSC half of that:
it reads the address space out of the firmware tables (via `sk_osc.py`) and builds a layout with a
control bound to every address a given build answers.

The address space is layer-2 and engine-independent, which is the property the design exists to
protect, so the interesting output is the **universal** layout: one surface that drives every
engine, because `/sk/a/param/speed` is the PITCH knob on all of them. The per-engine layouts are
the same surface narrowed to the slots an engine's `live_params()` declares - fewer controls, none
of them dead - and captioned with that engine's layer-3 labels, which is where a fader reads
"station" on a radio build while still sending to `/sk/a/param/speed`.

What the layouts contain, one page each:

  a, b     every deck-scoped param as a radial, the CV inlets as faders, and the stimulus verbs
           (gate, pads, seq, fx) as buttons. Params also LISTEN on `/sk/reply/<same path>`, so a
           read - or an ack-enabled write - moves the control.
  both     the write-only `ab` fan-out alias, and the global params that carry no deck segment.
  cfg      the selector-int configs, as radios over the labels `describe` publishes.
  state    a read button per state address with a readout beside it. A read is a message with no
           arguments, which is exactly what these buttons send.
  dev      mode, describe/caps/help, the CPU and USB reads, reset, presets, MIDI, and readouts for
           `/sk/err` and `/sk/log`.

Usage:
    scripts/gen_tosc.py                          # universal + every engine, into dist/tosc/
    scripts/gen_tosc.py --engine radio           # one engine
    scripts/gen_tosc.py --universal              # the unmasked surface only
    scripts/gen_tosc.py --size 1366x1024         # a different canvas
    scripts/gen_tosc.py --xml                    # also write the readable .xml export
    scripts/gen_tosc.py --describe capture.txt   # mask/label from a device's describe block

Needs py2tosc (`pip install --group dev`, or `pip install py2tosc`).
"""

from __future__ import annotations

import argparse
import math
import sys
from collections.abc import Callable
from dataclasses import dataclass
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import sk_osc
from sk_osc import Address, AddressSpace

try:
    import py2tosc
    from py2tosc import ui
    from py2tosc.enums import Conversion
except ImportError as exc:   # a clearer failure than a bare traceback three frames in
    raise SystemExit(
        "gen_tosc.py needs py2tosc: `.venv/bin/pip install --group dev`, or `pip install py2tosc`"
    ) from exc

REPO = Path(__file__).resolve().parent.parent
OUT_DIR = REPO / "dist/tosc"

#: Canvas, in TouchOSC points. A 4:3 tablet in landscape; `--size` overrides it.
CANVAS = (1024, 768)

#: One colour per kind, so the binding a control carries is legible at a glance rather than only
#: in its tag. Deliberately muted: a control surface read in a dark room, not a palette.
COLOURS = {
    "param": "#3f7d5c",
    "global": "#2f6f8f",
    "both": "#6f5b9e",
    "cv": "#9c8218",
    "trigger": "#9c5a24",
    "toggle": "#96393c",
    "cfg": "#4a6b85",
    "state": "#4a4a4a",
    "dev": "#63417f",
    "readout": "#1e1e1e",
}


# --- controls ---------------------------------------------------------------------------------
#
# Every control gets `tag` set to the generic address it drives. TouchOSC shows it in the editor
# and a Lua script can read it, which makes the layout self-describing: the binding is recoverable
# from the control without reading the message partials.


def _send(addr: Address, **kwargs):
    """The outgoing binding for an address, sending the control's own value."""
    return ui.osc(addr.address, receive=False, **kwargs)


def _listen(addr: Address, key: str = "x", conversion: Conversion = Conversion.FLOAT):
    """The incoming binding: the reply mirror of the same path, feeding one of the control's values.

    Receive-only and trigger-less. Triggers are what decides when a control *sends*; a binding that
    only listens has nothing to be triggered by, and leaving the default `x`-changed trigger on it
    would make the control echo every value the device just told it.
    """
    return ui.osc(
        addr.reply,
        args=[ui.value(key, conversion=conversion)],
        send=False, receive=True, triggers=[],
    )


def squared(control: py2tosc.Control, aspect: float) -> py2tosc.Control:
    """Inset a control so it occupies a square inside a cell of the given aspect.

    A RADIAL is drawn round, and TouchOSC draws it to fill its frame: given a cell twice as wide
    as it is tall it comes out an ellipse with the ends of its arc clipped. The inset is a
    fraction rather than a pixel count, so it is applied when the frame finally comes down.
    """
    if aspect < 1:
        margin = (1 - aspect) / 2
        ui.inset(control, (margin, 0, margin, 0))
    elif aspect > 1:
        margin = (1 - 1 / aspect) / 2
        ui.inset(control, (0, margin, 0, margin))
    return control


def knob(addr: Address, colour: str, aspect: float = 1.0) -> py2tosc.Control:
    """A param: a radial that sends on the address and follows the reply on the mirror path."""
    messages = [_send(addr)]
    if addr.readable:
        messages.append(_listen(addr))
    control = py2tosc.radial(name=addr.slot, color=colour, tag=addr.address, messages=messages)
    # The caption keeps the whole cell; only the dial is squared, so the text stays centred and
    # has the full cell width to run in.
    return ui.labelled(squared(control, aspect), addr.caption, size=12)


def cv_fader(addr: Address, colour: str) -> py2tosc.Control:
    """A float stimulus (the CV inlets, `modspeed`): a fader. Write-only - none of these read back."""
    control = py2tosc.fader(
        name=addr.slot.replace("/", "-"), color=colour, tag=addr.address,
        messages=[_send(addr)],
    )
    return ui.labelled(control, addr.caption.replace("cv/", ""), size=11)


def _verb_caption(addr: Address) -> str:
    """A caption for a stimulus verb: the whole leaf path, spaced.

    The last segment alone is ambiguous on the surface even though the addresses are distinct -
    `pad/clear` and `seq/clear` would both read "clear", and `fx/flux` and `fx/lock/flux` both
    "flux". The verbs are short enough that spelling them out costs nothing.
    """
    return addr.caption.replace("/", " ")


def trigger(addr: Address, colour: str, caption: str | None = None) -> py2tosc.Control:
    """A bare trigger: a momentary button sending a message with NO arguments.

    Arity is the whole grammar here - an argument-less message is a read on an addressable kind and
    a bare trigger on a stimulus verb - so the argument list is empty rather than carrying the
    button's value. Firing on RISE only means one press is one message, where the default
    press-and-release would send the trailing zero the device then has to suppress.
    """
    control = py2tosc.button(
        name=addr.slot.replace("/", "-") or addr.address.rsplit("/", 1)[-1],
        color=colour, tag=addr.address,
        messages=[ui.osc(addr.address, args=[], on="RISE")],
    )
    return ui.labelled(control, caption or _verb_caption(addr), size=11)


def toggle(addr: Address, colour: str, caption: str | None = None) -> py2tosc.Control:
    """A boolean address: a latching button sending its float value.

    The device accepts `f` where it wants a bool and reads non-zero as true, which is exactly what
    a TouchOSC toggle sends, so no coercion of our own is needed.
    """
    control = py2tosc.button(
        name=addr.slot.replace("/", "-"), color=colour, tag=addr.address,
        button_type=1,   # latch on release
        messages=[_send(addr)],
    )
    return ui.labelled(control, caption or _verb_caption(addr), size=11)


def selector(addr: Address, colour: str) -> py2tosc.Control:
    """A config: a radio over the selector ints, captioned with the labels `describe` publishes.

    The radio's `x` is quantised to its steps, so converting it to an integer over `0..n-1` sends
    the selector the device expects. Configs are write-only on the device (`set_config` does not
    read back), so there is no reply binding.
    """
    steps = max(len(addr.values), 2)
    control = py2tosc.radio(
        name=addr.slot, color=colour, tag=addr.address, steps=steps,
        messages=[_send(addr, args=[
            ui.value("x", conversion=Conversion.INTEGER, scale=(0, steps - 1)),
        ])],
    )
    values = "  ".join(f"{i}:{v}" for i, v in sorted(addr.values.items()))
    return ui.column(
        ui.labelled(control, addr.caption, size=12),
        text_label(f"{addr.slot}-values", values, color=COLOURS["readout"], text_size=10),
        sizes=(3, 1),
    )


def text_label(name: str, text: str, **props) -> py2tosc.Control:
    """A LABEL showing fixed text.

    The text a label draws is one of its *values*, not a property of the same name: setting a
    `text` property leaves the label blank and earns a warning from `validate`. Everything that
    writes a caption goes through here so that distinction is made once.
    """
    return py2tosc.label(
        name=name, background=False,
        values=[py2tosc.Value("text", default=text), py2tosc.Value("touch", default=False)],
        **props,
    )


def readout(addr: Address, colour: str) -> py2tosc.Control:
    """A read: the argument-less button that asks, over a label that shows what came back."""
    return ui.column(
        trigger(addr, colour),
        text_label(
            f"{addr.slot}-out", "-", color=COLOURS["readout"], text_size=12, tag=addr.reply,
            messages=[_listen(addr, key="text", conversion=Conversion.STRING)],
        ),
        sizes=(2, 1), gap=2,
    )


def const_buttons(addr: Address, colour: str, count: int) -> list[py2tosc.Control]:
    """`count` buttons sending a fixed integer to one address - the preset slots.

    A button can only send its own value, and these addresses want a slot number, so the number is
    a constant partial and there is one button per slot.
    """
    out = []
    for n in range(count):
        control = py2tosc.button(
            name=f"{addr.slot.replace('/', '-')}-{n}", color=colour, tag=addr.address,
            messages=[ui.osc(
                addr.address, args=[ui.const(str(n), conversion=Conversion.INTEGER)], on="RISE",
            )],
        )
        out.append(ui.labelled(control, str(n), size=11))
    return out


def caption(text: str, size: int = 12) -> py2tosc.Control:
    """A section heading."""
    return text_label(text, text, color=COLOURS["readout"], text_size=size)


# --- pages ------------------------------------------------------------------------------------
#
# Sections are laid out in pixels rather than in abstract weights. That is not fussiness: cell
# SHAPE is the thing being decided, and a weight system decides it only by accident. A knob wants a
# square cell, a fader a wide one, a button a squat one, and the page has to divide a fixed canvas
# among however many rows each engine's masking leaves.

PAD, GAP = 8, 6
HEAD_H = 18       #: a section heading
TITLE_H = 26      #: the document title strip above the pager
TABBAR = 40       #: the pager's own tab bar, which `resolve` reserves out of every page

#: Wanted cell height as a fraction of cell width, per kind of control.
ASPECT = {"knob": 1.0, "fader": 0.5, "button": 0.55, "selector": 0.62, "readout": 0.7}


@dataclass(frozen=True)
class Geo:
    """The pixel budget one page has to divide up."""

    width: int
    height: int

    def cell_width(self, columns: int) -> float:
        return (self.width - 2 * PAD - (columns - 1) * GAP) / columns


@dataclass
class Band:
    """A titled row-band of one kind of control, sized before its controls are built.

    The controls are built last, because how tall a cell ends up deciding what shape to draw in it
    - and that is not known until every band on the page has asked for its height and the total
    has been fitted to the canvas.
    """

    title: str
    items: list
    columns: int = 1
    kind: str = "button"
    build: Callable[[object, float], py2tosc.Control] | None = None
    raw: py2tosc.Control | None = None      # an already-built block rather than a tiled row
    px: float | None = None                 # an explicit row height, for a band with no cells
    cell_h: float = 0.0

    @property
    def live(self) -> bool:
        return bool(self.items) or self.raw is not None

    @property
    def rows(self) -> int:
        if self.raw is not None:
            return 1
        return math.ceil(len(self.items) / self.columns) if self.items else 0

    @property
    def fixed(self) -> float:
        """The part of the band's height that does not scale: heading, and the gaps between rows."""
        return HEAD_H + GAP + max(self.rows - 1, 0) * GAP

    @property
    def height(self) -> float:
        return self.fixed + self.rows * self.cell_h

    def wanted(self, geo: Geo) -> float:
        """The row height this band asks for, before the page fits them all to the canvas."""
        if self.px is not None:
            return self.px
        return geo.cell_width(self.columns) * ASPECT.get(self.kind, 0.55)

    def render(self, geo: Geo) -> py2tosc.Control:
        body = self.raw
        if body is None:
            aspect = self.cell_h / geo.cell_width(self.columns)
            builder = self.build or (lambda item, _: item)
            body = _tiles([builder(item, aspect) for item in self.items], self.columns)
        return ui.column(
            caption(self.title, size=11), body,
            sizes=(HEAD_H, self.rows * self.cell_h + max(self.rows - 1, 0) * GAP), gap=GAP,
        )


def _tiles(children, columns, **props):
    """`ui.tiles` with a fixed column count.

    The count is not shrunk to fit: a band holding one control should put it in a cell the size of
    every other cell, not stretch it across the page.
    """
    if not children:
        return None
    return ui.tiles(*children, columns=columns, gap=GAP, **props)


def _page(name: str, geo: Geo, bands: list[Band]) -> py2tosc.Control | None:
    """Fit the page's bands to the canvas, then build and stack them.

    A page that does not fill the canvas is padded with an invisible spacer rather than having its
    few bands stretched over the whole of it - so a knob is the same size on the page with six of
    them as on the page with eighteen. A page that overflows has its cells scaled down together,
    which keeps their shapes in step with each other even when none of them gets what it asked for.
    """
    live = [b for b in bands if b.live]
    if not live:
        return None

    for band in live:
        band.cell_h = band.wanted(geo)
    available = geo.height - 2 * PAD - GAP * (len(live) - 1)
    fixed = sum(b.fixed for b in live)
    variable = sum(b.rows * b.cell_h for b in live)
    if variable and fixed + variable > available:
        scale = max((available - fixed) / variable, 0.1)
        for band in live:
            band.cell_h *= scale

    children = [b.render(geo) for b in live]
    sizes = [b.height for b in live]
    slack = available - sum(sizes)
    if slack > 1:
        children.append(py2tosc.box(name="spacer", background=False, outline=False))
        sizes.append(slack)
    return ui.column(*children, sizes=sizes, gap=GAP, pad=PAD, name=name)


def deck_page(space: AddressSpace, geo: Geo, deck: str) -> py2tosc.Control | None:
    """One deck: its params, its CV inlets, and its stimulus verbs."""
    stim = space.deck(deck, "stim")
    return _page(deck, geo, [
        Band(
            f"deck {deck} - params  /sk/{deck}/param/*",
            space.deck(deck, "param"), columns=6, kind="knob",
            build=lambda a, asp: knob(a, COLOURS["param"], asp),
        ),
        Band(
            "cv inlets and mod rate",
            [a for a in stim if a.argtype == "float"], columns=5, kind="fader",
            build=lambda a, _: cv_fader(a, COLOURS["cv"]),
        ),
        Band(
            "stimulus - gate, pads, seq, fx",
            [a for a in stim if a.argtype in ("bool", "trigger")], columns=8, kind="button",
            build=lambda a, _: (toggle(a, COLOURS["toggle"]) if a.argtype == "bool"
                                else trigger(a, COLOURS["trigger"])),
        ),
    ])


def both_page(space: AddressSpace, geo: Geo) -> py2tosc.Control | None:
    """The globals, and the `ab` fan-out alias.

    `ab` is write-only by design - one request cannot have two answers on one reply address - so
    these knobs send and never follow. They are the reason the page exists: setting both decks
    from one control is the fan-out anyone actually asks for.
    """
    return _page("both", geo, [
        Band(
            "global params - no deck segment  /sk/param/*",
            space.globals, columns=6, kind="knob",
            build=lambda a, asp: knob(a, COLOURS["global"], asp),
        ),
        Band(
            "ab - writes both decks at once, never reads  /sk/ab/param/*",
            space.deck(sk_osc.BOTH, "param"), columns=6, kind="knob",
            build=lambda a, asp: knob(a, COLOURS["both"], asp),
        ),
    ])


def cfg_page(space: AddressSpace, geo: Geo) -> py2tosc.Control | None:
    """The selector-int configs, per deck and global."""
    return _page("cfg", geo, [
        Band(
            f"deck {deck} configs" if deck else "global config",
            [a for a in space.configs if a.deck == deck], columns=5, kind="selector",
            build=lambda a, _: selector(a, COLOURS["cfg"]),
        )
        for deck in ("", *sk_osc.DECKS)
    ])


def state_page(space: AddressSpace, geo: Geo) -> py2tosc.Control | None:
    """The reads. Every button here sends a message with no arguments; that is what makes it a read."""
    return _page("state", geo, [
        Band(
            f"deck {deck} state" if deck else "global state",
            [a for a in space.states if a.deck == deck], columns=7, kind="readout",
            build=lambda a, _: readout(a, COLOURS["state"]),
        )
        for deck in ("", *sk_osc.DECKS)
    ])


def dev_page(space: AddressSpace, geo: Geo) -> py2tosc.Control | None:
    """The platform section, plus the two receive-only readouts a rig wants: errors and logs."""
    by_slot = {a.slot: a for a in space.dev}

    verbs, reads, presets = [], [], {}
    for a in space.dev:
        if a.slot.startswith("preset/"):
            # One section per verb: two unlabelled rows of 0..7 would be indistinguishable.
            presets[a.slot] = const_buttons(a, COLOURS["dev"], 8)
        elif a.readable and not a.writable:
            reads.append(readout(a, COLOURS["state"]))
        elif a.argtype == "bool":
            verbs.append(toggle(a, COLOURS["toggle"], caption=a.slot))
        else:
            verbs.append(trigger(a, COLOURS["dev"], caption=a.slot))

    # `reset` also accepts a deck as a string argument; the bare form resets everything.
    reset = by_slot.get("reset")
    if reset is not None:
        for deck in sk_osc.DECKS:
            control = py2tosc.button(
                name=f"reset-{deck}", color=COLOURS["dev"], tag=reset.address,
                messages=[ui.osc(reset.address, args=[ui.const(deck)], on="RISE")],
            )
            verbs.append(ui.labelled(control, f"reset {deck}", size=11))

    midi = []
    transport = next((a for a in space.stimulus if a.slot == "midi/transport"), None)
    if transport is not None:
        midi.append(toggle(transport, COLOURS["toggle"], caption="transport"))
    note = next((a for a in space.stimulus if a.slot == "midi/note"), None)
    if note is not None:
        # `,ii` is a channel and a note number, neither of which a button has to give, so an octave
        # of buttons carries both as constants. `/sk/midi/msg ,iii` is left out on the same logic
        # taken one step further: three fixed bytes is a layout per message, not a control.
        for semitone in range(12):
            control = py2tosc.button(
                name=f"note-{60 + semitone}", color=COLOURS["dev"], tag=note.address,
                messages=[ui.osc(note.address, on="RISE", args=[
                    ui.const("0", conversion=Conversion.INTEGER),
                    ui.const(str(60 + semitone), conversion=Conversion.INTEGER),
                ])],
            )
            midi.append(ui.labelled(control, str(60 + semitone), size=10))

    # Receive-only readouts. `/sk/err ,ss` carries the offending address and the reason; the second
    # label lists the argument twice so that, if TouchOSC applies arguments in order, it ends up
    # holding the reason. Worst case it shows the address, the same as the first.
    err = text_label(
        "err", "-", color=COLOURS["readout"], text_size=12, tag="/sk/err",
        messages=[ui.osc("/sk/err", args=[ui.value("text", conversion=Conversion.STRING)],
                         send=False, receive=True, triggers=[])],
    )
    err_reason = text_label(
        "err-reason", "-", color=COLOURS["readout"], text_size=12, tag="/sk/err",
        messages=[ui.osc("/sk/err", send=False, receive=True, triggers=[], args=[
            ui.value("text", conversion=Conversion.STRING),
            ui.value("text", conversion=Conversion.STRING),
        ])],
    )
    # `/sk/log ,s` exists only on a build that wraps its log output in OSC rather than forcing
    # INFS_LOG=0 (document, "Framing: SLIP"). Listening for it costs nothing on a build that does not.
    log = text_label(
        "log", "-", color=COLOURS["readout"], text_size=11, tag="/sk/log",
        messages=[ui.osc("/sk/log", args=[ui.value("text", conversion=Conversion.STRING)],
                         send=False, receive=True, triggers=[])],
    )

    monitor = ui.column(
        err, err_reason,
        caption("/sk/log ,s - the firmware log, if this build wraps it in OSC", size=11), log,
        sizes=(2, 2, 1, 2), gap=2,
    )
    return _page("dev", geo, [
        Band("platform verbs  /sk/dev/*", verbs, columns=7, kind="button"),
        Band("platform reads", reads, columns=6, kind="readout"),
        *[Band(f"{slot} - slot 0..7", buttons, columns=8, kind="button")
          for slot, buttons in presets.items()],
        Band("midi - transport, then one octave from note 60", midi, columns=13, kind="button"),
        # A block rather than a row of cells, so it names its own height: deriving one from a
        # one-column cell width would ask for most of the page.
        Band("/sk/err ,ss - the request address, then the reason", [], raw=monitor, px=110),
    ])


# --- the document -----------------------------------------------------------------------------


def build_document(space: AddressSpace, size: tuple[int, int] = CANVAS) -> py2tosc.Document:
    """Assemble the pages into a document, sized to the canvas.

    The pager is deliberately not the root: TouchOSC treats the root as the canvas and gives it
    none of its type's behaviour, so a PAGER there draws tabs and then stacks every page at once.
    """
    width, height = size
    # What a page actually gets: the canvas, less the title strip above the pager and the tab bar
    # `resolve` reserves inside it. Every cell size on every page is measured against this.
    geo = Geo(width=width, height=height - TITLE_H - 2 - TABBAR)

    pages = [p for p in (
        deck_page(space, geo, "a"),
        deck_page(space, geo, "b"),
        both_page(space, geo),
        cfg_page(space, geo),
        state_page(space, geo),
        dev_page(space, geo),
    ) if p is not None]
    if not pages:
        raise ValueError(f"{space.engine}: nothing to lay out")

    title = f"sk  {space.engine}  -  layer-2 OSC" + ("" if space.masked else "  (unmasked)")
    doc = py2tosc.Document(root=ui.column(
        text_label("title", title, color=COLOURS["readout"], text_size=13),
        ui.pager(*pages, name="pages"),
        sizes=(TITLE_H, height - TITLE_H - 2), gap=2,
        frame=(0, 0, width, height),
        name=f"sk-{space.engine}",
    ))
    doc.resolve()
    return doc


def space_from_describe(text: str) -> AddressSpace:
    """Build the space from a captured `describe` block instead of from the source tree.

    What a running device reports is authoritative, where the static parse is an inference - so
    this is the path to prefer when a device is to hand, and the only one that reaches the gen~
    engines, whose liveness masks are computed at run time and cannot be read out of a header.

    The block is the line codec's, which is what `skdev` already parses and what a capture on the
    bench holds. That codec's `describe` carries no labels - only the OSC codec's bundle does, as
    `/sk/reply/dev/describe/param ,ssffs <address> <label> <lo> <hi> <scope>` - so labels here
    still come from the engine's `param_label()` table as read out of the source tree. A caller
    holding a decoded OSC bundle should pass its own: `tools/skdev/semantic.py` already turns that
    bundle into this mapping.
    """
    sys.path.insert(0, str(REPO / "tools"))
    from skdev.descriptor import (
        parse_describe,
    )

    lines = [ln for ln in text.splitlines() if ln.strip() and not ln.startswith("[")]
    d = parse_describe(lines)
    name = d.engine or "device"
    space = sk_osc.build_space(labels=sk_osc.read_labels(name))
    return sk_osc.mask_space(space, sk_osc.LiveMask(name, set(d.params), set(d.configs)))


def generate(
    engine: str | None,
    out_dir: Path,
    *,
    size: tuple[int, int] = CANVAS,
    xml: bool = False,
    describe: Path | None = None,
) -> tuple[Path, AddressSpace]:
    """Build one layout and write it. Returns the path written and the space it covers."""
    if describe is not None:
        space = space_from_describe(describe.read_text())
    else:
        name = engine or "universal"
        space = sk_osc.space_for(engine)
        space.engine = name

    doc = build_document(space, size)
    issues = [i for i in doc.validate() if str(i).startswith("error")]
    if issues:
        raise ValueError(
            f"{space.engine}: layout is invalid\n  " + "\n  ".join(str(i) for i in issues)
        )

    out_dir.mkdir(parents=True, exist_ok=True)
    path = out_dir / f"sk-{space.engine}.tosc"
    doc.save(path)
    if xml:
        doc.save(path.with_suffix(".xml"))
    return path, space


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--engine", action="append", default=[],
                    help="engine to generate for; repeatable (default: universal + every engine)")
    ap.add_argument("--universal", action="store_true", help="the unmasked surface only")
    ap.add_argument("-o", "--out", type=Path, default=OUT_DIR, help=f"output directory ({OUT_DIR})")
    ap.add_argument("--size", default="x".join(map(str, CANVAS)), help="canvas, WxH")
    ap.add_argument("--xml", action="store_true", help="also write the readable .xml export")
    ap.add_argument("--describe", type=Path,
                    help="a captured `describe` block; masks and labels from the device instead")
    args = ap.parse_args(argv)

    try:
        width, height = (int(n) for n in args.size.lower().split("x"))
    except ValueError:
        ap.error(f"--size wants WxH, got {args.size!r}")

    if args.describe:
        targets: list[str | None] = [None]
    elif args.universal:
        targets = [None]
    elif args.engine:
        targets = list(args.engine)
    else:
        targets = [None, *sk_osc.engines()]

    for target in targets:
        path, space = generate(target, args.out, size=(width, height), xml=args.xml,
                               describe=args.describe)
        where = path.relative_to(REPO) if path.is_relative_to(REPO) else path
        note = ""
        if target is not None and not space.masked:
            # The Faust and gen~ wrappers compute their mask in a loop over a binding table, so
            # there is nothing to read statically and this layout is the universal one by another
            # name. A capture from the device is the way to narrow it.
            note = "  (no static live_params - universal surface; use --describe to narrow)"
        print(f"{where}  {len(space.all())} addresses{note}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
