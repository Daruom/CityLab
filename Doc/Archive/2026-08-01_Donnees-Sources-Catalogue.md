# Catalogue des données sources — CityLab / Survol

> État au **2026-07-29**. Trois colonnes : ce qu'on **utilise déjà** (audité dans les
> scripts `SourceData/Fetch-*.ps1` + `CityImportTools.cpp`), ce qu'on **propose de
> télécharger** pour la France, et **toutes les options** (dont de meilleures/plus précises).
> Tout IGN = **Licence Ouverte/Etalab 2.0** (commercial OK + attribution « IGN »). OSM = ODbL
> (attribution + share-alike sur la donnée dérivée — à surveiller pour un jeu commercial).

## Résumé exécutif (les points qui comptent)

1. **On utilise déjà du HAUTE RÉSOLUTION** : MNT **RGE ALTI 1 m** (drapé sur tout) + ortho **BD ORTHO 20 cm** (teinte). Un socle « 5 m » serait un **downgrade**.
2. **Correctif de stratégie** : on garde la **haute-déf par ville construite** (déjà en main pour Toulouse : 1 m + 20 cm), et on ne rapatrie en **bulk France** que le **socle vecteur** + une **base terrain/teinte allégée** pour les villes *pas encore construites*. → **aucune perte** sur l'existant.
3. **Deux améliorations possibles vs l'existant** : les **routes** (on est sur OSM → BD TOPO IGN serait plus homogène/autoritaire pour la France) et les **arbres** (OSM → **LiDAR HD MNH** = vraies positions/hauteurs de canopée).

---

## Par catégorie

### A. Bâtiments — emprise 2D + hauteur
- **Utilisé** : **BD TOPO IGN** (WFS Géoplateforme `BDTOPO_V3:batiment`), emprise + `hauteur`. 🟢 réel, France entière, autoritaire.
- **Options** :
  - BD TOPO (actuel) — la référence.
  - **BDNB / RNB** (Référentiel National des Bâtiments) — identifiants + attributs (usage, énergie, année) ; **pas de 3D** → sert à *enrichir*, pas à remplacer.
  - **OSM buildings** — couverture variable, parfois `building:levels`, `roof:shape`.
- **Verdict** : garder BD TOPO. Enrichissement possible via RNB (usage/hauteur d'étages).

### B. Forme du toit  ⭐ (le sujet chaud)
- **Utilisé** : **squelette droit** (bpypolyskel, `Tools/j3b_prep_toits.py`) calculé depuis l'emprise. 🔴 **DEVINÉ** (BD TOPO n'a pas la forme des toits) — idéalisé mais colorable + robuste.
- **Options** :
  - Skeleton (actuel).
  - **LiDAR HD + Roofer** (notre PoC, LoD2.2) — 🟢 forme réelle, mais **avale la clutter** + Roofer instable. Réserve « LiDAR-blanc ».
  - **IGN BATI-3D LOD2** — forme réelle propre, **mais démonstrateur bêta non téléchargeable** (généré avec roofer, mêmes limites).
  - **OSM `roof:shape`** — donne l'archétype (gabled/hipped/flat/dome) là où c'est tagué (couverture **éparse**) → pourrait piloter un skeleton *plus intelligent*.
- **Verdict** : E2 = skeleton (socle) ; LiDAR-blanc en réserve. Piste future : skeleton piloté par archétype (OSM ou LiDAR).

### C. Couleur du toit (teinte terracotta)
- **Utilisé** : **BD ORTHO 20 cm** (WMS), médiane par bâtiment. 🟢 (surdimensionné : on n'extrait qu'une couleur moyenne).
- **Options** :
  - BD ORTHO **20 cm** (actuel) — max détail, **réutilisable un jour comme vraie texture**.
  - BD ORTHO **50 cm** — plus léger.
  - **BD ORTHO 5 m** (reformatée GPKG, ~150 Mo/dept, ~15 Go France) — **parfait pour la teinte seule**.
  - **BD ORTHO IRC** (infrarouge) — détection végétation.
- **Verdict** : garder 20 cm **par ville construite** ; pour le **bulk France** (villes futures), le **5 m** suffit à la teinte. Pas de perte sur l'existant.

### D. Terrain / relief (MNT)
- **Utilisé** : **RGE ALTI 1 m** (`ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES`, heightmap 10k×10k, altitude en cm) — **drapé sur bâtiments/routes/surfaces/bordures**. 🟢 haute-déf. (import desktop sans relief = refusé.)
- **Options** :
  - RGE ALTI **1 m** (actuel) — relief fin ; **~200+ Go France** (bulk lourd).
  - RGE ALTI **5 m** — relief général ; **~9,4 Go France**.
  - **LiDAR HD MNT** (≤ 0,5 m possible) — potentiellement plus fin que RGE ALTI ; + **MNS** (surface) et **MNH** (hauteur canopée).
- **Verdict** : garder 1 m **par ville** ; **bulk France en 5 m** comme base, 1 m récupéré à la construction de chaque ville. On garde le 1 m Toulouse déjà en main.

### E. Routes / voirie
- **Utilisé** : **OpenStreetMap** (Overpass, en tuiles). ⚠️ **pas IGN** — ODbL (share-alike).
- **Options** :
  - OSM (actuel) — bonne géométrie mais hétérogène + licence ODbL.
  - **BD TOPO Transport** (IGN) — **autoritaire, France entière, classifié** (autoroute/rue/chemin) + attributs ; **Licence Ouverte** (pas de share-alike). **Probablement meilleur** pour l'homogénéité nationale et la licence.
  - ROUTE 500 / ROUTE 120 — petites échelles (aperçu national).
- **Verdict (amélioration)** : envisager le **passage à BD TOPO Transport** → homogène France + licence plus simple pour un jeu commercial.

### F. Surfaces / occupation du sol (places, trottoirs, sols)
- **Utilisé** : corridor public calculé (parcelles soustraites) + surfaces BD TOPO (J3c).
- **Options** :
  - BD TOPO (surfaces de route, places, zones).
  - **OCS GE** (Occupation du Sol Grande Échelle, IGN) — classification land cover (bâti/végé/eau/route), **déploiement national en cours** → pourrait piloter les **matériaux de sol**.
  - OSM landuse.
- **Verdict** : garder l'approche ; **OCS GE** = piste pour varier les matériaux de sol.

### G. Parcelles (cadastre, pour le rognage)
- **Utilisé** : **PARCELLAIRE EXPRESS (PCI)** (`GrandFetch/parcelles.json`).
- **Options** : PARCELLAIRE EXPRESS (IGN) **ou** **Cadastre Etalab** (même donnée, packaging plus propre/scriptable, ~30 Go France).
- **Verdict** : garder ; Cadastre Etalab plus pratique pour le bulk France.

### H. Noms de lieux / POI / adresses
- **Utilisé** : markers (`toulouse10_markers.json`).
- **Options** :
  - **BD TOPO toponymes / zones d'activité / POI** (IGN).
  - **BAN** (Base Adresse Nationale) — adresses, `adresse.data.gouv.fr`.
  - OSM POI, GéoNames.
- **Verdict** : BD TOPO toponymes + BAN (léger, utile pour « débloquer la France »/cartes postales).

### I. Végétation / arbres
- **Utilisé** : **arbres OSM** (Overpass).
- **Options** :
  - OSM trees (actuel) — positions ponctuelles, éparses.
  - **BD TOPO zones de végétation** (IGN) — surfaces boisées.
  - **⭐ LiDAR HD MNH** (Modèle Numérique de Hauteur) — **vraies hauteurs de canopée** → positions + hauteurs d'arbres **réelles et denses**. Nettement mieux qu'OSM.
  - OCS GE (couche végétation).
- **Verdict (amélioration)** : **LiDAR HD MNH** = grosse amélioration possible sur les arbres (on a déjà les dalles LiDAR Toulouse).

### J. Eau / hydrographie
- **Options** : BD TOPO hydrographie (cours d'eau, plans d'eau), BD Carthage. (À câbler si la Garonne/canaux doivent être traités proprement.)

---

## Plan de téléchargement révisé (sans perte sur l'existant)

**Bulk France maintenant (petit, réutilisable, sans regret)** :
- **BD TOPO France** — thèmes **Bâti + Transport + Surfaces/Occupation + Toponymes** (GPKG par thème, pas les 83 Go complets).
- **Cadastre Etalab France** (~30 Go) — rognage.
- **RGE ALTI 5 m France** (~9,4 Go) — **base** terrain (la haute-déf 1 m viendra par ville).
- **BD ORTHO 5 m France** (~15 Go) — **base** teinte (le 20 cm viendra par ville).
- **BAN France** (qq Go).
- → **~60–90 Go** selon filtrage des thèmes.

**Par ville construite (haute-déf, à la demande — AUCUNE perte)** :
- **RGE ALTI 1 m** (déjà en main pour Toulouse).
- **BD ORTHO 20 cm** (déjà en main pour Toulouse).
- **LiDAR HD** (dalles ; déjà en main pour Toulouse Métropole, 550 dalles) — si forme LiDAR ou arbres MNH.

**Décision produit en suspens** : « toute la France au détail bâtiment » vs « villes-hubs » — n'impacte que la **haute-déf** (1 m / 20 cm), pas le socle.
