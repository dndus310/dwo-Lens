"""Minimal dwo-Forge live viewer used to verify the DwoStateMonitor live link end to end.

Listens on 127.0.0.1:<port> (default 47731), turns auto-observe on as soon as a game
connects, collects JSON Lines for N seconds, prints a summary and exits non-zero unless
it received a hello and snaps for GameMode, GameState and PC0 (and PC0.Pawn when
--require-pawn is given) whose values are JSON objects.

Python standard library only:
    python tests/live_probe.py --seconds 20 [--port 47731] [--require-pawn] [--dump out.jsonl]
"""

import argparse
import json
import socket
import sys
import time


def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--port", type=int, default=47731)
    parser.add_argument("--seconds", type=float, default=20.0, help="collect for this long after the first connection")
    parser.add_argument("--wait", type=float, default=120.0, help="give up if no game connects within this time")
    parser.add_argument("--interval", type=float, default=0.25)
    parser.add_argument("--engine", action="store_true", help="ask for engine-declared properties too")
    parser.add_argument("--require-pawn", action="store_true")
    parser.add_argument("--dump", help="write every received line to this file")
    return parser.parse_args(argv)


def send(conn, message):
    conn.sendall((json.dumps(message) + "\n").encode("utf-8"))


def main(argv):
    args = parse_args(argv)
    server = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    server.bind(("127.0.0.1", args.port))
    server.listen(4)
    server.settimeout(args.wait)
    print(f"probe: listening on 127.0.0.1:{args.port}", flush=True)
    try:
        conn, peer = server.accept()
    except socket.timeout:
        print("FAIL: no game connected", flush=True)
        return 2
    print(f"probe: game connected from {peer[0]}:{peer[1]}", flush=True)
    send(conn, {"t": "auto", "on": True, "interval": args.interval, "engine": args.engine})

    conn.settimeout(0.2)
    buffer = b""
    lines = []
    bad_lines = []
    deadline = time.monotonic() + args.seconds
    closed = False
    resynced = False
    resync_index = 0
    while time.monotonic() < deadline and not closed:
        try:
            chunk = conn.recv(65536)
            if not chunk:
                closed = True
            buffer += chunk
        except socket.timeout:
            pass
        while b"\n" in buffer:
            raw, buffer = buffer.split(b"\n", 1)
            try:
                lines.append(json.loads(raw.decode("utf-8")))
            except (UnicodeDecodeError, json.JSONDecodeError) as error:
                bad_lines.append((str(error), raw[:200]))
        # Exercise the other commands once the stream is running.
        if not resynced and time.monotonic() > deadline - args.seconds / 2:
            send(conn, {"t": "pause", "on": True})
            send(conn, {"t": "unknown-command", "x": 1})
            send(conn, {"t": "pause", "on": False})
            send(conn, {"t": "resync"})
            resynced = True
            resync_index = len(lines)
    conn.close()
    server.close()

    if args.dump:
        with open(args.dump, "w", encoding="utf-8") as out:
            for message in lines:
                out.write(json.dumps(message, ensure_ascii=False) + "\n")

    hellos = [m for m in lines if m.get("t") == "hello"]
    snaps = [m for m in lines if m.get("t") == "snap"]
    removes = [m for m in lines if m.get("t") == "remove"]
    worlds = [m for m in lines if m.get("t") == "world"]
    latest = {}
    counts = {}
    for snap in snaps:
        latest[snap.get("id")] = snap
        counts[snap.get("id")] = counts.get(snap.get("id"), 0) + 1

    print(f"probe: {len(lines)} messages ({len(hellos)} hello, {len(snaps)} snap, {len(removes)} remove, "
          f"{len(worlds)} world), {len(bad_lines)} unparsable, connection closed early: {closed}")
    if hellos:
        print("hello: " + json.dumps(hellos[0]))
    for source_id in sorted(latest):
        snap = latest[source_id]
        value = snap.get("value")
        fields = len(value) if isinstance(value, dict) else "?"
        print(f"  {source_id:<40} x{counts[source_id]:<4} {snap.get('kind')}/{snap.get('type')} fields={fields} "
              f"auto={snap.get('auto')} truncated={snap.get('truncated')} object={snap.get('object')}")

    failures = []
    if bad_lines:
        failures.append(f"{len(bad_lines)} lines were not valid JSON, first: {bad_lines[0]}")
    if not hellos:
        failures.append("no hello")
    else:
        hello = hellos[0]
        for key in ("v", "project", "world", "netMode", "pie", "pid", "engine", "build", "time"):
            if key not in hello:
                failures.append(f"hello misses {key}")
        if hello.get("v") != 1:
            failures.append("hello version is not 1")
    required = ["GameMode", "GameState", "PC0"] + (["PC0.Pawn"] if args.require_pawn else [])
    for source_id in required:
        snap = latest.get(source_id)
        if snap is None:
            failures.append(f"no snap for {source_id}")
            continue
        if not isinstance(snap.get("value"), dict):
            failures.append(f"{source_id} value is not a JSON object")
        if snap.get("kind") != "object" or not snap.get("object") or snap.get("auto") is not True:
            failures.append(f"{source_id} is not an auto object snap")
        if not isinstance(snap.get("changes"), list) or not isinstance(snap.get("seq"), int):
            failures.append(f"{source_id} has malformed seq/changes")
    if resynced and snaps:
        resent = [m for m in lines[resync_index:] if m.get("t") == "snap" and m.get("id") == "GameMode"]
        if not resent:
            failures.append("resync did not resend GameMode")

    if failures:
        for failure in failures:
            print("FAIL: " + failure)
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
