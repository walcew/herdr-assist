#!/bin/sh
# Sobe a ponte herdr-assist destacada e sai — o startup de plugin do Herdr
# roda uma vez por subida da sessão e não supervisiona daemons.
#
# Idempotente: se a porta já responde, não sobe uma segunda ponte (o startup
# re-executa em live handoff do Herdr). Com --restart, derruba a atual antes.
#
# As mensagens saem em inglês (fixo): a tela de administração as ecoa ao
# reiniciar a ponte, e ali o idioma é o do locale do host — um shell script não
# tem como acompanhar isso sem carregar uma tabela só para dois echo.
#
# Config opcional em $HERDR_PLUGIN_CONFIG_DIR/env (KEY=VALUE por linha):
# BRIDGE_PORT, BRIDGE_BIND. O token vive em $HERDR_PLUGIN_CONFIG_DIR/token
# (gerado pela ponte na primeira subida).
set -u

DIR=$(cd "$(dirname "$0")" && pwd)
CONF="${HERDR_PLUGIN_CONFIG_DIR:-$HOME/.config/herdr-assist}"
STATE="${HERDR_PLUGIN_STATE_DIR:-$CONF}"
mkdir -p "$CONF" "$STATE"

# set -a exporta tudo que o arquivo env definir (BRIDGE_BIND etc.)
if [ -f "$CONF/env" ]; then
    set -a
    . "$CONF/env"
    set +a
fi
PORT="${BRIDGE_PORT:-9375}"
PIDFILE="$STATE/bridge.pid"

if [ "${1:-}" = "--restart" ] && [ -f "$PIDFILE" ]; then
    kill "$(cat "$PIDFILE")" 2>/dev/null
    rm -f "$PIDFILE"
    sleep 1
fi

# porta em uso = ponte já de pé (bind falha -> exit 1 -> não sobe outra)
if ! python3 -c "import socket; s=socket.socket(); s.bind(('127.0.0.1', $PORT)); s.close()" 2>/dev/null; then
    echo "bridge already running on port $PORT"
    exit 0
fi

BRIDGE_PORT="$PORT" nohup python3 "$DIR/herdr_bridge.py" >> "$STATE/bridge.log" 2>&1 &
echo $! > "$PIDFILE"
echo "bridge started (pid $!, port $PORT, log at $STATE/bridge.log)"
