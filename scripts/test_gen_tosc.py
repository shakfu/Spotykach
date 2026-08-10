"""Tests for the OSC address model (`sk_osc.py`) and the TouchOSC generator (`gen_tosc.py`).

Three things can go wrong here, and there is a group of tests for each.

1. **The model drifts from the firmware.** `sk_osc` parses `names.cpp`, `dispatch.cpp` and
   `engine_params.h` rather than restating them, so the failure mode is not a stale copy but a
   *parse* that silently stops matching - a regex that no longer fires returns an empty table and
   composes a smaller address space with no other symptom. Every table is asserted non-empty and
   asserted against the counts `docs/dev/terminal-osc.md` states.

2. **The composed addresses disagree with the document.** The device resolves an address by the
   shape rules in that document; a surface that composes them differently produces controls that
   are `unknown-address` on contact. The rules are asserted directly: kind segment always present,
   deck segment iff the slot is deck-scoped, lowercase throughout, `ab` write-only.

3. **The layout does not bind what it claims to.** A control that looks right and sends to the
   wrong address, sends an argument where the grammar wants none (which turns a read into a write,
   or a trigger into a rejected message), or listens on a path the device never replies on. The
   layout is walked and every OSC message it holds is checked against the address it belongs to.

Nothing here needs a device: the address space is a property of the source tree.
"""

from __future__ import annotations

import json
import string

import pytest
import sk_osc

py2tosc = pytest.importorskip("py2tosc", reason="the layout generator needs py2tosc")
import gen_tosc

#: The totals `docs/dev/terminal-osc.md` states, under "Totals". The document and this model are
#: two independent readings of the same firmware tables; if they disagree, one of them is wrong.
DOC_TOTALS = {
    "params_deck": 17,
    "params_global": 4,
    "configs_deck": 5,
    "configs_global": 1,
    "state_deck": 6,
    "state_global": 2,
    "stimulus_deck": 19,
    "stimulus_global": 3,
    "platform": 14,
}

ADDRESS_CHARS = set(string.ascii_lowercase + string.digits + "/")


@pytest.fixture(scope="module")
def vocab():
    return sk_osc.read_vocabulary()


@pytest.fixture(scope="module")
def space():
    return sk_osc.build_space()


# --- 1. the firmware tables parse -------------------------------------------------------------


def test_firmware_sources_exist():
    for path in (sk_osc.NAMES_CPP, sk_osc.DISPATCH_CPP, sk_osc.ENGINE_PARAMS_H, sk_osc.MAKEFILE):
        assert path.exists(), f"{path} moved; sk_osc parses it"


def test_vocabulary_matches_the_enums(vocab):
    # The firmware asserts these lengths at compile time. A parse that lost a member would compose
    # a surface missing a control and nothing else would notice.
    assert len(vocab.param_enum) == len(vocab.param_names) == 24
    assert len(vocab.config_enum) == len(vocab.config_names) == 6
    assert vocab.param_wire("Speed") == "speed"
    assert vocab.config_wire("Route") == "route"


def test_scope_predicates_parse(vocab):
    assert vocab.platform_params == {"tempo", "keyinterval", "modspeed"}
    assert vocab.global_params == {"tempo", "clickmix", "panspeed", "panrange", "keyinterval",
                                   "crossfade"}
    assert vocab.global_configs == {"route"}


def test_config_labels_parse(vocab):
    assert vocab.config_labels["route"] == {0: "stereo", 1: "dmono", 2: "genstereo"}
    assert vocab.config_labels["mode"] == {0: "slice", 1: "reel", 2: "drift"}


def test_platform_queries_parse():
    queries = {q.name: q for q in sk_osc.read_queries()}
    assert len(queries) >= 13
    assert queries["empty"].scope == "deck" and queries["empty"].kind == "bool"
    assert queries["route"].values == {0: "stereo", 1: "dmono", 2: "genstereo"}
    # Latching: asking changes the answer, so the firmware keeps it out of describe.
    assert queries["reseed"].safe is False


def test_engines_are_the_makefile_branches():
    names = sk_osc.engines()
    assert "granular" in names and "radio" in names
    # `gen/` and `faust/` hold the shared wrappers engines are built from, not engines.
    assert "gen" not in names and "faust" not in names
    catalogue = json.loads((sk_osc.REPO / "web/engines.json").read_text())
    assert set(names) == {e["name"] for e in catalogue["engines"]}


# --- 2. the address space matches the document ------------------------------------------------


def test_totals_match_the_document(space):
    slots = space.slots()
    assert len(slots) == DOC_TOTALS["params_deck"]
    assert len(space.globals) == DOC_TOTALS["params_global"]
    assert len(space.params) == DOC_TOTALS["params_deck"] * 3          # a, b, ab
    assert len({a.slot for a in space.configs if a.deck}) == DOC_TOTALS["configs_deck"]
    assert len([a for a in space.configs if not a.deck]) == DOC_TOTALS["configs_global"]
    assert len({a.slot for a in space.states if a.deck}) == DOC_TOTALS["state_deck"]
    assert len([a for a in space.states if not a.deck]) == DOC_TOTALS["state_global"]
    assert len({a.slot for a in space.stimulus if a.deck}) == DOC_TOTALS["stimulus_deck"]
    assert len([a for a in space.stimulus if not a.deck]) == DOC_TOTALS["stimulus_global"]
    assert len(space.dev) == DOC_TOTALS["platform"]


def test_addresses_are_unique_and_lowercase(space):
    addresses = [a.address for a in space.all()]
    assert len(addresses) == len(set(addresses))
    for a in addresses:
        assert a.startswith("/sk/"), a
        assert "//" not in a, a
        assert set(a) <= ADDRESS_CHARS, f"{a} is not all-lowercase"


def test_kind_segment_is_never_elided(space):
    for a in [*space.params, *space.globals]:
        assert a.address.split("/")[-2] == "param", a.address
    for a in space.configs:
        assert a.address.split("/")[-2] == "cfg", a.address
    for a in space.states:
        assert a.address.split("/")[-2] == "state", a.address


def test_deck_segment_is_structural(space):
    # Deck scope is a property of the id, so a global param carries no deck segment at all.
    for a in space.params:
        assert a.address.split("/")[2] in (*sk_osc.DECKS, sk_osc.BOTH), a.address
    for a in space.globals:
        assert a.address.startswith("/sk/param/"), a.address


def test_platform_owned_params_have_no_address(space):
    addressed = {a.slot for a in [*space.params, *space.globals]}
    assert addressed.isdisjoint({"tempo", "keyinterval", "modspeed"})
    # ModSpeed keeps a deck-scoped stimulus address of its own; it routes to set_mod_speed.
    assert "/sk/a/modspeed" in {a.address for a in space.stimulus}


def test_unsafe_and_special_queries_have_no_address(space):
    readable = {a.slot for a in [*space.states, *space.dev]}
    assert "reseed" not in readable      # latching: asking changes the answer
    assert "fit" not in readable         # takes an argument, and that would spell "write"


def test_ab_is_write_only(space):
    both = [a for a in space.params if a.deck == sk_osc.BOTH]
    assert both, "the ab alias produced nothing"
    assert all(not a.readable and a.writable for a in both)


def test_reply_mirrors_the_request_path(space):
    for a in space.all():
        assert a.reply == a.address.replace("/sk/", "/sk/reply/", 1)


def test_the_two_collisions_are_distinguished(space):
    addresses = {a.address for a in space.all()}
    assert {"/sk/a/param/mix", "/sk/state/mix"} <= addresses
    assert {"/sk/cfg/route", "/sk/state/route"} <= addresses


# --- masking ----------------------------------------------------------------------------------


def test_radio_mask_is_read_from_its_header(vocab):
    mask = sk_osc.read_live_mask("radio", vocab)
    assert not mask.derived
    assert mask.params == {"pos", "size", "speed", "mix", "env", "aux", "crossfade"}
    assert mask.configs == {"route"}


def test_masking_narrows_params_but_not_the_platform(space):
    masked = sk_osc.space_for("radio")
    assert masked.masked
    assert {a.slot for a in masked.params} == {"pos", "size", "speed", "mix", "env", "aux"}
    assert [a.address for a in masked.stimulus] == [a.address for a in space.stimulus]
    assert [a.address for a in masked.dev] == [a.address for a in space.dev]


def test_generated_engines_mask_or_say_why():
    for engine in sk_osc.engines():
        mask = sk_osc.read_live_mask(engine)
        if mask.derived:
            # The Faust and gen~ wrappers compute liveness in a loop; nothing to read statically.
            assert mask.params is None
        else:
            assert mask.params is not None


# --- labels, read from the firmware's own param_label() tables ---------------------------------


def test_labels_come_from_the_engine_headers(vocab):
    """`IEngine::param_label()` is the authority; nothing here keeps a second copy of it."""
    assert sk_osc.read_labels("radio", vocab) == {
        "param:speed": "station", "param:pos": "start", "param:size": "varispeed",
        "param:env": "static", "param:mix": "volume", "param:aux": "bank",
    }
    # granular declares none on purpose: the shared ParamId vocabulary is already its own words,
    # so a table would be a second copy of kParamNames, free to drift from it.
    assert sk_osc.read_labels("granular", vocab) == {}


def test_generated_faust_engines_label_from_their_bind_table(vocab):
    """Their `param_label()` walks a bind table, where the slider label IS the layer-3 name."""
    assert sk_osc.read_labels("filter", vocab) == {
        "param:speed": "cutoff", "param:pos": "reso", "param:size": "drive", "param:mix": "mix",
    }
    # A chain binds one role in both stages - voice puts Speed on the oscillator's "freq" and the
    # filter's "cutoff" - and stage A wins, as FaustChainEngine implements it.
    assert sk_osc.read_labels("voice", vocab)["param:speed"] == "freq"


def test_every_label_names_a_real_and_live_slot(vocab):
    for engine in sk_osc.engines():
        mask = sk_osc.read_live_mask(engine, vocab)
        for key, label in sk_osc.read_labels(engine, vocab).items():
            _, _, slot = key.partition(":")
            assert slot in vocab.param_names, f"{engine}: {key} is not a layer-2 name"
            if mask.params is not None and slot not in vocab.platform_params:
                assert slot in mask.params, f"{engine}: {key} is labelled but not live"
            assert label.isascii(), f"{engine}: {label!r} is not ASCII"


def test_labels_reach_the_caption_without_touching_the_address():
    space = sk_osc.space_for("radio")
    speed = next(a for a in space.params if a.slot == "speed" and a.deck == "a")
    assert speed.caption == "station"             # layer 3, printed
    assert speed.address == "/sk/a/param/speed"   # layer 2, sent - unchanged by the label


def test_an_unlabelled_slot_falls_back_to_its_slot_name():
    space = sk_osc.space_for("granular")
    assert all(a.caption == a.slot for a in space.params)


# --- the two ways of learning what a build implements agree -----------------------------------

#: What `describe` emits for the radio build, in the line codec's format - the same masking the
#: static parse of `radio_engine.h` infers, arrived at from the other end.
RADIO_DESCRIBE = """\
descr engine=radio version=0.5.1-radio masked=1
param pos deck 0.000..1.000
param size deck 0.000..1.000
param speed deck 0.000..1.000
param mix deck 0.000..1.000
param env deck 0.000..1.000
param aux deck 0.000..1.000
param crossfade global 0.000..1.000
config route 0:stereo 1:dmono 2:genstereo
query empty deck bool
query mix global float
caps 0x00000133
end
"""


def test_describe_and_the_static_parse_agree():
    """A device's own account of itself should compose the same surface the header parse does.

    This is the check that keeps `--describe` and the offline path from producing two different
    layouts for one build, and it is also how the static parse gets corroborated at all: the
    header is an inference, the descriptor is the device speaking.
    """
    from_device = gen_tosc.space_from_describe(RADIO_DESCRIBE)
    from_source = sk_osc.space_for("radio")
    assert from_device.engine == from_source.engine
    assert [a.address for a in from_device.all()] == [a.address for a in from_source.all()]
    assert [a.caption for a in from_device.all()] == [a.caption for a in from_source.all()]


# --- 3. the layout binds what it claims to ----------------------------------------------------


def osc_messages(doc):
    """Every (control, message) pair in a document, with the address flattened out of the path."""
    for control in doc.walk():
        for message in getattr(control, "messages", []):
            if isinstance(message, py2tosc.OscMessage):
                yield control, message, "".join(p.value for p in message.path)


@pytest.fixture(scope="module")
def universal():
    return gen_tosc.build_document(sk_osc.space_for(None))


#: Addresses a surface legitimately drives from more than one control, because the value they take
#: is a fixed number or word rather than something a control has: a deck for `reset`, a slot for
#: the presets, a note number for MIDI. Every other address is one control's to send.
MULTI_SENDER = {"/sk/dev/reset", "/sk/dev/preset/save", "/sk/dev/preset/load", "/sk/midi/note"}


def test_every_address_is_reachable_from_exactly_one_control(universal):
    space = sk_osc.space_for(None)
    senders: dict[str, int] = {}
    for _, message, address in osc_messages(universal):
        if message.send:
            senders[address] = senders.get(address, 0) + 1

    wanted = {a.address for a in space.all()}
    # `/sk/midi/msg ,iii` takes three fixed bytes from nothing a control supplies: it would be a
    # layout per message rather than a control, and is deliberately left out.
    assert wanted - set(senders) == {"/sk/midi/msg"}
    assert set(senders) - wanted == set()
    doubled = {a for a, n in senders.items() if n > 1} - MULTI_SENDER
    assert not doubled, f"driven by two controls: {sorted(doubled)}"


def test_reads_and_triggers_never_send_a_live_value(universal):
    """An argument is what makes a message a write, so a read must not carry the control's value.

    A fixed argument is a different thing: `reset ,s <deck>` and `preset/save ,i <slot>` are the
    two forms the document gives, and both are constants a button carries rather than a value it
    reads off itself. What must never appear on one of these addresses is a VALUE partial.
    """
    space = {a.address: a for a in sk_osc.space_for(None).all()}
    for _, message, address in osc_messages(universal):
        if not message.send:
            continue
        addr = space[address]
        if addr.kind != "state" and addr.argtype != "trigger":
            continue
        assert [t.condition for t in message.triggers] == ["RISE"], address
        for arg in message.arguments:
            assert arg.type == "CONSTANT", f"{address} would be a write, not a read"
        if address not in MULTI_SENDER:
            assert message.arguments == [], f"{address} takes no argument at all"


def test_params_follow_their_reply_mirror(universal):
    listened = {a for _, m, a in osc_messages(universal) if m.receive}
    for addr in sk_osc.space_for(None).params:
        if addr.readable:
            assert addr.reply in listened, f"{addr.address} never follows the device"
        else:      # ab: one request cannot have two answers on one reply address
            assert addr.reply not in listened, f"{addr.address} listens but can never be read"


def test_listeners_never_send(universal):
    for _, message, address in osc_messages(universal):
        if address.startswith("/sk/reply/") or address in ("/sk/err", "/sk/log"):
            assert not message.send, f"{address} is a device-to-host path"
            assert message.triggers == [], f"{address} would echo what it just received"


def test_every_bound_control_tags_its_address(universal):
    for control, message, address in osc_messages(universal):
        if not message.send:
            continue
        assert control.get("tag") == address, f"{control.get('name')} is tagged wrong"


def test_config_radios_send_the_selector_int(universal):
    space = {a.address: a for a in sk_osc.space_for(None).configs}
    for _, message, address in osc_messages(universal):
        if address not in space or not message.send:
            continue
        (arg,) = message.arguments
        assert arg.conversion == "INTEGER"
        assert (arg.scale_min, arg.scale_max) == (0, len(space[address].values) - 1)


@pytest.mark.parametrize("engine", [None, "radio", "passthrough", "granular"])
def test_layouts_validate_and_round_trip(engine, tmp_path):
    path, space = gen_tosc.generate(engine, tmp_path)
    assert path.exists()
    doc = py2tosc.load(path)
    assert [str(i) for i in doc.validate()] == []
    assert len(doc.find_all()) == len(gen_tosc.build_document(space).find_all())


def test_every_engine_lays_out(tmp_path):
    for engine in [None, *sk_osc.engines()]:
        path, _ = gen_tosc.generate(engine, tmp_path)
        assert path.exists()


def test_the_committed_inventory_is_current():
    """`host/osc_addresses.txt` is what `host/test_osc_addr.cpp` probes against the real resolver.

    It is a build artifact of this model, committed so the off-target test needs no Python at build
    time. A stale copy is the failure that matters: the C++ side would go on asserting last week's
    address space and report parity while the model had moved. Same guard, and the same reason, as
    `scripts/test_web_export.py` puts on the committed `web/` export.
    """
    committed = sk_osc.INVENTORY.read_text()
    fresh = sk_osc.INVENTORY_HEADER + "\n".join(
        f"{a.address} {a.kind} {a.argtype}" for a in sk_osc.build_space().all()
    ) + "\n"
    assert committed == fresh, (
        "host/osc_addresses.txt is stale - regenerate with `scripts/sk_osc.py --inventory`"
    )


def test_the_inventory_covers_the_whole_space():
    entries = [ln.split() for ln in sk_osc.INVENTORY.read_text().splitlines()
               if ln and not ln.startswith("#")]
    assert len(entries) == len(sk_osc.build_space().all())
    assert all(len(e) == 3 and e[0].startswith("/sk/") for e in entries)


def test_no_control_is_laid_out_with_an_empty_frame(universal):
    for control in universal.walk():
        frame = control.get("frame")
        if frame is None:
            continue
        assert frame.w > 0 and frame.h > 0, f"{control.get('name')} has no area"


@pytest.mark.parametrize("engine", [None, "shuttle", "granular", "passthrough"])
def test_every_radial_is_laid_out_square(engine):
    """A RADIAL fills its frame, so a cell wider than it is tall draws a clipped ellipse.

    The knobs are inset to a square inside whatever cell the page fitting gave them; this asserts
    the inset actually reaches the resolved frame, which is the part that is easy to get wrong -
    an inset is a fraction applied at resolve time, not a property of the control.
    """
    doc = gen_tosc.build_document(sk_osc.space_for(engine))
    radials = [c for c in doc.walk() if c.control_type is py2tosc.ControlType.RADIAL]
    assert radials or engine == "passthrough", "no knobs were laid out"
    for control in radials:
        frame = control.get("frame")
        # A couple of pixels of slack: the tile split hands whatever will not divide evenly to
        # the last cell, so a row's cells are not all identical to the pixel.
        assert abs(frame.w - frame.h) <= 3, (
            f"{control.get('tag')} is {frame.w:.0f}x{frame.h:.0f}, not round"
        )


def test_a_page_never_overflows_its_canvas(universal):
    """Bands are sized in pixels, so the arithmetic has to add up rather than be normalised away."""
    pager = next(c for c in universal.walk() if c.control_type is py2tosc.ControlType.PAGER)
    for page in pager.children:
        page_frame = page.get("frame")
        for band in page.children:
            frame = band.get("frame")
            assert frame.y >= -0.5, f"{page.get('name')} starts a band above its page"
            assert frame.y + frame.h <= page_frame.h + 0.5, (
                f"{page.get('name')} runs {frame.y + frame.h - page_frame.h:.0f}px past its page"
            )
