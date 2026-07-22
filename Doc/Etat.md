# CityLab — état du labo

Labo de bâtiments procéduraux pour DroneFPV. **Rien ici n'est shippé** : le pipeline
validé sera porté dans DroneFPV (`Source/DroneFPVEditor` + Migrate des assets retenus).

## Objectif en cours

**Test « bâtiment héros » palier 2** : prouver qu'un générateur C++ paramétrique
(fenêtres en creux, corniche, parapet, ombrage cuit en vertex colors) produit une
qualité visuelle suffisante pour un quartier FPV, dans le contexte de rendu DroneFPV
(unlit, LDR, zéro bake). Verdict utilisateur sur screenshot éditeur (preview ES3.1)
puis capture Redmi. Si KO → pivot plan B (Blender headless blosm+Buildify + décimation).

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

## Fait

- 2026-07-22 : scaffolding du projet (modules, toolset, config, git). Voir git log.
- 2026-07-22 : pipeline de bout en bout PROUVÉ : GenerateBuilding (façades, fenêtres
  en creux, corniche, parapet, vitrines RDC) + matériaux unlit créés par MCP +
  captures/mesures pixel automatisées. Reste le verdict visuel en ES3.1.

## Reste / prochaines étapes

1. Premier bâtiment héros dans L_BuildingTest, matériaux unlit (VertexColor × texture
   de façade), itération visuelle via MCP.
2. Collision simple (boîte) sur le mesh généré — nécessaire pour le vol FPV à terme.
3. Variation par graine (hauteurs, textures, retraits d'étage) puis rangée de bâtiments.
4. Si palier 2 validé : pipeline Overpass API → footprints réels → rue entière.
