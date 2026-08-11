"""Conformance against a FOREIGN OSC implementation: liblo, driven through ctypes.

Everything else that validates this codec is in-house - the C++ firmware encoder, the Python client,
the TypeScript client. Three implementations by one author agreeing proves they were written from the
same understanding, not that the understanding matches OSC. This is the test that closes that gap:
liblo is the reference C implementation, is what ``oscsend``/``oscdump`` are built on, and knows
nothing about this project.

No device and no network are required for most of it - liblo's serialiser and parser are called
directly. The end-to-end case additionally runs the real ``skbridge`` loop against a pty, which is the
complete path a TouchOSC or Max user takes: **UDP -> bridge -> SLIP -> the bytes the device receives**.

liblo is dynamically loaded, so this skips cleanly where it is absent (it is a runtime library, not a
build dependency of anything here).
"""

import ctypes
import ctypes.util
import os
import socket
import struct
import threading
import time

import pytest

from skbridge import Bridge
from skdev import osc

SAMPLE = os.path.join(os.path.dirname(__file__), "..", "host", "build", "describe_osc_sample.bin")


def _load_liblo():
    for name in (ctypes.util.find_library("lo"), "liblo.so.7", "liblo.7.dylib", "liblo.so"):
        if not name:
            continue
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    return None


lo = _load_liblo()
if lo is None:                                 # pragma: no cover - environment without liblo
    pytest.skip("liblo is not installed", allow_module_level=True)

# Only the non-variadic API: `lo_send` and friends are MACROS that expand to `*_internal` calls
# carrying __FILE__/__LINE__, so they are not symbols and cannot be reached through ctypes.
lo.lo_message_new.restype = ctypes.c_void_p
lo.lo_message_add_float.argtypes = [ctypes.c_void_p, ctypes.c_float]
lo.lo_message_add_int32.argtypes = [ctypes.c_void_p, ctypes.c_int32]
lo.lo_message_add_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p]
lo.lo_message_add_true.argtypes = [ctypes.c_void_p]
lo.lo_message_add_false.argtypes = [ctypes.c_void_p]
lo.lo_message_serialise.restype = ctypes.c_void_p
lo.lo_message_serialise.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p,
                                    ctypes.POINTER(ctypes.c_size_t)]
lo.lo_message_deserialise.restype = ctypes.c_void_p
lo.lo_message_deserialise.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_int)]
lo.lo_message_get_types.restype = ctypes.c_char_p
lo.lo_message_get_types.argtypes = [ctypes.c_void_p]
lo.lo_message_get_argc.restype = ctypes.c_int
lo.lo_message_get_argc.argtypes = [ctypes.c_void_p]
lo.lo_address_new.restype = ctypes.c_void_p
lo.lo_address_new.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
lo.lo_send_message.restype = ctypes.c_int
lo.lo_send_message.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_void_p]

FLOAT, INT, STRING, TRUE, FALSE = (
    lo.lo_message_add_float, lo.lo_message_add_int32, lo.lo_message_add_string,
    lo.lo_message_add_true, lo.lo_message_add_false)


def liblo_message(args):
    m = lo.lo_message_new()
    for adder, value in args:
        adder(m) if value is None else adder(m, value)
    return m


def liblo_encode(path, args):
    """The exact bytes liblo would put on the wire for this message."""
    size = ctypes.c_size_t(0)
    p = lo.lo_message_serialise(liblo_message(args), path.encode(), None, ctypes.byref(size))
    return ctypes.string_at(p, size.value)


def liblo_decode(packet):
    """Parse `packet` with liblo. Returns (path, result_code, typetags); result 0 means accepted."""
    buf = ctypes.create_string_buffer(packet, len(packet))
    res = ctypes.c_int(0)
    m = lo.lo_message_deserialise(ctypes.cast(buf, ctypes.c_void_p), len(packet), ctypes.byref(res))
    if not m:
        return None, res.value, None
    return packet.split(b"\0", 1)[0].decode(), res.value, (lo.lo_message_get_types(m) or b"").decode()


# --- liblo encodes, we must match ---------------------------------------------------

@pytest.mark.parametrize("path,args,ours", [
    ("/sk/a/param/speed", [(FLOAT, 0.5)], osc.encode("/sk/a/param/speed", 0.5)),
    ("/sk/cfg/route", [(INT, 2)], osc.encode("/sk/cfg/route", 2)),
    ("/sk/dev/reset", [(STRING, b"A")], osc.encode("/sk/dev/reset", "A")),
    ("/sk/a/pad/play", [(TRUE, None)], osc.encode("/sk/a/pad/play", True)),
    ("/sk/a/fx/grit", [(FALSE, None)], osc.encode("/sk/a/fx/grit", False)),
    ("/sk/midi/note", [(INT, 144), (INT, 60)], osc.encode("/sk/midi/note", 144, 60)),
    # 0xC0 is SLIP's END and 0xDB its ESC. -2.0 is exactly 0xC0000000, so this is not a contrived
    # payload - it is an ordinary parameter value, and it is the one a bridge that forgot to escape
    # would corrupt while every other value worked.
    ("/sk/a/param/speed", [(FLOAT, -2.0)], osc.encode("/sk/a/param/speed", -2.0)),
])
def test_liblo_serialises_byte_identically(path, args, ours):
    assert liblo_encode(path, args) == ours


def test_our_decoder_reads_liblo_bytes():
    packet = liblo_encode("/sk/a/param/speed", [(FLOAT, 0.5)])
    assert osc.decode(packet) == ("/sk/a/param/speed", [0.5])


# --- we encode, liblo must accept ---------------------------------------------------

@pytest.mark.parametrize("packet,types", [
    (osc.encode("/sk/a/param/speed", 0.5), "f"),
    (osc.encode("/sk/midi/note", 144, 60), "ii"),
    (osc.encode("/sk/dev/reset", "A"), "s"),
    (osc.encode("/sk/a/pad/play", True), "T"),
])
def test_liblo_accepts_our_bytes(packet, types):
    path, result, got = liblo_decode(packet)
    assert result == 0, "liblo rejected our packet (code {})".format(result)
    assert got == types
    assert path.startswith("/sk/")


def test_liblo_accepts_every_row_of_the_real_firmware_bundle():
    """The strongest available claim: liblo parses what the FIRMWARE emitted, row by row.

    `host/build/describe_osc_sample.bin` comes from the C++ encode path via
    `host/test_terminal_osc.cpp`, so this is a foreign reference implementation validating real
    firmware output rather than one of ours validating another.
    """
    if not os.path.exists(SAMPLE):
        pytest.skip("run `make -C host test-terminal-osc` to produce {}".format(SAMPLE))
    raw = open(SAMPLE, "rb").read()
    assert osc.is_bundle(raw)
    off, rows = 16, 0
    while off + 4 <= len(raw):
        (size,) = struct.unpack_from(">i", raw, off)
        off += 4
        _path, result, _types = liblo_decode(raw[off:off + size])
        assert result == 0, "liblo rejected describe row {} (code {})".format(rows, result)
        off += size
        rows += 1
    assert rows > 1
    assert rows == len(osc.decode_packet(raw)), "liblo and our decoder found the same row count"


# --- the read form: the one place the two spellings differ --------------------------

def test_a_read_is_spelled_differently_by_liblo_and_both_are_reads():
    """liblo cannot omit the type-tag string; this codec's encoder does.

    OSC 1.0 permits a message with no type-tag string at all (for pre-1.0 senders) and this codec uses
    that as its READ form - "arity, not a verb". liblo, like python-osc and Max, always emits at least
    a comma, so a read from any of them arrives as an EMPTY tag string instead. The packets genuinely
    differ on the wire; both must mean zero arguments, or every read from every third-party client
    fails while the in-house ones pass.

    The firmware side of this is asserted in `host/test_terminal_osc.cpp` against its own decoder.
    """
    theirs = liblo_encode("/sk/a/param/speed", [])
    ours = osc.encode("/sk/a/param/speed")
    assert theirs != ours, "if these ever match, this test has stopped saying anything"
    assert theirs.endswith(b",\0\0\0"), "liblo emits an empty type-tag string"
    assert ours == b"/sk/a/param/speed\0\0\0", "we emit none at all"
    # Both decode to zero arguments, which is what makes them the same request.
    assert osc.decode(theirs) == ("/sk/a/param/speed", [])
    assert osc.decode(ours) == ("/sk/a/param/speed", [])


# --- end to end: liblo -> UDP -> bridge -> SLIP -------------------------------------

def test_liblo_reaches_the_device_through_the_bridge():
    """The complete path a TouchOSC or Max user takes, with liblo standing in for the surface.

    liblo opens its own UDP socket and sends its own bytes; the bridge SLIP-frames them; the far end
    of a pty receives exactly what a device would. This is the test the whole bridge exists to make
    possible - before it, foreign OSC had no way to reach this hardware at all.
    """
    serial = pytest.importorskip("serial", reason="the pty rig needs pyserial")
    master, slave = os.openpty()
    ser = serial.Serial(os.ttyname(slave), timeout=0)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    port = sock.getsockname()[1]
    bridge = Bridge(ser, sock, log=open(os.devnull, "w"))
    thread = threading.Thread(target=bridge.serve_forever, kwargs={"timeout": 0.02}, daemon=True)
    thread.start()
    try:
        target = lo.lo_address_new(b"127.0.0.1", str(port).encode())
        assert target, "liblo could not build an address"
        msg = liblo_message([(FLOAT, 0.5)])
        assert lo.lo_send_message(target, b"/sk/a/param/speed", msg) != -1, "liblo failed to send"

        dec = osc.SlipDecoder()
        deadline = time.time() + 2.0
        frames = []
        while not frames and time.time() < deadline:
            frames = dec.feed(os.read(master, 4096))
        assert frames, "nothing reached the device end"
        # Byte-identical to what the device's own encoder would have produced for this write.
        assert frames[0] == osc.encode("/sk/a/param/speed", 0.5)
        assert osc.decode(frames[0]) == ("/sk/a/param/speed", [0.5])
    finally:
        bridge.stop()
        thread.join(timeout=2.0)
        sock.close()
        ser.close()
        os.close(master)
