'use strict';
/*
 * A minimal reader for Lantmateriet's Markhojdmodell COGs -- enough of TIFF to pull float32
 * elevations out of a tiled, deflated, predictor-encoded raster over HTTP byte ranges.
 *
 * There is no GDAL here on purpose: topology is a single-file tool with no dependencies, and the
 * subset of TIFF these tiles use is small and fixed (classic little-endian TIFF, one float32 band,
 * 512x512 tiles, deflate). Everything else is rejected loudly rather than guessed at.
 */
const zlib = require('zlib');

const T = { WIDTH: 256, LENGTH: 257, BITS: 258, COMPRESSION: 259, PREDICTOR: 317, TILE_W: 322, TILE_L: 323, TILE_OFFSETS: 324, TILE_BYTES: 325, SAMPLES: 277, SAMPLE_FORMAT: 339, PIXEL_SCALE: 33550, TIEPOINT: 33922, NODATA: 42113 };
const TYPE_SIZE = { 1: 1, 2: 1, 3: 2, 4: 4, 5: 8, 6: 1, 7: 1, 8: 2, 9: 4, 10: 8, 11: 4, 12: 8, 16: 8 };

class Cog {
    constructor(fetchRange) {
        this.fetchRange = fetchRange;
    }

    /* `fetchRange(start, endInclusive) -> Promise<Buffer>` is the only I/O this needs, so the same
     reader serves an HTTP range request and a local file read. */
    static async open(fetchRange) {
        const c = new Cog(fetchRange);
        await c._readHeader();
        return c;
    }

    async _readHeader() {
        const head = await this.fetchRange(0, 15);
        if (head.toString('latin1', 0, 2) !== 'II') throw new Error('not a little-endian TIFF');
        if (head.readUInt16LE(2) !== 42) throw new Error('BigTIFF is not supported');
        const ifd0 = head.readUInt32LE(4);

        /* Read the directory, then any tag whose value spilled out of its 4-byte slot. The tile
       offset/count arrays are the big ones: 400 tiles here, so a few KB. */
        const nbuf = await this.fetchRange(ifd0, ifd0 + 1);
        const n = nbuf.readUInt16LE(0);
        const dir = await this.fetchRange(ifd0 + 2, ifd0 + 2 + n * 12 - 1);
        const tags = {};
        const spill = [];
        for (let i = 0; i < n; i++) {
            const o = i * 12;
            const tag = dir.readUInt16LE(o),
                type = dir.readUInt16LE(o + 2);
            const count = dir.readUInt32LE(o + 4);
            const size = (TYPE_SIZE[type] || 0) * count;
            if (!size) continue;
            if (size <= 4) tags[tag] = this._vals(dir.subarray(o + 8, o + 12), type, count);
            else spill.push({ tag, type, count, off: dir.readUInt32LE(o + 8), size });
        }
        for (const s of spill) {
            const b = await this.fetchRange(s.off, s.off + s.size - 1);
            tags[s.tag] = this._vals(b, s.type, s.count);
        }
        this.tags = tags;

        const one = (t) => (tags[t] ? Number(tags[t][0]) : undefined);
        this.width = one(T.WIDTH);
        this.height = one(T.LENGTH);
        this.tileW = one(T.TILE_W);
        this.tileH = one(T.TILE_L);
        this.compression = one(T.COMPRESSION);
        this.predictor = one(T.PREDICTOR) || 1;
        this.samples = one(T.SAMPLES) || 1;
        this.bits = one(T.BITS);
        this.format = one(T.SAMPLE_FORMAT);
        this.tileOffsets = tags[T.TILE_OFFSETS];
        this.tileBytes = tags[T.TILE_BYTES];
        this.nodata = tags[T.NODATA] ? parseFloat(String(tags[T.NODATA]).replace(/\0+$/, '')) : null;

        const ps = tags[T.PIXEL_SCALE],
            tp = tags[T.TIEPOINT];
        if (!ps || !tp) throw new Error('no GeoTIFF tiepoint/pixel scale');
        this.sx = Number(ps[0]);
        this.sy = Number(ps[1]);
        this.originX = Number(tp[3]);
        this.originY = Number(tp[4]);

        if (this.samples !== 1 || this.bits !== 32 || this.format !== 3) throw new Error(`expected one float32 band, got samples=${this.samples} bits=${this.bits} fmt=${this.format}`);
        if (this.compression !== 8 && this.compression !== 32946) throw new Error(`expected deflate, got compression=${this.compression}`);
        if (!this.tileW) throw new Error('expected a tiled TIFF (COG)');
        this.tilesAcross = Math.ceil(this.width / this.tileW);
        this._cache = new Map();
    }

    _vals(buf, type, count) {
        const out = [];
        for (let i = 0; i < count; i++) {
            switch (type) {
                case 1:
                case 7:
                    out.push(buf.readUInt8(i));
                    break;
                case 2:
                    out.push(String.fromCharCode(buf.readUInt8(i)));
                    break;
                case 3:
                    out.push(buf.readUInt16LE(i * 2));
                    break;
                case 4:
                    out.push(buf.readUInt32LE(i * 4));
                    break;
                case 8:
                    out.push(buf.readInt16LE(i * 2));
                    break;
                case 9:
                    out.push(buf.readInt32LE(i * 4));
                    break;
                case 11:
                    out.push(buf.readFloatLE(i * 4));
                    break;
                case 12:
                    out.push(buf.readDoubleLE(i * 8));
                    break;
                case 5:
                    out.push(buf.readUInt32LE(i * 8) / buf.readUInt32LE(i * 8 + 4));
                    break;
                default:
                    out.push(0);
            }
        }
        return type === 2 ? out.join('') : out;
    }

    /* Pixel column/row for a projected coordinate. North-up only, which is all these tiles are. */
    colRow(x, y) {
        return [(x - this.originX) / this.sx, (this.originY - y) / this.sy];
    }

    async tile(tx, ty) {
        const idx = ty * this.tilesAcross + tx;
        if (this._cache.has(idx)) return this._cache.get(idx);
        const off = Number(this.tileOffsets[idx]),
            len = Number(this.tileBytes[idx]);
        if (!len) return null; /* sparse tile: no data written */
        const raw = await this.fetchRange(off, off + len - 1);
        let buf = zlib.inflateSync(raw);
        if (this.predictor === 3) buf = this._unpredictFloat(buf);
        else if (this.predictor === 2) throw new Error('horizontal predictor on float is not valid');
        const f = new Float32Array(buf.buffer, buf.byteOffset, this.tileW * this.tileH);
        this._cache.set(idx, f);
        return f;
    }

    /* The floating-point predictor (3) splits each row into byte planes -- all the most significant
     bytes, then the next, and so on -- and delta encodes along the row. Undo both, per row.
     The delta stride is SAMPLES PER PIXEL (1 here), not bytes per sample: libtiff's fpAcc walks
     the byte planes one byte at a time, and using 4 silently decodes to garbage. */
    _unpredictFloat(buf) {
        const w = this.tileW,
            h = this.tileH,
            bpp = 4,
            sp = this.samples,
            row = w * bpp;
        const out = Buffer.allocUnsafe(buf.length);
        for (let r = 0; r < h; r++) {
            const base = r * row;
            for (let i = sp; i < row; i++) buf[base + i] = (buf[base + i] + buf[base + i - sp]) & 0xff;
            /* little-endian: plane b holds byte (bpp-1-b) of each float, most significant plane first */
            for (let c = 0; c < w; c++) for (let b = 0; b < bpp; b++) out[base + c * bpp + (bpp - 1 - b)] = buf[base + b * w + c];
        }
        return out;
    }

    /* Nearest-neighbour value at a projected coordinate, or null outside the raster / on nodata. */
    async valueAt(x, y) {
        const [fc, fr] = this.colRow(x, y);
        const c = Math.floor(fc),
            r = Math.floor(fr);
        if (c < 0 || r < 0 || c >= this.width || r >= this.height) return null;
        const t = await this.tile(Math.floor(c / this.tileW), Math.floor(r / this.tileH));
        if (!t) return null;
        const v = t[(r % this.tileH) * this.tileW + (c % this.tileW)];
        return this.nodata != null && v === this.nodata ? null : v;
    }

    /* Pull every COG tile overlapping a projected rectangle, so the caller can then read a whole
     block synchronously. Filling a 1km block pixel by pixel through valueAt means a million
     promises for data that is already resident after the first few. */
    async prefetch(xMin, yMin, xMax, yMax) {
        const [c0, r1] = this.colRow(xMin, yMin),
            [c1, r0] = this.colRow(xMax, yMax);
        const tx0 = Math.max(0, Math.floor(c0 / this.tileW));
        const tx1 = Math.min(this.tilesAcross - 1, Math.floor(c1 / this.tileW));
        const ty0 = Math.max(0, Math.floor(r0 / this.tileH));
        const ty1 = Math.min(Math.ceil(this.height / this.tileH) - 1, Math.floor(r1 / this.tileH));
        for (let ty = ty0; ty <= ty1; ty++) for (let tx = tx0; tx <= tx1; tx++) await this.tile(tx, ty);
        return (tx1 - tx0 + 1) * (ty1 - ty0 + 1);
    }

    /* Only valid for coordinates covered by a preceding prefetch; returns undefined otherwise so a
     missed prefetch shows up as a loud failure rather than silent nodata. */
    valueAtSync(x, y) {
        const [fc, fr] = this.colRow(x, y);
        const c = Math.floor(fc),
            r = Math.floor(fr);
        if (c < 0 || r < 0 || c >= this.width || r >= this.height) return null;
        const idx = Math.floor(r / this.tileH) * this.tilesAcross + Math.floor(c / this.tileW);
        if (!this._cache.has(idx)) return undefined;
        const t = this._cache.get(idx);
        if (!t) return null;
        const v = t[(r % this.tileH) * this.tileW + (c % this.tileW)];
        return this.nodata != null && v === this.nodata ? null : v;
    }

    /* Release decoded tiles; a wide --radius spans several source tiles and they add up fast. */
    evict() {
        this._cache.clear();
    }
}

module.exports = { Cog };
