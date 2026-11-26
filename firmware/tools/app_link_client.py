#!/usr/bin/env python3
"""Test client for the C5's app link (main/app_link.c, wire format in main/app_link_proto.h).

Needs the `cryptography` package (use a throwaway venv, not the IDF env):
  python3 -m venv /tmp/v && /tmp/v/bin/pip install cryptography
  /tmp/v/bin/python tools/app_link_client.py --state /tmp/link.json pair
  /tmp/v/bin/python tools/app_link_client.py --state /tmp/link.json get
  /tmp/v/bin/python tools/app_link_client.py --state /tmp/link.json set overdue=30
  /tmp/v/bin/python tools/app_link_client.py --state /tmp/link.json forget

The unit's address comes from --host, or from DNS-SD (_meddot._udp via the border routers).
The state file holds the pairing key: keep it private, delete it when done.
"""
import argparse
import hashlib
import hmac
import json
import os
import re
import socket
import struct
import subprocess
import sys
import time

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

PORT = 49232
MSG_PAIR, MSG_PAIR_WAIT, MSG_PAIR_OK, MSG_PAIR_NO, MSG_REQ, MSG_RESP = 1, 2, 3, 4, 5, 6
OP_GET, OP_SET, OP_FORGET, OP_GET_DOSES, OP_SET_DOSE, OP_DELETE_DOSE = 1, 2, 3, 4, 5, 6
STATUS = {0: "ok", 1: "unknown client (pair again)", 2: "bad value", 3: "bad op", 4: "refused"}
DAYS = ["SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"]
FIELDS = ["overdue"]  # mask bit 0


def hdr(t):
    return b"MA" + bytes([1, t])


def discover():
    """First _meddot._udp instance -> IPv6 address, via macOS dns-sd."""
    def run(args, pattern, secs=4):
        p = subprocess.Popen(["dns-sd"] + args, stdout=subprocess.PIPE)
        end = time.time() + secs
        try:
            os.set_blocking(p.stdout.fileno(), False)
            buf = ""
            while time.time() < end:
                chunk = (p.stdout.read() or b"").decode(errors="replace")
                buf += chunk
                m = re.search(pattern, buf, re.M)
                if m:
                    return m
                time.sleep(0.2)
        finally:
            p.kill()
        return None

    m = run(["-B", "_meddot._udp"], r"Add\s+\S+\s+\S+\s+(\S+)\s+_meddot\._udp\.\s+(.+)$")
    if not m:
        sys.exit("no _meddot._udp service found (is the C5 on the new firmware and on Thread?)")
    domain, instance = m.group(1), m.group(2).strip()
    m = run(["-L", instance, "_meddot._udp", domain], r"can be reached at (\S+?)\.?:(\d+)")
    if not m:
        sys.exit(f"could not resolve {instance}")
    host = m.group(1)
    m = run(["-G", "v6", host], r"\s((?i:f[0-9a-f]{3}:[0-9a-f:]+))(%\S*)?\s")
    if not m:
        sys.exit(f"no IPv6 address for {host}")
    addr = m.group(1).lower()
    print(f"found {instance} at {addr}")
    return addr


def exchange(sock, addr, msg, want, tries=5, timeout=1.0):
    sock.settimeout(timeout)
    for _ in range(tries):
        sock.sendto(msg, addr)
        try:
            while True:
                data, _ = sock.recvfrom(512)
                if data[:3] == b"MA\x01" and data[3] in want:
                    return data
        except socket.timeout:
            continue
    return None


def pair(sock, addr, state, name):
    priv = ec.generate_private_key(ec.SECP256R1())
    pub = priv.public_key().public_bytes(serialization.Encoding.X962,
                                         serialization.PublicFormat.UncompressedPoint)
    nm = name.encode()[:32]
    msg = hdr(MSG_PAIR) + pub + bytes([len(nm)]) + nm
    shown = False
    key = None
    deadline = time.time() + 70
    while time.time() < deadline:
        data = exchange(sock, addr, msg, {MSG_PAIR_WAIT, MSG_PAIR_OK, MSG_PAIR_NO}, tries=1)
        if not data:
            continue
        unit_pub = data[4:69]
        if data[3] == MSG_PAIR_NO:
            sys.exit("refused (cancelled on the C5, timed out, or another pairing is under way)")
        if key is None:
            peer = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), unit_pub)
            shared = priv.exchange(ec.ECDH(), peer)
            okm = HKDF(hashes.SHA256(), 36, b"meddot-pair-v1", pub + unit_pub).derive(shared)
            key, code = okm[:32], struct.unpack("<I", okm[32:36])[0] % 1000000
        if data[3] == MSG_PAIR_WAIT and not shown:
            print(f"code {code // 1000:03d} {code % 1000:03d}: tap Pair on the C5 if it shows the same")
            shown = True
        if data[3] == MSG_PAIR_OK:
            if not hmac.compare_digest(hmac.new(key, data[:70], hashlib.sha256).digest()[:16], data[70:86]):
                sys.exit("PAIR_OK with a bad MAC")
            state.update(client=data[69], key=key.hex(), counter=0)
            print(f"paired as app {data[69]}")
            return
        time.sleep(1)
    sys.exit("timed out")


def request(sock, addr, state, op, body=b""):
    key = bytes.fromhex(state["key"])
    state["counter"] += 1
    msg = hdr(MSG_REQ) + struct.pack("<BIB", state["client"], state["counter"], op) + body
    msg += hmac.new(key, msg, hashlib.sha256).digest()[:16]
    data = exchange(sock, addr, msg, {MSG_RESP})
    if not data:
        sys.exit("no answer")
    client, counter, rop, status = struct.unpack("<BIBB", data[4:11])
    op = rop
    if status == 1:
        sys.exit(STATUS[1])
    if not hmac.compare_digest(hmac.new(key, data[:-16], hashlib.sha256).digest()[:16], data[-16:]):
        sys.exit("answer with a bad MAC")
    if counter != state["counter"]:
        sys.exit("answer to another request")
    print("status:", STATUS.get(status, status))
    body = data[11:-16]
    if status == 4:
        print("reason:", body.decode(errors="replace"))
    elif op in (OP_GET, OP_SET) and len(body) == 4:
        print({"overdue": body[2]})
    elif body:
        for i in range(body[0]):
            uid, minute, days, color, name = struct.unpack("<BHBB16s", body[1 + 21 * i:22 + 21 * i])
            day = "daily" if days == 0x7F else DAYS[days.bit_length() - 1]
            print(f"  uid {uid:3d}  {minute // 60:02d}{minute % 60:02d}  {day:5s}  color {color:2d}  "
                  f"{name.rstrip(bytes(1)).decode()}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", help="the unit's IPv6 address (default: DNS-SD)")
    ap.add_argument("--state", required=True, help="JSON file for the pairing key")
    ap.add_argument("--name", default="Mac test")
    ap.add_argument("cmd", choices=["pair", "get", "set", "forget", "doses", "dose", "delete"])
    ap.add_argument("values", nargs="*", help="set: field=value (%s)" % ", ".join(FIELDS))
    a = ap.parse_args()

    state = json.load(open(a.state)) if os.path.exists(a.state) else {}
    host = a.host or state.get("host") or discover()
    state["host"] = host
    sock = socket.socket(socket.AF_INET6, socket.SOCK_DGRAM)
    addr = (host, PORT, 0, 0)
    try:
        if a.cmd == "pair":
            pair(sock, addr, state, a.name)
        elif "key" not in state:
            sys.exit("not paired: run pair first")
        elif a.cmd == "get":
            request(sock, addr, state, OP_GET)
        elif a.cmd == "doses":
            request(sock, addr, state, OP_GET_DOSES)
        elif a.cmd == "dose":  # dose uid=0 name=Test time=0300 days=daily|SUN.. color=0
            v = dict(x.split("=", 1) for x in a.values)
            t = int(v["time"])
            d = v.get("days", "daily").upper()
            days = 0x7F if d == "DAILY" else 1 << DAYS.index(d)
            body = struct.pack("<BHBB16s", int(v.get("uid", 0)), t // 100 * 60 + t % 100, days,
                               int(v.get("color", 0)), v["name"].encode())
            request(sock, addr, state, OP_SET_DOSE, body)
        elif a.cmd == "delete":
            request(sock, addr, state, OP_DELETE_DOSE, bytes([int(a.values[0].split("=")[-1])]))
        elif a.cmd == "forget":
            request(sock, addr, state, OP_FORGET)
            state.pop("key")
        else:
            vals = dict(v.split("=") for v in a.values)
            mask = 0x01 if "overdue" in vals else 0
            mask |= 0x02 if "bogus" in vals else 0  # tests that other bits are refused
            request(sock, addr, state, OP_SET, bytes([1, mask, int(vals.get("overdue", 0)), 0]))
    finally:
        with open(a.state, "w") as f:
            json.dump(state, f)
        os.chmod(a.state, 0o600)


if __name__ == "__main__":
    main()
