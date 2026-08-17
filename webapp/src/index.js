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
		'Access-Control-Allow-Headers': 'Content-Type',
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
		`SELECT u.id, u.email, u.verified_at, s.id AS session_id, s.expires_at
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
		} catch (e) {
			/* Diagnose-Detail, solange Phase 2 stabilisiert wird. */
			return json({ error: 'server_error', detail: String(e && e.message || e).slice(0, 200) }, 500, cors);
		}

		return json({ error: 'not_found' }, 404, cors);
	},
};
