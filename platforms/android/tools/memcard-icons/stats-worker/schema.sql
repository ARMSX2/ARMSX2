-- D1 schema for the Online Icons and texture pack download counters (worker.js). Run once in the
-- database's console; the texture tables can be run on their own, after the icon ones.

-- Downloads per icon per UTC day.
CREATE TABLE IF NOT EXISTS hits (
    day TEXT NOT NULL,      -- YYYY-MM-DD, UTC
    hash TEXT NOT NULL,     -- the icon, as named in index.txt
    n INTEGER NOT NULL,
    PRIMARY KEY (day, hash)
);
CREATE INDEX IF NOT EXISTS hits_by_day ON hits (day, n DESC);

-- Who already counted which icon today, so one person can't inflate a count. "who" is a salted
-- hash of the day and the IP, never the IP, and the rows go after a day.
CREATE TABLE IF NOT EXISTS seen (
    day TEXT NOT NULL,
    who TEXT NOT NULL,
    hash TEXT NOT NULL,
    PRIMARY KEY (day, who, hash)
);

-- Texture packs: downloads per pack per UTC day, and who already counted which pack today, the
-- same way as the icons. "id" is the pack's id in the texture catalog (textures.json).
CREATE TABLE IF NOT EXISTS tex_hits (
    day TEXT NOT NULL,
    id TEXT NOT NULL,
    n INTEGER NOT NULL,
    PRIMARY KEY (day, id)
);
CREATE INDEX IF NOT EXISTS tex_hits_by_day ON tex_hits (day, n DESC);

CREATE TABLE IF NOT EXISTS tex_seen (
    day TEXT NOT NULL,
    who TEXT NOT NULL,
    id TEXT NOT NULL,
    PRIMARY KEY (day, who, id)
);
