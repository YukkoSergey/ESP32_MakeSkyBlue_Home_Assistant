#!/usr/bin/env bash
# One-shot deploy of generator / heavy-load control from Home Assistant.
#
#   bash vps/home-assistant/deploy_generator_control.sh
#
# 1. A random HA_CMD_TOKEN goes into the dacha-bridge-env Secret and, as
#    "Bearer <token>", into HA /config/secrets.yaml (never printed).
# 2. New bridge.py + cluster-internal Service, bridge restarted.
# 3. HA: package copied, packages enabled in configuration.yaml, a "Дача"
#    dashboard view added; config checked before HA restarts.
# Every HA file touched is backed up as <file>.bak-<timestamp> first.
set -euo pipefail
cd "$(dirname "$0")/../.."
NS=homeassistant
TS=$(date +%Y%m%dT%H%M%S)
HA="kubectl -n $NS exec deploy/homeassistant -c homeassistant --"

echo "== token"
if kubectl -n $NS get secret dacha-bridge-env -o jsonpath='{.data.HA_CMD_TOKEN}' | grep -q .; then
    TOKEN=$(kubectl -n $NS get secret dacha-bridge-env -o jsonpath='{.data.HA_CMD_TOKEN}' | base64 -d)
    echo "reusing existing HA_CMD_TOKEN"
else
    TOKEN=$(python3 -c "import secrets; print(secrets.token_urlsafe(32))")
    kubectl -n $NS patch secret dacha-bridge-env --type merge \
        -p "{\"data\":{\"HA_CMD_TOKEN\":\"$(printf %s "$TOKEN" | base64 -w0)\"}}" >/dev/null
    echo "HA_CMD_TOKEN added to dacha-bridge-env"
fi

echo "== bridge"
kubectl -n $NS create configmap dacha-bridge-code --from-file=bridge.py=vps/dacha-bridge/bridge.py \
    --dry-run=client -o yaml | kubectl apply -f -
kubectl apply -f vps/dacha-bridge/k8s/deployment.yaml
kubectl -n $NS rollout restart deploy/dacha-bridge
kubectl -n $NS rollout status deploy/dacha-bridge --timeout=180s

echo "== HA files"
$HA sh -c "mkdir -p /config/packages; cp /config/configuration.yaml /config/configuration.yaml.bak-$TS;
           [ -f /config/secrets.yaml ] && cp /config/secrets.yaml /config/secrets.yaml.bak-$TS || true"
kubectl -n $NS exec -i deploy/homeassistant -c homeassistant -- sh -c 'cat > /config/packages/dacha_generator.yaml' \
    < vps/home-assistant/packages/dacha_generator.yaml
# secrets.yaml: replace our one line, keep the rest. Token goes via stdin, not argv.
printf %s "$TOKEN" | kubectl -n $NS exec -i deploy/homeassistant -c homeassistant -- python3 -c '
import sys, os
tok = sys.stdin.read().strip()
p = "/config/secrets.yaml"
lines = open(p).read().splitlines() if os.path.exists(p) else []
lines = [l for l in lines if not l.startswith("dacha_bridge_auth:")]
lines.append("dacha_bridge_auth: \"Bearer %s\"" % tok)
open(p, "w").write("\n".join(lines) + "\n")
os.chmod(p, 0o600)
'
$HA python3 -c '
p = "/config/configuration.yaml"
s = open(p).read()
if "packages:" not in s:
    s = s.replace("default_config:\n", "default_config:\n\nhomeassistant:\n  packages: !include_dir_named packages\n", 1)
    open(p, "w").write(s)
    print("packages enabled")
else:
    print("packages already enabled")
'

echo "== dashboard view"
$HA cp /config/.storage/lovelace.lovelace /config/.storage/lovelace.lovelace.bak-$TS
kubectl -n $NS exec -i deploy/homeassistant -c homeassistant -- python3 - < vps/home-assistant/add_dacha_view.py

echo "== check config"
$HA python3 -m homeassistant --script check_config -c /config | tail -20

echo "== restart HA"
kubectl -n $NS rollout restart deploy/homeassistant
kubectl -n $NS rollout status deploy/homeassistant --timeout=300s

echo "== smoke test (read-only)"
$HA python3 -c '
import urllib.request, urllib.error
print("healthz:", urllib.request.urlopen("http://dacha-bridge.homeassistant.svc.cluster.local:8080/healthz", timeout=10).read().decode())
try:
    urllib.request.urlopen(urllib.request.Request("http://dacha-bridge.homeassistant.svc.cluster.local:8080/cmd",
        data=b"{}", headers={"Authorization": "Bearer wrong"}), timeout=10)
except urllib.error.HTTPError as e:
    print("wrong token ->", e.code, "(expected 401)")
'
kubectl -n $NS logs deploy/dacha-bridge --tail=5
echo "done: open the \"Дача\" tab in HA"
