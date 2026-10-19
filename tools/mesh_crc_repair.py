#!/usr/bin/env python3
"""Repair Meshtastic frames that failed their LoRa CRC, from the mesh log (mesh-packets-YYYY-MM-DD*.csv, columns crc_payload_hex and
crc_rx, written by the receiver since the mesh log v2).

Two ways, each checked against the CRC the frame carried:
  1. one wrong bit: every single-bit change of the frame (and of its CRC) is tried; a unique one that makes the CRC right is taken.
  2. copies: a packet relayed by several nodes goes out several times with the same encrypted body (only the hop limit, the relay
     and next-hop bytes change). Copies of one packet (sender and packet id within a few bits, same length, within 3 minutes) are
     voted bit by bit; where two copies disagree with no third to settle it, every combination is tried (up to 2^12). A result is
     taken when it makes the CRC of two copies right, or of one copy when three or more voted.
A packet that also arrived with a good CRC is listed as such (nothing to repair). Repaired frames are decrypted with the default
LongFast key when possible and their port is shown.

  python3 tools/mesh_crc_repair.py ~/OnAir-mesh-log/mesh-packets-2026-10-16-c.csv [--csv out.csv]
  python3 tools/mesh_crc_repair.py --selftest
"""
import argparse, base64, csv, itertools, random, struct, sys
from collections import Counter

DEFAULT_KEY = base64.b64decode("1PG7OiApB1nwvP+rz05pAQ==")
PORTS = {1: "TEXT_MESSAGE_APP", 3: "POSITION_APP", 4: "NODEINFO_APP", 5: "ROUTING_APP", 6: "ADMIN_APP", 34: "PAXCOUNTER_APP",
         65: "STORE_FORWARD_APP", 67: "TELEMETRY_APP", 70: "TRACEROUTE_APP", 71: "NEIGHBORINFO_APP", 73: "MAP_REPORT_APP"}


def payload_crc(p):
    """The LoRa payload CRC as the receiver computes it (mesh_lora_code.cpp payloadCrc)."""
    crc = 0
    for v in p[:-2] if len(p) >= 2 else b"":
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if ((crc & 0x8000) >> 8) ^ (v & 0x80) else (crc << 1) & 0xFFFF
            v = (v << 1) & 0xFF
    if len(p) >= 1: crc ^= p[-1]
    if len(p) >= 2: crc ^= p[-2] << 8
    return crc


def header(p):
    to, fr, pid = struct.unpack("<III", p[:12])
    return {"to": to, "from": fr, "id": pid, "flags": p[12], "hash": p[13], "next": p[14], "relay": p[15]}


def bits(x): return bin(x).count("1")


def one_bit(p, crc):
    """The single-bit changes of the frame or of its CRC that make the CRC right: (frame, crc) pairs."""
    out = []
    b = bytearray(p)
    for i in range(len(b) * 8):
        b[i >> 3] ^= 0x80 >> (i & 7)
        if payload_crc(b) == crc: out.append((bytes(b), crc))
        b[i >> 3] ^= 0x80 >> (i & 7)
    for k in range(16):
        if payload_crc(p) == crc ^ (1 << k): out.append((p, crc ^ (1 << k)))
    return out


def vote(frames):
    """Bit by bit majority of equal-length frames; also the positions where the vote is a tie."""
    n = len(frames[0])
    out, ties = bytearray(n), []
    for i in range(n * 8):
        ones = sum((f[i >> 3] >> (7 - (i & 7))) & 1 for f in frames)
        if ones * 2 > len(frames): out[i >> 3] |= 0x80 >> (i & 7)
        elif ones * 2 == len(frames): ties.append(i)
    return out, ties


def own_fix(p, crc):
    """The CRC is right as it stands, or after one bit of the copy's own bytes (flags, next hop, relay) is changed: the frame then."""
    if payload_crc(p) == crc: return p
    b = bytearray(p)
    for byte in (12, 14, 15):
        for k in range(8):
            b[byte] ^= 1 << k
            if payload_crc(b) == crc: return bytes(b)
            b[byte] ^= 1 << k
    return None


def repair_group(copies, max_ties=10):
    """copies: [(frame bytes, crc)] of one packet. The body (from byte 16) and the packet fields (to, from, id, channel hash) are
    shared; flags, next hop and relay belong to each copy. Returns (body+shared bytes, list of copy indexes whose CRC it fixes)."""
    shared = [bytes(f[:14]) + bytes(f[16:]) for f, _ in copies]     # leave out next hop and relay (bytes 14, 15)
    flags_free = [bytes(f[:12]) + bytes(f[13:14]) + bytes(f[16:]) for f, _ in copies]
    best = None
    for view, rebuild in ((shared, lambda s, f: s[:14] + f[14:16] + s[14:]),
                          (flags_free, lambda s, f: s[:12] + f[12:13] + s[12:13] + f[14:16] + s[13:])):
        v, ties = vote(view)
        if len(ties) > max_ties: continue
        for combo in itertools.product((0, 1), repeat=len(ties)):
            cand = bytearray(v)
            for i, bit in zip(ties, combo):
                if bit: cand[i >> 3] |= 0x80 >> (i & 7)
                else: cand[i >> 3] &= ~(0x80 >> (i & 7)) & 0xFF
            ok = [k for k, (f, crc) in enumerate(copies) if own_fix(rebuild(bytes(cand), f), crc)]
            need = 1 if len(copies) >= 3 and not ties else 2
            if len(ok) >= need and (best is None or len(ok) > len(best[1])):
                best = (own_fix(rebuild(bytes(cand), copies[ok[0]][0]), copies[ok[0]][1]), ok)
        if best: return best
    return None


def fields(b):
    i, out = 0, []
    def varint(i):
        r = s = 0
        while True:
            c = b[i]; i += 1; r |= (c & 0x7F) << s; s += 7
            if c < 0x80: return r, i
    while i < len(b):
        k, i = varint(i); f, t = k >> 3, k & 7
        if t == 0: v, i = varint(i)
        elif t == 1: v = b[i:i + 8]; i += 8
        elif t == 5: v = b[i:i + 4]; i += 4
        elif t == 2:
            l, i = varint(i); v = b[i:i + l]; i += l
        else: raise ValueError(t)
        if i > len(b): raise ValueError("short")
        out.append((f, v))
    return out


def port_of(frame):
    """Decrypt with the default LongFast key: the Data port, or None (another key, or not a Data message)."""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
    except ImportError:
        return None
    h = header(frame)
    nonce = struct.pack("<QII", h["id"], h["from"], 0)
    dec = Cipher(algorithms.AES(DEFAULT_KEY), modes.CTR(nonce)).decryptor().update(frame[16:])
    try:
        d = dict(fields(dec))
    except Exception:
        return None
    p = d.get(1)
    return PORTS.get(p, str(p)) if isinstance(p, int) else None


def load(paths):
    rows = []
    for path in paths:
        with open(path, newline="") as f:
            for r in csv.DictReader(f):
                rows.append(r)
    return rows


def run(rows, out_csv=None):
    good = {}
    for r in rows:
        if r.get("crc_ok") == "1" and r.get("packet_id") and r.get("from", "").startswith("!"):
            good.setdefault((int(r["from"][1:], 16), int(r["packet_id"])), []).append(r)
    bad = []
    for r in rows:
        hx, cr = r.get("crc_payload_hex", ""), r.get("crc_rx", "")
        if not hx or not cr or len(hx) < 40: continue
        bad.append({"row": r, "frame": bytes.fromhex(hx), "crc": int(cr, 16), "t": float(r["unix_time"])})
    st, out = Counter(), []
    for b in bad:
        b["h"] = header(b["frame"])
        b["state"] = None
    # 1. a good copy of the same packet (sender and id within 4 bits)
    for b in bad:
        h = b["h"]
        for (fr, pid), rs in good.items():
            if bits(fr ^ h["from"]) + bits(pid ^ h["id"]) <= 4 and any(abs(float(r["unix_time"]) - b["t"]) < 180 for r in rs):
                b["state"] = "good copy"; b["fixed"] = None; b["port"] = rs[0].get("type"); break
    # 2. one wrong bit
    for b in bad:
        if b["state"]: continue
        sol = one_bit(b["frame"], b["crc"])
        if len(sol) == 1:
            b["state"] = "1 bit"; b["fixed"] = sol[0][0]; b["port"] = port_of(sol[0][0])
    # 3. copies voted
    left = [b for b in bad if not b["state"]]
    used = set()
    for i, b in enumerate(left):
        if i in used: continue
        grp = [i] + [j for j in range(i + 1, len(left)) if j not in used and len(left[j]["frame"]) == len(b["frame"])
                     and abs(left[j]["t"] - b["t"]) < 180
                     and bits(left[j]["h"]["from"] ^ b["h"]["from"]) + bits(left[j]["h"]["id"] ^ b["h"]["id"]) <= 6]
        if len(grp) < 2: continue
        res = repair_group([(left[k]["frame"], left[k]["crc"]) for k in grp])
        if not res: continue
        fixed, ok = res
        for k in grp:
            used.add(k)
            left[k]["state"] = "copies (%d)" % len(grp); left[k]["fixed"] = fixed; left[k]["port"] = port_of(fixed)
    for b in bad:
        s = b["state"] or "not repaired"
        st[s.split(" (")[0]] += 1
        h = header(b["fixed"]) if b.get("fixed") else b["h"]
        out.append({"time": b["row"]["time"], "snr_db": b["row"]["snr_db"], "state": s, "from": "!%08x" % h["from"],
                    "to": "all" if h["to"] == 0xFFFFFFFF else "!%08x" % h["to"], "packet_id": h["id"], "relay": "%02x" % h["relay"],
                    "port": b.get("port") or "", "frame_hex": b["fixed"].hex() if b.get("fixed") else ""})
    print("%d frames with a CRC error and the whole frame logged" % len(bad))
    for k in ("good copy", "1 bit", "copies", "not repaired"):
        print("  %-13s %d" % (k, st[k]))
    for o in out:
        if o["state"] not in ("good copy", "not repaired"):
            print("  %s %s dB  %-11s %s -> %s id %d relay %s %s" % (o["time"], o["snr_db"], o["state"], o["from"], o["to"], o["packet_id"], o["relay"], o["port"]))
    if out_csv:
        with open(out_csv, "w", newline="") as f:
            w = csv.DictWriter(f, fieldnames=list(out[0].keys()) if out else ["time"])
            w.writeheader(); w.writerows(out)
    return st


def selftest():
    rnd = random.Random(1)
    fails = 0
    body = bytes(rnd.getrandbits(8) for _ in range(40))
    def copy(relay, hl):
        p = struct.pack("<III", 0xFFFFFFFF, 0x433B07FC, 123456789) + bytes([(3 << 5) | hl, 0x08, 0, relay]) + body
        return p, payload_crc(p)
    def flip(p, n):
        b = bytearray(p)
        for i in rnd.sample(range(len(b) * 8), n): b[i >> 3] ^= 0x80 >> (i & 7)
        return bytes(b)
    p, c = copy(0xFC, 2)
    sol = one_bit(flip(p, 1), c)
    if not (len(sol) == 1 and sol[0][0] == p): print("FAIL one bit"); fails += 1
    # three copies from three relays, each with 3 wrong bits in different places: the vote rebuilds the packet
    cs = [copy(r, hl) for r, hl in ((0xAC, 2), (0xFC, 1), (0x0C, 1))]
    bad = [(flip(f, 3), crc) for f, crc in cs]
    res = repair_group(bad)
    if not res or res[0][16:] != body: print("FAIL three copies"); fails += 1
    # two copies, 4 wrong bits each: the ties are tried and both CRCs must agree
    bad2 = [(flip(f, 4), crc) for f, crc in cs[:2]]
    res2 = repair_group(bad2)
    if not res2 or res2[0][16:] != body or len(res2[1]) < 2: print("FAIL two copies"); fails += 1
    # nothing made up from noise
    junk = [(bytes(rnd.getrandbits(8) for _ in range(56)), rnd.getrandbits(16)) for _ in range(2)]
    if repair_group(junk): print("FAIL junk repaired"); fails += 1
    print("selftest:", "FAILED" if fails else "ok")
    return 1 if fails else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("csv", nargs="*")
    ap.add_argument("--csv-out", dest="out")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest: sys.exit(selftest())
    if not a.csv: ap.error("give the packets CSV files")
    run(load(a.csv), a.out)
