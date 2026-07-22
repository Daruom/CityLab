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

## Fait

- 2026-07-22 : scaffolding du projet (modules, toolset, config, git). Voir git log.

## Reste / prochaines étapes

1. Premier bâtiment héros dans L_BuildingTest, matériaux unlit (VertexColor × texture
   de façade), itération visuelle via MCP.
2. Collision simple (boîte) sur le mesh généré — nécessaire pour le vol FPV à terme.
3. Variation par graine (hauteurs, textures, retraits d'étage) puis rangée de bâtiments.
4. Si palier 2 validé : pipeline Overpass API → footprints réels → rue entière.
