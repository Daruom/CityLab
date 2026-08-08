# -*- coding: utf-8 -*-
"""mq_lib.py — le SOCLE de la maquette blanche : lecture du contrat, evaluation
des lois de Z, triangulation, empaquetage binaire.

Ne DECIDE rien du plan : il ne fait qu'EXECUTER ce que le contrat declare.
  * loi `constante`       -> z = z_m
  * loi `profil_troncon`  -> z = profil(s) le long de `axe` (abscisse projetee)
  * loi `drapage`         -> z = le lecteur declare par le plan
                             (niveaux.json::lecteur = work/BERGES/b_lib.py::SolRendu)
ASCII pur. Interpreteur : C:\\LidarPoC\\venv\\Scripts\\python.exe
"""
import base64
import hashlib
import io
import json
import os
import re
import sys
import time

import numpy as np
import shapely
from shapely.geometry import Polygon

PLAN = r"C:\LidarPoC\work\PLAN"
V1 = os.path.join(PLAN, "plan_ville", "v1")
DATA = os.path.join(V1, "data")
MQ = os.path.join(PLAN, "maquette")

# ---- PARAMETRES DECLARES DE LA MAQUETTE (ce que le contrat ne porte PAS) -----
# Chacun est un aveu : la maquette doit poser une valeur pour construire, le
# contrat devra la porter. Aucun n'est une regle du plan.
#
# L'EPAISSEUR DE TABLIER N'EN FAIT PLUS PARTIE : le contrat final porte
# `epaisseur_tablier_m` et `cote_intrados_m` sur les 56 ponts (regle
# `epaisseur_tablier` du registre, portee/20 bornee a [0,40 ; 2,50] m). La
# maquette ne declare donc plus aucune epaisseur.
PARAMS = {
    "MARCHE_H_MAX_M": 0.16,
    "MARCHE_GIRON_MIN_M": 0.28,
    "MARCHE_BLONDEL_M": [0.60, 0.65],
    "MARCHE_note": "registre::geometrie_marche (arrete du 15/01/2007 + Blondel)",
    "JUPE_OUVRAGE_MIN_M": 0.30,
    "VEG_TRONC_M": [0.18, 2.2],
    "VEG_note": "volume ultra-simple : prisme + bloc de houppier, aucune espece",
}

# les 20 familles peuplees + les familles vides, teintes plates de maquette
# `dalot` est la famille NEUVE du contrat final (regle `ouvrage_affleurant`) :
# 62 ouvrages affleurants que le contrat separe des 56 vrais ponts.
FAMILLES = [
    "chaussee", "trottoir", "carrefour", "voie_ferree", "canal", "piste_aero",
    "rond_point", "echangeur", "pont", "dalot", "escalier", "gradins", "mur_sout",
    "ouvrage_hydro", "aqueduc", "edicule", "tremie", "sol_mineral",
    "sol_vegetal", "eau_surface", "parking", "terrain_sport", "batiment",
    "terrassement", "terrain_naturel", "semis", "breakline", "sous_sol",
]
FAM_ID = {f: i for i, f in enumerate(FAMILLES)}

CATALOGUE = ["rien", "affleurement", "bordure", "emmarchement", "mur", "talus",
             "arbitrage_demande"]
CAT_ID = {c: i for i, c in enumerate(CATALOGUE)}


def jalon(msg, fichier="progress.log"):
    ligne = "JALON: %s | %s" % (time.strftime("%H:%M:%S"), msg)
    with io.open(os.path.join(MQ, fichier), "a", encoding="utf-8") as f:
        f.write(ligne + "\n")
    try:
        print(ligne, flush=True)
    except UnicodeEncodeError:
        print(ligne.encode("ascii", "replace").decode("ascii"), flush=True)


def chrono(poste, secondes, detail=""):
    with io.open(os.path.join(MQ, "chronos.log"), "a", encoding="utf-8") as f:
        f.write("%s | %-28s %8.2f s | %s\n"
                % (time.strftime("%H:%M:%S"), poste, secondes, detail))


def jload(p):
    with io.open(p, encoding="utf-8") as f:
        return json.load(f)


def b64(a):
    """Un tableau numpy -> base64 de ses octets bruts (little endian)."""
    return base64.b64encode(np.ascontiguousarray(a).tobytes()).decode("ascii")


# ============================================================== LE REGISTRE ===
class Registre(object):
    """LE REGISTRE DES REGLES, lu DANS LE CONTRAT : `data/registre.json`.

    C'est la reponse au trou signale a la passe precedente (les gabarits
    etaient alors lus dans la page L1a, HORS contrat). Le contrat final le
    porte, et son empreinte est dans `plan_index.json::fichiers` : on la
    VERIFIE avant de lire quoi que ce soit. Une conformite se rejoue donc
    depuis le seul contrat.

    Aucun seuil n'est recopie dans ce fichier : ils sont EXTRAITS de l'enonce
    des regles. Une classe absente de l'enonce reste absente — on ne devine
    jamais un gabarit.
    """

    FICHIER = "registre.json"

    def __init__(self):
        p = os.path.join(DATA, self.FICHIER)
        if not os.path.isfile(p):
            raise AssertionError(
                "REGISTRE ABSENT DU CONTRAT : %s. Le contrat doit porter son "
                "registre, sinon une mesure de conformite ne peut pas etre "
                "refaite depuis le seul contrat." % p)
        octets = io.open(p, "rb").read()
        self.chemin = p
        self.md5 = hashlib.md5(octets).hexdigest()
        idx = jload(os.path.join(DATA, "plan_index.json"))
        att = (idx.get("fichiers") or {}).get(self.FICHIER) or {}
        self.md5_attendu = att.get("md5_octets")
        if not self.md5_attendu:
            raise AssertionError(
                "le registre n'a pas d'empreinte dans plan_index.json::"
                "fichiers : il n'est pas scelle par le contrat")
        if self.md5_attendu != self.md5:
            raise AssertionError(
                "EMPREINTE DU REGISTRE FAUSSE : disque %s, contrat %s"
                % (self.md5, self.md5_attendu))
        self.d = json.loads(octets.decode("utf-8"))
        self.regles = {r["cle"]: r for r in self.d.get("regles") or []}
        n = ((idx.get("registre") or {}).get("regles"))
        self.compte_annonce = n
        self.compte_reel = len(self.regles)
        if n is not None and int(n) != self.compte_reel:
            raise AssertionError(
                "le contrat annonce %s regles, le registre en porte %d"
                % (n, self.compte_reel))

    def regle(self, cle):
        r = self.regles.get(cle)
        if r is None:
            raise AssertionError(
                "le registre du contrat ne porte pas de regle `%s`" % cle)
        return r

    @staticmethod
    def _nb(s):
        return float(s.replace(",", "."))

    def gabarits(self):
        """Les hauteurs libres normalisees, EXTRAITES de l'enonce de la regle
        `hauteur_libre`. Rien n'est recopie : ce que l'enonce ne dit pas
        n'existe pas."""
        r = self.regle("hauteur_libre")
        txt = r["enonce"]
        motifs = {
            "route": r"route\D{0,14}?(\d+[,.]\d+)\s*m",
            "fer": r"voie ferree[^.;]{0,32}?(\d+[,.]\d+)\s*m",
            "eau": r"voie d'eau\D{0,14}?(\d+[,.]\d+)\s*m",
            "pieton": r"pieton\D{0,14}?(\d+[,.]\d+)\s*m",
        }
        seuils = {}
        for cle, mo in motifs.items():
            m = re.search(mo, txt)
            if m:
                seuils[cle] = self._nb(m.group(1))
        if not seuils:
            raise AssertionError(
                "aucun gabarit extrait de l'enonce de `hauteur_libre` : "
                "l'enonce a change de forme, l'extraction doit etre revue "
                "(on ne devine pas un seuil)")
        return {"seuils_m": seuils, "enonce": txt,
                "reference": r.get("reference"),
                "provenance": r.get("provenance"), "mesure": r.get("mesure"),
                "invariant": r.get("invariant"),
                "source": "CONTRAT plan_ville/v1/data/registre.json "
                          "(md5 %s, scelle dans plan_index.json)" % self.md5}

    def seuil_affleurant(self):
        """Le seuil de la regle `ouvrage_affleurant` : sous cette hauteur
        declaree, un ouvrage de franchissement est un dalot/buse/ponceau, il
        ne porte AUCUNE exigence de hauteur libre."""
        r = self.regle("ouvrage_affleurant")
        m = re.search(r"\((\d+[,.]\d+)\s*m\)", r["enonce"])
        if not m:
            raise AssertionError(
                "seuil de `ouvrage_affleurant` introuvable dans son enonce")
        return {"seuil_m": self._nb(m.group(1)), "enonce": r["enonce"],
                "invariant": r.get("invariant"),
                "reference": r.get("reference"),
                "provenance": r.get("provenance")}

    def marche(self):
        """Les bornes de `geometrie_marche`, extraites de son enonce."""
        r = self.regle("geometrie_marche")
        t = r["enonce"]
        h = re.search(r"hauteur\s*<=\s*(\d+[,.]\d+)\s*m", t)
        g = re.search(r"giron\s*>=\s*(\d+[,.]\d+)\s*m", t)
        b = re.search(r"entre\s*(\d+[,.]\d+)\s*et\s*(\d+[,.]\d+)\s*m", t)
        if not (h and g and b):
            return None
        return {"h_max_m": self._nb(h.group(1)),
                "giron_min_m": self._nb(g.group(1)),
                "blondel_m": [self._nb(b.group(1)), self._nb(b.group(2))],
                "enonce": t, "reference": r.get("reference")}


# =============================================================== LE CONTRAT ===
class Contrat(object):
    """Le plan, lu une fois. LECTURE SEULE.

    REGLE DE PROVENANCE (arbitrage coordinateur 08/08) : chaque champ est
    cherche DANS LE CONTRAT d'abord. L'instantane `mq_snapshot.json` n'est
    qu'un REPLI de transition, et chaque recours est COMPTE (`self.repli`).
    En mode `strict=True` (`--contrat-seul`) tout repli est refuse : la
    maquette ne se construit alors que si le contrat est COMPLET. C'est le
    critere de completude du contrat lui-meme.
    """

    # Les motifs de saut qui sont des REPONSES DU CONTRAT et non des pertes :
    # le contrat a dit quelque chose, et ce quelque chose est « pas de piece
    # ici ». Tout autre motif est une PERTE et fait echouer --contrat-seul.
    SAUT_RIEN = "interface resolue a `rien`"
    SAUT_SANS_PIECE = ("contact vers une parcelle que le contrat declare SANS "
                       "PIECE EMISE")
    SAUT_EMM_PLAT = ("emmarchement dont LE CONTRAT LUI-MEME declare un "
                     "denivele nul")
    SAUTS_JUSTIFIES = {
        SAUT_RIEN:
            "resolution `rien` du catalogue : le contrat declare qu'AUCUNE "
            "piece ne materialise ce contact. Rien n'est perdu.",
        "cote de vegetation prise au lecteur de drapage":
            "semis hors de toute parcelle : la cote vient du lecteur de "
            "drapage QUE LE CONTRAT DESIGNE (niveaux.json::lecteur). "
            "L'instance est bien construite.",
        SAUT_EMM_PLAT:
            "le contrat resout ce contact en `emmarchement` mais porte lui-"
            "meme `dz_m = 0` : la piece a construire serait une volee de "
            "hauteur nulle. Rien n'est perdu (l'escalier, lui, est bati comme "
            "volume d'ouvrage) — mais c'est une BIZARRERIE DE CONTRAT, "
            "signalee au coordinateur. La justification n'est accordee que si "
            "le dz DECLARE est nul : si le contrat annonce un denivele que je "
            "ne retrouve pas, c'est MA lecture qui est en faute et le saut "
            "redevient une perte.",
        SAUT_SANS_PIECE:
            "l'autre cote du contact est une parcelle du plan qui n'a EMIS "
            "AUCUNE PIECE : il n'existe aucune geometrie a raccorder. LE "
            "CONTRAT LE DECLARE DESORMAIS LUI-MEME, frontiere par frontiere, "
            "par le champ `vers_parcelle_non_emise` : ce n'est plus une "
            "justification que je construis, c'est une LECTURE. La "
            "comptabilite des juges (du_plan = emises + sans_piece) est "
            "conservee en SECOND RIDEAU et fait tomber la justification si "
            "le compte deborde.",
    }

    def __init__(self, avec_snapshot=True, strict=False):
        t = time.time()
        self.strict = bool(strict)
        self.repli = {}
        self.du_contrat = {}
        # --- LE REGISTRE DES SAUTS (exigence : zero saut silencieux) --------
        # Toute piece que le lecteur n'a PAS construite, quelle qu'en soit la
        # raison, atterrit ici avec son identifiant. Un saut jamais declare
        # avait deja ete paye une fois (bnd/1333#0, batiment sans hauteur que
        # le verrou ne comptait pas).
        self.sauts = {}
        self.SAUT_IDS_MAX = 40
        self.sans_piece_vus = set()   # les parcelles absentes reellement vues
        self.sans_piece_declares = 0  # ... et dont le contrat le DIT
        self.sans_piece_deduits = 0   # ... que j'ai du deduire moi-meme
        self.plan = jload(os.path.join(V1, "plan.json"))
        self.index = jload(os.path.join(DATA, "plan_index.json"))
        self.juges = jload(os.path.join(V1, "juges.json"))
        self.solveur = jload(os.path.join(V1, "solveur.json"))
        self.niveaux = jload(os.path.join(V1, "niveaux.json"))
        self.matrice = jload(os.path.join(V1, "matrice_mesuree.json"))
        self.cells = sorted(self.index["par_cellule"].keys())
        self.cell_m = float(self.index["domaine"]["cellule_m"])
        # --- instantane des couches que le contrat n'exporte pas encore ------
        self.snap = {}
        p = os.path.join(MQ, "mq_snapshot.json")
        if avec_snapshot and not self.strict and os.path.isfile(p):
            self.snap = jload(p)
        self.fam_snap = self.snap.get("familles", {})
        self.haut_snap = self.snap.get("hauteur_batiment_m", {})
        self.assiettes_snap = self.snap.get("assiettes", {})
        self.terr_snap = self.snap.get("terrassements", [])
        self.secondes_chargement = time.time() - t

    def _repli(self, quoi):
        self.repli[quoi] = self.repli.get(quoi, 0) + 1
        if self.strict:
            raise AssertionError(
                "CONTRAT INCOMPLET : `%s` absent du contrat et mode "
                "--contrat-seul actif (aucun repli autorise)" % quoi)

    def _contrat(self, quoi):
        self.du_contrat[quoi] = self.du_contrat.get(quoi, 0) + 1

    # ------------------------------------------------ LE REGISTRE DES SAUTS --
    def saut(self, motif, ident=None):
        """Declare qu'une piece n'a PAS ete construite. Le motif est le
        libelle exact ; l'identifiant est conserve (les premiers, plus le
        compte total). Aucun saut ne doit exister sans passer par ici."""
        e = self.sauts.get(motif)
        if e is None:
            e = self.sauts[motif] = {
                "n": 0, "ids": [],
                "justifie": motif in self.SAUTS_JUSTIFIES,
                "motif_contrat": self.SAUTS_JUSTIFIES.get(motif)}
        e["n"] += 1
        if ident is not None and len(e["ids"]) < self.SAUT_IDS_MAX:
            e["ids"].append(str(ident))
        return e

    def profil_intrados(self, p):
        """La loi d'INTRADOS EN LONG que le contrat porte desormais sur les
        tabliers en pente (`profil_intrados` = [[s, z], ...], sur la MEME
        abscisse que le profil d'extrados de la loi).

        C'est la reponse du compilateur au defaut que j'avais remonte : un
        `cote_intrados_m` scalaire ne peut pas representer l'intrados d'une
        travee qui monte (l'ecart atteignait 1,60 m). Rendue sous la forme
        d'une loi `profil_troncon`, elle s'evalue avec le meme code que toutes
        les autres lois du plan."""
        pi = p.get("profil_intrados")
        if not pi:
            return None
        axe = (p.get("loi") or {}).get("axe")
        if not axe:
            return None
        self._contrat("profil_intrados")
        return {"forme": "profil_troncon", "axe": axe, "profil": pi}

    def verifier_sans_piece(self):
        """La justification `SANS PIECE EMISE` n'est pas un mot : elle se
        VERIFIE contre la comptabilite du contrat. Le contrat declare N
        parcelles du plan sans piece emise ; si j'en rencontre plus que N, ou
        si la comptabilite n'est pas fermee, la justification TOMBE et les
        sauts redeviennent des pertes."""
        j = (self.index.get("juges") or {})
        n_dec = j.get("parcelles_sans_piece")
        ferme = bool(j.get("comptabilite_fermee"))
        somme_ok = (j.get("parcelles_du_plan") ==
                    (j.get("parcelles_emises") or 0) + (n_dec or 0))
        vus = len(self.sans_piece_vus)
        ok = bool(ferme and somme_ok and n_dec is not None and vus <= n_dec)
        v = {
            "declare_par_le_contrat": self.sans_piece_declares,
            "NON declare (deduit par moi)": self.sans_piece_deduits,
            "frontieres_marquees_au_contrat":
                self.index.get("frontieres_vers_parcelle_non_emise"),
            "parcelles_du_plan": j.get("parcelles_du_plan"),
            "parcelles_emises": j.get("parcelles_emises"),
            "parcelles_sans_piece_declarees": n_dec,
            "comptabilite_fermee": ferme,
            "somme_verifiee": somme_ok,
            "parcelles_absentes_rencontrees": vus,
            "dans_le_compte_declare": ok,
            "justification_tenue": ok,
        }
        e = self.sauts.get(self.SAUT_SANS_PIECE)
        if e is not None:
            e["justifie"] = ok
            e["verification"] = v
            if not ok:
                e["motif_contrat"] = (
                    "JUSTIFICATION TOMBEE : %d parcelles absentes rencontrees "
                    "pour %s declarees sans piece (comptabilite fermee=%s)"
                    % (vus, n_dec, ferme))
        return v

    def sauts_perdus(self):
        """Les sauts qui font PERDRE une piece (les seuls qui comptent pour le
        verrou). Un saut justifie n'en est pas un : le contrat a repondu."""
        self.verifier_sans_piece()
        return {k: v for k, v in self.sauts.items() if not v["justifie"]}

    def bilan_sauts(self):
        p = self.sauts_perdus()
        return {
            "verification_sans_piece": self.verifier_sans_piece(),
            "pieces_sautees_total": sum(v["n"] for v in self.sauts.values()),
            "pieces_perdues": sum(v["n"] for v in p.values()),
            "motifs_de_perte": len(p),
            "zero_saut_silencieux": True,
            "detail": {k: {"n": v["n"], "justifie": v["justifie"],
                           "motif_contrat": v["motif_contrat"],
                           "ids_premiers": v["ids"],
                           "ids_tronques": v["n"] > len(v["ids"])}
                       for k, v in sorted(self.sauts.items())},
        }

    def qui(self, c):
        return jload(os.path.join(DATA, "plan_qui_%s.json" % c))

    def itf(self, c):
        return jload(os.path.join(DATA, "plan_interfaces_%s.json" % c))

    def semis(self, c):
        return jload(os.path.join(DATA, "plan_semis_%s.json" % c))

    def terrassements(self, c, qui=None, itf=None):
        """La table des terrassements de la cellule. Le contrat re-exporte la
        portera (par cellule) ; d'ici la, l'instantane filtre par cellule."""
        for src in (qui, itf):
            if src and src.get("terrassements") is not None:
                self._contrat("terrassements")
                return src["terrassements"]
        p = os.path.join(DATA, "plan_terrassements_%s.json" % c)
        if os.path.isfile(p):
            self._contrat("terrassements")
            d = jload(p)
            return d.get("terrassements", d) if isinstance(d, dict) else d
        self._repli("terrassements")
        ci, cj = [int(v) for v in c.split("_")]
        ox, oy = ci * self.cell_m, cj * self.cell_m
        return [t for t in self.terr_snap
                if ox <= t["x"] < ox + self.cell_m
                and oy <= t["y"] < oy + self.cell_m]

    def assiettes(self, c):
        """La table des assiettes de la cellule (contrat), indexee par parcelle."""
        p = os.path.join(DATA, "plan_assiettes_%s.json" % c)
        if os.path.isfile(p):
            self._contrat("assiettes")
            d = jload(p)
            t = d.get("assiettes", d) if isinstance(d, dict) else d
            return {a["parcelle"]: a for a in t}
        self._repli("assiettes")
        return dict(self.assiettes_snap)

    def cote_intrados(self, p):
        """La cote d'INTRADOS declaree par le contrat (regle `hauteur_libre`).

        Le contrat distingue DEUX situations, et les deux sont des REPONSES :
          * `cote_intrados_m` : l'ouvrage porte un tablier ; sa cote d'intrados
            est declaree, avec epaisseur, portee et provenance ;
          * `intrados: "sans_objet"` + `intrados_motif` : l'ouvrage ne porte
            PAS de tablier (mur, escalier, edicule...). Absence MOTIVEE, donc
            reponse du contrat — surtout pas un manque.
        Un ouvrage muet sur les deux serait, lui, un vrai trou de contrat."""
        z = p.get("cote_intrados_m")
        if z is None:
            z = (p.get("loi") or {}).get("cote_intrados_m")
        if z is not None:
            self._contrat("cote_intrados_m")
            return float(z), "contrat"
        if p.get("intrados") == "sans_objet":
            self._contrat("intrados_sans_objet")
            return None, "sans_objet"
        self._repli("cote_intrados_m")
        return None, "aucune declaration d'intrados"

    # -- la famille de matrice : du CONTRAT s'il la porte, sinon instantane ---
    def famille(self, p):
        f = p.get("famille")
        if f:
            self._contrat("famille")
            return f
        pid = p["id"]
        f = self.fam_snap.get(pid)
        if f:
            self._repli("famille")
            return f
        self._repli("famille_deduite")
        # dernier recours, deductible du contrat seul (grossier, trace)
        prop = p["proprietaire"]
        mat = p["matiere"]
        if prop == "batiment":
            return "batiment"
        if prop == "voie_ferree":
            return "voie_ferree"
        if prop == "canal":
            return "canal"
        if prop == "ouvrage":
            return "mur_sout"
        if prop == "voirie":
            return "trottoir" if p.get("bande") else "chaussee"
        if prop == "surface_reglee":
            return "parking"
        if mat == "eau":
            return "eau_surface"
        if prop == "zone":
            return "sol_vegetal" if mat == "vegetal" else "sol_mineral"
        return "terrain_naturel" if mat == "vegetal" else "sol_mineral"

    def hauteur_batiment(self, p, par_cellule=None):
        h = p.get("hauteur_m")
        if h is None:
            h = (p.get("loi") or {}).get("hauteur_m")
        if h is None and par_cellule:
            # une bande annexee a un batiment herite de sa loi : donc de sa
            # hauteur. L'heritage est declare PAR LE CONTRAT.
            src = (p.get("loi") or {}).get("loi_heritee_de")
            if src:
                h = par_cellule.get(src)
        if h is not None:
            self._contrat("hauteur_m")
            return float(h)
        # les bandes annexees a un batiment (bnd/...) heritent de sa loi :
        # elles heritent donc aussi de sa hauteur
        for cle in (p["id"].split("#")[0],
                    str((p.get("loi") or {}).get("loi_heritee_de") or "")
                    .split("#")[0]):
            h = self.haut_snap.get(cle)
            if h is not None:
                self._repli("hauteur_m")
                return float(h)
        return None


# ========================================================= L'EVALUATION DU Z ==
class ZEval(object):
    """z(x, y) pour une parcelle, STRICTEMENT selon la loi que le contrat lui
    donne. Aucune interpolation inventee."""

    def __init__(self, contrat):
        self.lois = {}          # id nu -> loi
        self.haut = {}          # id nu -> hauteur_m (index GLOBAL : une bande
        #                         annexee peut heriter d'un batiment d'une
        #                         AUTRE cellule)
        self.sol = None
        t = time.time()
        for c in contrat.cells:
            for p in contrat.qui(c)["parcelles"]:
                pid = p["id"]
                if pid not in self.lois:
                    self.lois[pid] = p["loi"]
                if p.get("hauteur_m") is not None:
                    self.haut[pid] = p["hauteur_m"]
        self.secondes_index = time.time() - t

    def _sol(self):
        if self.sol is None:
            sys.path.insert(0, r"C:\LidarPoC\work\BERGES")
            sys.path.insert(0, r"C:\LidarPoC\work\DISCONT")
            import b_lib
            t = time.time()
            self.sol = b_lib.SolRendu()
            chrono("lecteur SolRendu", time.time() - t,
                   "%dx%d px (niveaux.json::lecteur)" % (self.sol.W, self.sol.H))
        return self.sol

    def z_loi(self, loi, x, y):
        x = np.asarray(x, dtype=np.float64)
        y = np.asarray(y, dtype=np.float64)
        f = loi.get("forme")
        if f == "constante":
            zv = loi.get("z_m")
            if zv is None:
                return None
            return np.full(x.shape, float(zv), dtype=np.float64)
        if f == "profil_troncon":
            return self._z_profil(loi, x, y)
        if f == "drapage":
            return self._sol().z(x, y).astype(np.float64)
        return None

    def z(self, pid, x, y):
        loi = self.lois.get(pid.split("#")[0]) or self.lois.get(pid)
        if loi is None:
            return None
        return self.z_loi(loi, x, y)

    @staticmethod
    def _z_profil(loi, x, y):
        """Abscisse curviligne projetee sur `axe`, puis `profil` = [[s, z], ...]."""
        axe = loi.get("axe")
        pro = loi.get("profil")
        if not axe or not pro or len(axe) < 2:
            if pro:
                return np.full(x.shape, float(pro[0][1]), dtype=np.float64)
            return None
        A = np.asarray(axe, dtype=np.float64)
        seg0 = A[:-1]
        seg1 = A[1:]
        d = seg1 - seg0
        L = np.hypot(d[:, 0], d[:, 1])
        L[L < 1e-9] = 1e-9
        cum = np.concatenate([[0.0], np.cumsum(L)])
        px = x.reshape(-1, 1) - seg0[:, 0].reshape(1, -1)
        py = y.reshape(-1, 1) - seg0[:, 1].reshape(1, -1)
        t = (px * d[:, 0].reshape(1, -1) + py * d[:, 1].reshape(1, -1)) \
            / (L ** 2).reshape(1, -1)
        t = np.clip(t, 0.0, 1.0)
        cx = seg0[:, 0].reshape(1, -1) + t * d[:, 0].reshape(1, -1)
        cy = seg0[:, 1].reshape(1, -1) + t * d[:, 1].reshape(1, -1)
        dist = (x.reshape(-1, 1) - cx) ** 2 + (y.reshape(-1, 1) - cy) ** 2
        k = np.argmin(dist, axis=1)
        s = cum[k] + t[np.arange(len(k)), k] * L[k]
        P = np.asarray(pro, dtype=np.float64)
        return np.interp(s, P[:, 0], P[:, 1])


# ============================================================ LA GEOMETRIE ====
def polygone(anneaux):
    """`anneaux` du contrat (ext puis trous, parts concatenees) -> polygone."""
    if not anneaux:
        return None
    if len(anneaux) == 1:
        g = Polygon(anneaux[0])
        return g if g.is_valid else g.buffer(0)
    parts = [Polygon(r) for r in anneaux]
    aires = np.array([abs(p.area) for p in parts])
    ordre = np.argsort(-aires)
    ext, holes = [], []
    for i in ordre:
        pi = parts[i]
        if pi.area <= 0:
            continue
        dedans = False
        for j in ext:
            if parts[j].contains(pi.representative_point()):
                dedans = True
                break
        if dedans:
            holes.append(i)
        else:
            ext.append(i)
    gs = []
    for i in ext:
        trous = [anneaux[j] for j in holes
                 if parts[i].contains(parts[j].representative_point())]
        try:
            g = Polygon(anneaux[i], trous)
        except Exception:
            g = Polygon(anneaux[i])
        gs.append(g if g.is_valid else g.buffer(0))
    if not gs:
        return None
    g = gs[0] if len(gs) == 1 else shapely.union_all(gs)
    return g if g.is_valid else g.buffer(0)


def trianguler(g):
    """Polygone -> (sommets xy uniques, triangles). Triangulation de Delaunay
    CONTRAINTE (GEOS) : aucun sommet ajoute, les trous sont respectes."""
    if g is None or g.is_empty or g.area <= 0:
        return None, None
    try:
        t = shapely.constrained_delaunay_triangles(g)
    except Exception:
        return None, None
    co = shapely.get_coordinates(t)
    if len(co) < 4 or len(co) % 4 != 0:
        return None, None
    tri = co.reshape(-1, 4, 2)[:, :3, :]
    pts = tri.reshape(-1, 2)
    key = np.round(pts * 1000.0).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True,
                              return_inverse=True)
    verts = pts[np.sort(first)]
    # np.unique renvoie les indices dans l'ordre trie : on remappe
    ordre = np.argsort(np.sort(first))
    remap = np.empty(len(first), dtype=np.int64)
    remap[np.argsort(first)] = np.arange(len(first))
    idx = remap[inv.reshape(-1)].reshape(-1, 3)
    return verts, idx.astype(np.int64)


VMAX = 65000          # sommets par troncon : les indices tiennent en Uint16


class Tampon(object):
    """Accumulateur de maillage, decoupe en TRONCONS de moins de 65 000
    sommets pour que les indices tiennent en Uint16. Les positions sont
    quantifiees au centimetre dans la boite du troncon (Uint16). La famille et
    l'identifiant vivent PAR PLAGE, jamais par sommet."""

    def __init__(self, table):
        self.table = table          # table de chaines partagee de la cellule
        self.chunks = []
        self._P, self._I, self._H = [], [], []
        self._n = 0                 # sommets du troncon courant
        self._t = 0                 # triangles du troncon courant
        self._plages = []           # [idx_id, tri_debut, tri_nb, fam]
        self.nt = 0
        self.n = 0

    def _cle(self, s):
        i = self.table.get(s)
        if i is None:
            i = len(self.table)
            self.table[s] = i
        return i

    def _fermer(self):
        if not self._I:
            return
        P = np.concatenate(self._P)
        I = np.concatenate(self._I).astype(np.uint16)
        lo = P.min(axis=0)
        hi = P.max(axis=0)
        span = np.maximum(hi - lo, 1e-6)
        Q = np.round((P - lo) / span * 65535.0).astype(np.uint16)
        pl = np.asarray(self._plages, dtype=np.int64)
        self.chunks.append({
            "q": [float(v) for v in lo] + [float(v) for v in span],
            "p": b64(Q), "i": b64(I),
            "nv": int(len(P)), "nt": int(len(I) // 3),
            "rid": b64(pl[:, 0].astype(np.uint32)),
            "rs": b64(pl[:, 1].astype(np.uint32)),
            "rc": b64(pl[:, 2].astype(np.uint32)),
            "rf": b64(pl[:, 3].astype(np.uint8)),
            "hv": b64(np.concatenate(self._H).astype(np.uint8)),
        })
        self._P, self._I, self._plages, self._H = [], [], [], []
        self._n = 0
        self._t = 0

    def ajouter(self, xyz, tris, fam_id, ident, haut=None):
        """`haut` : 1 pour les sommets de l'ARETE HAUTE d'une face d'interface.
        Il ne sert qu'a la carte des marches, ou la face est dressee en ailette
        pour devenir visible depuis le ciel."""
        if xyz is None or tris is None or len(tris) == 0:
            return
        xyz = np.asarray(xyz, dtype=np.float64)
        if self._n and self._n + len(xyz) > VMAX:
            self._fermer()
        if haut is None:
            haut = np.zeros(len(xyz), dtype=np.uint8)
        self._H.append(np.asarray(haut, dtype=np.uint8))
        self._P.append(xyz)
        self._I.append((np.asarray(tris, dtype=np.int64) + self._n).ravel())
        self._plages.append([self._cle(ident), self._t, int(len(tris)),
                             int(fam_id)])
        self._n += len(xyz)
        self._t += len(tris)
        self.n += len(xyz)
        self.nt += len(tris)

    def vide(self):
        return self.nt == 0

    def paquet(self, extra=None):
        self._fermer()
        if not self.chunks:
            return None
        d = {"k": self.chunks, "nv": int(self.n), "nt": int(self.nt)}
        if extra:
            d.update(extra)
        return d


def quad_strip(xy, za, zb):
    """Bande verticale le long d'une polyligne, entre deux nappes de cote.
    Renvoie (sommets, triangles) — deux faces par segment, double face."""
    n = len(xy)
    if n < 2:
        return None, None
    V = np.empty((2 * n, 3), dtype=np.float64)
    V[0::2, 0] = xy[:, 0]
    V[0::2, 1] = xy[:, 1]
    V[0::2, 2] = za
    V[1::2, 0] = xy[:, 0]
    V[1::2, 1] = xy[:, 1]
    V[1::2, 2] = zb
    k = np.arange(n - 1)
    a = 2 * k
    b = 2 * k + 1
    c = 2 * k + 2
    d = 2 * k + 3
    T = np.concatenate([np.stack([a, b, d], 1), np.stack([a, d, c], 1)])
    return V, T


def prisme(anneau_xy, z_bas, z_haut):
    """Volume extrude : murs + toit plat. `anneau_xy` ferme ou non.

    `z_bas` et `z_haut` acceptent un NOMBRE ou une FONCTION f(xy) -> z par
    sommet. La forme fonction sert aux tabliers en pente, dont le contrat
    porte desormais l'intrados en long (`profil_intrados`) : leur dessous
    suit la travee au lieu d'etre un plan."""
    r = np.asarray(anneau_xy, dtype=np.float64)
    if len(r) > 1 and abs(r[0, 0] - r[-1, 0]) < 1e-9 and abs(r[0, 1] - r[-1, 1]) < 1e-9:
        r = r[:-1]
    n = len(r)
    if n < 3:
        return None, None
    zb = z_bas(r) if callable(z_bas) else z_bas
    zh = z_haut(r) if callable(z_haut) else z_haut
    if zb is None or zh is None:
        return None, None
    V = np.empty((2 * n, 3), dtype=np.float64)
    V[:n, :2] = r
    V[:n, 2] = zb
    V[n:, :2] = r
    V[n:, 2] = zh
    k = np.arange(n)
    k2 = (k + 1) % n
    T = np.concatenate([
        np.stack([k, k2, n + k2], 1),
        np.stack([k, n + k2, n + k], 1),
    ])
    g = Polygon(r)
    if not g.is_valid:
        g = g.buffer(0)
    vt, it = trianguler(g)
    if vt is not None and len(it):
        base = len(V)
        Vt = np.empty((len(vt), 3), dtype=np.float64)
        Vt[:, :2] = vt
        Vt[:, 2] = z_haut(vt) if callable(z_haut) else z_haut
        V = np.concatenate([V, Vt])
        T = np.concatenate([T, it + base])
        # le DESSOUS aussi, quand il est gauche (tablier en pente) : sans lui
        # on verrait a travers l'ouvrage par en dessous
        if callable(z_bas):
            base = len(V)
            Vb = np.empty((len(vt), 3), dtype=np.float64)
            Vb[:, :2] = vt
            Vb[:, 2] = z_bas(vt)
            V = np.concatenate([V, Vb])
            T = np.concatenate([T, it[:, ::-1] + base])
    return V, T
