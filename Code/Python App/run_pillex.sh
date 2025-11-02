#!/usr/bin/env bash
# /home/pi/app/run_pillex.sh
# Wrapper for Pillex to set up env and run the app safely.

set -Eeuo pipefail

APP_DIR="/home/pi/app"
PY="/usr/bin/python3"
ENTRYPOINT="$APP_DIR/app.py"

cd "$APP_DIR"

# If you use a virtualenv at /home/pi/app/venv, activate it automatically
if [[ -f "$APP_DIR/venv/bin/activate" ]]; then
  # shellcheck disable=SC1091
  source "$APP_DIR/venv/bin/activate"
  PY="python"   # use venv's python
fi

# Optional: export environment variables for your app
# export PILLEX_ENV=production

# Use 'exec' so systemd can track the real process
exec "$PY" "$ENTRYPOINT"
