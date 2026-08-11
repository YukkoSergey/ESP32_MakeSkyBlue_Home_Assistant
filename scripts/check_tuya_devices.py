#!/usr/bin/env python3
"""Check all Tuya devices and report online/offline status."""
import hashlib
import hmac
import json
import time
import urllib.parse
import urllib.request
import os

CID = os.environ.get("TUYA_CLIENT_ID", "")
SEC = os.environ.get("TUYA_CLIENT_SECRET", "")
BASE = "https://openapi.tuyaeu.com"


def _sign(method, path, params=None, body="", tok=""):
    t = str(int(time.time() * 1000))
    ch = hashlib.sha256(body.encode()).hexdigest()
    if params:
        qs = "&".join(f"{k}={v}" for k, v in sorted(params.items()))
        canonical = f"{path}?{qs}"
    else:
        canonical = path
    msg = CID + tok + t + "" + f"{method}\n{ch}\n\n{canonical}"
    sig = hmac.new(SEC.encode(), msg.encode(), hashlib.sha256).hexdigest().upper()
    url = f"{BASE}{path}?{urllib.parse.urlencode(params)}" if params else f"{BASE}{path}"
    return url, t, sig


def get(path, params=None, tok=""):
    url, t, sig = _sign("GET", path, params, tok=tok)
    headers = {
        "client_id": CID,
        "sign": sig,
        "t": t,
        "nonce": "",
        "sign_method": "HMAC-SHA256",
    }
    if tok:
        headers["access_token"] = tok
    return json.loads(
        urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=15).read()
    )


def get_token():
    resp = get("/v1.0/token", {"grant_type": "1"})
    if not resp.get("success"):
        raise RuntimeError(f"Token error: {resp.get('msg')}")
    return resp["result"]["access_token"], resp["result"]["uid"]


def list_all_devices(tok, uid):
    devices = []
    last_row_key = ""
    page = 1
    while True:
        params = {"uid": uid, "page_size": "50"}
        if last_row_key:
            params["last_row_key"] = last_row_key
        resp = get("/v1.0/iot-01/associated-users/devices", params, tok)
        if not resp.get("success"):
            raise RuntimeError(f"API error {resp.get('code')}: {resp.get('msg')}")
        result = resp["result"]
        batch = result.get("devices", [])
        devices.extend(batch)
        print(f"  page {page}: {len(batch)} devices")
        if not result.get("has_more", False) or not batch:
            break
        last_row_key = result.get("last_row_key", "")
        page += 1
    return devices


def main():
    if not CID or not SEC:
        print("Error: Set TUYA_CLIENT_ID and TUYA_CLIENT_SECRET environment variables")
        exit(1)

    print("Fetching device list from Tuya Cloud...")
    tok, uid = get_token()
    print(f"Account UID: {uid}\n")
    devices = list_all_devices(tok, uid)

    online = []
    offline = []

    print(f"\n{'Name':<40} {'Online':<8} {'Category'}")
    print("-" * 70)
    for d in sorted(devices, key=lambda x: x.get("name", "")):
        name = d.get('name', '')[:38]
        online_status = d.get('online', '')
        category = d.get('category', '')
        status_str = "🟢" if online_status else "🔴"
        print(f"{name:<40} {status_str:<8} {category}")
        if online_status:
            online.append(d)
        else:
            offline.append(d)

    print(f"\n{'='*70}")
    print(f"🟢 Online:  {len(online)}")
    print(f"🔴 Offline: {len(offline)}")

    if offline:
        print(f"\n🔴 OFFLINE DEVICES:")
        for d in offline:
            print(f"  - {d.get('name')} [{d.get('id')}]")

    exit(0 if len(offline) == 0 else 1)


if __name__ == "__main__":
    main()