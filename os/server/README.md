# Mehrspieler-Server der Marcel Edition

Damit mehrere Leute gemeinsam in einer Welt bauen können, braucht es
einen Rechner, der dauerhaft läuft. Dieses Verzeichnis enthält alles
dafür; zu tun bleibt: einen Host anlegen und die Adresse eintragen.

## Warum überhaupt ein Server?

Browser können nur WebSockets sprechen, kein normales Netzwerk. Deshalb
laufen im Bild **zwei** Programme: das Spiel als Server und ein
Übersetzer davor, der WebSockets in normales Netzwerk umsetzt.

Und: Server und Browser-Fassung müssen **aus demselben Stand** gebaut
sein. OpenTTD vergleicht die Versionskennung und weist sonst jeden
Beitritt ab. Der Workflow `server-image.yml` baut das Bild deshalb bei
jedem Push automatisch mit.

## Einrichten mit Fly.io (empfohlen)

Fly liefert von sich aus eine Adresse mit gültigem Zertifikat, damit
entfällt die ganze Zertifikats-Arbeit. Kosten: etwa 3–5 € im Monat.

```sh
# einmalig: Fly-Konto anlegen und anmelden
fly auth login

# App anlegen (noch nicht starten)
fly launch --no-deploy --name openttd-marcel --copy-config --config os/server/fly.toml

# Platz für Spielstände (überlebt Neustarts)
fly volume create ottd_data --size 1 --region fra

# Bild starten
fly deploy --image ghcr.io/marcelweissgerberit/openttdmarcel-server:latest
```

Läuft der Server, trage seine Adresse in `web/server.json` ein:

```json
{ "host": "openttd-marcel.fly.dev", "secure": true, "name": "Marcels Welt" }
```

Push genügt — der Web-Deploy nimmt die Datei mit. Ab dann steht
„Marcels Welt" bei jedem Spieler im Mehrspieler-Menü, ohne dass das
Spiel neu gebaut werden muss.

Ist das Bild in der Registry privat, einmalig freigeben: auf GitHub
unter *Packages → openttdmarcel-server → Package settings* die
Sichtbarkeit auf öffentlich stellen, sonst braucht Fly Zugangsdaten.

## Alternative: eigener kleiner Server (VPS)

```sh
docker run -d --name ottd --restart unless-stopped \
  -p 8080:8080 -v ottd_data:/data \
  ghcr.io/marcelweissgerberit/openttdmarcel-server:latest
```

Davor gehört ein Reverse-Proxy mit TLS (z. B. Caddy), denn eine
Seite über `https` darf nur `wss` ansprechen:

```
ottd.deine-domain.de {
    reverse_proxy 127.0.0.1:8080
}
```

## Betrieb

* **Welt sichern:** die Spielstände liegen im Volume unter
  `/data/save/autosave`. Der Server sichert monatlich und beim
  Herunterfahren; beim Start spielt er den neuesten Stand weiter.
* **Welt neu würfeln:** `/data/save/autosave` leeren und neu starten.
* **Einstellungen ändern:** `/data/openttd.cfg` bearbeiten
  (Vorlage: `os/server/server.cfg`), dann neu starten.
* **Nach einem Spiel-Update:** neues Bild ausrollen
  (`fly deploy --image ...:latest`), sonst passen Server und
  Browser-Fassung nicht mehr zusammen und niemand kommt herein.
