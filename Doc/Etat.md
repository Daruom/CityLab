# CityLab — état du labo

Labo de bâtiments procéduraux pour DroneFPV. **Rien ici n'est shippé** : le pipeline
validé sera porté dans DroneFPV (`Source/DroneFPVEditor` + Migrate des assets retenus).

## Objectif en cours

**Test « bâtiment héros » palier 2** : prouver qu'un générateur C++ paramétrique
(fenêtres en creux, corniche, parapet, ombrage cuit en vertex colors) produit une
qualité visuelle suffisante pour un quartier FPV, dans le contexte de rendu DroneFPV
(unlit, LDR, zéro bake). Verdict utilisateur sur screenshot éditeur (preview ES3.1)
puis capture Redmi. Si KO → pivot plan B (Blender headless blosm+Buildify + décimation).

## Jalon J2 — profil de génération desktop

Conception dans **`Doc/J2-ProfilDesktop.md`** (budgets relevés : MNT RGE ALTI 1 m comme
sol réel, PBR Lumen + Nanite, fenêtres en creux complètes, atlas façades, UV monde pour
la BD ORTHO, streaming ×5-10, HLOD auto). Données terrain prêtes :
`SourceData/Fetch-Toulouse10-MNT.ps1` → `toulouse10_mnt.png` (10 000², 16 bits, valeur
= alt en cm) + `toulouse10_mnt.json` (géoréférencement exact). Rien d'implémenté côté C++.

## Décisions & conventions

- **MCP port 8101** (le 8100 = éditeur DroneFPV ; les deux tournent en parallèle).
- Config rendu/device profiles **copiées de DroneFPV** — ne pas faire diverger sans le
  noter ici. Zéro éclairage précalculé (règle F.38), pas d'UV de lightmap.
- Package Android : `com.daruom.citylab` (installable à côté du jeu).
- Compil C++ : fermer l'éditeur CityLab seulement ; `-WaitMutex -NoHotReloadFromIDE`
  obligatoires (mutex Live Coding de l'éditeur DroneFPV).
- Toolset : `CityLabEditor.BuildingTools` (NewLevel, SaveLevel, ExecConsoleCommand,
  GenerateBuilding). Tests automation : filtre `CityLab.BuildingTools`.
- `GenerateBuilding` régénère en place : même AssetPath = itération sans respawn.

## Pièges payés ici (à reporter dans DroneFPV si pertinent)

- **VertexColor blanc en viewport SM6 desktop** : sur cette config (ini copié DroneFPV),
  le node VertexColor rend BLANC dans les scènes de level (StaticMeshActor ET HISM),
  alors que le ColorVertexBuffer contient bien les couleurs (diagnostic C++ : 2716
  entrées, (45,45,45)) et que la vignette d'asset (CaptureAssetImage) les affiche
  correctement. Ni la Mesh Paint VT (désactivée depuis), ni les overrides par
  composant, ni l'état du level (repro dans un level neuf). Non résolu en SM6 —
  contourné : **juger en preview ES3.1** (la cible réelle du jeu, règle DroneFPV).
- **Auto-exposition du viewport éditeur** : scène sombre → tout est cramé blanc et
  l'exposition ÉGALISE les luminances (a masqué l'ombrage pendant des heures).
  Fix : PostProcessVolume unbound, histogramme, EV100 min=max=0 dans le level de test.
- **Mesh Paint Virtual Texture** (UE 5.7+) : `r.MeshPaintVirtualTexture.Support` +
  `r.StaticMesh.DefaultMeshPaintTextureSupport` mis à False ici (divergence ini
  documentée) — le système peut détourner le node VertexColor vers une texture de
  peinture blanche. Les rochers F.39 (HISM) ne voient pas ce système.
- **Compensation gamma des vertex colors** : le build mesh encode ToFColor(true)
  (sRGB) mais le shader lit les octets bruts → stocker pow(L, 2.2) à la génération
  (fait dans BuildingTools.cpp).
- `QUIT_EDITOR` via ExecConsoleCommand = fermeture propre pilotable (après SaveLevel) ;
  CloseMainWindow peut échouer silencieusement si une modale de save traîne.
- Toolsets MCP moteur : noms de paramètres incohérents entre outils (mesh vs
  static_mesh, asset_path vs asset, material vs material_or_function) — toujours
  vérifier le schéma via describe_toolset avant d'appeler.
- **Cohabitation des deux sessions Claude** : toute fermeture d'éditeur doit filtrer
  par titre de fenêtre (`MainWindowTitle -match 'CityLab'` ou `'DroneFPV'`) — le
  2026-07-22, la session DroneFPV a fermé TOUS les UnrealEditor pour son rebuild et
  a embarqué l'éditeur CityLab (fermeture propre, rien perdu, mais séance interrompue).

## Fait

- 2026-07-22 : scaffolding du projet (modules, toolset, config, git). Voir git log.
- 2026-07-22 : pipeline de bout en bout PROUVÉ : GenerateBuilding (façades, fenêtres
  en creux, corniche, parapet, vitrines RDC) + matériaux unlit créés par MCP +
  captures/mesures pixel automatisées. Reste le verdict visuel en ES3.1.
- 2026-07-23 : **quartier Capitole (Toulouse) importé** — `L_Capitole`, 1 015 bâtiments
  (BD TOPO IGN, hauteurs réelles au mètre), 514 routes OSM (rubans + trottoirs +
  marquages), 315 arbres réels (HISM), 37 meshes (fusion par cellules de 150 m).
  Toolset `CityImportTools.ImportCityDistrict` + fetch `SourceData/capitole.json`
  (script Overpass + WFS Géoplateforme). Place du Capitole reconnaissable en vue
  aérienne, canyons de rue volables. Teintes par usage (brique/enduit) + ombrage
  cuits en vertex colors — qui S'AFFICHENT dans le level (le mystère « murs blancs »
  du 22/07 ne se reproduit pas sur ce contenu ; à re-caractériser avant de fermer).
  14/14 tests PASS. Licences données : © OpenStreetMap contributors (ODbL) +
  IGN BD TOPO (Licence Ouverte 2.0) — mentions à prévoir dans l'app.

- 2026-07-23 (soir) : **ville entière 10×10 km importée** — `L_Toulouse10`, 131 257
  bâtiments / 45 767 routes / 104 296 arbres (fetch `SourceData/Fetch-Toulouse10.ps1`
  → `toulouse10.json`, 27,5 Mo, même origine Capitole), cellules de **500 m** (486
  meshes, 0,58 Go d'assets sous `/Game/City/Toulouse10`). Même logique que L_Capitole
  (tout résident, unlit, vertex colors, AUCUN LOD) — décision utilisateur explicite.
  ~56 M de triangles statiques estimés (l'actuelle : 1,25 M). **414 repères** dont
  nouveaux kinds `district` (24 zones, totem turquoise 35 m, label 11 m) et `quarter`
  (105 quartiers, totem bleu 22 m, label 6,5 m) ajoutés à `MarkerKinds()`.
  Captures : `Saved/Screenshots/toulouse10_*.png`. Le niveau n'a NI ciel NI brume
  (niveau vide + unlit) et tout est à z=0 (pas de MNT). L'étape qui tranche : APK device.
- Pièges payés ce soir : la Géoplateforme WFS jette les pages enchaînées sans pause
  (throttling → 1,2 s entre pages) ; `HighResShot` via ExecConsoleCommand n'écrit RIEN
  (viewport non-realtime) — la voie fiable est `py unreal.AutomationLibrary.
  take_high_res_screenshot(...)` + `editor_invalidate_viewports()`, caméra via
  `UnrealEditorSubsystem.set_level_viewport_camera_info` (BugItGo est sans effet en éditeur).

- 2026-07-23 (nuit) : **passe surfaces de repérage** sur L_Toulouse10 — nouveau tool
  `ImportCitySurfaces` (polygones plats teintés + rubans, placeholders à remplacer) :
  **186 eau** (BD TOPO surface_hydrographique : Garonne, Canal du Midi, lacs),
  **3 885 verts** (1 762 bois BD TOPO zone_de_vegetation + parcs/pelouses OSM),
  **697 rails** (OSM hors tunnel), **13 711 arbres dispersés** dans les bois (grille
  28 m jitter, cap 80 k), 450 meshes `SM_Surface_*` (cellules 500 m). Empilement z
  sous les routes : parc 1,0 < bois 1,6 < eau 2,4 < rail 3,0+. Fetch versionné
  `Fetch-Toulouse10-Surfaces.ps1` (anneaux amincis 4 m, cap 600 pts — ear-clipping).
  Tests : 8/8 PASS (2 nouveaux). Reste cosmétique : quelques polygones d'eau/rails
  débordent de la dalle 10 km (features WFS à cheval sur la bbox, géométrie complète
  conservée) — flottent sur le vide, sans conséquence.

- 2026-07-24 : **map re-générée en 3 COUCHES STREAMÉES** (verdict device : la version
  tout-résident OOM-crash au chargement sur Redmi 4 Go, kill silencieux) — nouveau tool
  `ImportCityStreamed` : couche résidente sol+routes (485 SM_Ground_*, collision) +
  proxy bâtiments-boîtes rétrécies 30 cm (36 SM_Proxy_* de 2 km, SANS collision ni
  trimesh cuit via CTF_UseSimpleAsComplex) + bâtiments détaillés (457 SM_Bldg_*,
  cellules 500 m) dans **136 sous-niveaux** `L_T10_B_<bx>_<by>` de 1 km
  (ULevelStreamingDynamic, bInitiallyLoaded=false). Le runtime (fork DroneCity
  uniquement) : ACityStreamManager, distance au carré du bloc, hystérésis 1500/2000 m.
  10/10 tests. Teintes proxy = même seed UsageTint que le détail (continuité).
- **PIÈGE MAJEUR PAYÉ (3 crashs)** : générer ~60 M tris puis rendre la main à
  l'éditeur = **TDR GPU garanti** (D3D12 TerminateOnGPUCrash au premier tick, fenêtre
  minimisée comprise — c'est la rafale d'upload/rendu qui dépasse le watchdog ~2 s,
  pas la VRAM : 145 Mo utilisés au crash). Parade DANS l'outil : sauver chaque
  sous-niveau juste après remplissage puis le MASQUER (SetLevelVisibility false,
  DontModify), et SaveDirtyPackages AVANT de rendre la main. « Tout est sauve » dans
  le log = données à l'abri même si l'éditeur meurt après. Même famille que le TDR
  GPU Lightmass. Ne JAMAIS compter sur un SaveLevel externe post-outil pour du volume.
- Piège annexe : `CreateNewStreamingLevel` prend un chemin de PACKAGE (conversion
  fichier interne) — lui donner un nom de fichier échoue en silence (nullptr).
- 2026-07-24 midi : **VALIDÉ SUR DEVICE (v5)** après itérations mesurées (baseline
  moteur 0,90 Go, zone fatale MIUI ~1,9-2,1 Go) : surfaces ET proxys sans collision,
  sol en collision BOÎTE simple (FKBoxElem par cellule — le trimesh des routes coûtait
  ~90 Mo pour <20 cm de précision), rayons streaming 500/800 m → plateau stable
  1,61-1,65 Go pendant 5 min sur Redmi (pic transitoire 1,94 au chargement initial).

- 2026-07-25 : **jalon J2 (tranche données + conception)** — MNT RGE ALTI 1 m fetché
  sur la dalle 10×10 km (`Fetch-Toulouse10-MNT.ps1`, 25 tuiles BIL float32 via WMS-R
  Géoplateforme, couche ELEVATION.ELEVATIONGRIDCOVERAGE.HIGHRES — pas de WCS sur
  data.geopf.fr) assemblé en `toulouse10_mnt.png` 16 bits (alt en cm = Z Unreal) +
  `toulouse10_mnt.json` (géoréf : 1 px = 1 m local, nord = -Y, coin NW = -5000 m).
  Conception du profil desktop écrite : `Doc/J2-ProfilDesktop.md`. Tuiles brutes
  `SourceData/MNT/` hors git (régénérables, 400 Mo).

- 2026-07-25 (après-midi) : **J2 Lot A « le relief » implémenté** — `FCityGenProfile` (mobile
  par défaut = golden path bit-à-bit ; `Desktop()` : sol 64×64 drapé MNT + collision trimesh 16×16
  dédiée, routes ré-échantillonnées 15 m + ponts `bridge` interpolés, eau plane p10, socle bâtiments). 30/30 tests.

- 2026-07-25 (soir) : **J2 Lot B « matière et modénature » implémenté** — profil étendu (bWindowReveals :
  fenêtres géométriques en creux, +18 tris/fenêtre exact ; bSplitWallGlass : SM_Bldg_*_Wall/_Glass ; bNanite
  sur opaques seulement ; bPBRMaterials : M_CityWall_PBR atlas 2048² 4×4 + Glass opaque lisse (choix Q3) +
  Ground/Road lit, vertex colors LINÉAIRES, Shade() coupé, UV1 monde sol/routes/toits). 32/32 tests.

- 2026-07-25 (soir) : **J2c — VILLE DESKTOP GÉNÉRÉE** : L_Toulouse10 régénérée EN PLACE
  au profil `Desktop()` complet (sol 64×64 drapé MNT + collision 16×16 dédiée, routes
  15 m + ponts, fenêtres en creux, split Wall/Glass, Nanite opaques, PBR Lumen, UV1
  monde) — mêmes paramètres que les générations mobiles (Wall/Glass = /Game/Dev/M_Bldg*,
  cellules 500/1000/2000 m, ordre Streamed→Surfaces→Markers). Chiffres : 131 257
  bâtiments / 45 162 routes / 104 296 arbres / 962 sols / 36 proxys / **913 meshes
  détail (457 Wall + 456 Glass)** / 136 blocs (51 min) ; surfaces 186 eau / 3 708
  verts / 697 rails / 13 633 arbres dispersés / 451 meshes (34 min) ; 414 marqueurs
  drapés. Habillage desktop posé (labels City*) : soleil Movable atmosphère pitch -35,
  SkyLight real-time capture, **SkyAtmosphere** (pas de dôme custom), fog 0.005,
  CityPlayerStart (-7000, 0, 500). Scripts rejouables `Tools/gen_desktop_*.py`
  (headless -run=pythonscript -nullrhi). Destination : Survol (copie fichiers pilotée
  par manifeste des packages réellement référencés, Saved/desktop_manifest_strict.txt).
- **Pièges payés J2c** : (1) **OOM silencieux mono-processus** — la passe unique est
  morte à ~101 Go de working set (accumulation builders + MeshDescriptions/Nanite de
  ~2 400 meshes, jamais libérés) au début des surfaces, SANS crash log (WER seulement) ;
  parade VALIDÉE : **un processus headless par passe** (streamed | surfaces | markers),
  la passe surfaces seule tient à ~16 Go. Les données étaient saines : Streamed sauve
  tout AVANT de rendre la main. (2) `ImportCitySurfaces`/`ImportCityMarkers` **ne
  sauvent PAS eux-mêmes** (seul Streamed appelle SaveDirtyPackages) → chaque passe
  doit appeler `save_dirty_packages` explicitement. (3) Python headless sur les outils
  AICallable sans glue Blueprint : `CDO.call_method('ImportCityStreamed', args=(...))`
  fonctionne ; les UPROPERTY() nus refusent `set_editor_property` (« protected ») →
  `struct.import_text('(bDesktop=True)')` et lecture des compteurs via `export_text`.
  (4) **Purge d'idempotence AVEUGLE en commandlet** : à l'ouverture headless de la map,
  les sous-niveaux ne sont PAS chargés (ils ne le sont qu'au remplissage, via
  FlushLevelStreaming) → le TActorIterator de la purge d'ImportCityStreamed n'a pas vu
  les anciens acteurs SM_Bldg_* des blocs : ville EN DOUBLE (mobile + desktop) dans
  chaque bloc. En éditeur interactif le bug n'existe pas (sous-niveaux chargés à
  l'ouverture). Correctif : passe D (`Tools/gen_desktop_pass_d_purge_blocks.py`),
  un load_map PAR bloc + destruction des labels mobiles exacts + resauvegarde.
  À corriger dans l'outil si une régénération headless doit redevenir mono-passe.

- **Chronologie complète, mesures device et procédures de reprise : voir
  `../DroneCity/Doc/Journal-Toulouse10.md`** (LE document de référence de la
  campagne Toulouse 10 km, v1→v14b : RAM, GPU, miroir nord=-Y, routes texturées,
  sol peint, bisection bench en cours).

## Prochaines étapes ville

1. Verdict utilisateur sur les captures / vol éditeur dans L_Capitole.
2. Budget : compter les tris par cellule, LODs (générer LOD grossier par cellule),
   puis APK device (profil GPU Redmi — l'étape qui tranche tout).
3. Qualité : tableaux de fenêtres (réactiver sur les grands bâtiments), portes/RDC
   commerçants, landmark Capitole (traitement dédié), textures façades atlas.
4. Consolider FCityMeshBuilder avec le builder de BuildingTools (duplication assumée).

## Reste / prochaines étapes

1. Premier bâtiment héros dans L_BuildingTest, matériaux unlit (VertexColor × texture
   de façade), itération visuelle via MCP.
2. Collision simple (boîte) sur le mesh généré — nécessaire pour le vol FPV à terme.
3. Variation par graine (hauteurs, textures, retraits d'étage) puis rangée de bâtiments.
4. Si palier 2 validé : pipeline Overpass API → footprints réels → rue entière.
