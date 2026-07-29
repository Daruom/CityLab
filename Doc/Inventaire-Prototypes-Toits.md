# Inventaire des prototypes toits / bâtiments — CityLab

> Relevé exhaustif au **2026-07-29** (avant rangement/archivage). But : ne rien perdre,
> savoir pour chaque proto la **technique**, les **données**, et surtout ce qui est
> **réel / ajusté / deviné**. Décision produit du jour : **socle = E2** (toits skeleton +
> terracotta + murs marbre) ; **LiDAR blanc gardé en réserve** pour test ultérieur.

## Légende de fiabilité

| Symbole | Sens | Exemple |
|---|---|---|
| 🟢 **RÉEL** | donnée mesurée officielle IGN (BD TOPO, LiDAR HD, orthophoto) | emprise au sol, forme LiDAR d'un toit |
| 🟡 **AJUSTÉ** | dérivé d'une donnée réelle mais transformé/idéalisé/simplifié | mur extrudé, teinte mappée sur tuile, contour lissé |
| 🔴 **DEVINÉ** | inventé par heuristique, **aucune vérité terrain** | forme skeleton d'un toit, fenêtres, séparation clutter LiDAR |

## Fondations communes à TOUTES les maps

Peu importe le proto, la base géométrique est la même :

| Aspect | Source | Fiabilité |
|---|---|---|
| Emprise au sol (footprint) | BD TOPO IGN | 🟢 RÉEL |
| Hauteur du bâtiment | BD TOPO (attribut `h`) ; côté LiDAR = faîtage mesuré | 🟢 RÉEL (défaut ajusté si attribut manquant → 🟡) |
| Calage géographique (Lambert93, origine = Capitole) | IGN | 🟢 RÉEL |
| **Murs** (géométrie) | extrusion emprise → hauteur | 🟡 AJUSTÉ (dérivé réel, surface inventée) |
| Teinte de toit (terracotta variée) | orthophoto IGN, médiane par bâtiment | 🟢 RÉEL (échantillon) / 🟡 (mapping vers tuile) |

Le vrai différenciateur entre protos = **la FORME du toit** (skeleton vs LiDAR) et
**l'habillage** (couleur toit, style de mur, fenêtres).

---

## Les familles de prototypes (chronologique)

### 1. `Dev/ProtoToits/` — premiers essais de toit (skeleton on/off)
- **Maps** : `L_ProtoToits`, `L_AB_avec_roof`, `L_AB_sans_roof` (+ `Blocks/`, `ABBlocks_avec_roof/`, `ABBlocks_sans_roof/`, `City/`, `AB_*`)
- **Technique** : toit par **squelette droit** (straight-skeleton, bpypolyskel) ; comparaison avec / sans toit.
- **Toit** : 🔴 DEVINÉ (forme idéalisée hip/gable calculée depuis l'emprise, pas le vrai toit).
- **Statut** : **historique / dépassé** → archivable.

### 2. `Dev/ProtoSols/` — proto de référence (base de test)
- **Map** : `L_ProtoSols` (quartier Capitole ±400 m, 4 blocs streamés `L_T10_B_{-1,0}_{-1,0}`). Copies d'assets figées dans `ProtoSols/City` + `ProtoSols/Blocks`.
- **Technique** : générateur desktop, murs texturés "carton" + **toits skeleton terracotta**.
- **Toit** : 🔴 skeleton + 🟡 teinte terracotta ortho. **Murs** : 🟡 extrusion + 🔴 fenêtres/modénature (atlas).
- **Statut** : **base de test vivante** (c'est ici que se lancent les boucles proto ~6 min). À GARDER.

### 3. `Dev/ProtoABC/` — variantes marbre A / B / C
- **Maps** : `L_ProtoSols_A`, `L_ProtoSols_B`, `L_ProtoSols_C` (assets sous `A/`, `B/`, `C/`).
- **Technique** : exploration direction artistique **marbre blanc** :
  - **A** = marbre nu (blocs simples, sans détail de toit) 🔴 forme minimale
  - **B** = A + **forme de toit** (skeleton) 🔴 ; a révélé des rayures verticales = **modénature** (corrigée depuis).
  - **C** = B + **fenêtres discrètes** 🔴 DEVINÉ (inventées).
- **Toit** : 🔴 skeleton, rendu **blanc marbre**. **Murs** : 🟡 marbre lisse.
- **Statut** : exploration DA → **archivable** (les enseignements sont passés dans D/E).

### 4. `Dev/ProtoDE/` — variantes D / E / E2  ⭐
- **Maps** : `L_ProtoSols_D`, `L_ProtoSols_E`, `L_ProtoSols_E2` (assets `D/`, `E/`, `E2/`).
  - **D** = comme B mais **murs qui gardent l'effet lisse de A** (marbre lisse + toit skeleton). 🔴 skeleton, blanc.
  - **E** = B + **les toits terracotta** repris de `L_ProtoSols`. 🔴 skeleton + 🟡 terracotta.
  - **E2** = E avec **murs marbre** explicites (skeleton terracotta + murs blancs lisses).
- **Toit** : 🔴 forme skeleton (idéalisée) + 🟡 couleur terracotta ortho (réelle, échantillonnée).
- **Statut** : **E2 = SOCLE RETENU ✅** (toits colorés pour l'identité des villes + murs marbre ; robuste, procédural, pas de reconstruction fragile). D/E = étapes vers E2, archivables.

### 5. `Dev/ProtoLIDAR/` — reconstruction LiDAR réelle
Forme de toit **issue du vrai nuage LiDAR HD IGN** (classe 6) reconstruit par **Roofer/3DBAG** (LoD2.2). La forme est 🟢 RÉELLE (avec une part 🟡 de reconstruction). Ce qui varie entre les maps = l'habillage et le traitement de la clutter.

| Map | Contenu | Assets | Fiabilité clé | Statut |
|---|---|---|---|---|
| `L_ProtoSols_LIDAR` | forme LiDAR, **rendu blanc marbre** | `City/`, `Blocks/` | 🟢 forme réelle, **0 devinage** (pas de coloration) | **RÉSERVE ✅** (test futur avec routes) |
| `L_ProtoSols_LIDAR_TOITS` | LiDAR, murs procéduraux | `CityTOITS/`, `BlocksTOITS/` | **KO** — trous mur↔toit | archivable (échec documenté) |
| `L_ProtoSols_LIDAR_FINAL` | LiDAR + terracotta varié + murs Roofer double-face + murs marbre | `CityFINAL/`, `BlocksFINAL/` | 🟢 forme + 🟡 terracotta, **mais clutter tuilée** | archivable |
| `L_ProtoSols_LIDAR_A` | declutter **v3.2** → terracotta net (sans boîtes) | `CityA/`, `BlocksA/` | 🟢 forme + 🔴 **séparation clutter (deviné)** | archivable |
| `L_ProtoSols_LIDAR_B` | declutter **v3.2** + **9977 boîtes grises** (objets extraits) | `CityB/`, `BlocksB/` | 🟢 forme + 🔴 clutter deviné (couverture ~66 %) | archivable |

- **Enseignement majeur** : colorer des toits LiDAR est **structurellement du devinage** (la donnée ne tague pas les objets de toiture → séparation heuristique, toujours des ratés) ; **Roofer LoD2.2 est chaotiquement instable** (retirer 0,5 % de points peut effondrer un toit). → parade *merged shell* (repli sur base sur les bâtiments qui s'effondrent).
- **Doctrine figée** : si un jour LiDAR, alors **BLANC** — on ne retente jamais terracotta-sur-LiDAR.

---

## Maps cibles (production, pas des protos de toit mais alimentées par le même générateur)

| Map | Rôle |
|---|---|
| `Maps/L_Toulouse10` (+ `Maps/T10Blocks/`, ~150 blocs) | **ville entière 10 km** (cible de génération finale) |
| `Maps/L_Capitole` | zone Capitole héros |
| `Maps/L_BuildingTest`, `Maps/L_Probe` | bancs d'essai bâtiment isolé |
| `Dev/Test/L_TestRun{,2,3}` | scratch jetable → **supprimable** |

## Données sources — `SourceData/`

| Fichier | Contenu | Fiabilité |
|---|---|---|
| `capitole.json`, `toulouse10.json` | BD TOPO brut (bâtiments IGN) | 🟢 RÉEL |
| `toulouse10_bati.json` (62 Mo) | géométrie bâtiment **traitée** (pipeline) | 🟢→🟡 |
| `toulouse10_bati.avant_*.json` | snapshots avant chaque étape : `cours` → `flatcut` → `interstices` → `rognage` → `tint` → `j3f` | trace de traitement |
| `toulouse10_surfaces.json` | surfaces (routes/places/sols) | 🟢 |
| `toulouse10_markers.json`, `capitole_markers.json` | POI / noms de lieux | 🟢 |
| `toulouse10_mnt.json` / `.png` | terrain (MNT) | 🟢 |
| `toulouse10_tint.*` | teinte ortho par bâtiment | 🟢 (échantillon) |
| `Fetch-*.ps1` | scripts d'acquisition IGN Géoservices | outillage |

**PoC LiDAR (hors projet UE)** : `C:\LidarPoC\` — 550 dalles métropole (~79 Go), binaire Roofer,
scripts de segmentation/génération sous `C:\LidarPoC\work\AB\` (voir `Survol/Doc/Reprise-LiDAR-J3f.md`).

---

## État du rangement (2026-07-29) — organisation LOGIQUE

**Décision** : la réorganisation PHYSIQUE des assets a été **abandonnée**. L'outillage UE
(rename/déplacement headless) casse les références des maps à sous-niveaux streamés, et une
partie du Content est sous git. L'organisation reste **LOGIQUE**, portée par ce document.
Après une tentative d'archivage ratée, un `git checkout -- Content/` a proprement remis le
Content suivi à l'état du dernier commit (les protos actifs E2 / LIDAR-blanc / ProtoSols,
non suivis, sont restés intacts).

**Leçon** : ne PAS déplacer/supprimer des maps UE bakées par script headless — le Content
Browser de l'éditeur (Migrate / drag-drop, qui gère les redirectors) est la seule voie sûre,
en manuel. Sauvegarde complète : `C:\LidarPoC\work\CityLab_Dev_backup` (235 Mo).
`ProtoToits` (obsolète, premiers essais de toit) a été retiré de l'arbre ; ses assets réels
restent dans `backup/_Archive/ProtoToits`.

## Carte des dossiers `Content/Dev/`

| Dossier | Rôle | Statut |
|---|---|---|
| `ProtoSols` | proto de référence (base de test, boucle rapide ~6 min) | **ACTIF** (gitignore) |
| `ProtoDE/E2` + `L_ProtoSols_E2` | **SOCLE RETENU** (skeleton + terracotta + murs marbre) | **ACTIF** (non suivi git) |
| `ProtoDE/D`, `ProtoDE/E` | étapes vers E2 | référence |
| `ProtoLIDAR/L_ProtoSols_LIDAR` (+ `City` + `Blocks`) | **réserve LiDAR-blanc** | **ACTIF** (non suivi) |
| `ProtoLIDAR/{_A, _B, _FINAL, _TOITS}` | expériences LiDAR terracotta (déprécié) | référence |
| `ProtoABC` (A/B/C) | exploration DA marbre | référence |
| `OrthoAB` | données ortho partagées (teinte) | données |
| `Test` | scratch (L_TestRun) — **⚠️ contient 3 meshes dont dépend `Maps/L_BuildingTest`** (production) : ne pas supprimer | garder |

**Prochaine variante en cours** : `L_ProtoSols_E2_Sol1` = E2 avec **routes BD TOPO Transport**
(au lieu d'OSM) + **arbres LiDAR HD MNH** (au lieu d'OSM). Voir `Doc/Donnees-Sources-Catalogue.md`.
