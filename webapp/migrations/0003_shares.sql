-- Geteilte Welten: kurzer Link -> Spielstand in R2
CREATE TABLE IF NOT EXISTS shares (
	id TEXT PRIMARY KEY,			-- kurze oeffentliche ID (Link)
	name TEXT NOT NULL,			-- Anzeigename der Welt
	size INTEGER NOT NULL,
	r2_key TEXT NOT NULL,
	ip_hash TEXT,				-- fuer die Missbrauchsbremse, keine Klartext-IP
	user_id INTEGER,			-- gesetzt, wenn angemeldet geteilt wurde
	downloads INTEGER NOT NULL DEFAULT 0,
	created_at INTEGER NOT NULL,
	expires_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_shares_ip ON shares(ip_hash, created_at);
CREATE INDEX IF NOT EXISTS idx_shares_expires ON shares(expires_at);
