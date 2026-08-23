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
RUN cmake -B /build -S /src \
		-DCMAKE_BUILD_TYPE=Release \
		-DOPTION_DEDICATED=ON \
		-DOPTION_USE_ASSERTS=OFF \
	&& cmake --build /build -j"$(nproc)" --target openttd language_files

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
	ca-certificates python3 python3-numpy libfontconfig1 liblzma5 liblzo2-2 libpng16-16 zlib1g \
	python3-pip \
	&& pip3 install --break-system-packages --no-cache-dir websockify \
	&& apt-get purge -y python3-pip && apt-get autoremove -y \
	&& rm -rf /var/lib/apt/lists/*

# OpenTTD findet seine Daten neben dem Arbeitsverzeichnis - genau so
# laeuft auch der lokal gepruefte Server.
COPY --from=build /build/openttd /usr/local/bin/openttd
COPY --from=build /build/baseset /app/baseset
COPY --from=build /build/lang /app/lang
COPY --from=build /build/ai /app/ai
COPY --from=build /build/game /app/game
# Dieselben Grafiken wie in der Browser-Fassung.
COPY web/baseset/ /app/baseset/
COPY os/server/start.sh /usr/local/bin/start.sh
COPY os/server/server.cfg /etc/openttd/server.cfg

WORKDIR /app
RUN chmod +x /usr/local/bin/start.sh && mkdir -p /data

# Spielstaende und Konfiguration liegen hier - als Volume einhaengen,
# damit die Welt einen Neustart ueberlebt.
VOLUME ["/data"]

# Aussen spricht nur der Uebersetzer; der Spielport bleibt intern.
ENV OTTD_WS_PORT=8080
EXPOSE 8080

CMD ["/usr/local/bin/start.sh"]
