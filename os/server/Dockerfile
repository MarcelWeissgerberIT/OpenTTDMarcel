# Mehrspieler-Server der Marcel Edition.
#
# Zwei Teile in einem Bild: der Fork als Dedicated Server (spricht
# normales TCP) und ein WebSocket-Uebersetzer davor - ohne den kann die
# Browser-Fassung des Spiels nicht verbinden, denn Browser koennen nur
# WebSockets.
#
# WICHTIG: Server und Browser-Fassung muessen aus DEMSELBEN Commit
# gebaut sein. OpenTTD vergleicht die Versionskennung und weist sonst
# jeden Beitritt ab. Der Workflow server-image.yml baut das Bild
# deshalb bei jedem Push mit.

FROM debian:bookworm-slim AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
	build-essential cmake git ca-certificates \
	libfontconfig-dev liblzma-dev liblzo2-dev libpng-dev zlib1g-dev \
	&& rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# Dedicated: ohne Fenster, ohne Ton - der Server rechnet nur.
# Installiert wird nach /usr/local, weil OpenTTD den Datenpfad beim
# Uebersetzen einbrennt: nur so findet der Server Sprachdateien und
# Grafiken auch dann, wenn "-c" auf eine Konfiguration in /data zeigt.
RUN cmake -B /build -S /src \
		-DCMAKE_BUILD_TYPE=Release \
		-DOPTION_DEDICATED=ON \
		-DOPTION_USE_ASSERTS=OFF \
		-DCMAKE_INSTALL_PREFIX=/usr/local \
	&& cmake --build /build -j"$(nproc)" --target openttd language_files \
	&& cmake --install /build

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates curl python3 python3-numpy libfontconfig1 liblzma5 liblzo2-2 libpng16-16 zlib1g \
	python3-pip \
	&& pip3 install --break-system-packages --no-cache-dir websockify \
	&& apt-get purge -y python3-pip && apt-get autoremove -y \
	&& rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/games/openttd /usr/local/bin/openttd
COPY --from=build /usr/local/share/games/openttd /usr/local/share/games/openttd
# Dieselben Grafiken wie in der Browser-Fassung.
COPY web/baseset/ /usr/local/share/games/openttd/baseset/
COPY os/server/start.sh /usr/local/bin/start.sh
COPY os/server/server.cfg /etc/openttd/server.cfg

WORKDIR /data
RUN chmod +x /usr/local/bin/start.sh && mkdir -p /data

# Spielstaende und Konfiguration liegen hier - als Volume einhaengen,
# damit die Welt einen Neustart ueberlebt.
VOLUME ["/data"]

# Aussen spricht nur der Uebersetzer; der Spielport bleibt intern.
ENV OTTD_WS_PORT=8080

# Version des Spiels in diesem Bild. Server und Browser-Fassung muessen
# uebereinstimmen, sonst weist der Server jeden Beitritt ab - die
# oeffentliche Serverliste blendet unpassende Server deshalb aus.
ARG GIT_SHA=unbekannt
ENV OTTD_BUILD=$GIT_SHA

# Fuer den Eintrag in der oeffentlichen Serverliste (alles freiwillig):
#   OTTD_PUBLIC_HOST   Name, unter dem Spieler den Server erreichen
#   OTTD_SERVER_NAME   Anzeigename in der Liste
#   OTTD_ANNOUNCE_KEY  selbst gewaehltes Geheimnis (schuetzt den Eintrag)
#   OTTD_SERVER_NOTE   kurze Beschreibung
ENV OTTD_ANNOUNCE_URL=https://openttd-api.marcelweissgerber81.workers.dev/api/servers
EXPOSE 8080

CMD ["/usr/local/bin/start.sh"]
