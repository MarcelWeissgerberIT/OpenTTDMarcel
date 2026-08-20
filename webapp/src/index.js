/**
 * OpenTTD-Web API (Cloudflare Worker).
 * Phase 2: E-Mail+Passwort-Auth mit D1-Sessions (HttpOnly-Cookie).
 */

const SESSION_COOKIE = 'ottd_session';
const SESSION_DAYS = 30;
/* Workers-Free-Tier erlaubt nur ~10 ms CPU pro Request; die Iterationszahl
 * steckt im Hash-Format und kann bei einem spaeteren Tarifwechsel pro Login
 * transparent erhoeht werden (Rehash bei erfolgreicher Anmeldung). */
const PBKDF2_ITERATIONS = 20000;

const ALLOWED_ORIGINS = [
	'https://marcelweissgerberit.github.io',
	'https://openttd-web-ene.pages.dev',
	'http://localhost:8788',
];

function corsHeaders(request) {
	const origin = request.headers.get('Origin');
	const allowed = ALLOWED_ORIGINS.includes(origin) ? origin : ALLOWED_ORIGINS[1];
	return {
		'Access-Control-Allow-Origin': allowed,
		'Access-Control-Allow-Methods': 'GET, POST, PUT, DELETE, OPTIONS',
		'Access-Control-Allow-Headers': 'Content-Type, Authorization',
		'Access-Control-Expose-Headers': 'X-Save-Name',
		'Access-Control-Allow-Credentials': 'true',
		'Vary': 'Origin',
	};
}

function json(data, status, cors, extra = {}) {
	return new Response(JSON.stringify(data), {
		status,
		headers: { 'Content-Type': 'application/json', ...cors, ...extra },
	});
}

/* ---------- Passwort-Hashing (PBKDF2-SHA256, WebCrypto) ---------- */

function toHex(buf) {
	return [...new Uint8Array(buf)].map(b => b.toString(16).padStart(2, '0')).join('');
}

function fromHex(hex) {
	return new Uint8Array(hex.match(/.{2}/g).map(b => parseInt(b, 16)));
}

async function derive(password, salt, iterations) {
	const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(password), 'PBKDF2', false, ['deriveBits']);
	return crypto.subtle.deriveBits({ name: 'PBKDF2', hash: 'SHA-256', salt, iterations }, key, 256);
}

async function hashPassword(password) {
	const salt = crypto.getRandomValues(new Uint8Array(16));
	const bits = await derive(password, salt, PBKDF2_ITERATIONS);
	return `pbkdf2$${PBKDF2_ITERATIONS}$${toHex(salt)}$${toHex(bits)}`;
}

async function verifyPassword(password, stored) {
	const [scheme, iter, saltHex, hashHex] = stored.split('$');
	if (scheme !== 'pbkdf2') return false;
	const bits = await derive(password, fromHex(saltHex), parseInt(iter, 10));
	const a = new Uint8Array(bits), b = fromHex(hashHex);
	if (a.length !== b.length) return false;
	let diff = 0;
	for (let i = 0; i < a.length; i++) diff |= a[i] ^ b[i];
	return diff === 0;
}

/* ---------- Sessions ---------- */

function sessionCookie(token, maxAge) {
	return `${SESSION_COOKIE}=${token}; Path=/; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=None`;
}

async function createSession(env, userId) {
	const token = crypto.randomUUID() + crypto.randomUUID();
	const expires = Date.now() + SESSION_DAYS * 86400000;
	await env.DB.prepare('INSERT INTO sessions (id, user_id, expires_at) VALUES (?, ?, ?)')
		.bind(token, userId, expires).run();
	return token;
}

async function getSessionUser(env, request) {
	let token = null;
	const auth = request.headers.get('Authorization') || '';
	if (auth.startsWith('Bearer ')) token = auth.slice(7).trim();
	if (token === null) {
		const cookie = request.headers.get('Cookie') || '';
		const m = cookie.match(new RegExp(`${SESSION_COOKIE}=([\\w-]+)`));
		if (m) token = m[1];
	}
	if (token === null) return null;
	const row = await env.DB.prepare(
		`SELECT u.id, u.email, u.verified_at, u.is_admin, s.id AS session_id, s.expires_at
		 FROM sessions s JOIN users u ON u.id = s.user_id WHERE s.id = ?`)
		.bind(token).first();
	if (!row) return null;
	if (row.expires_at < Date.now()) {
		await env.DB.prepare('DELETE FROM sessions WHERE id = ?').bind(row.session_id).run();
		return null;
	}
	return row;
}

/* ---------- Handlers ---------- */

const EMAIL_RE = /^[^\s@]+@[^\s@]+\.[^\s@]+$/;

async function handleRegister(request, env, cors) {
	const body = await request.json().catch(() => null);
	if (!body || !EMAIL_RE.test(body.email || '') || typeof body.password !== 'string' || body.password.length < 8) {
		return json({ error: 'invalid_input', message: 'E-Mail ungueltig oder Passwort kuerzer als 8 Zeichen.' }, 400, cors);
	}
	const email = body.email.trim().toLowerCase();
	const existing = await env.DB.prepare('SELECT id FROM users WHERE email = ?').bind(email).first();
	if (existing) return json({ error: 'email_taken', message: 'Diese E-Mail ist bereits registriert.' }, 409, cors);

	const pwHash = await hashPassword(body.password);
	const res = await env.DB.prepare('INSERT INTO users (email, pw_hash, created_at) VALUES (?, ?, ?)')
		.bind(email, pwHash, Date.now()).run();
	const userId = res.meta.last_row_id;
	const token = await createSession(env, userId);
	return json({ ok: true, email, token }, 201, cors, { 'Set-Cookie': sessionCookie(token, SESSION_DAYS * 86400) });
}

async function handleLogin(request, env, cors) {
	const body = await request.json().catch(() => null);
	if (!body || !body.email || !body.password) return json({ error: 'invalid_input' }, 400, cors);
	const email = String(body.email).trim().toLowerCase();
	const user = await env.DB.prepare('SELECT id, email, pw_hash FROM users WHERE email = ?').bind(email).first();
	if (!user || !(await verifyPassword(body.password, user.pw_hash))) {
		return json({ error: 'bad_credentials', message: 'E-Mail oder Passwort falsch.' }, 401, cors);
	}
	const token = await createSession(env, user.id);
	return json({ ok: true, email: user.email, token }, 200, cors, { 'Set-Cookie': sessionCookie(token, SESSION_DAYS * 86400) });
}

async function handleLogout(request, env, cors) {
	const user = await getSessionUser(env, request);
	if (user) await env.DB.prepare('DELETE FROM sessions WHERE id = ?').bind(user.session_id).run();
	return json({ ok: true }, 200, cors, { 'Set-Cookie': sessionCookie('', 0) });
}

async function handleMe(request, env, cors) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	const ents = await env.DB.prepare('SELECT feature FROM entitlements WHERE user_id = ?').bind(user.id).all();
	return json({
		ok: true,
		email: user.email,
		verified: user.verified_at != null,
		entitlements: (ents.results || []).map(r => r.feature),
	}, 200, cors);
}

/* ---------- Spielstaende (Phase 4: D1-Metadaten + R2-Blobs) ---------- */

const MAX_SAVE_BYTES = 8 * 1024 * 1024;
const MAX_SLOTS = 3;
const SAVE_NAME_RE = /^[\w\-. ()\[\]#+',!äöüÄÖÜß]{1,120}\.sav$/;

async function handleSaveList(request, env, cors) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	const rows = await env.DB.prepare(
		'SELECT slot, name, size, updated_at FROM saves WHERE user_id = ? ORDER BY slot').bind(user.id).all();
	return json({ ok: true, max_slots: MAX_SLOTS, saves: rows.results || [] }, 200, cors);
}

async function handleSaveUpload(request, env, cors, url) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	const name = url.searchParams.get('name') || '';
	if (!SAVE_NAME_RE.test(name)) return json({ error: 'bad_name', message: 'Ungueltiger Spielstand-Name.' }, 400, cors);
	const body = await request.arrayBuffer();
	if (body.byteLength === 0 || body.byteLength > MAX_SAVE_BYTES) {
		return json({ error: 'bad_size', message: 'Spielstand leer oder groesser als 8 MB.' }, 400, cors);
	}
	/* Slot-Wahl: gleicher Name -> gleicher Slot; sonst freier Slot; sonst
	 * wird der aelteste Slot ueberschrieben (3 Slots halten das R2-Budget
	 * fuer 10.000 Nutzer im Free-Tier). */
	const rows = (await env.DB.prepare('SELECT slot, name, updated_at FROM saves WHERE user_id = ?').bind(user.id).all()).results || [];
	let slot = null;
	const same = rows.find(r => r.name === name);
	if (same) {
		slot = same.slot;
	} else {
		for (let s = 1; s <= MAX_SLOTS; s++) if (!rows.some(r => r.slot === s)) { slot = s; break; }
		if (slot === null) slot = rows.slice().sort((a, b) => a.updated_at - b.updated_at)[0].slot;
	}
	const key = `saves/${user.id}/${slot}.sav`;
	await env.SAVES.put(key, body);
	await env.DB.prepare(
		`INSERT INTO saves (user_id, slot, name, size, r2_key, updated_at) VALUES (?, ?, ?, ?, ?, ?)
		 ON CONFLICT(user_id, slot) DO UPDATE SET name = excluded.name, size = excluded.size,
		   r2_key = excluded.r2_key, updated_at = excluded.updated_at`)
		.bind(user.id, slot, name, body.byteLength, key, Date.now()).run();
	return json({ ok: true, slot, name, size: body.byteLength }, 200, cors);
}

async function handleSaveDownload(request, env, cors, url) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	const slot = parseInt(url.searchParams.get('slot') || '', 10);
	const row = await env.DB.prepare('SELECT name, r2_key FROM saves WHERE user_id = ? AND slot = ?')
		.bind(user.id, slot).first();
	if (!row) return json({ error: 'not_found' }, 404, cors);
	const obj = await env.SAVES.get(row.r2_key);
	if (!obj) return json({ error: 'not_found' }, 404, cors);
	return new Response(obj.body, {
		status: 200,
		headers: { 'Content-Type': 'application/octet-stream', 'X-Save-Name': encodeURIComponent(row.name), ...cors },
	});
}

async function handleSaveDelete(request, env, cors, url) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	const slot = parseInt(url.searchParams.get('slot') || '', 10);
	const row = await env.DB.prepare('SELECT r2_key FROM saves WHERE user_id = ? AND slot = ?')
		.bind(user.id, slot).first();
	if (!row) return json({ error: 'not_found' }, 404, cors);
	await env.SAVES.delete(row.r2_key);
	await env.DB.prepare('DELETE FROM saves WHERE user_id = ? AND slot = ?').bind(user.id, slot).run();
	return json({ ok: true }, 200, cors);
}

/* ---------- Einstellungen (openttd.cfg als Text-Blob) ---------- */

const MAX_SETTINGS_BYTES = 256 * 1024;

async function handleSettingsGet(request, env, cors) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	const row = await env.DB.prepare('SELECT json, updated_at FROM settings WHERE user_id = ?').bind(user.id).first();
	return json({ ok: true, json: row ? row.json : null, updated_at: row ? row.updated_at : 0 }, 200, cors);
}

async function handleSettingsPut(request, env, cors) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	const text = await request.text();
	if (text.length === 0 || text.length > MAX_SETTINGS_BYTES) return json({ error: 'bad_size' }, 400, cors);
	await env.DB.prepare(
		`INSERT INTO settings (user_id, json, updated_at) VALUES (?, ?, ?)
		 ON CONFLICT(user_id) DO UPDATE SET json = excluded.json, updated_at = excluded.updated_at`)
		.bind(user.id, text, Date.now()).run();
	return json({ ok: true }, 200, cors);
}

/* ---------- Stripe-Webhook + Kauf-Verwaltung ---------- */

/* Stripe signiert jeden Webhook-Aufruf (Header "Stripe-Signature":
 * t=<unix>,v1=<hmac>). Ohne gültige Signatur wird nichts gespeichert. */
async function verifyStripeSignature(payload, sigHeader, secret) {
	if (!sigHeader) return false;
	let t = null;
	const v1s = [];
	for (const part of sigHeader.split(',')) {
		const [k, v] = part.split('=', 2);
		if (k.trim() === 't') t = v;
		if (k.trim() === 'v1') v1s.push(v);
	}
	if (!t || v1s.length === 0) return false;
	if (Math.abs(Date.now() / 1000 - parseInt(t, 10)) > 600) return false;
	const key = await crypto.subtle.importKey('raw', new TextEncoder().encode(secret),
		{ name: 'HMAC', hash: 'SHA-256' }, false, ['sign']);
	const mac = await crypto.subtle.sign('HMAC', key, new TextEncoder().encode(`${t}.${payload}`));
	const expected = toHex(mac);
	return v1s.some(v => {
		if (v.length !== expected.length) return false;
		let diff = 0;
		for (let i = 0; i < v.length; i++) diff |= v.charCodeAt(i) ^ expected.charCodeAt(i);
		return diff === 0;
	});
}

async function upsertPurchase(env, session, status) {
	const cd = session.customer_details || {};
	await env.DB.prepare(
		`INSERT INTO purchases (checkout_session, payment_intent, email, name, amount, currency, status, created_at, updated_at)
		 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
		 ON CONFLICT(checkout_session) DO UPDATE SET payment_intent = excluded.payment_intent,
		   email = excluded.email, name = excluded.name, amount = excluded.amount,
		   currency = excluded.currency, status = excluded.status, updated_at = excluded.updated_at`)
		.bind(session.id, session.payment_intent || null, cd.email || null, cd.name || null,
			session.amount_total ?? null, session.currency || null, status, Date.now(), Date.now()).run();
}

async function handleStripeWebhook(request, env) {
	/* Kein CORS: der Aufruf kommt von Stripe-Servern, nicht aus dem Browser. */
	if (!env.STRIPE_WEBHOOK_SECRET) return new Response('webhook secret not configured', { status: 503 });
	const payload = await request.text();
	const ok = await verifyStripeSignature(payload, request.headers.get('Stripe-Signature'), env.STRIPE_WEBHOOK_SECRET);
	if (!ok) return new Response('bad signature', { status: 400 });

	let event;
	try { event = JSON.parse(payload); } catch { return new Response('bad payload', { status: 400 }); }
	const obj = event.data && event.data.object;
	if (!obj) return new Response('ok', { status: 200 });

	switch (event.type) {
		case 'checkout.session.completed':
			await upsertPurchase(env, obj, obj.payment_status === 'paid' ? 'paid' : 'pending');
			break;
		case 'checkout.session.async_payment_succeeded':
			await upsertPurchase(env, obj, 'paid');
			break;
		case 'checkout.session.async_payment_failed':
			await upsertPurchase(env, obj, 'failed');
			break;
		case 'charge.refunded':
			if (obj.payment_intent) {
				await env.DB.prepare('UPDATE purchases SET status = ?, updated_at = ? WHERE payment_intent = ?')
					.bind('refunded', Date.now(), obj.payment_intent).run();
			}
			break;
		default:
			/* Unbekannte Ereignisse quittieren, sonst wiederholt Stripe sie endlos. */
			break;
	}
	return new Response('ok', { status: 200 });
}

async function handleAdminPurchases(request, env, cors) {
	const user = await getSessionUser(env, request);
	if (!user) return json({ error: 'not_logged_in' }, 401, cors);
	if (user.is_admin !== 1) return json({ error: 'forbidden' }, 403, cors);
	const rows = await env.DB.prepare(
		`SELECT checkout_session, payment_intent, email, name, amount, currency, status, created_at, updated_at
		 FROM purchases ORDER BY created_at DESC LIMIT 200`).all();
	return json({ ok: true, purchases: rows.results || [] }, 200, cors);
}

export default {
	async fetch(request, env) {
		const url = new URL(request.url);
		const cors = corsHeaders(request);

		if (request.method === 'OPTIONS') return new Response(null, { status: 204, headers: cors });

		try {
			if (url.pathname === '/api/health') {
				let db = false;
				try {
					const row = await env.DB.prepare('SELECT 1 AS ok').first();
					db = row?.ok === 1;
				} catch {}
				return json({ ok: true, db, ts: Date.now() }, 200, cors);
			}
			if (url.pathname === '/api/register' && request.method === 'POST') return await handleRegister(request, env, cors);
			if (url.pathname === '/api/login' && request.method === 'POST') return await handleLogin(request, env, cors);
			if (url.pathname === '/api/logout' && request.method === 'POST') return await handleLogout(request, env, cors);
			if (url.pathname === '/api/me' && request.method === 'GET') return await handleMe(request, env, cors);
			if (url.pathname === '/api/saves' && request.method === 'GET') return await handleSaveList(request, env, cors);
			if (url.pathname === '/api/saves/upload' && request.method === 'PUT') return await handleSaveUpload(request, env, cors, url);
			if (url.pathname === '/api/saves/download' && request.method === 'GET') return await handleSaveDownload(request, env, cors, url);
			if (url.pathname === '/api/saves/delete' && request.method === 'DELETE') return await handleSaveDelete(request, env, cors, url);
			if (url.pathname === '/api/settings' && request.method === 'GET') return await handleSettingsGet(request, env, cors);
			if (url.pathname === '/api/settings' && request.method === 'PUT') return await handleSettingsPut(request, env, cors);
			if (url.pathname === '/api/stripe/webhook' && request.method === 'POST') return await handleStripeWebhook(request, env);
			if (url.pathname === '/api/admin/purchases' && request.method === 'GET') return await handleAdminPurchases(request, env, cors);
		} catch (e) {
			/* Diagnose-Detail, solange Phase 2 stabilisiert wird. */
			return json({ error: 'server_error', detail: String(e && e.message || e).slice(0, 200) }, 500, cors);
		}

		return json({ error: 'not_found' }, 404, cors);
	},
};
