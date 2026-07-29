# j3f_prep_facade.py -- ETAPE 1 du chantier « J3f, forme finale ».
#
# Enrichit SourceData/toulouse10_bati.json (le JSON batiment de PRODUCTION, deja
# rogne + teinte + toits + cours) avec 3 champs COURTS destines a la future pose de
# materiaux de facade (le travail materiaux est fait par un AUTRE agent -- ici on ne
# fait QUE de la prep de donnee) :
#
#   matfam : famille de materiau des murs {brique,pierre,enduit,beton,autre}
#   mon    : true si le batiment est un monument (nature IGN), sinon champ ABSENT
#   uc     : classe d'usage {resid,comm,annexe,indus,autre}
#
# SOURCE d'enrichissement : SourceData/GrandFetch/bati_enrichi.json (BD TOPO IGN,
# 131357 bat. dans le MEME ordre et la MEME origine que toulouse10_bati.json).
# L'appariement se fait par INDEX apres verification du centroide d'emprise
# (garde-fou : si le centroide diverge de > MAX_CENTROID_M, on n'enrichit pas ce
# batiment et on prend le fallback -- protege d'un eventuel decalage d'ordre).
#
# ------------------------------------------------------------------------------
# MAPPING matfam  <-  materiaux_des_murs (code IGN BD TOPO, issu de la variable
# DGFiP/CEREMA « dmatgm » -- materiau des gros murs ; cf. bdtopoexplorer.ign.fr et
# doc-datafoncier.cerema.fr). Le code est sur 2 caracteres ; le chiffre des DIZAINES
# = materiau PRINCIPAL, celui des UNITES = materiau secondaire :
#     0 = INDETERMINE   1 = PIERRE      2 = MEULIERE   3 = BETON
#     4 = BRIQUES       5 = AGGLOMERE   6 = BOIS        9 = AUTRES
# On retient la famille du chiffre PRINCIPAL ; s'il est 0 (indetermine) on retombe
# sur le secondaire ; si les deux sont indetermines -> "enduit" (defaut Toulouse).
#   pierre  <- 1 (pierre) et 2 (meuliere : c'est une pierre)
#   beton   <- 3 (beton banche)
#   brique  <- 4 (Toulouse « la ville rose », materiau dominant local)
#   enduit  <- 5 (agglomere/parpaing : TOUJOURS enduit en facade en pratique) ET
#              defaut quand le champ est absent / indetermine (majorite Toulouse)
#   autre   <- 6 (bois) et 9 (autres)
# NB : choix documente et facile a basculer -- l'agglomere (5) pourrait rejoindre
# "beton" si on veut la famille STRUCTURELLE plutot que l'aspect de facade.
# ------------------------------------------------------------------------------
# MAPPING uc  <-  usage_1 (BD TOPO). resid/comm/annexe/indus/autre.
# MAPPING mon <-  nature (BD TOPO) : Eglise, Chapelle, Chateau, Tour+donjon, Tribune.
# ------------------------------------------------------------------------------
import json
import os
import sys
import unicodedata

sys.stdout.reconfigure(encoding="utf-8", errors="replace")

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "SourceData")
BATI_PATH = os.path.join(SRC, "toulouse10_bati.json")
ENRI_PATH = os.path.join(SRC, "GrandFetch", "bati_enrichi.json")
BAK_PATH = os.path.join(SRC, "toulouse10_bati.avant_j3f.json")

MAX_CENTROID_M = 25.0  # au-dela : on suppose un desappariement -> fallback


# --- serialisation CANONIQUE, identique a j3b_ajoute_cours.py (round-trip verifie
#     octet-a-octet sur les champs preserves) --------------------------------------
def dumps_bati(root):
    return ('{"source":%s,"origin":%s,"sizeM":%s,"buildings":[%s]}') % (
        json.dumps(root["source"], ensure_ascii=False),
        json.dumps(root.get("origin"), separators=(",", ":")),
        json.dumps(root.get("sizeM"), separators=(",", ":")),
        ",".join(json.dumps(b, separators=(",", ":"), ensure_ascii=False)
                 for b in root["buildings"]))


def norm(s):
    """minuscule sans accents (contre le mojibake / variantes d'encodage)."""
    if s is None:
        return ""
    s = unicodedata.normalize("NFKD", str(s))
    s = "".join(c for c in s if not unicodedata.combining(c))
    return s.strip().lower()


def centroid(pts):
    n = len(pts)
    return (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n)


# --- matfam depuis le code dmatgm ------------------------------------------------
DIGIT_FAM = {
    "0": None,        # indetermine
    "1": "pierre",
    "2": "pierre",    # meuliere = pierre
    "3": "beton",
    "4": "brique",
    "5": "enduit",    # agglomere/parpaing -> enduit (aspect facade)
    "6": "autre",     # bois
    "7": "autre",     # (hors nomenclature) -> autre par prudence
    "8": "autre",     # (hors nomenclature) -> autre par prudence
    "9": "autre",     # autres
}


def matfam_from_code(code):
    """Renvoie la famille, ou None si le champ est absent (=> fallback enduit)."""
    if code is None:
        return None
    c = str(code).strip()
    if len(c) == 1:
        c = "0" + c
    if len(c) != 2 or not c.isdigit():
        return "autre"
    principal = DIGIT_FAM.get(c[0])
    if principal is not None:
        return principal
    secondaire = DIGIT_FAM.get(c[1])       # principal indetermine -> secondaire
    if secondaire is not None:
        return secondaire
    return "enduit"                        # 00 : totalement indetermine


# --- uc depuis usage_1 -----------------------------------------------------------
USAGE_UC = {
    "residentiel": "resid",
    "commercial et services": "comm",
    "annexe": "annexe",
    "industriel": "indus",
    "agricole": "indus",       # batiment agricole = fonctionnel/production
    "sportif": "autre",
    "religieux": "autre",
    "indifferencie": "autre",
}

# --- mon depuis nature -----------------------------------------------------------
MONUMENT_NAT = {"eglise", "chapelle", "chateau", "tribune"}  # + "tour, donjon" gere a part


def is_monument(nature):
    nn = norm(nature)
    if nn in MONUMENT_NAT:
        return True
    if "tour" in nn and "donjon" in nn:   # "Tour, donjon"
        return True
    return False


def main():
    with open(BATI_PATH, encoding="utf-8") as f:
        root = json.load(f)
    with open(ENRI_PATH, encoding="utf-8") as f:
        enri = json.load(f)
    B = root["buildings"]
    E = enri["buildings"]
    print("batiments prod  : %d" % len(B))
    print("batiments enrichi: %d" % len(E))
    if len(B) != len(E):
        print("ECHEC : nombre de batiments different -> appariement par index impossible")
        sys.exit(1)

    # --- garde-fou : le serialiseur reproduit-il le fichier ACTUEL a l'octet pres ?
    with open(BATI_PATH, "rb") as f:
        raw = f.read()
    if dumps_bati(root).encode("utf-8") == raw:
        print("round-trip serialiseur = fichier actuel OCTET POUR OCTET : OK")
    else:
        print("round-trip serialiseur != fichier actuel : ATTENTION (formatage non canonique)")

    # --- sauvegarde AVANT j3f (une seule fois, jamais ecrasee) -------------------
    if not os.path.exists(BAK_PATH):
        with open(BAK_PATH, "wb") as f:
            f.write(raw)
        print("sauvegarde -> %s" % os.path.basename(BAK_PATH))
    else:
        print("sauvegarde deja presente (conservee) : %s" % os.path.basename(BAK_PATH))

    # --- empreinte des champs existants (pour le self-test round-trip) ----------
    RESERVED = ("matfam", "mon", "uc")
    before = [dict(b) for b in B]  # copie superficielle des champs d'origine

    # --- passe d'enrichissement -------------------------------------------------
    from collections import Counter
    fam_c, uc_c = Counter(), Counter()
    n_mon = n_far = n_mat_present = n_mat_absent = 0
    max_d = 0.0

    for i, b in enumerate(B):
        e = E[i]
        # garde-fou centroide
        ca = centroid(b["pts"])
        cb = centroid(e["pts"])
        d = ((ca[0] - cb[0]) ** 2 + (ca[1] - cb[1]) ** 2) ** 0.5
        max_d = max(max_d, d)
        matched = d <= MAX_CENTROID_M
        if not matched:
            n_far += 1

        # matfam
        code = e.get("materiaux_des_murs") if matched else None
        if matched and code is not None:
            n_mat_present += 1
        else:
            n_mat_absent += 1
        fam = matfam_from_code(code)
        if fam is None:
            fam = "enduit"          # champ absent -> defaut Toulouse
        b["matfam"] = fam
        fam_c[fam] += 1

        # uc
        uc = USAGE_UC.get(norm(e.get("usage_1")) if matched else "", "autre")
        b["uc"] = uc
        uc_c[uc] += 1

        # mon (champ present UNIQUEMENT si monument)
        if matched and is_monument(e.get("nature")):
            b["mon"] = True
            n_mon += 1

    # --- ecriture ---------------------------------------------------------------
    out = dumps_bati(root)
    with open(BATI_PATH, "w", encoding="utf-8") as f:
        f.write(out)

    # --- SELF-TEST --------------------------------------------------------------
    print("\n=== SELF-TEST ===")
    ok = True
    # 1) aucun batiment perdu / ordre conserve
    with open(BATI_PATH, encoding="utf-8") as f:
        reread = json.load(f)["buildings"]
    if len(reread) != len(before):
        print("  [KO] nombre de batiments change apres ecriture"); ok = False
    else:
        print("  [OK] %d batiments (aucun perdu)" % len(reread))
    # 2) champs d'origine preserves a l'identique (round-trip valeur par valeur)
    mism = 0
    for orig, now in zip(before, reread):
        for k, v in orig.items():
            if k in RESERVED:
                continue
            if now.get(k) != v:
                mism += 1
                if mism <= 5:
                    print("      champ modifie #%s : %r %r->%r" % (k, orig.get("pts", [""])[0], v, now.get(k)))
    if mism == 0:
        print("  [OK] tous les champs d'origine preserves (0 divergence)")
    else:
        print("  [KO] %d champ(s) d'origine diverge(nt)" % mism); ok = False
    # 3) les 3 nouveaux champs sont bien poses
    miss_fam = sum(1 for b in reread if "matfam" not in b)
    miss_uc = sum(1 for b in reread if "uc" not in b)
    n_mon_out = sum(1 for b in reread if b.get("mon") is True)
    print("  [%s] matfam sur tous : %d manquant(s)" % ("OK" if miss_fam == 0 else "KO", miss_fam))
    print("  [%s] uc sur tous     : %d manquant(s)" % ("OK" if miss_uc == 0 else "KO", miss_uc))
    print("  [%s] mon relu = mon pose : %d / %d" % ("OK" if n_mon_out == n_mon else "KO", n_mon_out, n_mon))
    if miss_fam or miss_uc or n_mon_out != n_mon:
        ok = False

    # --- COMPTES ----------------------------------------------------------------
    print("\n=== COMPTES ===")
    print("materiaux_des_murs present (apparie) : %d / %d (absent/fallback : %d)"
          % (n_mat_present, len(B), n_mat_absent))
    print("matfam :")
    for k in ("brique", "pierre", "enduit", "beton", "autre"):
        print("    %-7s : %6d (%.1f%%)" % (k, fam_c[k], 100 * fam_c[k] / len(B)))
    print("uc :")
    for k in ("resid", "comm", "annexe", "indus", "autre"):
        print("    %-7s : %6d (%.1f%%)" % (k, uc_c[k], 100 * uc_c[k] / len(B)))
    print("monuments (mon=true) : %d" % n_mon)
    print("centroides > %.0f m (fallback, non apparies) : %d ; distance max = %.2f m"
          % (MAX_CENTROID_M, n_far, max_d))

    print("\n%s" % ("=== ETAPE 1 : PASS ===" if ok else "=== ETAPE 1 : ECHEC SELF-TEST ==="))
    sys.exit(0 if ok else 2)


if __name__ == "__main__":
    main()
