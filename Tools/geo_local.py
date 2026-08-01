# geo_local.py -- conversions LAMBERT 93 <-> WGS84 <-> repere LOCAL CityLab,
# et decodage des geometries GPKG. STDLIB PURE (ni GDAL, ni pyproj, ni numpy) :
# la machine n'a pas d'outil geo, et le pipeline n'en a jamais eu besoin.
#
# POURQUOI CE MODULE : la source riche des routes est le GPKG BD TOPO
# departemental (SRS 2154, Lambert 93), alors que tout le pipeline CityLab
# travaille en metres LOCAUX equirectangulaires autour du Capitole, avec
# NORD = -Y (convention gelee, cf. SourceData/GrandFetch/GF-Common.ps1 l. 21-28
# et C:/LidarPoC/work/E2SOL1/fetch_routes_bdtopo.py l. 24-29).
#
# La chaine WFS historique passait par EPSG:4326 ; on refait EXACTEMENT la meme
# arrivee (meme Lat0/Lon0, memes facteurs metriques, meme arrondi) apres avoir
# deprojete le Lambert 93. L'ecart mesure avec l'ancien chemin est reporte par
# fetch_routes_gpkg.py --controle (ordre du centimetre : c'est la meme donnee IGN,
# seul le transport change).
import math
import struct

# --- Repere local CityLab (GELE : ne pas toucher sans regenerer TOUTE la ville) --
LAT0 = 43.6045
LON0 = 1.4442
M_PER_LAT = 110540.0
M_PER_LON = 111320.0 * math.cos(math.radians(LAT0))

# --- Lambert 93 / RGF93 (EPSG:2154), conique conforme secante (LCC 2SP) ---------
_A = 6378137.0                       # GRS80
_INV_F = 298.257222101
_F = 1.0 / _INV_F
_E2 = 2.0 * _F - _F * _F
_E = math.sqrt(_E2)
_LON0_L93 = math.radians(3.0)
_LAT0_L93 = math.radians(46.5)
_LAT1 = math.radians(44.0)
_LAT2 = math.radians(49.0)
_FE = 700000.0
_FN = 6600000.0


def _m(phi):
    return math.cos(phi) / math.sqrt(1.0 - _E2 * math.sin(phi) ** 2)


def _t(phi):
    s = math.sin(phi)
    return (math.tan(math.pi / 4.0 - phi / 2.0)
            / ((1.0 - _E * s) / (1.0 + _E * s)) ** (_E / 2.0))


_M1, _M2 = _m(_LAT1), _m(_LAT2)
_T1, _T2 = _t(_LAT1), _t(_LAT2)
_N = (math.log(_M1) - math.log(_M2)) / (math.log(_T1) - math.log(_T2))
_BIGF = _M1 / (_N * _T1 ** _N)
_C = _A * _BIGF                       # constante IGN C
_R0 = _C * _t(_LAT0_L93) ** _N
_YS = _FN + _R0                       # constante IGN Ys


def lamb93_to_wgs84(x, y):
    """Lambert 93 (m) -> (lon, lat) en degres RGF93 (== WGS84 a ~1 cm pres)."""
    dx = x - _FE
    dy = _R0 - (y - _FN)
    r = math.copysign(math.hypot(dx, dy), _N)
    theta = math.atan2(dx, dy)
    t = (r / _C) ** (1.0 / _N)
    phi = math.pi / 2.0 - 2.0 * math.atan(t)
    for _ in range(12):               # convergence < 1e-12 rad en ~5 tours
        s = math.sin(phi)
        phi_new = (math.pi / 2.0
                   - 2.0 * math.atan(t * ((1.0 - _E * s) / (1.0 + _E * s)) ** (_E / 2.0)))
        if abs(phi_new - phi) < 1e-13:
            phi = phi_new
            break
        phi = phi_new
    lam = theta / _N + _LON0_L93
    return math.degrees(lam), math.degrees(phi)


def wgs84_to_lamb93(lon, lat):
    """(lon, lat) degres -> Lambert 93 (m). Utilise pour fabriquer les fenetres."""
    phi = math.radians(lat)
    lam = math.radians(lon)
    r = _C * _t(phi) ** _N
    theta = _N * (lam - _LON0_L93)
    return _FE + r * math.sin(theta), _YS - r * math.cos(theta)


# --- Repere local (equirectangulaire autour du Capitole, NORD = -Y) -------------
def wgs84_to_local(lon, lat):
    return (lon - LON0) * M_PER_LON, (LAT0 - lat) * M_PER_LAT


def local_to_wgs84(x, y):
    return LON0 + x / M_PER_LON, LAT0 - y / M_PER_LAT


def lamb93_to_local(x, y):
    lon, lat = lamb93_to_wgs84(x, y)
    return wgs84_to_local(lon, lat)


def local_to_lamb93(x, y):
    lon, lat = local_to_wgs84(x, y)
    return wgs84_to_lamb93(lon, lat)


def local_bbox_to_lamb93(minx, miny, maxx, maxy, marge_m=50.0):
    """Fenetre locale -> bbox Lambert 93 ENGLOBANTE (les 4 coins + marge).

    Le meridien de convergence fait tourner le repere de ~1 deg a Toulouse :
    on prend l'enveloppe des 4 coins reprojetes, jamais 2 coins opposes.
    """
    pts = [local_to_lamb93(minx - marge_m, miny - marge_m),
           local_to_lamb93(maxx + marge_m, miny - marge_m),
           local_to_lamb93(minx - marge_m, maxy + marge_m),
           local_to_lamb93(maxx + marge_m, maxy + marge_m)]
    xs = [p[0] for p in pts]
    ys = [p[1] for p in pts]
    return min(xs), min(ys), max(xs), max(ys)


# --- GPKG : decodage des geometries en stdlib ----------------------------------
_ENV_SIZE = {0: 0, 1: 32, 2: 48, 3: 48, 4: 64}
Z_ABSENT = -1000.0            # sentinelle BD TOPO « altitude non renseignee »


def gpkg_lines(blob):
    """Blob geometrie GPKG -> liste de polylignes [(x, y, z|None), ...].

    Gere LINESTRING et MULTILINESTRING, 2D / Z / M / ZM, little et big endian.
    Retourne [] si la geometrie est vide, nulle ou d'un autre type.
    """
    if blob is None or len(blob) < 8 or blob[:2] != b"GP":
        return []
    flags = blob[3]
    env = _ENV_SIZE.get((flags >> 1) & 0x07)
    if env is None:
        return []
    if flags & 0x10:                              # bit « geometrie vide »
        return []
    wkb = blob[8 + env:]
    return _wkb_lines(wkb, 0)[0]


def _wkb_lines(wkb, off):
    order = "<" if wkb[off] == 1 else ">"
    gtype = struct.unpack(order + "I", wkb[off + 1:off + 5])[0]
    base = gtype % 1000
    hi = gtype // 1000
    dims = 2 + (1 if hi in (1, 3) else 0) + (1 if hi in (2, 3) else 0)
    has_z = hi in (1, 3)
    off += 5
    if base == 2:                                  # LINESTRING
        n = struct.unpack(order + "I", wkb[off:off + 4])[0]
        off += 4
        pts = []
        for _ in range(n):
            if has_z:
                x, y, z = struct.unpack(order + "ddd", wkb[off:off + 24])
                pts.append((x, y, None if z <= Z_ABSENT + 1e-6 else z))
            else:
                x, y = struct.unpack(order + "dd", wkb[off:off + 16])
                pts.append((x, y, None))
            off += 8 * dims
        return ([pts] if len(pts) >= 2 else []), off
    if base == 5:                                  # MULTILINESTRING
        n = struct.unpack(order + "I", wkb[off:off + 4])[0]
        off += 4
        out = []
        for _ in range(n):
            sub, off = _wkb_lines(wkb, off)
            out.extend(sub)
        return out, off
    return [], off


# --- Grille de cellules --------------------------------------------------------
def cell_of(x, y, cell_m):
    return int(math.floor(x / cell_m)), int(math.floor(y / cell_m))


def cell_box(cx, cy, cell_m):
    return cx * cell_m, cy * cell_m, (cx + 1) * cell_m, (cy + 1) * cell_m


def noeud_key(x, y):
    """Cle de noeud QUANTIFIEE AU DECIMETRE — meme convention que le C++
    (FJunctionMap::Key, CityImportTools.cpp l. 1230-1233). C'est elle qui donne
    l'identite d'un noeud, y compris de part et d'autre d'une frontiere de
    cellule : le recollage entre cellules voisines est une egalite de cles."""
    return int(round(x * 10.0)), int(round(y * 10.0))


def _selftest():
    ok = True

    def chk(nom, got, want, tol):
        nonlocal ok
        d = abs(got - want)
        s = "OK " if d <= tol else "KO "
        if d > tol:
            ok = False
        print("  %s %-34s got=%.6f want=%.6f (ecart %.3e)" % (s, nom, got, want, d))

    # 1. Constantes officielles IGN de la projection Lambert 93 (NT/G 71)
    chk("n", _N, 0.7256077650, 1e-9)
    chk("C", _C, 11754255.426, 1e-2)
    chk("Ys", _YS, 12655612.050, 1e-2)

    # 2. Aller-retour Lambert 93 sur un semis couvrant le departement
    dmax = 0.0
    for gx in range(480000, 640000, 20000):
        for gy in range(6180000, 6330000, 20000):
            lon, lat = lamb93_to_wgs84(gx, gy)
            bx, by = wgs84_to_lamb93(lon, lat)
            dmax = max(dmax, math.hypot(bx - gx, by - gy))
    chk("aller-retour L93 max (m)", dmax, 0.0, 1e-6)

    # 3. Aller-retour repere local
    dmax = 0.0
    for lx in (-5000.0, 0.0, 5000.0):
        for ly in (-5000.0, 0.0, 5000.0):
            gx, gy = local_to_lamb93(lx, ly)
            bx, by = lamb93_to_local(gx, gy)
            dmax = max(dmax, math.hypot(bx - lx, by - ly))
    chk("aller-retour local max (m)", dmax, 0.0, 1e-6)

    # 4. L'origine locale doit retomber sur le Capitole en Lambert 93
    ox, oy = wgs84_to_lamb93(LON0, LAT0)
    print("  -- origine locale (0,0) = Lambert 93 (%.1f, %.1f)" % (ox, oy))

    # 5. Decodage WKB : LINESTRING Z fabriquee a la main
    wkb = struct.pack("<BI I", 1, 1002, 2) + struct.pack("<6d", 1, 2, 3, 4, 5, -1000.0)
    blob = b"GP" + bytes([0, 0x01]) + struct.pack("<i", 2154) + wkb
    lines = gpkg_lines(blob)
    print("  %s WKB LINESTRING Z -> %r" % ("OK " if lines == [[(1.0, 2.0, 3.0), (4.0, 5.0, None)]] else "KO ", lines))
    if lines != [[(1.0, 2.0, 3.0), (4.0, 5.0, None)]]:
        ok = False

    # 6. Grille
    chk("cell_of(-1, -1)", cell_of(-1.0, -1.0, 500.0)[0], -1, 0)
    chk("cell_of(499, 0)", cell_of(499.0, 0.0, 500.0)[0], 0, 0)
    chk("cell_of(500, 0)", cell_of(500.0, 0.0, 500.0)[0], 1, 0)
    print("SELFTEST geo_local :", "PASS" if ok else "ECHEC")
    return 0 if ok else 1


if __name__ == "__main__":
    import sys
    sys.exit(_selftest())
