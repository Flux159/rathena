#!/usr/bin/env python3
"""A headless RO client, just enough to put one character in the world.

Logs in as tester/tester123 (PACKETVER 20211103, RagexeRE), selects slot 0,
enters the map server, finishes loading (so OnPCLoginEvent runs), sends the
skill-ups given on the command line, and then stays connected -- ticking so
the server keeps it -- until `--seconds` have passed or the map server's
log (`--log`) says "S|end". Meanwhile it does what the scenario asks of a
player in that log: "S|client use <index>" uses an inventory item,
"S|client produce <item id>" picks an entry of a crafting menu, and
"S|client relog" logs out and back in.
"""
import argparse
import socket
import struct
import sys
import time


def recv_exact(s, n, timeout=10.0):
    s.settimeout(timeout)
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise RuntimeError("connection closed")
        buf += chunk
    return buf


def drain(s, seconds):
    s.settimeout(0.2)
    end = time.time() + seconds
    got = b""
    while time.time() < end:
        try:
            chunk = s.recv(65536)
            if not chunk:
                raise RuntimeError("connection closed")
            got += chunk
        except socket.timeout:
            pass
    return got


def login(host, port, user, password):
    s = socket.create_connection((host, port))
    pkt = struct.pack("<HI24s24sB", 0x0064, 55, user.encode(), password.encode(), 0)
    s.sendall(pkt)
    cmd, = struct.unpack("<H", recv_exact(s, 2))
    if cmd == 0x006A or cmd == 0x083E:
        raise RuntimeError("login refused: %r" % recv_exact(s, 20 if cmd == 0x006A else 24))
    if cmd != 0x0AC4:
        raise RuntimeError("unexpected login answer 0x%04x" % cmd)
    length, = struct.unpack("<H", recv_exact(s, 2))
    body = recv_exact(s, length - 4)
    login_id1, aid, login_id2 = struct.unpack_from("<III", body, 0)
    sex = body[42]
    s.close()
    return aid, login_id1, login_id2, sex


def select_char(host, port, aid, login_id1, login_id2, sex, slot):
    s = socket.create_connection((host, port))
    s.sendall(struct.pack("<HIIIHB", 0x0065, aid, login_id1, login_id2, 0, sex))
    echoed, = struct.unpack("<I", recv_exact(s, 4))
    assert echoed == aid, "char server echoed %d" % echoed
    drain(s, 1.5)  # the character list; not needed
    s.sendall(struct.pack("<HB", 0x0066, slot))
    while True:
        cmd, = struct.unpack("<H", recv_exact(s, 2, 15))
        if cmd == 0x0AC5:
            body = recv_exact(s, 154)
            cid, = struct.unpack_from("<I", body, 0)
            mapname = body[4:20].split(b"\0")[0].decode()
            ip = socket.inet_ntoa(body[20:24])
            port, = struct.unpack_from("<H", body, 24)
            return s, cid, mapname, ip, port
        if cmd == 0x006C:
            raise RuntimeError("char select refused")
        if cmd == 0x0840:
            raise RuntimeError("char server: map server not available")
        # Something left over from the list; skip what we can't frame.
        drain(s, 0.5)


def enter_map(ip, port, aid, cid, login_id1, sex):
    s = socket.create_connection((ip, port))
    tick = int(time.time() * 1000) & 0xFFFFFFFF
    s.sendall(struct.pack("<HIIIIIB", 0x0436, aid, cid, login_id1, tick, 0, sex))
    cmd, echoed = struct.unpack("<HI", recv_exact(s, 6))  # ZC_AID
    assert cmd == 0x0283 and echoed == aid, "map server answered 0x%04x %d" % (cmd, echoed)
    first = recv_exact(s, 2, 15)  # ZC_ACCEPT_ENTER: the auth went through
    drain(s, 0.5)
    s.sendall(struct.pack("<H", 0x007D))  # CZ_NOTIFY_ACTORINIT: done loading
    return s


def connect(args):
    """Login, character select and map: the map server's socket and the
    char server's, and the account id."""
    aid, l1, l2, sex = login(args.host, args.login_port, args.user, args.password)
    print("login ok: aid %d sex %d" % (aid, sex), flush=True)
    cs, cid, mapname, ip, port = select_char(args.host, args.char_port, aid, l1, l2, sex, 0)
    print("char ok: cid %d on %s, map server %s:%d" % (cid, mapname, ip, port), flush=True)
    ms = enter_map(args.host if ip.startswith("0.") else ip, port, aid, cid, l1, sex)
    print("in game", flush=True)
    return aid, cs, ms


def reconnect(args, tries=10):
    """connect(), again while the servers still count the last session."""
    for _ in range(tries):
        try:
            return connect(args)
        except (RuntimeError, OSError, AssertionError) as e:
            print("not yet: %s" % e, flush=True)
            time.sleep(2)
    raise RuntimeError("could not log in again")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--login-port", type=int, default=6900)
    ap.add_argument("--char-port", type=int, default=6121)
    ap.add_argument("--user", default="tester")
    ap.add_argument("--password", default="tester123")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--log", help="map server log to watch for S|end")
    ap.add_argument("--skillup", type=int, action="append", default=[])
    args = ap.parse_args()

    aid, cs, ms = connect(args)
    time.sleep(0.5)
    for skill_id in args.skillup:
        ms.sendall(struct.pack("<HH", 0x0112, skill_id))  # CZ_UPGRADE_SKLEVEL
        time.sleep(0.2)
    end = time.time() + args.seconds
    next_tick = 0
    done_actions = 0
    while time.time() < end:
        if time.time() >= next_tick:
            ms.sendall(struct.pack("<HI", 0x0360, int(time.time() * 1000) & 0xFFFFFFFF))
            next_tick = time.time() + 10
        drain(ms, 0.5)
        if args.log:
            try:
                with open(args.log, errors="replace") as f:
                    text = f.read()
            except FileNotFoundError:
                continue
            # What the scenario asks the client to do: "S|client use <index>"
            # (CZ_USE_ITEM2) and "S|client produce <item id>" (CZ_REQMAKINGITEM).
            actions = [line.split("S|client ", 1)[1].split() for line in text.splitlines() if "S|client " in line]
            for action in actions[done_actions:]:
                print("client action %s" % " ".join(action), flush=True)
                if action[0] == "use":
                    ms.sendall(struct.pack("<HHI", 0x0439, int(action[1]) + 2, aid))
                elif action[0] == "produce":
                    ms.sendall(struct.pack("<HIIII", 0x018E, int(action[1]), 0, 0, 0))
                elif action[0] == "relog":
                    # Long enough offline that a timer still running would show.
                    ms.close()
                    cs.close()
                    time.sleep(6)
                    aid, cs, ms = reconnect(args)
                    next_tick = 0
            done_actions = len(actions)
            if "S|end" in text:
                print("scenario finished", flush=True)
                break
    ms.close()
    cs.close()


if __name__ == "__main__":
    sys.exit(main())
