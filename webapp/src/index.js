/**
 * OpenTTD-Web API (Cloudflare Worker).
 * Phase 1: Skeleton mit Health-Check. Auth/Saves/Settings folgen in Phase 2/4.
 */

const CORS = {
	'Access-Control-Allow-Origin': '*',
	'Access-Control-Allow-Methods': 'GET, POST, PUT, DELETE, OPTIONS',
	'Access-Control-Allow-Headers': 'Content-Type, Authorization',
};

export default {
	async fetch(request, env) {
		const url = new URL(request.url);

		if (request.method === 'OPTIONS') {
			return new Response(null, { status: 204, headers: CORS });
		}

		if (url.pathname === '/api/health') {
			let db = false;
			try {
				const row = await env.DB.prepare('SELECT 1 AS ok').first();
				db = row?.ok === 1;
			} catch {}
			let r2 = false;
			try {
				await env.SAVES.head('__healthcheck__');
				r2 = true;
			} catch {}
			return Response.json({ ok: true, db, r2, ts: Date.now() }, { headers: CORS });
		}

		return Response.json({ error: 'not_found' }, { status: 404, headers: CORS });
	},
};
