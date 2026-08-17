# If you change this version, change the numbers in .github/workflows/ci-emscripten.yml (2x)
# and .github/workflows/preview-build.yml (2x) too.
FROM emscripten/emsdk:6.0.1

RUN apt-get update \
    && apt-get clean \
    && rm -rf /var/lib/apt/lists/*

COPY ports/liblzma.py /emsdk/upstream/emscripten/tools/ports/contrib/liblzma.py

# Fork: Emscripten-Ports (freetype, sdl2, zlib, libpng, liblzma) liegen im
# Repo, damit der Build nicht vom Port-Download-Server abhaengt (der
# antwortete am 2026-08-17 stundenlang mit HTTP 503). Gleiche Dateinamen
# und Versionen wie der 6.0.1-Cache erwartet.
COPY ports-cache/ /emsdk/upstream/emscripten/cache/ports/
