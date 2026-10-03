// ARMSX2 Online Icons and texture packs: which icons and packs people downloaded today, for the
// "Popular Today" tabs.
//
// A Cloudflare Worker with one D1 database bound as DB (schema.sql). The icons themselves stay
// plain files in the R2 bucket; this only counts.
//
// POST /hit      Body: icon hashes as named in index.txt, one per line, at most 100. The app sends
//                one when the player downloads icons on purpose (a tile, or "Download my games");
//                previews and "Download all" never send anything. Each icon counts once per
//                person per UTC day, a person being a salted hash of the day and their IP: the IP
//                itself is never stored, and those rows go after a day.
// GET  /popular  Today's six most downloaded icons, "HASH COUNT" per line, most first. Early in the
//                day, when today has fewer than six, yesterday's fill the rest (with count 0).
//                Cached at the edge for five minutes, so the database is read a handful of times
//                an hour however many players open the tab.
//
// The texture packs count the same way, in tables of their own (tex_hits, tex_seen):
// POST /textures/hit      Body: pack ids from the texture catalog, one per line, at most 10. The app
//                         sends one when a pack the player chose has installed.
// GET  /textures/popular  Today's ten most downloaded packs, "ID COUNT" per line, most first, filled
//                         from yesterday's early in the day. Cached for five minutes too.

const HASH = /^[0-9a-f]{16}$/;
const MAX_PER_HIT = 100;
const TOP = 6;
const KEEP_DAYS = 3;
const PACK_ID = /^[a-z0-9][a-z0-9._-]{1,254}$/; // as the catalog names them: "bmt619-speed-racer-v1.0"
const MAX_PACKS_PER_HIT = 10;
const PACK_TOP = 10;

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === "/hit" && request.method === "POST") return hit(request, env, ctx);
    if (url.pathname === "/popular" && request.method === "GET") return popular(request, env, ctx);
    if (url.pathname === "/textures/hit" && request.method === "POST") return textureHit(request, env, ctx);
    if (url.pathname === "/textures/popular" && request.method === "GET") return texturePopular(request, env, ctx);
    return new Response("not found\n", { status: 404 });
  },
};

/** The UTC day, YYYY-MM-DD, [offset] days from today. */
function day(offset = 0) {
  return new Date(Date.now() + offset * 86400000).toISOString().slice(0, 10);
}

async function sha256hex(text) {
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text));
  return [...new Uint8Array(digest)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

async function hit(request, env, ctx) {
  const body = (await request.text()).slice(0, MAX_PER_HIT * 20);
  const hashes = [...new Set(body.split(/\s+/).filter((h) => HASH.test(h)))].slice(0, MAX_PER_HIT);
  if (hashes.length === 0) return new Response("nothing to count\n", { status: 400 });

  const today = day();
  const ip = request.headers.get("CF-Connecting-IP") || "";
  const who = (await sha256hex(`armsx2-online-icons|${today}|${ip}`)).slice(0, 32);

  // First, which of these this person hasn't counted today; then count only those.
  const seen = await env.DB.batch(hashes.map((h) =>
    env.DB.prepare("INSERT OR IGNORE INTO seen (day, who, hash) VALUES (?1, ?2, ?3)").bind(today, who, h)));
  const fresh = hashes.filter((_, i) => seen[i].meta.changes > 0);
  if (fresh.length > 0) {
    await env.DB.batch(fresh.map((h) =>
      env.DB.prepare("INSERT INTO hits (day, hash, n) VALUES (?1, ?2, 1) ON CONFLICT (day, hash) DO UPDATE SET n = n + 1")
        .bind(today, h)));
  }

  // Now and then, forget what is too old to matter.
  if (Math.random() < 0.02) ctx.waitUntil(forget(env));
  return new Response(`counted ${fresh.length}\n`, { headers: { "content-type": "text/plain; charset=utf-8" } });
}

async function forget(env) {
  await env.DB.batch([
    env.DB.prepare("DELETE FROM hits WHERE day < ?1").bind(day(-KEEP_DAYS)),
    env.DB.prepare("DELETE FROM seen WHERE day < ?1").bind(day(-1)),
  ]);
}

async function popular(request, env, ctx) {
  const cache = caches.default;
  const key = new Request(new URL("/popular", request.url).toString());
  const cached = await cache.match(key);
  if (cached) return cached;

  const top = (d, limit) =>
    env.DB.prepare("SELECT hash, n FROM hits WHERE day = ?1 ORDER BY n DESC, hash LIMIT ?2").bind(d, limit).all();
  const rows = (await top(day(), TOP)).results;
  if (rows.length < TOP) {
    for (const r of (await top(day(-1), TOP * 2)).results) {
      if (rows.length >= TOP) break;
      if (!rows.some((x) => x.hash === r.hash)) rows.push({ hash: r.hash, n: 0 });
    }
  }

  const text = rows.map((r) => `${r.hash} ${r.n}\n`).join("");
  const response = new Response(text, {
    headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "public, max-age=300" },
  });
  ctx.waitUntil(cache.put(key, response.clone()));
  return response;
}

// ---- texture packs: the same counting as the icons, in tables of their own ----------------------

async function textureHit(request, env, ctx) {
  const body = (await request.text()).slice(0, MAX_PACKS_PER_HIT * 260);
  const ids = [...new Set(body.split(/\s+/).filter((id) => PACK_ID.test(id)))].slice(0, MAX_PACKS_PER_HIT);
  if (ids.length === 0) return new Response("nothing to count\n", { status: 400 });

  const today = day();
  const ip = request.headers.get("CF-Connecting-IP") || "";
  const who = (await sha256hex(`armsx2-texture-packs|${today}|${ip}`)).slice(0, 32);

  const seen = await env.DB.batch(ids.map((id) =>
    env.DB.prepare("INSERT OR IGNORE INTO tex_seen (day, who, id) VALUES (?1, ?2, ?3)").bind(today, who, id)));
  const fresh = ids.filter((_, i) => seen[i].meta.changes > 0);
  if (fresh.length > 0) {
    await env.DB.batch(fresh.map((id) =>
      env.DB.prepare("INSERT INTO tex_hits (day, id, n) VALUES (?1, ?2, 1) ON CONFLICT (day, id) DO UPDATE SET n = n + 1")
        .bind(today, id)));
  }

  if (Math.random() < 0.02) ctx.waitUntil(forgetTextures(env));
  return new Response(`counted ${fresh.length}\n`, { headers: { "content-type": "text/plain; charset=utf-8" } });
}

async function forgetTextures(env) {
  await env.DB.batch([
    env.DB.prepare("DELETE FROM tex_hits WHERE day < ?1").bind(day(-KEEP_DAYS)),
    env.DB.prepare("DELETE FROM tex_seen WHERE day < ?1").bind(day(-1)),
  ]);
}

async function texturePopular(request, env, ctx) {
  const cache = caches.default;
  const key = new Request(new URL("/textures/popular", request.url).toString());
  const cached = await cache.match(key);
  if (cached) return cached;

  const top = (d, limit) =>
    env.DB.prepare("SELECT id, n FROM tex_hits WHERE day = ?1 ORDER BY n DESC, id LIMIT ?2").bind(d, limit).all();
  const rows = (await top(day(), PACK_TOP)).results;
  if (rows.length < PACK_TOP) {
    for (const r of (await top(day(-1), PACK_TOP * 2)).results) {
      if (rows.length >= PACK_TOP) break;
      if (!rows.some((x) => x.id === r.id)) rows.push({ id: r.id, n: 0 });
    }
  }

  const text = rows.map((r) => `${r.id} ${r.n}\n`).join("");
  const response = new Response(text, {
    headers: { "content-type": "text/plain; charset=utf-8", "cache-control": "public, max-age=300" },
  });
  ctx.waitUntil(cache.put(key, response.clone()));
  return response;
}
