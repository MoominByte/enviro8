#!/usr/bin/env python3
"""Back up and restore an Enviro8's settings (Wi-Fi, IP, channel types, offsets).

USB (complete, includes the Wi-Fi password):
    python tools/enviro8_backup.py backup  --port COM5
    python tools/enviro8_backup.py restore --port COM5 --file backups/<name>.bin

HTTP (no Wi-Fi password: the API never returns it):
    python tools/enviro8_backup.py backup  --ip 10.0.37.130
    python tools/enviro8_backup.py restore --ip 10.0.37.130 --file backups/<name>.json [--password X]

    python tools/enviro8_backup.py decode backups/<name>.bin

USB backup dumps the NVS partition (0x9000, 0x6000 bytes, see partitions.csv)
and decodes the "app"/"config" blob into JSON next to it. Restoring the .bin
only works while NVS keeps that offset and size. Backups hold the Wi-Fi
password in clear text and are git-ignored.
"""

import argparse
import datetime
import json
import pathlib
import struct
import subprocess
import sys
import urllib.request

NVS_OFFSET = 0x9000
NVS_SIZE = 0x6000
PAGE_SIZE = 4096
ENTRY_SIZE = 32
ENTRIES_PER_PAGE = 126

TYPE_U8 = 0x01
TYPE_BLOB_DATA = 0x42
TYPE_BLOB_IDX = 0x48

# Mirrors sensor_type_t in main/app_config.h.
SENSOR_TYPES = ["Disabled", "Auto", "SHT45", "SHT41", "SHT40",
                "SHT35", "SHT31", "SHT30", "SHT20", "Other"]
CHANNELS = 8
# app_config_t on ESP32-C6: bool + char[33] + char[65] + 4 x char[16],
# 1 pad byte, then 8 x {enum, float, float}.
CONFIG_SIZE = 260
CHANNELS_OFFSET = 164

BACKUP_DIR = pathlib.Path(__file__).resolve().parent.parent / "backups"


def cstr(raw):
    return raw.split(b"\0", 1)[0].decode("utf-8", "replace")


def nvs_entries(image):
    """Yield (page_seq, entry_index, page, entry bytes) for every written entry."""
    for p in range(len(image) // PAGE_SIZE):
        page = image[p * PAGE_SIZE:(p + 1) * PAGE_SIZE]
        state, seq = struct.unpack_from("<II", page, 0)
        if state in (0xFFFFFFFF, 0x00000000):  # uninitialized / corrupt
            continue
        bitmap = page[32:64]
        i = 0
        while i < ENTRIES_PER_PAGE:
            bits = (bitmap[i // 4] >> ((i % 4) * 2)) & 0x3
            if bits != 0b10:  # only "written"
                i += 1
                continue
            off = 64 + i * ENTRY_SIZE
            entry = page[off:off + ENTRY_SIZE]
            span = entry[2] or 1
            yield seq, i, page, entry
            i += span


def nvs_read_blob(image, namespace, key):
    ns_index = None
    for _, _, _, e in nvs_entries(image):
        if e[0] == 0 and e[1] == TYPE_U8 and cstr(e[8:24]) == namespace:
            ns_index = e[24]
    if ns_index is None:
        raise SystemExit(f"namespace '{namespace}' not found in NVS")

    index = None
    chunks = {}
    for seq, i, page, e in nvs_entries(image):
        if e[0] != ns_index or cstr(e[8:24]) != key:
            continue
        if e[1] == TYPE_BLOB_IDX:
            size, count, start = struct.unpack_from("<IBB", e, 24)
            if index is None or seq >= index[0]:
                index = (seq, size, count, start)
        elif e[1] == TYPE_BLOB_DATA:
            size = struct.unpack_from("<H", e, 24)[0]
            off = 64 + (i + 1) * ENTRY_SIZE
            chunks[e[3]] = page[off:off + size]
    if index is None:
        raise SystemExit(f"blob '{namespace}/{key}' not found in NVS")

    _, size, count, start = index
    data = b"".join(chunks.get(start + c, b"") for c in range(count))
    if len(data) != size:
        raise SystemExit(f"blob '{namespace}/{key}' incomplete ({len(data)}/{size} bytes)")
    return data


def decode_config(blob):
    if len(blob) != CONFIG_SIZE:
        raise SystemExit(f"config blob is {len(blob)} bytes, expected {CONFIG_SIZE}: "
                         "app_config_t layout changed, update this tool")
    cfg = {
        "network": {
            "dhcp": bool(blob[0]),
            "ssid": cstr(blob[1:34]),
            "password": cstr(blob[34:99]),
            "ip": cstr(blob[99:115]),
            "gateway": cstr(blob[115:131]),
            "netmask": cstr(blob[131:147]),
            "dns": cstr(blob[147:163]),
        },
        "channels": [],
    }
    for ch in range(CHANNELS):
        t, temp, hum = struct.unpack_from("<iff", blob, CHANNELS_OFFSET + ch * 12)
        cfg["channels"].append({
            "channel": ch + 1,
            "type": SENSOR_TYPES[t] if 0 <= t < len(SENSOR_TYPES) else "Other",
            "temperatureOffset": round(temp, 4),
            "humidityOffset": round(hum, 4),
        })
    return cfg


def esptool(port, *args):
    cmd = [sys.executable, "-m", "esptool", "--chip", "esp32c6", "--port", port, *args]
    print(">", " ".join(cmd))
    subprocess.run(cmd, check=True)


def http(method, url, body=None):
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(url, data=data, method=method,
                                 headers={"Content-Type": "application/json",
                                          "Connection": "close"})
    with urllib.request.urlopen(req, timeout=20) as resp:
        return json.loads(resp.read())


def write_json(path, cfg):
    path.write_text(json.dumps(cfg, indent=2) + "\n", encoding="utf-8")
    print(f"saved {path}")


def cmd_backup(args):
    BACKUP_DIR.mkdir(exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    if args.port:
        bin_path = BACKUP_DIR / f"enviro8-nvs-{stamp}.bin"
        esptool(args.port, "read_flash", hex(NVS_OFFSET), hex(NVS_SIZE), str(bin_path))
        print(f"saved {bin_path}")
        cfg = decode_config(nvs_read_blob(bin_path.read_bytes(), "app", "config"))
        write_json(bin_path.with_suffix(".json"), cfg)
    else:
        cfg = http("GET", f"http://{args.ip}/api/v1/config")
        cfg["network"]["password"] = None  # not exposed by the API
        write_json(BACKUP_DIR / f"enviro8-{args.ip}-{stamp}.json", cfg)
    print(json.dumps({**cfg, "network": {**cfg["network"], "password": "***"}}, indent=2))


def cmd_restore(args):
    path = pathlib.Path(args.file)
    if args.port:
        if path.suffix != ".bin":
            raise SystemExit("USB restore needs the .bin NVS image")
        esptool(args.port, "write_flash", hex(NVS_OFFSET), str(path))
        return

    cfg = json.loads(path.read_text(encoding="utf-8"))
    base = f"http://{args.ip}/api/v1"
    print(json.dumps(http("PUT", f"{base}/config/channels", {"channels": cfg["channels"]}), indent=2))
    net = dict(cfg["network"])
    net["password"] = args.password or net.get("password") or ""
    if not net["password"]:
        del net["password"]  # keep the one already on the device
    print(http("PUT", f"{base}/config/network", net))
    print("network settings apply after a reboot (POST /api/v1/reboot)")


def cmd_decode(args):
    cfg = decode_config(nvs_read_blob(pathlib.Path(args.file).read_bytes(), "app", "config"))
    print(json.dumps(cfg, indent=2))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    for name in ("backup", "restore"):
        p = sub.add_parser(name)
        target = p.add_mutually_exclusive_group(required=True)
        target.add_argument("--port", help="serial port, e.g. COM5")
        target.add_argument("--ip", help="device IP")
        if name == "restore":
            p.add_argument("--file", required=True)
            p.add_argument("--password", help="Wi-Fi password for an HTTP restore")
    p = sub.add_parser("decode")
    p.add_argument("file")

    args = ap.parse_args()
    {"backup": cmd_backup, "restore": cmd_restore, "decode": cmd_decode}[args.cmd](args)


if __name__ == "__main__":
    main()
