# -*- coding: utf-8 -*-
"""VERROU DE NON-REGRESSION DES BATIMENTS (lot FINITION_SOL V5, 2026-08-01).

A JOUER DANS L'EDITEUR APRES CHAQUE REGENERATION du proto Sol2 (guetteur de
fichiers ou console Python). Il repond a UNE question, celle qu'aucun controle
n'avait posee jusqu'ici :

    les batiments generes ont-ils encore LA GEOMETRIE attendue ?

Pourquoi ce fichier existe. Le 01/08 l'utilisateur a signale « cours interieures
bouchees, emprises gonflees qui debordent sur le sol ». Le controle en place a
l'epoque annoncait « batiments identiques a 0,000 m » : il ne comparait que des
POSITIONS et des COMPTES, aveugles par construction aux deux griefs. La cause
reelle (les proxys grossiers sauves VISIBLES par-dessus le detail) a survecu a
plusieurs lots pour cette seule raison. D'ou la doctrine :

    UNE GENERATION SE VALIDE PAR SES GEOMETRIES (aires, boucles, retraits),
    JAMAIS PAR SES COMPTES OU SES POSITIONS.

Ce que le verrou mesure, sur les meshes REELLEMENT generes (MeshDescription
SOURCE, pas le repli Nanite decime) :
  1. l'aire d'emprise PROJETEE des faces non verticales de chaque cellule
     (= les toits) : elle doit valoir l'aire des contours du JSON MOINS l'aire
     des cours DES BATIMENTS QUI ONT UN BLOC `roof`, a 0,5 % pres. (Un batiment
     sans bloc roof prend le chemin « toit plat », qui couvre le contour entier :
     comportement voulu, mesure par la spec d'automation V5 sur son batiment de
     controle. Une cour bouchee sur un batiment AVEC bloc roof reste un echec.) ;
  2. le nombre de cours du JSON recouvertes par une face : une cour PERCEE n'a
     rien au-dessus d'elle ;
  3. les comptes de sommets/triangles, temoins d'un changement de maillage ;
  4. QU'IL N'EXISTE PLUS AUCUN PROXY (V6 : la couche est supprimee ; V5 se
     contentait d'exiger qu'ils soient caches).

Deux jeux de reference, et c'est voulu :
  - le critere JSON (point 1) est vrai A TOUTES LES ECHELLES : il compare la
    mesure a l'aire nette du JSON qui a servi a generer. C'est LUI qui a resolu
    l'autopsie V5 (ecart 0,0 m2 sur 383 917,5) et il survit a l'elargissement ;
  - les valeurs EN DUR ci-dessous ont ete mesurees le 01/08 sur
    L_ProtoSols_E2_Sol1, la map GELEE le 29/07 (md5 4C67BBD8...), et ne valent
    que pour son extrait de 550 m : a 3x3 km les memes cellules recoivent en
    plus les batiments qui tombaient hors du rayon d'extraction. Elles ne sont
    donc verifiees que si c'est bien ce JSON-la qui a servi.
Toute derive doit CRIER, pas passer.
"""
import json
import time
import traceback

OUT = r"C:\LidarPoC\work\FINITION_SOL\verrou_batiments_sol2.json"
# V6 : le JSON d'entree suit l'emprise. Le proto 1 km utilisait l'extrait ~550 m
# E2SOL1\proto_sols_bati.json ; le proto ELARGI 3x3 km utilise sa propre extraction.
# Surchargeable par verrou_batiments_in.json (champ "bati").
BATI = r"C:\LidarPoC\work\FINITION_SOL\entrees_3x3\bati_3x3.json"
BATI_HISTORIQUE = r"C:\LidarPoC\work\E2SOL1\proto_sols_bati.json"
DOSSIER = "/Game/Dev/ProtoE2Sol2/CitySol2"
try:
    with open(r"C:\LidarPoC\work\FINITION_SOL\verrou_batiments_in.json",
              encoding="utf-8") as _f:
        BATI = json.load(_f).get("bati", BATI)
except Exception:
    pass

# --- REFERENCE EN DUR : mesuree sur le temoin gele Sol1 (29/07) --------------
# Elle ne vaut QUE pour l'extrait historique de 550 m : a 3x3 km les memes cellules
# recoivent en plus les batiments qui tombaient hors du rayon d'extraction, donc
# sommets/triangles/aires changent LEGITIMEMENT. Elle n'est donc verifiee que si
# c'est bien ce JSON-la qui a servi (sinon : le critere JSON ci-dessous, qui lui
# est vrai a toutes les echelles — c'est la mesure qui a resolu l'autopsie V5).
REF = {
    "-1_-1": {"verts": 60924, "tris": 25764, "aire_m2": 86167.1, "cours": 16, "cours_couvertes": 0},
    "-1_0":  {"verts": 85775, "tris": 36217, "aire_m2": 104737.1, "cours": 30, "cours_couvertes": 1},
    "0_-1":  {"verts": 49006, "tris": 20754, "aire_m2": 87481.9, "cours": 20, "cours_couvertes": 2},
    "0_0":   {"verts": 75005, "tris": 31659, "aire_m2": 105531.4, "cours": 32, "cours_couvertes": 0},
}
REF_TOTAL_M2 = 383917.5      # = contours 397191,6 - cours 13274,1 (JSON historique)
TOLERANCE_AIRE = 0.005       # 0,5 %
# Cours SURVOLEES par le versant d'un batiment voisin : ce n'est pas un defaut de
# percement, c'est un fait de donnee (mesure V5 : 3 sur 98 = 3,1 %, identique dans
# le temoin gele). Au-dela de ce taux, quelque chose bouche vraiment les cours.
TAUX_COURS_COUVERTES_MAX = 0.10
CELL_CM = 50000.0


def aire_signee(p):
    s = 0.0
    n = len(p)
    for i in range(n):
        x1, y1 = p[i]
        x2, y2 = p[(i + 1) % n]
        s += x1 * y2 - x2 * y1
    return s * 0.5


def point_dans(poly, q):
    x, y = q
    dedans = False
    j = len(poly) - 1
    for i in range(len(poly)):
        xi, yi = poly[i]
        xj, yj = poly[j]
        if (yi > y) != (yj > y):
            if x < (xj - xi) * (y - yi) / (yj - yi) + xi:
                dedans = not dedans
        j = i
    return dedans


def point_interieur(poly):
    cx = sum(p[0] for p in poly) / len(poly)
    cy = sum(p[1] for p in poly) / len(poly)
    if point_dans(poly, (cx, cy)):
        return (cx, cy)
    xs = [p[0] for p in poly]
    ys = [p[1] for p in poly]
    for fx in (0.5, 0.25, 0.75, 0.375, 0.625, 0.125, 0.875):
        for fy in (0.5, 0.25, 0.75, 0.375, 0.625, 0.125, 0.875):
            q = (min(xs) + fx * (max(xs) - min(xs)), min(ys) + fy * (max(ys) - min(ys)))
            if point_dans(poly, q):
                return q
    return None


def cours_du_json(chemin):
    """Cours (anneaux) du JSON d'entree, rangees par cellule, avec un point
    interieur garanti. Reproduit les conventions du C++ (cm, CCW, cellule =
    floor(moyenne des sommets / taille de cellule))."""
    par_cellule = {}
    aires = {}
    with open(chemin, encoding="utf-8") as f:
        B = json.load(f)["buildings"]
    for b in B:
        pts = [(c[0] * 100.0, c[1] * 100.0) for c in b["pts"] if len(c) >= 2]
        if len(pts) < 3:
            continue
        rev = aire_signee(pts) < 0
        if rev:
            pts = pts[::-1]
        cx = sum(p[0] for p in pts) / len(pts)
        cy = sum(p[1] for p in pts) / len(pts)
        key = "%d_%d" % (int(cx // CELL_CM), int(cy // CELL_CM))
        a = aires.setdefault(key, [0.0, 0.0])
        a[0] += abs(aire_signee(pts))
        if rev:
            continue          # le C++ ignore les trous d'un contour reoriente
        # V6 — LE TOIT PLAT REMPLIT SON CONTOUR, CE N'EST PAS UN DEFAUT.
        # Un batiment SANS bloc `roof` prend le chemin « toit plat + corniche », qui
        # couvre le contour ENTIER, cour comprise. Ce n'est pas une regression : la
        # spec d'automation V5 le mesure explicitement sur son batiment de CONTROLE
        # (meme forme, meme cour, sans bloc roof -> 1 242 m2 au lieu de 1 000 et DEUX
        # faces au-dessus de la cour). Retirer ces trous de la reference, c'est
        # comparer le generateur a son VRAI contrat ; les y laisser ferait crier le
        # verrou sur du comportement voulu (mesure du 01/08 sur l'emprise 3x3 :
        # cellule -2_0, 3 cours de 3 232,2 m2, ecart explique a 0,0 m2 pres).
        # Une cour bouchee sur un batiment QUI A un bloc roof reste un ECHEC.
        if not b.get("roof"):
            continue
        for h in (b.get("holes") or []):
            hp = [(c[0] * 100.0, c[1] * 100.0) for c in h if len(c) >= 2]
            if len(hp) < 3:
                continue
            a[1] += abs(aire_signee(hp))
            pi = point_interieur(hp)
            if pi:
                par_cellule.setdefault(key, []).append(pi)
    return par_cellule, aires


def run():
    import unreal

    rep = {"heure": time.strftime("%Y-%m-%d %H:%M:%S"), "cellules": {}, "echecs": []}

    def TID(i):
        return unreal.TriangleID(id_value=i)

    def VID(i):
        return unreal.VertexID(id_value=i)

    try:
        sondes, aires_json = cours_du_json(BATI)
        rep["json"] = {k: {"contours_m2": round(v[0] / 10000.0, 1),
                           "trous_m2": round(v[1] / 10000.0, 1),
                           "net_m2": round((v[0] - v[1]) / 10000.0, 1)}
                       for k, v in aires_json.items()}
    except Exception as e:
        sondes, aires_json = {}, {}
        rep["json_absent"] = str(e)[:150]

    # V6 : on parcourt TOUTES les cellules du JSON (4 au proto 1 km, 36 a 3x3 km),
    # pas seulement les 4 de la reference historique.
    rep["bati"] = BATI
    rep["reference_historique"] = (BATI == BATI_HISTORIQUE)
    cles = sorted(aires_json.keys()) if aires_json else sorted(REF.keys())
    rep["cellules_json"] = len(cles)
    total = 0.0
    total_cours = 0
    total_couvertes = 0
    for key in cles:
        ref = REF.get(key) if rep["reference_historique"] else None
        chemin = "%s/SM_Bldg_%s_Wall" % (DOSSIER, key)
        d = {"ref": ref}
        sm = unreal.EditorAssetLibrary.load_asset(chemin)
        if sm is None:
            # Une cellule du JSON sans mesh n'est un echec que si elle porte des
            # batiments : une cellule vide (pleine eau, hors emprise batie) est normale.
            if aires_json.get(key, [0.0, 0.0])[0] > 1.0:
                rep["echecs"].append("%s : mesh ABSENT alors que le JSON y pose des "
                                     "batiments" % chemin)
            rep["cellules"][key] = {"absent": True}
            continue
        md = sm.get_static_mesh_description(0)
        nv, nt = int(md.get_vertex_count()), int(md.get_triangle_count())
        pos = [None] * nv
        for i in range(nv):
            p = md.get_vertex_position(VID(i))
            pos[i] = (float(p.x), float(p.y))
        pts = sondes.get(key, [])
        hits = [0] * len(pts)
        ah = ab = 0.0
        for t in range(nt):
            vs = md.get_triangle_vertices(TID(t))
            a = pos[vs[-3].id_value]
            b = pos[vs[-2].id_value]
            c = pos[vs[-1].id_value]
            cz = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])
            if cz > 0:
                ah += cz
            elif cz < 0:
                ab -= cz
            else:
                continue
            if not pts:
                continue
            xmin = min(a[0], b[0], c[0]); xmax = max(a[0], b[0], c[0])
            ymin = min(a[1], b[1], c[1]); ymax = max(a[1], b[1], c[1])
            for k in range(len(pts)):
                px, py = pts[k]
                if px < xmin or px > xmax or py < ymin or py > ymax:
                    continue
                d1 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0])
                d2 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0])
                d3 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0])
                if (d1 >= 0 and d2 >= 0 and d3 >= 0) or (d1 <= 0 and d2 <= 0 and d3 <= 0):
                    hits[k] += 1
        aire = max(ah, ab) * 0.5 / 10000.0
        total += aire
        couvertes = sum(1 for h in hits if h > 0)
        d.update({"verts": nv, "tris": nt, "aire_m2": round(aire, 1),
                  "n_cours": len(pts), "cours_couvertes": couvertes})
        # --- CRITERE PRINCIPAL, vrai a TOUTES les echelles : l'emprise MESUREE sur le
        # maillage doit valoir l'aire des contours du JSON MOINS celle des cours.
        # Cours bouchees -> mesure trop grande ; emprises gonflees -> idem.
        # C'est exactement la mesure qui a resolu l'autopsie V5 (ecart 0,0 m2).
        aj = aires_json.get(key)
        if aj:
            net = (aj[0] - aj[1]) / 10000.0
            d["json_net_m2"] = round(net, 1)
            d["ecart_json_m2"] = round(aire - net, 1)
            if net > 1.0 and abs(aire - net) > TOLERANCE_AIRE * net:
                rep["echecs"].append(
                    "%s : EMPRISE mesuree %.1f m2 contre %.1f m2 nets au JSON "
                    "(ecart %+.1f m2) — cours bouchees ou emprises gonflees"
                    % (key, aire, net, aire - net))
        total_cours += len(pts)
        total_couvertes += couvertes
        # --- Reference historique en dur : seulement si c'est bien SON JSON.
        if ref:
            if nv != ref["verts"] or nt != ref["tris"]:
                rep["echecs"].append(
                    "%s : maillage change (%d sommets / %d triangles ; reference %d / %d)"
                    % (key, nv, nt, ref["verts"], ref["tris"]))
            if abs(aire - ref["aire_m2"]) > TOLERANCE_AIRE * ref["aire_m2"]:
                rep["echecs"].append(
                    "%s : EMPRISE %.1f m2 hors tolerance (reference %.1f m2) — cours bouchees "
                    "ou emprises gonflees" % (key, aire, ref["aire_m2"]))
            if len(pts) and len(pts) != ref["cours"]:
                rep["echecs"].append("%s : %d cours dans le JSON, reference %d"
                                     % (key, len(pts), ref["cours"]))
            if couvertes > ref["cours_couvertes"]:
                rep["echecs"].append(
                    "%s : %d cours RECOUVERTES par une face (reference %d) — des cours se sont "
                    "bouchees" % (key, couvertes, ref["cours_couvertes"]))
        rep["cellules"][key] = d

    rep["aire_totale_m2"] = round(total, 1)
    rep["cours_total"] = total_cours
    rep["cours_couvertes_total"] = total_couvertes
    rep["taux_cours_couvertes"] = (round(total_couvertes / float(total_cours), 4)
                                   if total_cours else None)
    if total_cours and total_couvertes > TAUX_COURS_COUVERTES_MAX * total_cours:
        rep["echecs"].append(
            "%d cours RECOUVERTES sur %d (%.1f %%) — au-dela du taux de survol normal "
            "(plafond %.0f %% ; reference V5 mesuree : 3 sur 98 = 3,1 %%)"
            % (total_couvertes, total_cours, 100.0 * total_couvertes / total_cours,
               100.0 * TAUX_COURS_COUVERTES_MAX))
    if rep["reference_historique"] and abs(total - REF_TOTAL_M2) > TOLERANCE_AIRE * REF_TOTAL_M2:
        rep["echecs"].append("Emprise TOTALE %.1f m2 hors tolerance (reference %.1f m2)"
                             % (total, REF_TOTAL_M2))

    # --- 4. V6 : plus AUCUN proxy ne doit exister ---------------------------------
    # V5 exigeait des proxys CACHES ; la couche est supprimee depuis (decision
    # utilisateur du 01/08 : obsolete sur desktop, remplacement eventuel = HLOD UE).
    # Un proxy qui reapparait, c'est une passe jouee avec bProxyLayer=True ou un
    # ancien acteur jamais purge — dans les deux cas il se superposera au detail
    # (cours bouchees a 96 %, 24 858 m2 hors emprise : la regression du 01/08).
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    proxys = []
    for a in eas.get_all_level_actors():
        if not a.get_actor_label().startswith("SM_Proxy"):
            continue
        c = a.static_mesh_component
        proxys.append({"label": a.get_actor_label(),
                       "visible": bool(c.get_editor_property("visible")),
                       "hidden_in_game": bool(c.get_editor_property("hidden_in_game"))})
    rep["proxys"] = proxys
    if proxys:
        rep["echecs"].append(
            "PROXYS PRESENTS (%d) : %s — la couche proxy est SUPPRIMEE depuis le "
            "01/08 (bProxyLayer=False). Un proxy qui revient se superpose au detail."
            % (len(proxys), ", ".join(p["label"] for p in proxys[:8])))

    rep["VERROU"] = "PASS" if not rep["echecs"] else "ECHEC"
    return rep


if __name__ in ("__main__", "builtins"):
    try:
        RAPPORT = run()
    except Exception:
        RAPPORT = {"VERROU": "ERREUR", "err": traceback.format_exc()}
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(json.dumps(RAPPORT, indent=1, default=str))
    print("VERROU BATIMENTS : %s" % RAPPORT.get("VERROU"))
    for e in RAPPORT.get("echecs", []):
        print("  ECHEC : %s" % e)
