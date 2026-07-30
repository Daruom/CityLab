# Reprise — Végétation & Sol (Sol1 / Sol2) — session 2026-07-30

> Doc de reprise complet pour repartir « comme si de rien n'était » après compact.
> Sujet : construction de la végétation et du sol data-driven sur la base **E2**.
> Voir aussi : `Inventaire-Prototypes-Toits.md`, `Donnees-Sources-Catalogue.md`,
> `Survol/Doc/Reprise-LiDAR-J3f.md`.

## ⭐ TÂCHES PRIORITAIRES À FAIRE (reprise après compact)
1. **Arbres/herbe qui flottent encore** — hypothèse : le **sol vert est dessiné légèrement AU-DESSUS du pavé** (petit rebord visible) → les arbres posés dessus *paraissent* flotter au-dessus de la chaussée. À corriger : **cohérence de Z vert↔pavé** (le drapage des arbres, lui, est à 0,00 cm vs le sampler vert — vérifié). Vérifier le Z-lift des surfaces vertes vs routes.
2. **Remplacer la texture d'herbe** — l'utilisateur a téléchargé la **Megascans « Grass » 4K** (+ 2K, + plusieurs autres variantes d'herbe). Remplacer `M_GrassGround` (actuellement KikuyuGrass, pâle/évidemment tilé) par un matériau **VT 4K + macro-variation anti-répétition** (2ᵉ échelle de bruit/teinte), + variantes pour différencier les zones.
3. **Fosses d'arbres trop petites** — les agrandir NETTEMENT (proportionnel à la canopée, ~4–6 m pour les gros ; actuellement clamp 2–4 m, médian 3 m).
4. **Remplacer le « wheat grass »** (KikuyuGrass 3D grisâtre) par des assets de **`TemplateRessources\Content\Nature`** (3D téléchargés par l'utilisateur).
5. **Agent NAVIGATEUR** (en parallèle) : vérifier, pour **TOUS** les assets de `TemplateRessources`, s'il existe une **version 4K (textures)** / **qualité high (objets 3D)**. On a probablement téléchargé du **2K / medium par réflexe mobile** — à corriger (voir §Résolution).

## Décision résolution assets (IMPORTANT — corrige le réflexe « période mobile »)
- **Premium desktop** : on **author en 4K** pour tout ce qui se voit de près (le joueur atterrit dans les villes → herbe/sol vus de près). L'optimisation tous-GPU se fait **par le moteur**, pas en authorant bas :
  **Virtual Texturing** (streame les tuiles visibles) + **mip streaming** (résolution selon distance) + **Scalability** (mip bias / pool auto-réduit sur GPU faible) + **TSR** (upscale 1080p→4K).
- Le « 2K partout » = héritage mobile (mémoire fixe, pas de VT/streaming). **À bannir sur desktop.**
- **À auditer** : que CityLab/Survol n'ont **pas** de clamp mobile (TextureLODGroups du device profile Android) et que VT + streaming sont actifs.
- Réf : recherche session (3dtexel, StraySpark, UE5 Texture Streaming / Scalability docs).

---

## État actuel — `L_ProtoSols_E2_Sol2`
Map de travail = **E2 + routes BD TOPO Transport (Sol1) + végétation réelle data-driven (Sol2)**.
Chemin : `/Game/Dev/ProtoE2Sol2/L_ProtoSols_E2_Sol2` (+ `CitySol2`, `BlocksSol2`, `Sol2Materials`). Socle `L_ProtoSols_E2` et `L_ProtoSols_E2_Sol1` **intacts** (jamais touchés, vérifiés par hash à chaque passe).

**Contenu (vérifié fresh-load) :** 1657 bâtiments (skeleton, terracotta, murs marbre), routes BD TOPO (bordures/trottoirs/tirets, asphalte encore peint par masques OSM partagés), **2873 végétaux** = 1763 Maple + 236 Beech + 874 haies Elderberry (HISM `Sol2Veg_*`), fosses d'herbe sous arbres de rue, sol herbe `M_GrassGround`, **0 cône placeholder**.

**Fixes déjà appliqués cette session :** routes BD TOPO (vs OSM), arbres LiDAR filtrés routes/bâti, wind réduit (MI_WZ_* tronc 0,05/feuilles 0,10, WPO actif), **drapage MNT** (arbres/haies posés au sol, base au sol, résiduel 0,00 cm même sur les gros), samplers **Virtual** sur l'herbe (sinon damier), clip vert contre chaussées **+ trottoirs**, **fix PIE** (bâtiments visibles en Play).

## Recette végétation (comment c'est construit — data-driven, zéro devinette)
- **BD TOPO `zone_de_vegetation`** (WFS `data.geopf.fr`, national, Licence Ouverte) = le **« où » et « quoi »** : natures Bois / Forêt (feuillus/conifères/mixte) / Haie / Lande. **Ne donne PAS le nombre** d'arbres (surfacique). Couche `haie` linéaire = vide en centre dense.
- **LiDAR classe 5** (végétation haute, ≠ classe 6 bâti → **pas de clutter cheminée**) = le **« combien / où / hauteur »** : maxima de canopée (NMS R≈5 m, minH 3 m).
- **Placement** : arbres dans zones Bois/Forêt (variantes *Forest*, alternance Maple/Beech) ; arbres de rue = maxima LiDAR **hors zones** (variante *Field*, Maple) ; haies Elderberry le long de **l'axe médian (Voronoi)** des zones Haie ; **tout filtré** contre chaussées + bâtiments (`filter_trees`).
- **Échelle des arbres = vraie hauteur LiDAR** (mesurée).
- **Fosses** d'herbe sous les arbres de rue (fondues dans les meshes `SM_Surface`, drapées).

## Assets végétation copiés dans CityLab/Content (chemins `/Game/`)
- **Arbres** : `/Game/NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Field_*|Forest_*`, `/Game/EuropeanBeech/Geometry/SimpleWind/SM_EuropeanBeech_*`. (Variantes **PivotPainter** dispo = meilleur vent par branche, non utilisées.)
- **Haie** : `/Game/Megascans/3D_Plants/Elderberry/SM_Elderberry_01..07`.
- **Herbe (À REMPLACER)** : `/Game/Megascans/3D_Plants/KikuyuGrass/T_KikuyuGrass_01_BC|N` (VT) → matériau `Dev/ProtoE2Sol2/Sol2Materials/M_GrassGround`.
- **Masters** : `/Game/MSPresets/...MA_Foliage` (arbres, 12 Mo) + `/Game/Custom/...` (Megascans plants).
- Source du pack : `C:\Users\User\Documents\Unreal Projects\TemplateRessources\Content\` (BlackAlder, EuropeanBeech, EuropeanHornbeam, CommonHazel, NorwayMaple, Megascans/3D_Plants, **Nature** [nouveaux 3D], Ground, Material). Copie via **cp filesystem préservant le chemin /Game/** (fiable pour les refs) + copier les masters dont dépendent les assets.

## MNT / drapage — comment le refaire
- Le C++ `TerrainSampler.cpp` drape par vertex : **bilinéaire + offset −0,5 px + rebase Capitole**. Un nearest-neighbor donne **jusqu'à 4,4 m d'écart** sur les gros arbres (marches du MNT) → **répliquer le sampler exact**.
- Données : `CityLab/SourceData/toulouse10_mnt.png` (16-bit, cm) + `.json` (`alt_capitole_m`≈142,39 ; NW local (-5000,-5000), 1 px/m) + tuiles `SourceData/MNT/mnt_1m_*.bil` (float32). Le proto a **±13 m de relief**.
- ⚠️ **PIL vendorisée `Tools/pylib` = cp313, INCOMPATIBLE avec le Python 3.11 de l'éditeur** → lire les `.bil` en **stdlib** (struct/array), pas PIL dans l'éditeur.
- `Z_instance = solZ_bilin(x,y) − min_Z_local(mesh) × échelle_z` (pose la **BASE** au sol, pas le pivot ; ex. Beech_Forest_03 a min_Z=−82 cm).

## Leçons techniques payées cette session (NE PAS re-déboguer)
- **Archivage/déplacement d'assets UE en headless (rename_asset) = fragile** → a cassé les refs à l'aller ET au retour (rename renvoie False, redirectors, `_zzren`). **C'est `git checkout -- Content/` qui a fait le revert propre** (le Content EST suivi par git ; E2/LIDAR-blanc/ProtoSols sont *untracked* donc préservés ; ProtoToits obsolète retiré). → **Ne plus déplacer de maps bakées par script headless** ; réorg = à la main dans le Content Browser. Backup complet : `C:\LidarPoC\work\CityLab_Dev_backup` (235 Mo).
- **Éditeur GUI ouvert bloque le headless** (verrou mono-instance) + compile les **shaders feuillage lourds ~20 min** (hero-trees) → **fermer l'éditeur CityLab avant tout agent/script headless** (cibler `CityLab.uproject`, jamais HELIOS/EnvolFlight).
- **Agent trop itéré = contexte saturé → stalle** (ne lance plus son éditeur, aucun log). → relancer un **agent FRAIS**. Le **heartbeat ne bouge que pendant le script UE**, pas la prep Python de l'agent → **ne pas tuer pendant la prep** (fausse alerte de stall).
- **Textures Megascans = Virtual Textures** → les `TextureSample` du matériau doivent être en sampler **Virtual** (`SAMPLERTYPE_VIRTUAL_COLOR` / `VIRTUAL_NORMAL`) sinon `Failed to compile SM6` → **damier** (DefaultMaterial). Le **commandlet headless NE compile PAS le shadermap SM6** → vérifier via ouverture GUI + grep du log `M_GrassGround.*Failed to compile`.
- **WorldPosition** dans un matériau = LWC double précision en UE 5.8 → casse la compil si branché sur des UV (utiliser `WorldAlignedTexture` — chemin `Engine_MaterialFunctions01/Texturing/` — ou `TexCoord × échelle`).
- **HISM Python UE 5.8** : `mark_render_state_dirty()` **n'existe pas** (méthode C++ non bindée) → utiliser le **4ᵉ arg** de `update_instance_transform`. Persistance d'un HISM créé en Python = **`SubobjectDataSubsystem.add_new_subobject`** (attention : renvoie un `FText` truthy même vide → valider le composant, pas `if fail:`).
- **PIE / sous-niveaux streamés** : `BlocksSol2/L_T10_B_-1_-1|-1_0|0_-1|0_0` → mettre `initially_loaded=True` + `initially_visible=True` pour voir les **bâtiments en Play** (script `SOL2/fix_pie_sol2.py`). `should_be_visible_in_editor` n'existe pas sur `LevelStreamingDynamic` (warning inoffensif).
- Vérif visuelle : les captures async ne marchent pas en headless → **preuve par la géométrie** (compteurs, Z, résiduels) + ouverture GUI pour le rendu.

## Process dev (agents + supervision) — ce qui marche
- Sous-agents **Opus** pour les tâches lourdes ; **heartbeat + watchdog (Monitor) + autopsie** à la moindre stagnation ; sous-processus au **premier plan**.
- **Fermer l'éditeur CityLab** avant chaque agent headless ; **rouvrir après** sur la map pour l'utilisateur. Ne jamais fermer HELIOS/EnvolFlight/Survol/DroneFPV.
- Toujours **vérifier la compil du matériau herbe (log) AVANT de rouvrir l'éditeur** à l'utilisateur (sinon on remontre un damier).
- Petits fixes déterministes (PIE) : les faire **soi-même** en headless si l'agent stalle.

## Fichiers / chemins clés
- **Scripts** : `C:\LidarPoC\work\SOL2\` (gen_sol2/sol23, classify_place, fix_drape/fix_drape2, filter_trees, prep_sol24, fix_pie_sol2, `placement_v2.json`, `proto_capitole_surfaces_grass_v4.json`, `veg_bdtopo.json`, `heartbeat.txt`/`progress.log`) ; `C:\LidarPoC\work\E2SOL1\` (routes BD TOPO `proto_routes_bdtopo.json`, arbres LiDAR `proto_trees_lidar.json`, `veg_bouconne.json`).
- **PoC LiDAR** : `C:\LidarPoC\` (Roofer, 550 dalles métropole, venv laspy/shapely/pyproj, `work/AB/` = pipeline A/B).
- **MNT** : `CityLab/SourceData/toulouse10_mnt.png|.json` + `MNT/*.bil`.
- **Nouveaux assets utilisateur (à intégrer à la reprise)** : Megascans **Grass 4K** (+ 2K + variantes) téléchargés ; **`TemplateRessources\Content\Nature`** (3D pour remplacer le wheat grass). *(Confirmer les chemins de dézippage au retour.)*
