// Showcase anonymous usage. Showcase posts at most once a day:
// {"id": random UUID, and only the fields the user left on}.
// IP addresses are never stored; country comes from Cloudflare when asked for.
const FIELDS = ['version', 'package', 'model', 'ios', 'jailbreak', 'language'];
const UUID = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export default {
  async fetch(request, env) {
    if (request.method !== 'POST') return new Response(null, { status: 405 });
    let body;
    try { body = await request.json(); } catch { return new Response(null, { status: 400 }); }
    if (!body || typeof body.id !== 'string' || !UUID.test(body.id)) return new Response(null, { status: 400 });

    const value = (key) => typeof body[key] === 'string' && body[key].length ? body[key].slice(0, 64) : null;
    const row = Object.fromEntries(FIELDS.map((key) => [key, value(key)]));
    row.country = body.country === true ? (request.cf?.country ?? null) : null;
    const now = new Date().toISOString();

    // Fields the user turned off are cleared, not kept from an earlier ping.
    await env.DB.prepare(
      `INSERT INTO installs (id, first_seen, last_seen, version, package, model, ios, jailbreak, language, country)
       VALUES (?1, ?2, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9)
       ON CONFLICT(id) DO UPDATE SET last_seen = ?2, version = ?3, package = ?4, model = ?5,
         ios = ?6, jailbreak = ?7, language = ?8, country = ?9`
    ).bind(body.id.toLowerCase(), now, row.version, row.package, row.model, row.ios,
           row.jailbreak, row.language, row.country).run();
    return new Response(null, { status: 204 });
  },
};
