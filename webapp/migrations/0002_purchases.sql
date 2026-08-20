-- Kauf-Verwaltung: Käufe aus Stripe-Webhooks + Admin-Kennzeichen
ALTER TABLE users ADD COLUMN is_admin INTEGER NOT NULL DEFAULT 0;

CREATE TABLE IF NOT EXISTS purchases (
	id INTEGER PRIMARY KEY AUTOINCREMENT,
	checkout_session TEXT NOT NULL UNIQUE,	-- Stripe Checkout-Session (cs_...)
	payment_intent TEXT,			-- Stripe PaymentIntent (pi_...), Ref ins Dashboard
	email TEXT,
	name TEXT,
	amount INTEGER,				-- Kleinste Einheit (Cent)
	currency TEXT,
	status TEXT NOT NULL,			-- paid / pending / failed / refunded
	created_at INTEGER NOT NULL,
	updated_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_purchases_intent ON purchases(payment_intent);
