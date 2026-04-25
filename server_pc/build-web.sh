#!/bin/sh
set -e
cd "$(dirname "$0")/web"
if [ ! -f package.json ]; then
  echo "ERROR: web/package.json not found"
  exit 1
fi
npm install
npm run build
echo ""
echo "Built to server_pc/static/. Run: docker compose build"
