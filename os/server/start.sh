#!/bin/sh
# Startet den Spielserver und den WebSocket-Uebersetzer davor.
# Faellt einer der beiden aus, endet der Container - der Betreiber
# (Fly.io, Docker, systemd) startet ihn dann neu.
set -e

DATA_DIR="${OTTD_DATA_DIR:-/data}"
WS_PORT="${OTTD_WS_PORT:-8080}"
GAME_PORT="${OTTD_GAME_PORT:-3979}"

mkdir -p "$DATA_DIR"
# Beim ersten Start die mitgelieferte Konfiguration uebernehmen; spaetere
# Aenderungen des Betreibers in /data bleiben erhalten.
if [ ! -f "$DATA_DIR/openttd.cfg" ]; then
	cp /etc/openttd/server.cfg "$DATA_DIR/openttd.cfg"
fi

echo "Marcel Edition Server: Spiel auf ${GAME_PORT}, WebSocket auf ${WS_PORT}"

# Vorhandene Welt weiterspielen statt jedes Mal neu zu wuerfeln.
LAST_SAVE=""
if [ -d "$DATA_DIR/save/autosave" ]; then
	LAST_SAVE=$(ls -1t "$DATA_DIR"/save/autosave/*.sav 2>/dev/null | head -1 || true)
fi

if [ -n "$LAST_SAVE" ]; then
	echo "Setze bei $LAST_SAVE fort."
	openttd -D "0.0.0.0:$GAME_PORT" -c "$DATA_DIR/openttd.cfg" -g "$LAST_SAVE" &
else
	echo "Keine Welt gefunden - erzeuge eine neue."
	openttd -D "0.0.0.0:$GAME_PORT" -c "$DATA_DIR/openttd.cfg" &
fi
GAME_PID=$!

# Endet eines von beiden, geht der Container mit.
trap 'kill $GAME_PID 2>/dev/null; exit 0' TERM INT

python3 -m websockify "0.0.0.0:$WS_PORT" "127.0.0.1:$GAME_PORT" &
WS_PID=$!

wait -n $GAME_PID $WS_PID
echo "Ein Dienst hat sich beendet - Container faehrt herunter."
kill $GAME_PID $WS_PID 2>/dev/null || true
exit 1
