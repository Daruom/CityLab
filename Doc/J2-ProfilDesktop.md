# J2 — Profil de génération DESKTOP (conception)

**Statut : CONCEPTION — rien d'implémenté.** Ce doc définit le « profil desktop » de
`CityImportTools` : mêmes données sources, même toolset C++, mais des budgets relevés
pour une cible PC (SM6, Lumen, Nanite) au lieu du Redmi Note 7. Le profil mobile actuel
(v14b, validé device) reste le chemin par défaut et ne bouge pas.

## 1. Objectifs

1. **Un seul pipeline, deux profils.** Le générateur lit les mêmes JSON
   (`SourceData/toulouse10*.json`) et produit soit la map mobile actuelle, soit une map
   desktop plus riche. Aucun fork de données, aucun fork d'outil : un jeu de paramètres.
2. **Relief réel.** Le sol n'est plus une dalle z=0 : MNT RGE ALTI 1 m
   (`toulouse10_mnt.png` + `.json`, voir §2), bâtiments posés à l'altitude du terrain,
   routes et surfaces drapées.
3. **Éclairage dynamique.** Matériaux PBR éclairables Lumen à la place de l'unlit à
   ombrage cuit en vertex colors. Le soleil devient un vrai directional + skylight ;
   l'ombrage cuit disparaît (les vertex colors restent comme teinte/AO de variation).
4. **Modénature réelle.** Fenêtres en creux complètes (tableaux, appui, linteau) au lieu
   de la vitre en simple retrait ; atlas de façades 2-4K ; UVs monde préparées pour
   l'ortho BD ORTHO.
5. **Budgets ×5-10.** Rayons de streaming multipliés, HLOD auto à la place des
   proxys-boîtes, Nanite activé sur la géométrie opaque.
6. **Non-régression mobile.** Le self-test et les 10/10 tests d'import existants
   continuent de passer sur le profil mobile ; toute divergence d'ini est notée dans
   `Etat.md` (règle existante).

## 2. Entrées de données

| Donnée | Fichier | Source | Licence |
|---|---|---|---|
| Bâtiments (emprises + hauteurs) | `SourceData/toulouse10.json` | IGN BD TOPO (WFS) | Licence Ouverte 2.0 |
| Routes, arbres | `SourceData/toulouse10.json` | OSM Overpass | ODbL |
| Eau, verts, rails | `SourceData/toulouse10_surfaces.json` | BD TOPO + OSM | LO 2.0 + ODbL |
| Repères | `SourceData/toulouse10_markers.json` | OSM | ODbL |
| **MNT (nouveau)** | `SourceData/toulouse10_mnt.png` + `toulouse10_mnt.json` | IGN RGE ALTI 1 m, WMS-R Géoplateforme (`Fetch-Toulouse10-MNT.ps1`) | Licence Ouverte 2.0 |
| Ortho (à venir, §4.1) | à fetcher (WMS-R `HR.ORTHOIMAGERY.ORTHOPHOTOS`, BD ORTHO 20 cm) | IGN | Licence Ouverte 2.0 |

### Le MNT en deux mots (géoréférencement complet dans `toulouse10_mnt.json`)

- 10 000 × 10 000 px, **1 px = 1 m** dans la projection locale du pipeline (équirectangulaire
  Capitole lat0=43.6045 lon0=1.4442, **nord = -Y**), PNG 16 bits gris,
  **valeur = altitude NGF en centimètres** → 1 unité PNG = 1 cm Unreal en Z, direct.
- Coin NW du pixel (0,0) = local (-5000 m, -5000 m) = Unreal (-500000, -500000) cm ;
  ligne 0 = nord, les lignes croissent vers le sud (+Y). Centre pixel à (col+0,5 ; row+0,5).
- Mesuré sur la dalle : **min 120,34 m** (lit de la Garonne en aval, nord), **max
  259,40 m** (coteaux de Pech David au sud), moyenne 145,62 m, **Capitole 142,39 m**
  (recoupé au centimètre avec le service alti REST IGN, 0 nodata).
- **Rebase proposé : `Z_unreal = (alt_cm − alt_capitole_cm)`** — la place du Capitole
  reste près de z=0, tout le contenu existant (repères, caméras, spawn) garde son sens.
  L'alternative (altitudes NGF brutes, tout à +14 200 cm) est écartée. → Q11.

### Échantillonneur terrain (nouvelle brique C++)

`FTerrainSampler` (module CityLabEditor) : charge le PNG 16 bits via `IImageWrapper` +
le JSON de métadonnées, expose `float AltCmAt(double Xcm, double Ycm)` en bilinéaire,
et `float MinAltCmInPolygon(...)` / `MaxAltCmInPolygon(...)` pour la pose des bâtiments.
C'est LA brique commune : sol, routes, surfaces, bâtiments, arbres échantillonnent tous
le même sampler (cohérence verticale garantie entre couches).

## 3. Changements par couche

### 3.1 Sol (résident)

| | Mobile (actuel) | Desktop (proposé) |
|---|---|---|
| Géométrie | dalle plate z=0, grille 12×12 par cellule 500 m (~42 m/sommet) | grille **64×64** par cellule (~7,8 m/sommet), sommets déplacés en Z par le MNT |
| Teinte | vertex colors peintes (eau/verts échantillonnés) | conservée (variation) × **matériau PBR** avec UV monde |
| UVs | planaire 0,0025/cm | **UV0 détail** (tiling herbe/terre) + **UV1 monde** : (x,y) normalisé sur les 10 km, prête pour l'ortho BD ORTHO en base color lointaine |
| Collision | boîte simple par cellule (FKBoxElem) | **trimesh du sol basse résolution** (grille 16×16 dédiée, CTF simple-as-complex) — la boîte plate est fausse dès que le terrain ondule. Alternative heightfield → Q8 |
| Nanite | non | **oui** (le sol 64×64 reste léger, mais Nanite unifie le chemin de rendu) |

Budget : 485 cellules × 64×64×2 tris ≈ **4,0 M tris** de sol (contre ~0,14 M
aujourd'hui) — trivial pour Nanite desktop.

### 3.2 Routes

- **Drapage** : chaque polyline est ré-échantillonnée à pas fixe (~15 m) avant
  extrusion du ruban ; chaque sommet reçoit `Z = AltCmAt(x,y) + offset décimétrique`
  (l'empilement anti-z-fight actuel — dalle 0 < parc < eau < rail < route 55+ —
  devient **relatif au terrain**). Les longs segments droits OSM sont subdivisés,
  sinon la route traverse les bosses.
- Le principe « ruban texturé 1 quad/segment » (T_RoadStrip) est conservé — il a
  gagné le bench mobile et reste valable ; le matériau devient une version **PBR**
  (base color + normal + roughness asphalte, marquages dans la texture).
- **Ponts (Q5)** : le MNT est un sol nu — une route drapée plonge dans la Garonne.
  Les ways OSM taggés `bridge` (à ré-inclure au fetch, actuellement le tag est perdu)
  se drapent en **interpolation linéaire des Z entre les deux culées** au lieu du MNT.
- Collision : inchangée (films visuels sans collision, le sol porte la collision) —
  d'où l'importance du MÊME sampler pour le sol et les routes.

### 3.3 Bâtiments

- **Pose à l'altitude (nouveau)** : `ZBase = MinAltCmInPolygon(emprise)` ; le mur
  descend jusqu'à `ZBase − (MaxAlt − MinAlt) − 50 cm` (socle enterré) pour qu'aucun
  coin ne flotte en pente. Pas de terrassement du MNT en v1 (Q4).
- **Fenêtres en creux réelles** : aujourd'hui la vitre est un quad en retrait de 15 cm
  sans tableaux (commentaire explicite « sans tableaux (coût) » dans
  `BuildPolygonBuilding`). Le profil desktop ajoute par fenêtre : 4 quads de tableaux
  (retours de 15-20 cm), 1 appui saillant (2 quads), 1 linteau (1 quad) — soit
  **~+9 quads (+18 tris) par fenêtre**. RDC commerçants : vitrine pleine hauteur +
  bandeau (déjà esquissé dans BuildingTools, à porter).
- **Atlas de façades 2-4K** : une texture atlas (grille de 4×4 sous-tuiles 512-1024 px :
  brique toulousaine ×4 variantes, enduit ×4, moderne, industriel, toits…). Chaque
  travée mappe sa sous-tuile selon `UsageTint(usage, seed)` — la teinte vertex color
  actuelle devient un multiplicateur de variation, plus la source unique de couleur.
- **Matériaux** : `M_CityWall_PBR` (base color atlas × vertex color, normal de brique,
  roughness) et `M_CityGlass_PBR` (réflexions Lumen, légère émissive nocturne en
  option). Fini l'ombrage soleil cuit (`Shade()`) : **Lumen éclaire**.
- **Toits** : plats conservés en v1 (BD TOPO ne donne pas la forme). Piste v2 : ortho
  projetée sur les toits via UV monde (les toits de tuiles toulousains portent
  l'identité visuelle vue de drone) → Q7.
- **Nanite : oui** pour la géométrie opaque. Les vitres (translucides) ne sont pas
  Nanite → **séparer par cellule le mesh Wall (Nanite) du mesh Glass (classique)**
  au lieu des deux slots actuels sur un même mesh (Q3).

Budget estimé : ~131 k bâtiments, moyenne ~10-25 fenêtres → **+25-60 M tris** de
modénature sur la dalle entière (Nanite s'en charge ; le coût réel est le disque,
estimé ×2-3 sur les 0,58 Go d'assets actuels).

### 3.4 Surfaces (eau / verts / rails)

- **Verts et rails : drapés** sur le MNT comme les routes (offsets décimétriques
  conservés, relatifs au terrain).
- **Eau : PAS drapée.** Chaque polygone d'eau devient un plan horizontal à
  `Z = quantile bas (p10) du MNT sous le polygone` — la Garonne reste plane par
  tronçon au lieu d'épouser les berges (le MNT sol nu descend dans le lit). Matériau
  eau desktop : translucide + réflexions écran/Lumen ; `SingleLayerWater` à bencher (Q6).
- Les polygones qui débordent de la dalle (connu, cosmétique) : clamper l'échantillon
  MNT au bord de la grille.

### 3.5 Arbres

- Position Z : MNT (fini les arbres à z=0).
- La règle « masked = poison Adreno » ne s'applique PAS au desktop : LOD0 peut
  redevenir des cartes masked riches (2-3 k tris), l'arbre v6 opaque actuel sert de
  LOD1/imposteur lointain. Nanite foliage : à évaluer sur UE 5.8, pas un prérequis (Q9).
- Budget instances inchangé (~118 k arbres HISM) ; le coût passe au GPU desktop, OK.

### 3.6 Streaming, HLOD, proxys

| | Mobile (actuel) | Desktop (proposé) |
|---|---|---|
| Blocs | 136 sous-niveaux de 1 km | inchangé (le découpage est sain) |
| Rayons in/out | 500 / 800 m (hystérésis) | **3000 / 4000 m** de départ, bench jusqu'à 5000/8000 (×5-10) |
| Lointain | proxys-boîtes rétrécies 30 cm, résidents | **HLOD auto par bloc** : mesh proxy généré (merge + simplification + atlas bake) via l'API HLOD/MergeActors d'UE, streamé en négatif du bloc détail |
| Nanite | non | oui (détail) — le HLOD sert surtout les draw calls et la mémoire au-delà du rayon |

Note mémoire : à 4000 m de rayon, ~50 des 136 blocs sont chargés simultanément ;
avec des assets ×2-3 (~1,5-2 Go sur disque pour les bâtiments), le budget RAM desktop
(8-16 Go) tient sans effort là où le Redmi plafonnait à 1,65 Go.

## 4. Paramètres proposés — le « profil »

Un `USTRUCT FCityGenProfile` passé aux outils d'import (ou un nouveau tool
`ImportCityDesktop` qui délègue), avec deux préréglages nommés :

| Paramètre | `Mobile` (verrouillé = valeurs actuelles) | `Desktop` |
|---|---|---|
| `CellSizeM` / `BlockSizeM` | 500 / 1000 | 500 / 1000 |
| `GroundGrid` | 12×12, z=0 | 64×64, drapé MNT |
| `GroundCollision` | boîte | trimesh 16×16 |
| `StreamInM` / `StreamOutM` | 500 / 800 | 3000 / 4000 (bench →8000) |
| `LointainKind` | proxys-boîtes | HLOD auto |
| `WindowStyle` | vitre en retrait 15 cm | tableaux + appui + linteau |
| `Materials` | unlit vertex color | PBR Lumen (Wall/Glass/Road/Ground/Water) |
| `BakedShading` | oui (`Shade()` cuit) | non (Lumen), vertex color = variation |
| `FacadeAtlas` | — | `T_FacadeAtlas` 2-4K, 4×4 sous-tuiles |
| `WorldUVs` | — | UV1 monde 0-1 sur 10 km (ortho-ready) |
| `Terrain` | — (z=0) | `toulouse10_mnt.png` + rebase Capitole |
| `Nanite` | off | on (opaque : sol, murs, HLOD) |
| `GlassSplit` | slot sur le même mesh | mesh Glass séparé par cellule |
| `TreeLOD0` | v6 opaque | cartes masked + v6 en LOD1 |

Le profil `Mobile` est un **golden path** : les tests d'import existants tournent sur
lui et doivent produire bit-à-bit la même géométrie qu'aujourd'hui (garantie de
non-régression pendant tout le chantier desktop).

## 5. Rendu / config (à trancher avant J2c)

- CityLab a les ini **copiés de DroneFPV** (cible mobile : shadows off, unlit, ES3.1
  preview). Le profil desktop a besoin de Lumen + Nanite + SM6 **sans faire diverger
  la base mobile** : passer par un **DeviceProfile/scalability dédié** (ou un
  `-dpcvars`/`GameUserSettings` de labo) plutôt que d'éditer DefaultEngine.ini — et
  toute divergence est notée dans `Etat.md` (règle existante).
- La compensation gamma des vertex colors (pow 2.2 cuit à la génération, pour lecture
  brute en unlit) est **fausse pour un matériau PBR** qui lit VertexColor en linéaire :
  le profil desktop doit encoder en **linéaire** (Q10).

## 6. Risques & questions ouvertes (pour l'implémentation C++)

- **Q1 — Lumen × meshes fusionnés 500 m** : les distance fields de cellules de 500 m
  pleines de creux peuvent être chers/grossiers ; vérifier la qualité GI en canyon de
  rue et le coût de génération DF au cook. Fallback : Lumen en mode « detail traces »
  écran seulement.
- **Q2 — Nanite × vertex colors × MeshDescription** : chemin réputé OK en 5.8, à
  valider sur nos meshes générés (build Nanite au `PostEditChange` ou au cook ?).
- **Q3 — Split Wall/Glass** : un mesh est Nanite ou ne l'est pas — les vitres
  translucides imposent soit deux meshes par cellule (proposé), soit des vitres
  opaques-réfléchissantes dans le mesh Nanite (moins beau, moins cher). À bencher.
- **Q4 — Terrassement** : socle enterré (proposé, simple) vs aplatir le MNT sous
  chaque emprise (plus joli en forte pente, coûteux et risque d'artefacts entre
  bâtiments mitoyens). Toulouse est douce (139 m d'amplitude sur 10 km, et l'essentiel
  de la ville tient dans 120-160 m) : socle d'abord, terrassement seulement si les
  captures le réclament.
- **Q5 — Ponts** : re-fetch OSM avec le tag `bridge` (aujourd'hui perdu) + interpolation
  des Z entre culées. Les ponts sur la Garonne sont incontournables visuellement.
- **Q6 — Eau** : plan par polygone au p10 du MNT (proposé) ; `SingleLayerWater` vs
  translucide simple à bencher ; le raccord berge/eau en pente reste le point dur.
- **Q7 — Toits** : plats v1. L'ortho projetée (UV monde) donnerait les toits de tuiles
  « gratuits » mais fige l'éclairage (ombres cuites dans l'ortho) — acceptable vu de
  drone ? À juger sur capture.
- **Q8 — Landscape vs mesh généré pour le sol** : Landscape UE apporterait heightfield
  collision, LOD morphing et layers de matériaux gratuits, mais casse l'unité du
  pipeline (tout-généré, tout-streamé pareil) et Landscape+World Partition est un
  autre monde. Proposé : rester en static mesh généré v1, spike Landscape séparé si
  le sol 64×64 déçoit.
- **Q9 — Arbres desktop** : masked OK sur PC, mais 118 k arbres × 2-3 k tris = à
  bencher en HISM avec cull distance ; Nanite foliage optionnel.
- **Q10 — Gamma vertex colors** : encoder linéaire pour PBR (le pow 2.2 actuel est un
  hack unlit). Un seul flag dans le builder, mais à ne PAS oublier (piège documenté).
- **Q11 — Datum vertical** : rebase Capitole (~142 m → z=0) proposé ; valider que rien
  (spawn, repères, caméras de bench) ne suppose le sol exactement à z=0 — après
  drapage, le sol au Capitole reste ~0 mais ondule ailleurs de -22 m à +117 m.
- **Q12 — Volumes** : assets ×2-3 (disque), PNG MNT ~93 Mo en repo local, temps de
  génération éditeur (le TDR GPU « ne jamais rendre la main avec 60 M tris non
  sauvés » s'applique encore plus — garder la parade save+hide par sous-niveau).
- **Q13 — BD ORTHO** : fetch à écrire (même gabarit WMS-R que le MNT, couche
  `HR.ORTHOIMAGERY.ORTHOPHOTOS`, JPEG tuilé) ; à quelle résolution pour 10×10 km ?
  (20 cm plein = 50 Go — non ; 1-2 m pour le lointain = 25-100 Mo — oui).

## 7. Jalons d'implémentation proposés

1. **J2a — Terrain** : `FTerrainSampler` + sol drapé + routes/surfaces drapées,
   matériaux actuels inchangés. Verdict : vol éditeur, le relief se lit.
2. **J2b — Pose** : bâtiments/arbres à l'altitude, ponts interpolés, eau plane.
3. **J2c — Rendu** : profil rendu desktop (Lumen + Nanite + PBR), gamma linéaire,
   split Glass. Verdict : capture canyon de rue au soleil rasant.
4. **J2d — Modénature** : fenêtres complètes + atlas façades + UV monde + ortho lointaine.
5. **J2e — Échelle** : HLOD auto, rayons ×5-10, bench 60 fps, budgets mémoire.

Chaque jalon garde le profil Mobile bit-à-bit identique (tests golden path).
