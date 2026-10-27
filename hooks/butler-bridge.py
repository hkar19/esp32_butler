#!/usr/bin/env python3
"""The butler bridge: the one program that talks to the board over USB serial (see README.md, "How it talks").

Hooks, ./bt.sh and any other agent send it one JSON request per line on 127.0.0.1:47474 (BUTLER_BRIDGE_PORT); it
passes each to the board and sends back the board's answer line. It keeps the serial port to itself, so reports from
many sessions never interleave and nothing writes to the board while esptool flashes it (./bt.sh asks the bridge to
let go first). It remembers each session's last report and sends them again whenever the board starts or is plugged
back in, so unplugging or flashing the board loses nothing. It also sends a health request every 15 s: the board
shows no_host when that stops.

  butler-bridge.py send '<json>'   send one request and print the answer; starts the bridge if it isn't running
  butler-bridge.py serve           run the bridge in the foreground (the hook starts it in the background)
  butler-bridge.py stop            stop a running bridge, e.g. after changing this file

The serial port is BUTLER_SERIAL, or ESPPORT in firmware/butler.env. Log: ~/.butler/bridge.log (./bt.sh log).
Only the Python standard library.
"""
import fcntl
import json
import os
import queue
import re
import select
import socket
import socketserver
import subprocess
import sys
import termios
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = "127.0.0.1"
PORT = int(os.environ.get("BUTLER_BRIDGE_PORT", "47474"))
STATE_DIR = os.path.expanduser("~/.butler")
LOG_PATH = os.path.join(STATE_DIR, "bridge.log")
PAUSE_PATH = os.path.join(STATE_DIR, "paused")   # ./bt.sh writes its PID here while it needs the port
LOG_MAX_BYTES = 1 << 20
REPLY_TIMEOUT_S = 1.0
HEARTBEAT_S = 15          # well inside the board's 45 s no_host timeout
REOPEN_S = 2
SESSION_KEEP_S = 43200    # the board's default BT_SESSION_TTL_S: older sessions aren't sent again

_log_lock = threading.Lock()
_log_to_stderr = False


def log(text):
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + text + "\n"
    with _log_lock:
        with open(LOG_PATH, "a") as f:
            f.write(line)
        if _log_to_stderr:
            sys.stderr.write(line)


def error(code, message):
    return {"error": {"code": code, "message": message}}


def serial_path():
    if os.environ.get("BUTLER_SERIAL"):
        return os.environ["BUTLER_SERIAL"]
    env = os.path.join(HERE, "..", "firmware", "butler.env")
    try:
        with open(env) as f:
            for line in f:
                m = re.match(r"""\s*ESPPORT=["']?([^"'\s#]+)""", line)
                if m:
                    return m.group(1)
    except OSError:
        pass
    sys.exit(f"butler-bridge: no serial port: set ESPPORT in {os.path.normpath(env)} or BUTLER_SERIAL")


def paused():
    """True while ./bt.sh needs the port. A pause file left by a ./bt.sh that is gone is removed."""
    try:
        with open(PAUSE_PATH) as f:
            pid = int(f.read().strip() or 0)
    except (OSError, ValueError):
        return False
    try:
        os.kill(pid, 0)
        return True
    except PermissionError:
        return True
    except OSError:
        log(f"removing a pause left by ./bt.sh (pid {pid}), which has exited")
        try:
            os.unlink(PAUSE_PATH)
        except OSError:
            pass
        return False


class Board:
    """The serial port: raw, 115200 8N1, exclusive."""

    def __init__(self, path):
        self.path, self.fd, self.buf = path, None, b""

    def open(self):
        fd = os.open(self.path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        try:
            fcntl.ioctl(fd, termios.TIOCEXCL)   # anything else that opens it (esptool) fails at once, loudly
            a = termios.tcgetattr(fd)
            a[0] = a[1] = a[3] = 0              # no input, output or line processing
            # No HUPCL: DTR and RTS drive the board's reset and boot pins, so closing the port must leave them be
            a[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
            a[4] = a[5] = termios.B115200
            termios.tcsetattr(fd, termios.TCSANOW, a)
            termios.tcflush(fd, termios.TCIOFLUSH)
        except OSError:
            os.close(fd)
            raise
        self.fd, self.buf = fd, b""

    def close(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def write_line(self, text):
        data = (text + "\n").encode()
        while data:
            try:
                data = data[os.write(self.fd, data):]
            except BlockingIOError:
                select.select([], [self.fd], [], 0.5)

    def read_line(self, timeout):
        """The next whole line, or None after timeout seconds. OSError when the board is gone."""
        deadline = time.monotonic() + max(timeout, 0)
        while b"\n" not in self.buf:
            left = deadline - time.monotonic()
            if not select.select([self.fd], [], [], max(left, 0))[0]:
                if left <= 0:
                    return None
                continue
            try:
                data = os.read(self.fd, 4096)
            except BlockingIOError:
                continue
            if not data:
                raise OSError("the board went away")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode(errors="replace").rstrip("\r")


class Bridge:
    def __init__(self, path):
        self.board = Board(path)
        self.requests = queue.Queue()   # (request, answer callback or None)
        self.sessions = {}              # session -> (its last state request, when)
        self.replay = False
        self.next_id = 1
        self.last_sent = 0.0
        self.retry_at = 0.0
        self.down = None                # why the board can't be reached, as last logged

    def run(self):
        while True:
            try:
                req, answer = self.requests.get(timeout=0.2)
            except queue.Empty:
                req, answer = None, None
            if req is not None:
                self.remember(req)
            reachable = self.reachable()
            try:
                if not reachable:
                    reply = error("no_board", self.down or "the board is not connected")
                elif self.replay:
                    self.send_sessions()
                if reachable and req is not None:
                    reply = self.request(req)
                elif reachable and time.monotonic() - self.last_sent >= HEARTBEAT_S:
                    self.request({"op": "health"})
                elif reachable:
                    self.read_lines(0)
            except OSError as e:
                log(f"lost the board: {e}")
                self.board.close()
                self.retry_at = time.monotonic() + REOPEN_S
                reply = error("no_board", str(e))
            if answer is not None:
                answer(reply)

    def reachable(self):
        """Opens the port if it can. False while paused, unplugged or busy."""
        if paused():
            if self.board.fd is not None:
                self.board.close()
                log("released the serial port for ./bt.sh")
            self.set_down("paused: ./bt.sh is using the serial port")
            return False
        if self.board.fd is None:
            if time.monotonic() < self.retry_at:
                return False
            try:
                self.board.open()
            except OSError as e:
                self.retry_at = time.monotonic() + REOPEN_S
                self.set_down(f"can't open {self.board.path}: {e.strerror or e}")
                return False
            log(f"opened {self.board.path}")
            self.down = None
            self.replay = True
        return True

    def set_down(self, why):
        if why != self.down:
            log(why + (f" (retrying every {REOPEN_S} s)" if why.startswith("can't") else ""))
            self.down = why

    def remember(self, req):
        if req.get("op") == "clear":
            self.sessions.clear()
        elif req.get("op") == "state" and isinstance(req.get("session"), str):
            if req.get("state") == "end":
                self.sessions.pop(req["session"], None)
            else:
                self.sessions[req["session"]] = (req, time.time())

    def send_sessions(self):
        self.replay = False
        now = time.time()
        self.sessions = {s: v for s, v in self.sessions.items() if now - v[1] < SESSION_KEEP_S}
        for req, _ in list(self.sessions.values()):
            self.request(req)
        if self.sessions:
            log(f"sent the board {len(self.sessions)} session(s) it had missed")

    def request(self, req):
        """Sends one request, and returns the board's answer to it (an error after REPLY_TIMEOUT_S)."""
        rid, self.next_id = self.next_id, self.next_id + 1
        self.board.write_line(json.dumps(dict(req, id=rid), separators=(",", ":")))
        self.last_sent = time.monotonic()
        deadline = time.monotonic() + REPLY_TIMEOUT_S
        while True:
            left = deadline - time.monotonic()
            reply = self.read_lines(left) if left > 0 else None
            if reply is None:
                return error("no_answer", f"the board did not answer within {REPLY_TIMEOUT_S:g} s")
            if reply.get("id") == rid:
                reply.pop("id")
                return reply

    def read_lines(self, timeout):
        """Logs the board's log lines until an answer arrives (returned) or timeout passes (None)."""
        while True:
            line = self.board.read_line(timeout)
            if line is None:
                return None
            if not line.startswith("@bt "):
                if line.strip():
                    log("board: " + line)
                continue
            try:
                msg = json.loads(line[4:])
            except ValueError:
                log("board sent a bad answer: " + line)
                continue
            if msg.get("hello"):
                log(f"board started (fw {msg.get('fw')})")
                self.replay = True
                continue
            return msg


class Handler(socketserver.StreamRequestHandler):
    timeout = 10

    def handle(self):
        try:
            for raw in iter(lambda: self.rfile.readline(4096), b""):
                try:
                    req = json.loads(raw)
                except ValueError:
                    req = None
                if not isinstance(req, dict) or not isinstance(req.get("op"), str):
                    reply = error("bad_request", "send one JSON object with \"op\" per line")
                elif req["op"] == "quit":
                    self.reply({"ok": True})
                    log("stopped (asked to)")
                    os._exit(0)
                else:
                    done, box = threading.Event(), {}
                    self.server.bridge.requests.put((req, lambda r: (box.update(r=r), done.set())))
                    reply = box["r"] if done.wait(5) else error("busy", "the bridge is busy, try again")
                self.reply(reply)
        except OSError:
            pass   # the hook doesn't wait for the answer and hangs up

    def reply(self, msg):
        self.wfile.write((json.dumps(msg, separators=(",", ":")) + "\n").encode())


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True


def serve(foreground):
    global _log_to_stderr
    _log_to_stderr = foreground
    os.makedirs(STATE_DIR, exist_ok=True)
    if os.path.exists(LOG_PATH) and os.path.getsize(LOG_PATH) > LOG_MAX_BYTES:
        os.replace(LOG_PATH, LOG_PATH + ".old")
    path = serial_path()
    try:
        server = Server((HOST, PORT), Handler)
    except OSError:
        if foreground:
            sys.exit(f"butler-bridge: port {PORT} is taken: is a bridge already running? (butler-bridge.py stop)")
        return   # another bridge started first: fine
    bridge = Bridge(path)
    server.bridge = bridge
    threading.Thread(target=bridge.run, name="serial", daemon=True).start()
    log(f"started (pid {os.getpid()}), board on {path}, listening on {HOST}:{PORT}")
    server.serve_forever()


def connect(start):
    """A connection to the bridge, starting one first if start and none is running."""
    deadline = time.monotonic() + 3
    started = False
    while True:
        try:
            return socket.create_connection((HOST, PORT), timeout=5)
        except ConnectionRefusedError:
            if not start:
                return None
            if not started:
                subprocess.Popen([sys.executable, os.path.abspath(__file__), "serve-detached"],
                                 stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True, close_fds=True)
                started = True
            if time.monotonic() > deadline:
                sys.exit("butler-bridge: the bridge did not start: see ~/.butler/bridge.log")
            time.sleep(0.05)


def call(sock, req):
    sock.sendall((json.dumps(req, separators=(",", ":")) + "\n").encode())
    answer = sock.makefile("rb").readline()
    return answer.decode().strip() if answer else ""


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "send" and len(sys.argv) == 3:
        try:
            req = json.loads(sys.argv[2])
        except ValueError as e:
            sys.exit(f"butler-bridge: not JSON: {e}")
        answer = call(connect(start=True), req)
        print(answer)
        sys.exit(0 if answer and '"error"' not in answer else 1)
    elif cmd in ("serve", "serve-detached"):
        serve(foreground=cmd == "serve")
    elif cmd == "stop":
        sock = connect(start=False)
        print(call(sock, {"op": "quit"}) if sock else "the bridge is not running")
    else:
        sys.exit("usage: butler-bridge.py send '<json>' | serve | stop")


if __name__ == "__main__":
    main()
