// ARMSX2 Online Icons: which icons people downloaded today, for the "Popular Today" tab.
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

const HASH = /^[0-9a-f]{16}$/;
const MAX_PER_HIT = 100;
const TOP = 6;
const KEEP_DAYS = 3;

export default {
  async fetch(request, env, ctx) {
    const url = new URL(request.url);
    if (url.pathname === "/hit" && request.method === "POST") return hit(request, env, ctx);
    if (url.pathname === "/popular" && request.method === "GET") return popular(request, env, ctx);
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
