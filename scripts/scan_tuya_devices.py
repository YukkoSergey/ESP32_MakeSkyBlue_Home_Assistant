#!/usr/bin/env python3
"""ARP-scan the local network, identify Tuya devices by MAC OUI."""

import json
import re
import socket
import subprocess
import sys
import urllib.request
import time

SUBNET = "192.168.31"

# Known Tuya OUI prefixes (cached from maclookup API + manual)
TUYA_OUIS = {
    # Tuya Smart Inc. - Espressif-based modules
    # Confirmed on Yukko's network (2026-08-12):
    #   192.168.31.48  c4:82:e1:37:8e:7e
    #   192.168.31.49  f8:17:2d:b7:f9:cf
    #   192.168.31.62  38:a5:c9:9:13:f0
    #   192.168.31.133 fc:67:1f:e7:e9:1a
    #   192.168.31.148 f8:17:2d:7d:63:f7
    #   192.168.31.198 70:89:76:ec:e7:ec
    #   192.168.31.199 c0:f8:53:cb:64:45
    #   192.168.31.210 bc:35:1e:4d:69:a4
    # Also includes common Espressif OUI used by many IoT modules.
    "C482E1", "F8172D", "38A5C9", "FC671F", "708976", "BC351E",
    "C0F853",
    "D4A259", "000E8C", "10D561", "581F68", "5C3A45", "38A5C9",
    "F41F0B", "34CE00", "D8C80C", "A8F94B", "A4C138", "FCA563",
}


def get_oui_vendor(oui: str) -> str:
    """Look up vendor by OUI using maclookup API. Returns vendor or empty."""
    url = f"https://api.maclookup.app/v2/macs/{oui}"
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0"})
        with urllib.request.urlopen(req, timeout=5) as resp:
            data = json.loads(resp.read())
            return data.get("company", "")
    except Exception:
        return ""


def arp_sweep(subnet: str) -> list[tuple[str, str]]:
    """Ping sweep the /24 subnet, then read ARP table. Returns (ip, mac) pairs."""
    # Ping sweep in parallel chunks
    procs = []
    for i in range(1, 255):
        ip = f"{subnet}.{i}"
        p = subprocess.Popen(
            ["ping", "-c", "1", "-W", "0.1", ip],
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        procs.append(p)
        if len(procs) >= 50:
            for proc in procs:
                proc.wait()
            procs = []
    for p in procs:
        p.wait()

    # Read ARP table
    result = subprocess.run(["arp", "-a"], capture_output=True, text=True)
    devices = []
    for line in result.stdout.strip().split('\n'):
        if 'incomplete' in line or 'permanent' in line or 'mcast' in line:
            continue
        match = re.match(r"^\? \((\d+\.\d+\.\d+\.\d+)\) at \s*([0-9a-f:]{17})", line.strip())
        if match:
            ip, mac = match.groups()
            devices.append((ip, mac))
    return devices


def is_tuya_oui(mac: str) -> bool:
    oui = mac.replace(':', '')[:6].upper()
    return oui in TUYA_OUIS


def main():
    subnet = sys.argv[1] if len(sys.argv) > 1 else SUBNET
    print(f"=== Scanning {subnet}.0/24 ===\n")

    devices = arp_sweep(subnet)

    tuya_devices = []
    other_devices = []

    # Cache OUI lookups
    oui_cache = {}

    for ip, mac in devices:
        if is_tuya_oui(mac):
            tuya_devices.append((ip, mac))
        else:
            # Try vendor lookup
            oui = mac.replace(':', '')[:6].upper()
            if oui not in oui_cache:
                oui_cache[oui] = get_oui_vendor(oui)
            vendor = oui_cache[oui]
            other_devices.append((ip, mac, vendor))

    print("\n=== 🟢 TYUA DEVICES ===")
    for ip, mac in tuya_devices:
        print(f"  {ip:<18} {mac}")

    print("\n=== OTHER DEVICES ===")
    for ip, mac, vendor in other_devices:
        print(f"  {ip:<18} {mac:<20} {vendor}")

    print(f"\nTotal: {len(tuya_devices)} Tuya, {len(other_devices)} other")


if __name__ == "__main__":
    main()

