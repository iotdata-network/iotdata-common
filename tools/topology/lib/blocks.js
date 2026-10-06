'use strict';
/*
 * The terrain cache: 1km blocks on the SWEREF 99 TM kilometre grid, committed to the repo.
 *
 * Keying by national grid kilometre rather than by site or by Lantmateriet tile is what makes the
 * cache shareable: two sites a few hundred metres apart reuse the same blocks, a site that straddles
 * a tile boundary is no special case, and "cache 1km around every point" becomes a set union over
 * block keys. A node 10km away pulls blocks around ITSELF rather than the whole bounding rectangle.
 *
 * A block is a one-line JSON header, a newline, then deflated samples. The header carries the
 * quantisation explicitly, so the format can hold centimetres or raw float later without a version
 * bump -- see QUANTISATION below for why ground defaults to decimetres.
 */
const fs = require('fs'),
    path = require('path'),
    zlib = require('zlib');

const SIDE = 1000; /* block edge, metres -- also the cache's unit of sharing */
const MAGIC = 'iotdata-terrain';

/* QUANTISATION. Ground is stored as int16 decimetres: +-3276.7m covers Sweden with room to spare,
   and 0.1m is exactly the height uncertainty Lantmateriet publishes for this product
   (lagesosakerhethojd = 0.1), so the stored value is never the limiting error. It also compresses
   about 6x with row deltas, which is what keeps the cache committable. Storing the source float32
   instead costs roughly 3x the space to record digits the data does not actually carry. */
const LAYERS = {
    ground: { res: 1, dtype: 'i16', scale: 0.1, nodata: -32768, delta: true },
    canopy: { res: 2, dtype: 'u8', scale: 1.0, nodata: 255, delta: false },
};

const floorTo = (v, m) => Math.floor(v / m) * m;
const keyOf = (e0, n0) => `E${e0 / 1000}_N${n0 / 1000}`;
const fileOf = (dir, layer, e0, n0) => path.join(dir, layer, keyOf(e0, n0) + '.bin');

/* Every block within `radius` of any point -- a union of discs, not a bounding box. */
function blocksFor(points, radius) {
    const out = new Map();
    for (const p of points) {
        const e0 = floorTo(p.e - radius, SIDE),
            e1 = floorTo(p.e + radius, SIDE);
        const n0 = floorTo(p.n - radius, SIDE),
            n1 = floorTo(p.n + radius, SIDE);
        for (let e = e0; e <= e1; e += SIDE)
            for (let n = n0; n <= n1; n += SIDE) {
                /* keep the block if the disc actually reaches it, not just its bounding box */
                const dx = Math.max(e - p.e, 0, p.e - (e + SIDE));
                const dy = Math.max(n - p.n, 0, p.n - (n + SIDE));
                if (Math.hypot(dx, dy) <= radius) out.set(keyOf(e, n), { e0: e, n0: n, key: keyOf(e, n) });
            }
    }
    return [...out.values()].sort((a, b) => a.key.localeCompare(b.key));
}

function write(dir, layer, e0, n0, samples, extra = {}) {
    const L = LAYERS[layer],
        w = SIDE / L.res;
    if (samples.length !== w * w) throw new Error(`${layer}: expected ${w * w} samples, got ${samples.length}`);
    let payload = samples;
    if (L.delta) {
        /* row-wise deltas: terrain is smooth, residuals are tiny */
        payload = new Int16Array(samples.length);
        for (let r = 0; r < w; r++) {
            let prev = 0;
            for (let c = 0; c < w; c++) {
                const i = r * w + c;
                payload[i] = (samples[i] - prev) | 0;
                prev = samples[i];
            }
        }
    }
    const head = JSON.stringify({
        magic: MAGIC,
        v: 1,
        layer,
        e0,
        n0,
        side: SIDE,
        res: L.res,
        dtype: L.dtype,
        scale: L.scale,
        nodata: L.nodata,
        delta: !!L.delta,
        w,
        ...extra,
    });
    const body = zlib.deflateSync(Buffer.from(payload.buffer, payload.byteOffset, payload.byteLength), { level: 9 });
    const file = fileOf(dir, layer, e0, n0);
    fs.mkdirSync(path.dirname(file), { recursive: true });
    fs.writeFileSync(file + '.tmp', Buffer.concat([Buffer.from(head + '\n'), body]));
    fs.renameSync(file + '.tmp', file);
    return body.length + head.length + 1;
}

function read(file) {
    const buf = fs.readFileSync(file);
    const nl = buf.indexOf(0x0a);
    if (nl < 0) throw new Error(`${file}: no header`);
    const meta = JSON.parse(buf.subarray(0, nl).toString('utf8'));
    if (meta.magic !== MAGIC) throw new Error(`${file}: not a terrain block`);
    const raw = zlib.inflateSync(buf.subarray(nl + 1));
    let data = meta.dtype === 'u8' ? new Uint8Array(raw.buffer, raw.byteOffset, raw.byteLength) : new Int16Array(raw.buffer, raw.byteOffset, raw.byteLength / 2);
    if (meta.delta) {
        const out = new Int16Array(data.length),
            w = meta.w;
        for (let r = 0; r < w; r++) {
            let acc = 0;
            for (let c = 0; c < w; c++) {
                acc = (acc + data[r * w + c]) | 0;
                out[r * w + c] = acc;
            }
        }
        data = out;
    }
    return { meta, data };
}

/* The cache, read lazily.
 *
 * A block is 2MB of Int16 once decompressed, against about 350KB on disk, so decompressing the
 * whole cache at startup costs roughly 2MB per cached square kilometre: 360MB resident for a 5km
 * radius, and death on a small box for anything larger. Since a session only ever touches the
 * blocks under the paths it actually draws, blocks are indexed at startup -- the one-line header
 * is read, the payload is not -- and inflated on demand behind an LRU with a byte budget.
 *
 * The headers are enough for every question the page asks about provenance (survey dates, canopy
 * coverage, which squares exist), so the common case never inflates anything it does not sample.
 */
class Cache {
    constructor(dir, budget) {
        this.dir = dir;
        this.layers = { ground: new Map(), canopy: new Map() };
        this.manifest = null;
        this.budget = Number(budget) > 0 ? Number(budget) : 192e6;
        this.resident = 0;
        this.lru = new Map(); /* "layer/key" -> entry, oldest first */
        this.hits = 0;
        this.misses = 0;
        this.evictions = 0;
    }

    /* Read just the header line of a block: enough to know where it is and how it is encoded. */
    static header(file) {
        const fd = fs.openSync(file, 'r');
        try {
            const buf = Buffer.alloc(1024);
            const n = fs.readSync(fd, buf, 0, 1024, 0);
            const nl = buf.indexOf(0x0a);
            if (nl < 0 || nl >= n) throw new Error('no header');
            return JSON.parse(buf.subarray(0, nl).toString('utf8'));
        } finally {
            fs.closeSync(fd);
        }
    }

    load() {
        const mf = path.join(this.dir, 'manifest.json');
        this.manifest = fs.existsSync(mf) ? JSON.parse(fs.readFileSync(mf, 'utf8')) : null;
        let bytes = 0,
            counts = {};
        for (const layer of Object.keys(this.layers)) {
            const d = path.join(this.dir, layer);
            if (!fs.existsSync(d)) continue;
            for (const f of fs.readdirSync(d)) {
                if (!f.endsWith('.bin')) continue;
                const full = path.join(d, f);
                try {
                    const meta = Cache.header(full);
                    if (meta.magic !== MAGIC) throw new Error('not a terrain block');
                    this.layers[layer].set(keyOf(meta.e0, meta.n0), {
                        file: full,
                        meta,
                        data: null,
                    });
                    bytes += fs.statSync(full).size;
                } catch (e) {
                    console.error(`terrain: ${full}: ${e.message}`);
                }
            }
            counts[layer] = this.layers[layer].size;
        }
        return { counts, bytes };
    }

    /* The entry with its payload present, inflating and making room if this is the first touch. */
    _load(layer, key) {
        const b = this.layers[layer].get(key);
        if (!b) return null;
        const id = layer + '/' + key;
        if (b.data) {
            this.hits++;
            this.lru.delete(id);
            this.lru.set(id, b); /* touch: move to the young end */
            return b;
        }
        this.misses++;
        let got;
        try {
            got = read(b.file);
        } catch (e) {
            console.error(`terrain: ${b.file}: ${e.message}`);
            return null;
        }
        b.data = got.data;
        b.bytes = got.data.BYTES_PER_ELEMENT * got.data.length;
        this.resident += b.bytes;
        this.lru.set(id, b);
        /* Evict oldest until back inside budget, but never the block just asked for. */
        while (this.resident > this.budget && this.lru.size > 1) {
            const [oldId, old] = this.lru.entries().next().value;
            if (oldId === id) break;
            this.lru.delete(oldId);
            this.resident -= old.bytes || 0;
            old.data = null;
            old.bytes = 0;
            this.evictions++;
        }
        return b;
    }

    has(layer, e, n) {
        return this.layers[layer].has(keyOf(floorTo(e, SIDE), floorTo(n, SIDE)));
    }

    stats() {
        return {
            resident: this.resident,
            budget: this.budget,
            loaded: this.lru.size,
            hits: this.hits,
            misses: this.misses,
            evictions: this.evictions,
        };
    }

    /* Nearest-cell lookup. Returns null outside the cache or on nodata, never a guessed value --
     the caller has to be able to tell "no data here" from "zero metres here". */
    at(layer, e, n) {
        const b = this._load(layer, keyOf(floorTo(e, SIDE), floorTo(n, SIDE)));
        if (!b) return null;
        const { meta, data } = b,
            w = meta.w;
        const c = Math.min(w - 1, Math.max(0, Math.floor((e - meta.e0) / meta.res)));
        /* rows run north to south, matching the source raster */
        const r = Math.min(w - 1, Math.max(0, Math.floor((meta.n0 + SIDE - n) / meta.res)));
        const v = data[r * w + c];
        return v === meta.nodata ? null : v * meta.scale;
    }

    /* Bilinear, for ground profiles -- a 1m grid sampled nearest shows visible stair steps on a
     shallow slope, which reads as terrain roughness that is not there. */
    atSmooth(layer, e, n) {
        const b = this._load(layer, keyOf(floorTo(e, SIDE), floorTo(n, SIDE)));
        if (!b) return null;
        const { meta } = b,
            res = meta.res;
        const fe = (e - meta.e0) / res - 0.5,
            fn = (meta.n0 + SIDE - n) / res - 0.5;
        const c0 = Math.floor(fe),
            r0 = Math.floor(fn),
            tx = fe - c0,
            ty = fn - r0;
        let sum = 0,
            wsum = 0;
        for (const [dr, dc, wt] of [
            [0, 0, (1 - tx) * (1 - ty)],
            [0, 1, tx * (1 - ty)],
            [1, 0, (1 - tx) * ty],
            [1, 1, tx * ty],
        ]) {
            if (wt <= 0) continue;
            const v = this.at(layer, meta.e0 + (c0 + dc + 0.5) * res, meta.n0 + SIDE - (r0 + dr + 0.5) * res);
            if (v != null) {
                sum += v * wt;
                wsum += wt;
            }
        }
        return wsum > 0 ? sum / wsum : null;
    }

    extent() {
        const keys = [...this.layers.ground.keys()];
        return {
            blocks: keys,
            side: SIDE,
            canopyBlocks: [...this.layers.canopy.keys()],
        };
    }
}

module.exports = {
    SIDE,
    LAYERS,
    blocksFor,
    keyOf,
    fileOf,
    write,
    read,
    Cache,
    floorTo,
};
