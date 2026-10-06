/*
 * link.js -- the path physics, shared verbatim by the server and the browser.
 *
 * The receptivity overlay has to evaluate thousands of paths, which only the server can do quickly
 * because it holds the terrain; the profile panel evaluates one, in the page. Both must agree to
 * the last decibel or the map and the panel would tell different stories about the same link, so
 * the maths lives here once and is loaded by both.
 */
(function (root, factory) {
    if (typeof module === 'object' && module.exports) module.exports = factory();
    else root.LINK = factory();
})(typeof self !== 'undefined' ? self : this, function () {
    'use strict';

    const R_EARTH = 6371000,
        K_REFRACT = 4 / 3,
        C_LIGHT = 299792458;

    const lambda = (freqMHz) => C_LIGHT / ((Number(freqMHz) || 868) * 1e6);

    /* Earth curvature as a bulge added to the terrain, so the sight line stays a straight line.
     k = 4/3 is standard refraction -- optimistic in a cold inversion, which this valley gets. */
    const bulge = (d1, d2) => (d1 * d2) / (2 * K_REFRACT * R_EARTH);

    const fresnel = (lam, d1, d2, d) => Math.sqrt((lam * d1 * d2) / d);

    /* Free space path loss, dB. */
    const fspl = (d, freqMHz) => (d <= 1 ? 0 : 32.44 + 20 * Math.log10(d / 1000) + 20 * Math.log10(Number(freqMHz) || 868));

    /* ITU-R P.526 single knife-edge diffraction. v is the Fresnel-Kirchhoff parameter; v = -0.85 is
     exactly 60% F1 clearance, which is why the classic 60% rule and this curve agree that a path
     clearing 0.6 F1 pays essentially nothing. */
    function knifeEdge(v) {
        if (v <= -0.78) return 0;
        return 6.9 + 20 * Math.log10(Math.sqrt((v - 0.1) * (v - 0.1) + 1) + v - 0.1);
    }

    /* Weissberger's modified exponential decay -- excess loss through woodland, 230MHz to 95GHz,
     out to 400m of foliage. At 868MHz: ~0.43 dB/m for the first 14m, then sub-linear.
   *
   * Vegetation is deliberately kept out of the Fresnel verdict. A treeline attenuates, a ridge
   * blocks; scoring 4m of spruce as though it were 4m of granite would condemn working links. */
    function weissberger(depth, freqMHz) {
        if (!(depth > 0)) return 0;
        const f = Math.pow((Number(freqMHz) || 868) / 1000, 0.284);
        return depth <= 14 ? 0.45 * f * depth : 1.33 * f * Math.pow(depth, 0.588);
    }

    /* Terrain, canopy, sight line and Fresnel zone sampled along one path.
     *
     * prof  : [{ z, res, canopy }] ground metres AMSL, grid size, canopy metres above ground or null
     * d     : path length, metres
     * hA,hB : antenna heights AMSL (ground + agl), or null if not yet known
     * opt   : { freq, grow } -- grow is METRES of canopy growth to allow since the survey
     */
    function analyse(prof, d, hA, hB, opt) {
        opt = opt || {};
        const n = prof.length,
            freq = opt.freq,
            lam = lambda(freq);
        const grow = Number(opt.grow) || 0;
        const known = hA != null && hB != null;
        const pts = new Array(n);
        let peak = null,
            worst = null,
            vegPeak = null,
            vegMax = 0,
            vegSum = 0,
            vegUnknown = 0;

        for (let k = 0; k < n; k++) {
            const s = prof[k];
            const t = n > 1 ? k / (n - 1) : 0,
                d1 = d * t,
                d2 = d - d1;
            const p = { d1, z: s.z, res: s.res, ground: s.z + bulge(d1, d2) };
            p.vegKnown = s.canopy != null;
            /* growth only lifts cells that had vegetation -- bare ground does not sprout */
            p.veg = p.vegKnown ? Math.max(0, s.canopy + (s.canopy > 0 ? grow : 0)) : 0;
            p.top = p.ground + p.veg;
            if (known) {
                p.los = hA + (hB - hA) * t;
                p.f1 = fresnel(lam, d1, d2, d);
                p.clr = p.los - p.ground;
            }
            pts[k] = p;
            if (!peak || p.z > peak.z) peak = p;
            /* the tallest stand on the path, kept as a point so the map can flag where it is */
            if (p.veg > vegMax) {
                vegMax = p.veg;
                vegPeak = p;
            }
            vegSum += p.veg;
            if (!p.vegKnown) vegUnknown++;
            /* Ranked by clearance as a FRACTION of F1, not metres: 10m of clearance is plenty at 200m
         out and nothing mid-way through a 10km hop. F1 collapses to zero at the antennas, so the
         end samples are skipped rather than dividing by ~0. */
            if (known && p.f1 > 0.5 && (!worst || p.clr / p.f1 < worst.clr / worst.f1)) worst = p;
        }

        /* How much of the ray travels through foliage: under the canopy top but above the ground.
       Below the ground it is a terrain problem and the clearance verdict already has it. */
        let vegDepth = 0;
        if (known && n > 1) {
            const step = d / (n - 1);
            for (let k = 0; k < n; k++) if (pts[k].los < pts[k].top && pts[k].los >= pts[k].ground) vegDepth += step;
        }

        const out = { d, n, pts, peak, worst, vegPeak, known, lam, spacing: n > 1 ? d / (n - 1) : 0, hasVeg: vegUnknown < n, vegMax, vegMean: vegSum / n, vegUnknown: vegUnknown / n, vegDepth, vegLoss: weissberger(vegDepth, freq), grow };
        const rs = [];
        for (const s of prof) if (s.res != null) rs.push(s.res);
        out.resMax = rs.length ? Math.max.apply(null, rs) : null;

        if (known && worst) {
            /* obstruction height above the sight line; negative when the path is clear */
            const h = -worst.clr,
                d1 = worst.d1,
                d2 = d - worst.d1;
            out.v = d1 > 0 && d2 > 0 ? h * Math.sqrt((2 * d) / (lam * d1 * d2)) : 0;
            out.diffLoss = knifeEdge(out.v);
        } else {
            out.v = null;
            out.diffLoss = 0;
        }

        out.fspl = fspl(d, freq);
        out.loss = out.fspl + out.diffLoss + out.vegLoss;
        return out;
    }

    /* dB of margin left over a budget, and which band that lands in.
     *
     * The top band starts at 45dB rather than 30 because a LoRa SF12 budget is enormous next to free
     * space loss at these ranges -- 151dB against 91dB at a kilometre -- so coarser bands paint most
     * of a short-range disc one colour and hide exactly the structure you are siting against. Drop
     * the budget towards a faster spreading factor (SF7 is about 137dB) to sharpen it further. */
    const BANDS = [
        { min: 45, label: 'strong', colour: [30, 132, 73] },
        { min: 30, label: 'good', colour: [94, 184, 62] },
        { min: 20, label: 'fair', colour: [196, 214, 58] },
        { min: 10, label: 'usable', colour: [240, 180, 41] },
        { min: 0, label: 'marginal', colour: [222, 110, 40] },
        { min: -1e9, label: 'no link', colour: [200, 40, 60] },
    ];
    const bandOf = (margin) => {
        for (let i = 0; i < BANDS.length; i++) if (margin >= BANDS[i].min) return i;
        return BANDS.length - 1;
    };

    /* Receiver sensitivity by LoRa spreading factor at 125kHz, the EU868 default -- typical SX127x /
     SX126x datasheet figures. A spreading factor is the thing you actually choose on a node; the
     decibels are a consequence of it, so the UI asks for the former and derives the latter.
     Slower is more sensitive and much slower on air: SF12 buys 14dB over SF7 and costs roughly 32x
     the airtime, which the EU duty cycle then limits. */
    const LORA_SENS = { 7: -123, 8: -126, 9: -129, 10: -132, 11: -134.5, 12: -137 };
    const SFS = [7, 8, 9, 10, 11, 12];

    const sens = (sf) => (LORA_SENS[sf] != null ? LORA_SENS[sf] : LORA_SENS[12]);

    /* Everything the two ends contribute, before the path takes any of it away:
     *   transmit power + the transmitting antenna + the receiving antenna.
     * Both antennas count -- gain is reciprocal, so a 3dBi node on one end of a link costs 2dB
     * against the same link seen from a 5dBi node, which is exactly why these belong per device
     * rather than as one global figure. */
    const systemGain = (txdbm, gainTx, gainRx) => (Number(txdbm) || 0) + (Number(gainTx) || 0) + (Number(gainRx) || 0);

    /* Link budget at a spreading factor: what the ends give, minus what the receiver needs. */
    const budgetFor = (sf, sysGain) => (Number(sysGain) || 0) - sens(sf);

    /* Radiated power, for the regulatory check -- EU868 allows 14dBm ERP on the common channels
     and 27dBm on the g3 sub-band, and a 22dBm module behind a 5dBi antenna is already past the
     first of those. Worth showing; not worth refusing to compute. */
    const eirpOf = (txdbm, gain) => (Number(txdbm) || 0) + (Number(gain) || 0);

    /* The spreading factors that still close a path of this loss, fastest first. */
    const sfsThatClose = (loss, sysGain) => SFS.filter((sf) => budgetFor(sf, sysGain) - loss >= 0);

    return { R_EARTH, K_REFRACT, C_LIGHT, lambda, bulge, fresnel, fspl, knifeEdge, weissberger, analyse, BANDS, bandOf, LORA_SENS, SFS, sens, systemGain, budgetFor, eirpOf, sfsThatClose };
});
