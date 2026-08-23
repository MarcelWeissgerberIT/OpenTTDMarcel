-- Öffentliche Serverliste: wer einen eigenen Mehrspieler-Server betreibt,
-- meldet ihn hier an und hält ihn mit Lebenszeichen sichtbar.
CREATE TABLE IF NOT EXISTS servers (
	id TEXT PRIMARY KEY,			-- aus dem Hostnamen abgeleitet
	name TEXT NOT NULL,			-- Anzeigename in der Liste
	host TEXT NOT NULL,			-- oeffentlicher Name, ueber den Spieler verbinden
	secure INTEGER NOT NULL DEFAULT 1,	-- 1 = verschluesselt (wss)
	build TEXT,				-- Spielversion; nur passende Server sind spielbar
	note TEXT,				-- kurze Beschreibung des Betreibers
	players INTEGER NOT NULL DEFAULT 0,
	max_players INTEGER NOT NULL DEFAULT 0,
	key_hash TEXT NOT NULL,			-- nur wer den Schluessel kennt, darf aendern
	blocked INTEGER NOT NULL DEFAULT 0,	-- Notbremse gegen Missbrauch
	created_at INTEGER NOT NULL,
	seen_at INTEGER NOT NULL		-- letztes Lebenszeichen
);
CREATE INDEX IF NOT EXISTS idx_servers_seen ON servers(seen_at);
CREATE INDEX IF NOT EXISTS idx_servers_build ON servers(build);
