#!/bin/bash
exec > >(tee /tmp/can_window.log) 2>&1
chcp.com 65001 >/dev/null 2>&1 || true
cd "$(dirname "$0")/.." || { echo "cd 실패"; sleep 30; exit 1; }
./tools/can_watch.sh inf
echo
echo "[모니터 종료됨 — 아무 키나 누르면 창이 닫힙니다]"
read -r -n 1
