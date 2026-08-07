# -*- coding: utf-8 -*-
"""PLAN DE VILLE / etage 1 — LE SOCLE COMMUN DU COMPILATEUR.

Ce module ne DECIDE rien : il ne porte que des instruments de LECTURE et de
PREUVE, tous repris tels quels de lots anterieurs (doctrine du chantier :
« on reutilise les FAITS et les instruments, jamais les DECISIONS »).

  * `SolRendu`      : le Z du sol RENDU, parite moteur (bilineaire
                      FTerrainSampler::AltCmAt + enveloppe SUPERIEURE des deux
                      triangulations du quad, FRenderedGroundZ::At). IMPORTE
                      de `work\\BERGES\\b_lib.py`, non modifie, non recopie.
  * `lire_png_rgba` : decodeur PNG 8 bits RGBA (masques de sol) — recopie
                      litterale de `work\\BLOC\\m1_masque.py` (PIL absent).
  * `ecrire_png`    : ecriture PNG RGB — recopie de `work\\PART\\p3_cartes.py`.
  * `wgs84`         : la georeference du MNT (equirectangulaire locale), la
                      SEULE conversion vers le monde du visualiseur.
  * `jalon` / `chrono` / `md5_double` : la supervision et les empreintes.

ASCII pur. Interpreteur : C:\\LidarPoC\\venv\\Scripts\\python.exe
"""
import hashlib
import io
import json
import os
import struct
import sys
import time
import zlib

import numpy as np

PLAN = r"C:\LidarPoC\work\PLAN"
OUT = os.path.join(PLAN, "plan_ville", "v1")
CACHE = os.path.join(PLAN, "cache")
SRC = r"C:\Users\User\Documents\Unreal Projects\CityLab\SourceData"
ENT = r"C:\LidarPoC\work\FINITION_SOL\entrees_3x3"
BLOC = r"C:\LidarPoC\work\BLOC"
PART = r"C:\LidarPoC\work\PART"

for d in (OUT, CACHE):
    if not os.path.isdir(d):
        os.makedirs(d)

sys.path.insert(0, r"C:\LidarPoC\work\BERGES")
sys.path.insert(0, r"C:\LidarPoC\work\DISCONT")

CELL_M = 500.0
PROPS = ["ouvrage", "voirie", "batiment", "zone", "organique"]

_T0 = time.time()


def jalon(msg):
    ligne = "JALON: %s | %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    with io.open(os.path.join(PLAN, "progress.log"), "a", encoding="utf-8") as f:
        f.write(ligne + "\n")
    try:
        print(ligne, flush=True)
    except UnicodeEncodeError:
        print(ligne.encode("ascii", "replace").decode("ascii"), flush=True)


def bloque(msg):
    ligne = "BLOQUE: %s | %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    with io.open(os.path.join(PLAN, "progress.log"), "a", encoding="utf-8") as f:
        f.write(ligne + "\n")
    print(ligne, flush=True)


def chrono(etape, secondes, detail=""):
    ligne = "%s | %-24s | %8.1f s | %s" % (time.strftime("%H:%M:%S"), etape,
                                           secondes, detail)
    with io.open(os.path.join(PLAN, "chronos.log"), "a", encoding="utf-8") as f:
        f.write(ligne + "\n")
    print("CHRONO " + ligne, flush=True)


# ================================================================ EMPREINTES ==
def md5_octets(path):
    h = hashlib.md5()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def md5_logique(texte):
    """Empreinte du CONTENU LOGIQUE : fins de ligne normalisees en LF, BOM
    retire. C'est la garde contre le piege CRLF paye au lot partition."""
    if texte.startswith(u"\ufeff"):
        texte = texte[1:]
    return hashlib.md5(texte.replace("\r\n", "\n").replace("\r", "\n")
                       .encode("utf-8")).hexdigest()


def ecrire_json(path, obj):
    """Ecriture deterministe (cle triee, LF) + double empreinte."""
    s = json.dumps(obj, indent=1, sort_keys=True, ensure_ascii=False)
    with io.open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(s)
    return {"fichier": os.path.basename(path),
            "octets": os.path.getsize(path),
            "md5_octets": md5_octets(path),
            "md5_logique": md5_logique(s)}


# =============================================================== IMAGES PNG ===
def ecrire_png(path, rgb):
    """PNG RGB 8 bits (recopie de work/PART/p3_cartes.py)."""
    h, w, _ = rgb.shape
    brut = b"".join(b"\x00" + rgb[y].tobytes() for y in range(h))

    def blk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

    png = (b"\x89PNG\r\n\x1a\n"
           + blk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
           + blk(b"IDAT", zlib.compress(brut, 6)) + blk(b"IEND", b""))
    open(path, "wb").write(png)


def lire_png_rgba(path):
    """Decodeur PNG minimal 8 bits RGBA non entrelace (recopie de
    work/BLOC/m1_masque.py, avec les chemins rapides numpy pour les filtres
    None/Up/Sub : les masques font 1024x1024x4 et le lot en lit 36)."""
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "pas un PNG"
    i, idat, w, h, bd, ct = 8, [], 0, 0, 0, 0
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        if typ == b"IHDR":
            w, h, bd, ct = struct.unpack(">IIBB", data[i + 8:i + 18])
        elif typ == b"IDAT":
            idat.append(data[i + 8:i + 8 + ln])
        elif typ == b"IEND":
            break
        i += 12 + ln
    assert bd == 8 and ct == 6, "attendu 8 bits RGBA (bd=%d ct=%d)" % (bd, ct)
    raw = zlib.decompress(b"".join(idat))
    bpp, stride = 4, w * 4
    arr = np.frombuffer(raw, dtype=np.uint8).reshape(h, stride + 1)
    filt = arr[:, 0]
    rows = arr[:, 1:].astype(np.int32)
    out = np.zeros((h, stride), dtype=np.uint8)
    prev = np.zeros(stride, dtype=np.int32)
    for y in range(h):
        f = int(filt[y])
        line = rows[y]
        if f == 0:
            cur = line.copy()
        elif f == 1:                      # Sub : cumul par canal, vectorise
            cur = line.reshape(-1, bpp)
            cur = np.cumsum(cur, axis=0, dtype=np.int64) & 255
            cur = cur.reshape(stride).astype(np.int32)
        elif f == 2:                      # Up
            cur = (line + prev) & 255
        elif f == 3:
            cur = np.zeros(stride, dtype=np.int32)
            for x in range(stride):
                a = cur[x - bpp] if x >= bpp else 0
                cur[x] = (line[x] + ((a + prev[x]) >> 1)) & 255
        elif f == 4:
            cur = np.zeros(stride, dtype=np.int32)
            for x in range(stride):
                a = int(cur[x - bpp]) if x >= bpp else 0
                b = int(prev[x])
                c = int(prev[x - bpp]) if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[x] = (line[x] + pr) & 255
        else:
            raise AssertionError("filtre %d inconnu" % f)
        out[y] = cur.astype(np.uint8)
        prev = cur
    return out.reshape(h, w, 4)


# ========================================================== GEOREFERENCEMENT ==
_MNT_META = None


def mnt_meta():
    global _MNT_META
    if _MNT_META is None:
        with io.open(os.path.join(SRC, "toulouse10_mnt.json"),
                     encoding="utf-8-sig") as f:
            _MNT_META = json.load(f)
    return _MNT_META


def wgs84(x_m, y_m):
    """Repere local (x est, y SUD, metres) -> (lon, lat) WGS84, par la
    projection equirectangulaire locale declaree dans toulouse10_mnt.json."""
    m = mnt_meta()
    o = m["origin_wgs84"]
    p = m["projection_locale"]
    lon = o["lon0"] + np.asarray(x_m, dtype=np.float64) / p["m_per_deg_lon"]
    lat = o["lat0"] - np.asarray(y_m, dtype=np.float64) / p["m_per_deg_lat"]
    return lon, lat


def alt_capitole_m():
    return float(mnt_meta()["stats"]["alt_capitole_m"])


# ================================================================== LE SOL ====
_SOL = None


def sol_rendu():
    """Le sol RENDU, parite moteur. IMPORTE de work/BERGES/b_lib.py."""
    global _SOL
    if _SOL is None:
        import b_lib
        t = time.time()
        _SOL = b_lib.SolRendu()
        jalon("SOCLE/SolRendu charge depuis work/BERGES/b_lib.py "
              "(MNT %dx%d px, %.1f s) — bilineaire + enveloppe superieure des "
              "deux triangulations, quad %.4f m"
              % (_SOL.W, _SOL.H, time.time() - t, _SOL.step))
    return _SOL


# ================================================================ LE DOMAINE ==
def cellules_domaine():
    """LE DOMAINE : la liste des cellules de sol reellement generees, lue sur
    le disque (meme regle que work/PART/p0_carte.py::cellules_3x3)."""
    d = os.path.join(BLOC, "mesh_vis")
    out = []
    for f in os.listdir(d):
        if not (f.startswith("SM_Ground_") and f.endswith(".bin")):
            continue
        n = f[len("SM_Ground_"):-4]
        try:
            cx, cy = [int(v) for v in n.split("_")]
        except Exception:
            continue
        out.append((cx, cy))
    return sorted(set(out))


def charge_json(path):
    with io.open(path, encoding="utf-8-sig") as f:
        return json.load(f)


# ======================================================== LE SEMIS BRUT =======
# ⚠️ Lecteur UNIQUE du semis brut, partage par les juges et par l'export.
# Defaut paye : une premiere version lisait le fichier par tranches de 16 Mo en
# reportant 200 octets d'une tranche a la suivante — les instances a cheval
# etaient comptees DEUX fois (1 222 434 annoncees au lieu de 1 222 412). Ici le
# fichier est lu d'un bloc et le motif est applique une seule fois.
import re as _re                                                    # noqa: E402

_RX_SEMIS = _re.compile(
    rb'\{"mesh":\s*"([^"]+)",\s*"x":\s*(-?[\d.eE+]+),\s*"y":\s*(-?[\d.eE+]+),'
    rb'\s*"scale":\s*(-?[\d.eE+]+),\s*"yaw":\s*(-?[\d.eE+]+),'
    rb'\s*"kind":\s*"([^"]+)"\}')


def lit_semis_brut(chemin=None):
    """Rend (enregistrements, X, Y). Un enregistrement = le tuple d'octets
    (mesh, x, y, scale, yaw, kind) tel qu'il est ecrit dans le fichier."""
    p = chemin or os.path.join(r"C:\LidarPoC\work\FINITION_SOL\entrees_3x3",
                               "veg_3x3.json")
    raw = open(p, "rb").read()
    rec = _RX_SEMIS.findall(raw)
    n_obj = len(_re.findall(rb'\{[^{}]*\}', raw))
    if len(rec) != n_obj:
        rec = []
        for m in _re.finditer(rb'\{[^{}]*\}', raw):
            d = json.loads(m.group(0).decode("utf-8"))
            rec.append((d["mesh"].encode(), str(d["x"]).encode(),
                        str(d["y"]).encode(), str(d["scale"]).encode(),
                        str(d["yaw"]).encode(), d["kind"].encode()))
    X = np.array([float(r[1]) for r in rec])
    Y = np.array([float(r[2]) for r in rec])
    return rec, X, Y
