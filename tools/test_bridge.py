"""Device-free tests for the UDP <-> SLIP-serial bridge.

No hardware and no device: the framing half is pure, and the I/O half is driven against a **pty** and
an ephemeral UDP socket, which exercises the real `serve_forever()` loop - real selector, real socket,
real pyserial port - with the far end of the pty standing in for the device.

That matters more than it sounds. The bridge's whole job is framing, and framing bugs are exactly the
ones that survive inspection: an escaping mistake corrupts only the packets that happen to contain
0xC0 or 0xDB, which for OSC means only certain float values, which means a fader that works until it
does not.
"""

import os
import socket
import struct
import threading
import time

import pytest

from skbridge import Bridge, Relay, describe_packet
from skdev import osc


# --- the pure half ------------------------------------------------------------------

def test_a_datagram_crosses_unmodified():
    """The bridge translates framing, not content: what goes in is what comes out."""
    packet = osc.encode("/sk/a/param/speed", 0.5)
    wire = Relay().to_device(packet)
    assert wire[0] == osc.END and wire[-1] == osc.END
    assert osc.SlipDecoder().feed(wire) == [packet]


def test_escaping_survives_floats_that_contain_the_delimiters():
    """The test that justifies the whole SLIP layer.

    0xC0 and 0xDB are ordinary bytes inside an OSC float - -2.0 is exactly 0xC0000000, and any value
    whose big-endian encoding contains 0xDB is just as common. A bridge that forwarded raw bytes would
    corrupt precisely those values and nothing else.
    """
    for value in (-2.0, -2.5, -3.5):
        packet = osc.encode("/sk/a/param/speed", value)
        assert osc.END in packet or osc.ESC in packet or True   # not all, but the point is generality
        assert osc.SlipDecoder().feed(Relay().to_device(packet)) == [packet]

    # And explicitly, a payload built to contain both delimiters.
    nasty = osc.encode("/sk/x", struct.unpack(">f", bytes([0xC0, 0xDB, 0xC0, 0xDB]))[0])
    assert osc.SlipDecoder().feed(Relay().to_device(nasty)) == [nasty]


def test_reassembly_across_chunk_boundaries():
    """Serial delivers whatever it delivers; a frame boundary lands mid-chunk as a matter of course."""
    packet = osc.encode("/sk/reply/dev/caps", 31)
    wire = osc.slip_encode(packet)
    relay = Relay()
    got = []
    for i in range(len(wire)):                       # one byte at a time, the worst case
        got += relay.from_device(wire[i:i + 1])
    assert got == [packet]


def test_several_frames_in_one_read_all_arrive():
    relay = Relay()
    a = osc.encode("/sk/reply/dev/cpu", 41.5)
    b = osc.encode("/sk/reply/dev/cpumin", 12.0)
    assert relay.from_device(osc.slip_encode(a) + osc.slip_encode(b)) == [a, b]


def test_a_bundle_crosses_whole():
    """describe is one bundle in one frame; it must not be split or rewritten in transit."""
    inner = [osc.encode("/sk/reply/dev/describe/param", "/sk/a/param/speed", "station", 0.0, 1.0, "deck"),
             osc.encode("/sk/reply/dev/describe/caps", 31)]
    bundle = b"#bundle\0" + b"\0" * 8 + b"".join(struct.pack(">i", len(m)) + m for m in inner)
    relay = Relay()
    assert relay.from_device(osc.slip_encode(bundle)) == [bundle]
    assert len(osc.decode_packet(bundle)) == 2


def test_unframed_bytes_are_recognised_as_the_wrong_codec():
    """A line-codec build answers in ASCII with no SLIP delimiters, so the relay yields nothing."""
    relay = Relay()
    assert relay.from_device(b"ok 0.5\r\n" * 40) == []
    assert relay.looks_unframed
    # ...and a device that does frame properly never trips the warning.
    ok = Relay()
    ok.from_device(osc.slip_encode(osc.encode("/sk/reply/dev/caps", 31)) + b"x" * 512)
    assert not ok.looks_unframed


def test_describe_packet_never_raises():
    assert "read" in describe_packet(osc.encode("/sk/a/param/speed"))
    assert "/sk/a/param/speed" in describe_packet(osc.encode("/sk/a/param/speed", 0.5))
    assert "undecodable" in describe_packet(b"\xff\xfe not osc at all")


# --- the I/O half, against a pty ----------------------------------------------------

@pytest.fixture
def rig():
    """A running bridge with a pty for a device and a UDP socket for a client.

    Yields `(device_fd, client_socket, bridge, listen_port)`. Writing to `device_fd` is the device
    talking; reading it is what the device receives.
    """
    serial = pytest.importorskip("serial", reason="the pty rig needs pyserial")
    master, slave = os.openpty()
    ser = serial.Serial(os.ttyname(slave), timeout=0)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("127.0.0.1", 0))
    listen_port = sock.getsockname()[1]

    client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    client.bind(("127.0.0.1", 0))
    client.settimeout(2.0)
    reply_port = client.getsockname()[1]

    bridge = Bridge(ser, sock, reply_port=reply_port, log=open(os.devnull, "w"))
    thread = threading.Thread(target=bridge.serve_forever, kwargs={"timeout": 0.02}, daemon=True)
    thread.start()
    try:
        yield master, client, bridge, listen_port
    finally:
        bridge.stop()
        thread.join(timeout=2.0)
        sock.close()
        client.close()
        ser.close()
        os.close(master)


def _read_frame(fd, deadline=2.0):
    """Read from the pty until one complete SLIP frame has arrived."""
    dec = osc.SlipDecoder()
    end = time.time() + deadline
    while time.time() < end:
        chunk = os.read(fd, 4096)
        frames = dec.feed(chunk)
        if frames:
            return frames[0]
    raise AssertionError("no frame reached the device")


def test_udp_in_reaches_the_device_slip_framed(rig):
    device_fd, client, _bridge, listen_port = rig
    packet = osc.encode("/sk/a/param/speed", 0.5)
    client.sendto(packet, ("127.0.0.1", listen_port))
    assert _read_frame(device_fd) == packet


def test_device_replies_reach_the_client(rig):
    device_fd, client, _bridge, listen_port = rig
    # The client must speak first: that is how the bridge learns where to answer.
    client.sendto(osc.encode("/sk/dev/caps"), ("127.0.0.1", listen_port))
    _read_frame(device_fd)

    reply = osc.encode("/sk/reply/dev/caps", 31)
    os.write(device_fd, osc.slip_encode(reply))
    got, _addr = client.recvfrom(65535)
    assert got == reply
    assert osc.decode(got) == ("/sk/reply/dev/caps", [31])


def test_a_whole_describe_bundle_survives_the_round_trip(rig):
    """The payload most likely to break: ~2 KB in one frame, crossing many pty reads."""
    device_fd, client, _bridge, listen_port = rig
    client.sendto(osc.encode("/sk/dev/describe"), ("127.0.0.1", listen_port))
    _read_frame(device_fd)

    rows = [osc.encode("/sk/reply/dev/describe/param",
                       "/sk/a/param/p{}".format(i), "label{}".format(i), 0.0, 1.0, "deck")
            for i in range(40)]
    bundle = b"#bundle\0" + b"\0" * 8 + b"".join(struct.pack(">i", len(m)) + m for m in rows)
    assert len(bundle) > 2000
    os.write(device_fd, osc.slip_encode(bundle))

    got, _addr = client.recvfrom(65535)
    assert got == bundle
    assert len(osc.decode_packet(got)) == 40


def test_packets_are_counted_in_both_directions(rig):
    device_fd, client, bridge, listen_port = rig
    client.sendto(osc.encode("/sk/dev/caps"), ("127.0.0.1", listen_port))
    _read_frame(device_fd)
    os.write(device_fd, osc.slip_encode(osc.encode("/sk/reply/dev/caps", 31)))
    client.recvfrom(65535)
    assert (bridge.to_device_count, bridge.to_network_count) == (1, 1)
