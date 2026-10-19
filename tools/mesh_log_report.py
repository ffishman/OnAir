#!/usr/bin/env python3
"""Analysis of the Mesh recording (Mesh > Settings > Recording, or dect2cli --mesh-record <folder>).

    python3 tools/mesh_log_report.py <folder> [--home LAT,LON] [--days N] [--out report.html]

Reads every mesh-packets-*.csv and mesh-readings-*.csv in the folder and writes one HTML page (no other software needed, the charts
are drawn in the page) with:
  - the period, packets, frames with a CRC error, packets not decrypted, packet types
  - each node: packets, days heard, first and last heard, SNR (10 % / median / 90 %), hops, battery and voltage range, reboots
    (uptime going back), position and distance from --home
  - by hour of the day: packets, median SNR, CRC errors, channel use (when the network is busy, when the band opens)
  - by day: packets, nodes heard, new nodes, messages
  - charts: packets and SNR by hour, SNR of the nodes most heard over time, battery voltage over time, distance against SNR
A short summary is printed too.
"""
import argparse
import csv
import glob
import html
import math
import os
import statistics
import sys
from collections import Counter, defaultdict
from datetime import datetime


def num(s):
    try:
        return float(s) if s not in ("", None) else None
    except ValueError:
        return None


def load(folder, kind, since):
    rows = []
    for path in sorted(glob.glob(os.path.join(folder, "mesh-%s-*.csv" % kind))):
        with open(path, newline="", encoding="utf-8", errors="replace") as f:
            for r in csv.DictReader(f):
                t = num(r.get("unix_time"))
                if t is None or (since and t < since):
                    continue
                r["_t"] = t
                rows.append(r)
    rows.sort(key=lambda r: r["_t"])
    return rows


def pct(v, p):
    if not v:
        return None
    v = sorted(v)
    k = (len(v) - 1) * p
    lo, hi = math.floor(k), math.ceil(k)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def km(lat1, lon1, lat2, lon2):
    r = 6371.0
    p1, p2 = math.radians(lat1), math.radians(lat2)
    dp, dl = p2 - p1, math.radians(lon2 - lon1)
    a = math.sin(dp / 2) ** 2 + math.cos(p1) * math.cos(p2) * math.sin(dl / 2) ** 2
    return 2 * r * math.asin(math.sqrt(a))


def f1(v, d=1, unit=""):
    return "" if v is None else ("%.*f%s" % (d, v, unit))


def when(t):
    return datetime.fromtimestamp(t).strftime("%Y-%m-%d %H:%M")


# ---- charts (inline SVG) -------------------------------------------------------------------------------------------------------

COLORS = ["#2a78d6", "#d6602a", "#2aa876", "#a83ad6", "#c9a227", "#d63a6a", "#3ab4c9", "#7a7a7a"]


def svg_bars(values, labels, title, unit="", w=640, h=200):
    vals = [v if v is not None else 0 for v in values]
    top = max(vals) if vals and max(vals) > 0 else 1
    bw = (w - 50) / max(1, len(vals))
    out = ['<svg viewBox="0 0 %d %d" class="chart"><text x="4" y="14" class="t">%s</text>' % (w, h, html.escape(title))]
    for i, v in enumerate(vals):
        bh = (h - 50) * v / top
        x = 40 + i * bw
        out.append('<rect x="%.1f" y="%.1f" width="%.1f" height="%.1f" fill="#2a78d6"><title>%s: %s%s</title></rect>'
                   % (x + 1, h - 25 - bh, max(1, bw - 2), bh, html.escape(str(labels[i])), f1(values[i], 1), unit))
        if len(vals) <= 31 or i % max(1, len(vals) // 24) == 0:
            out.append('<text x="%.1f" y="%d" class="a">%s</text>' % (x + bw / 2 - 6, h - 10, html.escape(str(labels[i]))))
    out.append('<text x="2" y="%d" class="a">%s%s</text></svg>' % (h - 25 - (h - 50) + 10, f1(top, 0), unit))
    return "".join(out)


def svg_lines(series, title, unit="", w=640, h=220, scatter=False, xlabel=None):
    """series: [(name, [(x, y), ...])]; x is a unix time unless xlabel is given."""
    pts = [p for _, s in series for p in s if p[1] is not None]
    if not pts:
        return ""
    x0, x1 = min(p[0] for p in pts), max(p[0] for p in pts)
    y0, y1 = min(p[1] for p in pts), max(p[1] for p in pts)
    if x1 == x0:
        x1 = x0 + 1
    if y1 == y0:
        y1, y0 = y0 + 1, y0 - 1
    X = lambda x: 45 + (w - 60) * (x - x0) / (x1 - x0)
    Y = lambda y: h - 30 - (h - 55) * (y - y0) / (y1 - y0)
    out = ['<svg viewBox="0 0 %d %d" class="chart"><text x="4" y="14" class="t">%s</text>' % (w, h, html.escape(title))]
    out.append('<text x="2" y="%.0f" class="a">%s</text><text x="2" y="%.0f" class="a">%s</text>' % (Y(y1) + 4, f1(y1, 1) + unit, Y(y0), f1(y0, 1) + unit))
    if xlabel:
        out.append('<text x="45" y="%d" class="a">%s</text><text x="%d" y="%d" class="a" text-anchor="end">%s</text>'
                   % (h - 8, f1(x0, 0), w - 15, h - 8, f1(x1, 0) + " " + xlabel))
    else:
        out.append('<text x="45" y="%d" class="a">%s</text><text x="%d" y="%d" class="a" text-anchor="end">%s</text>'
                   % (h - 8, when(x0), w - 15, h - 8, when(x1)))
    for i, (name, s) in enumerate(series):
        c = COLORS[i % len(COLORS)]
        s = [p for p in s if p[1] is not None]
        if scatter:
            for p in s:
                out.append('<circle cx="%.1f" cy="%.1f" r="3.5" fill="%s"><title>%s</title></circle>' % (X(p[0]), Y(p[1]), c, html.escape(p[2] if len(p) > 2 else name)))
        elif s:
            out.append('<polyline fill="none" stroke="%s" stroke-width="1.5" points="%s"><title>%s</title></polyline>'
                       % (c, " ".join("%.1f,%.1f" % (X(p[0]), Y(p[1])) for p in s), html.escape(name)))
    if not scatter and len(series) > 1:
        for i, (name, _) in enumerate(series):
            out.append('<text x="%d" y="%d" class="a" style="fill:%s">%s</text>' % (w - 170, 28 + 12 * i, COLORS[i % len(COLORS)], html.escape(name[:28])))
    out.append("</svg>")
    return "".join(out)


def table(head, rows):
    o = ["<table><tr>" + "".join("<th>%s</th>" % html.escape(h) for h in head) + "</tr>"]
    for r in rows:
        o.append("<tr>" + "".join("<td>%s</td>" % html.escape(str(c)) for c in r) + "</tr>")
    return "".join(o) + "</table>"


# ---- analysis ------------------------------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("folder")
    ap.add_argument("--home", help="receiver position LAT,LON for the distances")
    ap.add_argument("--days", type=float, help="only the last N days")
    ap.add_argument("--out", help="the HTML file (default: <folder>/mesh-analysis.html)")
    a = ap.parse_args()
    since = (datetime.now().timestamp() - a.days * 86400) if a.days else None
    pk = load(a.folder, "packets", since)
    rd = load(a.folder, "readings", since)
    if not pk and not rd:
        sys.exit("no mesh-packets-*.csv or mesh-readings-*.csv in " + a.folder)
    home = tuple(float(x) for x in a.home.split(",")) if a.home else None

    t0 = min(r["_t"] for r in pk + rd)
    t1 = max(r["_t"] for r in pk + rd)
    good = [r for r in pk if r.get("crc_ok") == "1"]
    crc = len(pk) - len(good)
    notdec = sum(1 for r in good if r.get("network") == "Meshtastic" and r.get("decrypted") == "0")
    types = Counter(r.get("type") or "?" for r in good)

    # nodes
    # The SNR of a packet is that of the last transmitter, which for a relayed packet is the repeater, not the node: the node's own
    # signal statistics use only the packets heard directly (0 hops)
    N = defaultdict(lambda: {"snr": [], "st": [], "hops": [], "t": [], "name": "", "types": Counter(), "days": set()})
    for r in good:
        k = r.get("from")
        if not k:
            continue
        n = N[k]
        n["net"] = r.get("network")
        if r.get("name"):
            n["name"] = r["name"]
        s = num(r.get("snr_db"))
        h = num(r.get("hops"))
        if h is not None:
            n["hops"].append(int(h))
        if s is not None and (h == 0 or (h is None and r.get("network") != "Meshtastic")):
            n["snr"].append(s)
            n["st"].append(r["_t"])
        n["t"].append(r["_t"])
        n["types"][r.get("type")] += 1
        n["days"].add(r["time"][:10])
    R = defaultdict(list)
    for r in rd:
        R[r["id"]].append(r)
        if r.get("name") and r["id"] in N and not N[r["id"]]["name"]:
            N[r["id"]]["name"] = r["name"]
    nodes = []
    for k, n in N.items():
        rs = R.get(k, [])
        bat = [num(r["battery_pct"]) for r in rs if num(r["battery_pct"]) is not None]
        volt = [num(r["voltage"]) for r in rs if num(r["voltage"]) is not None]
        up = [(r["_t"], num(r["uptime_s"])) for r in rs if num(r["uptime_s"]) is not None]
        reboots = sum(1 for i in range(1, len(up)) if up[i][1] < up[i - 1][1])
        pos = [r for r in rs if num(r["lat"]) is not None]
        last = pos[-1] if pos else None
        dist = km(home[0], home[1], num(last["lat"]), num(last["lon"])) if (home and last) else None
        nodes.append({"id": k, "name": n["name"], "n": len(n["t"]), "days": len(n["days"]), "first": min(n["t"]), "last": max(n["t"]),
                      "p10": pct(n["snr"], .1), "med": pct(n["snr"], .5), "p90": pct(n["snr"], .9),
                      "hops": Counter(n["hops"]).most_common(1)[0][0] if n["hops"] else None,
                      "bat": (min(bat), max(bat)) if bat else None, "volt": (min(volt), max(volt)) if volt else None,
                      "reboots": reboots, "pos": last, "dist": dist, "snr": n["snr"], "st": n["st"], "t": n["t"],
                      "direct": sum(1 for h in n["hops"] if h == 0)})
    nodes.sort(key=lambda x: -x["n"])

    # by hour of the day
    H = defaultdict(lambda: {"n": 0, "crc": 0, "snr": [], "cu": []})
    for r in pk:
        h = int(r["time"][11:13])
        H[h]["n"] += 1
        if r.get("crc_ok") != "1":
            H[h]["crc"] += 1
        elif num(r.get("snr_db")) is not None and r.get("hops") in ("0", "") and not (r.get("hops") == "" and r.get("network") == "Meshtastic"):
            H[h]["snr"].append(num(r["snr_db"]))
    for r in rd:
        if num(r.get("channel_util_pct")) is not None:
            H[int(r["time"][11:13])]["cu"].append(num(r["channel_util_pct"]))
    days_span = max(1.0, (t1 - t0) / 86400)
    hours = list(range(24))

    # by day
    D = defaultdict(lambda: {"n": 0, "nodes": set(), "msg": 0})
    seen, new_by_day = set(), Counter()
    for r in good:
        d = r["time"][:10]
        D[d]["n"] += 1
        if r.get("from"):
            D[d]["nodes"].add(r["from"])
            if r["from"] not in seen:
                seen.add(r["from"])
                new_by_day[d] += 1
        if r.get("type") in ("TEXT_MESSAGE_APP", "GRP_TXT", "TXT_MSG"):
            D[d]["msg"] += 1
    days = sorted(D)

    # ---- text summary
    print("Period: %s - %s (%.1f days)" % (when(t0), when(t1), (t1 - t0) / 86400))
    print("Packets: %d good, %d with a CRC error (%.1f %%), %d not decrypted" % (len(good), crc, 100.0 * crc / max(1, len(pk)), notdec))
    print("Nodes: %d   Readings: %d" % (len(nodes), len(rd)))
    for n in nodes[:10]:
        sn = "SNR %s / %s / %s dB" % (f1(n["p10"]), f1(n["med"]), f1(n["p90"])) if n["snr"] else "relayed only"
        print("  %-28s %-10s %5d packets  %-26s %s" % ((n["name"] or "")[:28], n["id"], n["n"], sn,
                                                                       ("%.1f km" % n["dist"]) if n["dist"] is not None else ""))

    # ---- HTML
    o = ["""<!doctype html><meta charset="utf-8"><title>Mesh analysis</title><style>
body{font:14px -apple-system,Segoe UI,sans-serif;margin:24px;max-width:1100px;color:#222;background:#fff}
h1{font-size:20px}h2{font-size:16px;margin-top:28px}table{border-collapse:collapse;font-size:12px}td,th{border-bottom:1px solid #ddd;padding:3px 8px;text-align:left}
.chart{width:100%;max-width:760px;background:#fafafa;border:1px solid #eee;margin:6px 0}.t{font-size:12px;font-weight:600}.a{font-size:10px;fill:#666}
@media(prefers-color-scheme:dark){body{background:#151515;color:#ddd}.chart{background:#1d1d1d;border-color:#333}td,th{border-color:#333}.a{fill:#aaa}.t{fill:#ddd}}
</style><h1>Mesh analysis</h1>"""]
    o.append("<p>%s - %s (%.1f days). %d packets, %d with a CRC error (%.1f %%), %d not decrypted. %d nodes, %d telemetry / position reports.</p>"
             % (when(t0), when(t1), (t1 - t0) / 86400, len(good), crc, 100.0 * crc / max(1, len(pk)), notdec, len(nodes), len(rd)))
    o.append("<p>Packet types: %s</p>" % html.escape(", ".join("%s %d" % kv for kv in types.most_common())))

    o.append("<h2>Nodes</h2><p>The SNR of a relayed packet is the repeater's, not the node's: the SNR columns use only the packets heard "
             "directly (Direct = how many). A node only heard through repeaters has no SNR of its own here.</p>")
    o.append(table(["Node", "Id", "Packets", "Days", "First", "Last", "Direct", "SNR direct 10% / med / 90%", "Hops", "Battery %", "Voltage", "Reboots", "Position", "km"],
                   [[n["name"], n["id"], n["n"], n["days"], when(n["first"]), when(n["last"]), n["direct"], "%s / %s / %s" % (f1(n["p10"]), f1(n["med"]), f1(n["p90"])),
                     "" if n["hops"] is None else n["hops"], "" if not n["bat"] else "%.0f - %.0f" % n["bat"], "" if not n["volt"] else "%.2f - %.2f" % n["volt"],
                     n["reboots"] or "", "" if not n["pos"] else "%s, %s" % (n["pos"]["lat"], n["pos"]["lon"]), f1(n["dist"])] for n in nodes]))

    o.append("<h2>By hour of the day</h2>")
    o.append(svg_bars([H[h]["n"] / days_span for h in hours], hours, "Packets per hour (average over the days)"))
    o.append(svg_bars([pct(H[h]["snr"], .5) for h in hours], hours, "Median SNR by hour (packets heard directly)", " dB"))
    o.append(svg_bars([100.0 * H[h]["crc"] / H[h]["n"] if H[h]["n"] else None for h in hours], hours, "Frames with a CRC error by hour (interference, weak frames)", " %"))
    if any(H[h]["cu"] for h in hours):
        o.append(svg_bars([pct(H[h]["cu"], .5) for h in hours], hours, "Channel utilisation reported by the nodes, median by hour", " %"))

    o.append("<h2>By day</h2>")
    o.append(table(["Day", "Packets", "Nodes", "New nodes", "Messages"], [[d, D[d]["n"], len(D[d]["nodes"]), new_by_day.get(d, 0), D[d]["msg"]] for d in days]))
    if len(days) > 1:
        o.append(svg_bars([D[d]["n"] for d in days], [d[5:] for d in days], "Packets per day"))

    top = [n for n in nodes if len(n["snr"]) > 1][:6]
    if top:
        o.append("<h2>Signal over time</h2><p>Hourly median SNR of the nodes heard most. A slow fall over weeks points at the antenna or the cable; "
                 "a rise at night or before a front points at the troposphere.</p>")
        ser = []
        for n in top:
            b = defaultdict(list)
            for t, s in zip(n["st"], n["snr"]):
                b[int(t // 3600) * 3600].append(s)
            ser.append((n["name"] or n["id"], [(k, statistics.median(v)) for k, v in sorted(b.items())]))
        o.append(svg_lines(ser, "SNR (dB), hourly median", " dB"))

    vs = [(n["name"] or n["id"], [(r["_t"], num(r["voltage"])) for r in R.get(n["id"], []) if num(r["voltage"]) is not None]) for n in nodes]
    vs = [s for s in vs if len(s[1]) > 1][:8]
    if vs:
        o.append("<h2>Battery</h2><p>Voltage of each node with battery telemetry: a solar node that does not get back to full by the afternoon, "
                 "or that falls a little more each night, is short of panel or battery.</p>")
        o.append(svg_lines(vs, "Battery voltage (V)", " V"))

    env = [(n["name"] or n["id"], [(r["_t"], num(r["temp_c"])) for r in R.get(n["id"], []) if num(r["temp_c"]) is not None]) for n in nodes]
    env = [s for s in env if len(s[1]) > 1][:6]
    if env:
        o.append("<h2>Sensors</h2>")
        o.append(svg_lines(env, "Temperature (C)", " C"))

    if home:
        pts = [(n["dist"], n["med"], "%s: %.1f km, %s dB" % (n["name"] or n["id"], n["dist"], f1(n["med"]))) for n in nodes if n["dist"] is not None and n["med"] is not None]
        if pts:
            o.append("<h2>Coverage</h2><p>Distance from the receiver against the median SNR of each node with a position (direct and relayed alike: "
                     "the hops column above tells them apart).</p>")
            o.append(svg_lines([("nodes", pts)], "Median SNR (dB) against distance (km)", " dB", scatter=True, xlabel="km"))

    out = a.out or os.path.join(a.folder, "mesh-analysis.html")
    with open(out, "w", encoding="utf-8") as f:
        f.write("".join(o))
    print("Written: " + out)


if __name__ == "__main__":
    main()
