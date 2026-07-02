#!/usr/bin/env python3
"""skterm.py - interactive REPL for the sk-engines terminal channel.

A thin human front-end over :class:`skdev.device.Device`. On start it opens the
USB-C CDC port and runs ``describe`` to build a completion vocabulary from the
verb set plus the device's own params, configs, and decks. Each entered line is
sent verbatim and the reply is rendered (``ok``/results in green, ``err`` in red,
``describe`` as its multi-line block).

Line features:
  * readline editing with history persisted to ``~/.skterm_history``.
  * describe-driven tab completion (verbs, then param/config names, then A/B).
  * ``[tag]`` log lines shown dimmed; a background reader prints async logs while
    idle when ``!log on`` (default off / quiet).

Command kinds:
  * plain line    - sent verbatim to the device.
  * ``@path``     - source a file of commands (one per line, ``#`` comments).
  * ``!cmd``      - local command, never sent: ``!quit``, ``!reconnect``,
                    ``!log on|off``, ``!describe``, ``!port <dev>``.
  * ``!name``     - run a named macro from ``~/.skterm_macros`` (``name: cmd; cmd``).

Usage:
    python skterm.py [PORT]     # PORT optional; auto-discovered by USB VID otherwise

Exits cleanly (skips, does not crash) when no device is attached.
"""

import os
import sys
import threading
import time

try:
    import readline
except ImportError:                       # pragma: no cover - readline absent (e.g. Windows)
    readline = None

from skdev.device import Device
from skdev.protocol import CommandError, Timeout, is_log

GREEN, RED, DIM, RST = "\033[32m", "\033[31m", "\033[2m", "\033[0m"

HISTORY = os.path.expanduser("~/.skterm_history")
MACROS = os.path.expanduser("~/.skterm_macros")

VERBS = ["set", "get", "query", "cv", "gate", "midi", "pad", "fx", "config",
         "mode", "caps", "describe", "help"]


class Repl:
    def __init__(self, port=None):
        self.port = port
        self.show_log = False
        self._lock = threading.Lock()      # serializes all serial access
        self._stop = threading.Event()
        self._reader = None
        self._connect(port)

    # --- connection ----------------------------------------------------------
    def _connect(self, port):
        self.dev = Device(port, log_sink=self._on_log)
        self.port = self.dev.port
        self.desc = self.dev.describe()
        self._install_completer()
        self._start_reader()
        print("{}connected {}{}{} {} ({} params, {} configs){}".format(
            DIM, RST + GREEN, self.desc.engine, DIM, self.desc.version,
            len(self.desc.params), len(self.desc.configs), RST))

    def _start_reader(self):
        if self._reader and self._reader.is_alive():
            return
        self._stop.clear()
        self._reader = threading.Thread(target=self._log_reader, daemon=True)
        self._reader.start()

    def _log_reader(self):
        """Background: while idle and logging is on, drain and print [tag] lines."""
        while not self._stop.is_set():
            if not self.show_log:
                time.sleep(0.1)
                continue
            raw = b""
            if self._lock.acquire(timeout=0.2):
                try:
                    raw = self.dev.ser.readline()
                except Exception:
                    self._stop.set()
                    break
                finally:
                    self._lock.release()
            if not raw:
                continue
            line = raw.decode(errors="replace").rstrip("\r\n")
            if line and is_log(line):
                print("{}{}{}".format(DIM, line, RST))

    # --- logging -------------------------------------------------------------
    def _on_log(self, line):
        # Called from the foreground command path (reply interleave). Print if enabled.
        if self.show_log:
            print("{}{}{}".format(DIM, line, RST))

    # --- completion ----------------------------------------------------------
    def _vocab(self, text, state):
        words = VERBS + list(self.desc.params) + list(self.desc.configs) + ["A", "B"]
        opts = [w for w in words if w.startswith(text)]
        return opts[state] if state < len(opts) else None

    def _install_completer(self):
        if readline is None:
            return
        readline.set_completer(self._vocab)
        readline.parse_and_bind("tab: complete")

    # --- main loop -----------------------------------------------------------
    def run(self):
        if readline is not None and os.path.exists(HISTORY):
            try:
                readline.read_history_file(HISTORY)
            except OSError:
                pass
        try:
            while True:
                try:
                    line = input("sk> ").strip()
                except (EOFError, KeyboardInterrupt):
                    print()
                    break
                if not line:
                    continue
                if line.startswith("!"):
                    self._local(line)
                    continue
                if line.startswith("@"):
                    self._source(line[1:].strip())
                    continue
                self._send(line)
        finally:
            self._shutdown()

    def _shutdown(self):
        self._stop.set()
        if readline is not None:
            try:
                readline.write_history_file(HISTORY)
            except OSError:
                pass
        try:
            self.dev.close()
        except Exception:
            pass

    # --- sending -------------------------------------------------------------
    def _send(self, line):
        try:
            with self._lock:
                if line == "describe":
                    self.desc = self.dev.describe()
                    self._install_completer()
                    print("{}{} {}{} ({} params)".format(
                        GREEN, self.desc.engine, self.desc.version, RST,
                        len(self.desc.params)))
                else:
                    out = self.dev.cmd(line)
                    print("{}ok{}{}".format(GREEN, (" " + out) if out else "", RST))
        except CommandError as e:
            print("{}err {}{}".format(RED, e.reason, RST))
        except Timeout:
            print("{}timeout{}".format(RED, RST))

    # --- @file sourcing ------------------------------------------------------
    def _source(self, path):
        path = os.path.expanduser(path)
        try:
            with open(path) as f:
                for ln in f:
                    ln = ln.split("#", 1)[0].strip()
                    if not ln:
                        continue
                    print("{}@ {}{}".format(DIM, ln, RST))
                    self._send(ln)
        except OSError as e:
            print("{}cannot source {}: {}{}".format(RED, path, e, RST))

    # --- local (!) commands --------------------------------------------------
    def _local(self, line):
        parts = line[1:].split(None, 1)
        if not parts:
            print("{}empty local command{}".format(RED, RST))
            return
        cmd = parts[0]
        arg = parts[1].strip() if len(parts) > 1 else ""
        if cmd == "quit" or cmd == "q":
            raise SystemExit(0)
        elif cmd == "log":
            self.show_log = (arg == "on")
            print("{}log {}{}".format(DIM, "on" if self.show_log else "off", RST))
        elif cmd == "reconnect":
            self._reconnect()
        elif cmd == "port":
            if not arg:
                print("{}current port: {}{}".format(DIM, self.port, RST))
            else:
                self._reconnect(arg)
        elif cmd == "describe":
            self._redescribe()
        else:
            if not self._run_macro(cmd):
                print("{}unknown local: !{}{}".format(RED, cmd, RST))

    def _reconnect(self, port=None):
        self._stop.set()
        try:
            self.dev.close()
        except Exception:
            pass
        target = port if port is not None else self.port
        try:
            self._connect(target)
        except Timeout as e:
            print("{}reconnect failed: {}{}".format(RED, e, RST))

    def _redescribe(self):
        try:
            with self._lock:
                self.desc = self.dev.describe()
            self._install_completer()
            print("{}{} {}{} ({} params, {} configs){}".format(
                GREEN, self.desc.engine, self.desc.version, DIM,
                len(self.desc.params), len(self.desc.configs), RST))
        except (CommandError, Timeout) as e:
            print("{}describe failed: {}{}".format(RED, e, RST))

    # --- named macros --------------------------------------------------------
    def _run_macro(self, name):
        """Look up ``name`` in ~/.skterm_macros; run its ';'-joined commands."""
        if not os.path.exists(MACROS):
            return False
        try:
            with open(MACROS) as f:
                for raw in f:
                    raw = raw.split("#", 1)[0].strip()
                    if not raw or ":" not in raw:
                        continue
                    key, body = raw.split(":", 1)
                    if key.strip() != name:
                        continue
                    for cmd in body.split(";"):
                        cmd = cmd.strip()
                        if cmd:
                            print("{}! {}{}".format(DIM, cmd, RST))
                            self._send(cmd)
                    return True
        except OSError:
            return False
        return False


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else None
    try:
        repl = Repl(port)
    except Timeout as e:
        print("{}no sk-engines device attached: {}{}".format(RED, e, RST),
              file=sys.stderr)
        return 0
    repl.run()
    return 0


if __name__ == "__main__":
    sys.exit(main())
