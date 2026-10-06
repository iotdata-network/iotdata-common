'use strict';
/*
 * WGS84 <-> SWEREF 99 TM (EPSG:3006), the grid Lantmateriet's height data is published on.
 *
 * Gauss conformal projection, using Lantmateriet's own published series expansion. Accurate to a
 * few millimetres anywhere in Sweden, which is two orders below the 0.3m planar uncertainty of the
 * height model itself, so the projection is never the limiting error.
 *
 * SWEREF 99 and WGS 84 differ by a few centimetres, far below the grid resolution, so they are
 * treated as the same datum here -- as Lantmateriet themselves recommend for this purpose.
 */

const AXIS = 6378137.0,
    FLAT = 1 / 298.257222101; /* GRS 80 */
const K0 = 0.9996,
    FE = 500000.0,
    FN = 0.0,
    LON0 = (15 * Math.PI) / 180;

const e2 = FLAT * (2 - FLAT),
    n = FLAT / (2 - FLAT);
const ahat = (AXIS / (1 + n)) * (1 + (n * n) / 4 + n ** 4 / 64);

const sq = (x) => x * x;
const rad = (d) => (d * Math.PI) / 180,
    deg = (r) => (r * 180) / Math.PI;

/* conformal-latitude series, forward */
const A = e2;
const B = (5 * e2 ** 2 - e2 ** 3) / 6;
const C = (104 * e2 ** 3 - 45 * e2 ** 4) / 120;
const D = (1237 * e2 ** 4) / 1260;

const b1 = n / 2 - (2 * n ** 2) / 3 + (5 * n ** 3) / 16 + (41 * n ** 4) / 180;
const b2 = (13 * n ** 2) / 48 - (3 * n ** 3) / 5 + (557 * n ** 4) / 1440;
const b3 = (61 * n ** 3) / 240 - (103 * n ** 4) / 140;
const b4 = (49561 * n ** 4) / 161280;

/* and inverse */
const d1 = n / 2 - (2 * n ** 2) / 3 + (37 * n ** 3) / 96 - n ** 4 / 360;
const d2 = n ** 2 / 48 + n ** 3 / 15 - (437 * n ** 4) / 1440;
const d3 = (17 * n ** 3) / 480 - (37 * n ** 4) / 840;
const d4 = (4397 * n ** 4) / 161280;

const As = e2 + e2 ** 2 + e2 ** 3 + e2 ** 4;
const Bs = -(7 * e2 ** 2 + 17 * e2 ** 3 + 30 * e2 ** 4) / 6;
const Cs = (224 * e2 ** 3 + 889 * e2 ** 4) / 120;
const Ds = -(4279 * e2 ** 4) / 1260;

/* lat/lng in degrees -> { e, n } metres */
function toTM(lat, lng) {
    const phi = rad(lat),
        dl = rad(lng) - LON0;
    const s = Math.sin(phi),
        c = Math.cos(phi);
    const phiStar = phi - s * c * (A + B * sq(s) + C * s ** 4 + D * s ** 6);
    const xi = Math.atan(Math.tan(phiStar) / Math.cos(dl));
    const eta = Math.atanh(Math.cos(phiStar) * Math.sin(dl));
    const north = K0 * ahat * (xi + b1 * Math.sin(2 * xi) * Math.cosh(2 * eta) + b2 * Math.sin(4 * xi) * Math.cosh(4 * eta) + b3 * Math.sin(6 * xi) * Math.cosh(6 * eta) + b4 * Math.sin(8 * xi) * Math.cosh(8 * eta)) + FN;
    const east = K0 * ahat * (eta + b1 * Math.cos(2 * xi) * Math.sinh(2 * eta) + b2 * Math.cos(4 * xi) * Math.sinh(4 * eta) + b3 * Math.cos(6 * xi) * Math.sinh(6 * eta) + b4 * Math.cos(8 * xi) * Math.sinh(8 * eta)) + FE;
    return { e: east, n: north };
}

/* { e, n } metres -> { lat, lng } degrees */
function toWGS(e, north) {
    const xi = (north - FN) / (K0 * ahat),
        eta = (e - FE) / (K0 * ahat);
    const xiS = xi - d1 * Math.sin(2 * xi) * Math.cosh(2 * eta) - d2 * Math.sin(4 * xi) * Math.cosh(4 * eta) - d3 * Math.sin(6 * xi) * Math.cosh(6 * eta) - d4 * Math.sin(8 * xi) * Math.cosh(8 * eta);
    const etaS = eta - d1 * Math.cos(2 * xi) * Math.sinh(2 * eta) - d2 * Math.cos(4 * xi) * Math.sinh(4 * eta) - d3 * Math.cos(6 * xi) * Math.sinh(6 * eta) - d4 * Math.cos(8 * xi) * Math.sinh(8 * eta);
    const phiStar = Math.asin(Math.sin(xiS) / Math.cosh(etaS));
    const dl = Math.atan(Math.sinh(etaS) / Math.cos(xiS));
    const s = Math.sin(phiStar),
        c = Math.cos(phiStar);
    const phi = phiStar + s * c * (As + Bs * sq(s) + Cs * s ** 4 + Ds * s ** 6);
    return { lat: deg(phi), lng: deg(LON0 + dl) };
}

module.exports = { toTM, toWGS };
