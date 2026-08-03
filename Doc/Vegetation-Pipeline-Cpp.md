# Pipeline végétation — placement en C++ (anti-flottement)

> **⚠️ AMENDEMENTS 2026-08-02 (lots V6/VÉLOCITÉ/PIE — ce doc reste valable pour la POSE,
> mais le régime de rendu/physique a changé)** : les touffes sont désormais **Nanite,
> SANS cull de distance** (l'ancien fondu 45-60 m est SUPPRIMÉ — constante à zéro à la
> création), toujours sans ombre, et **SANS collision** (règle data-driven : mesh sans
> primitive simple = NoCollision à la création ; les arbres gardent la leur). Le semis
> utilise **`AddInstances` par lots** (passe 3×3 : ~30-42 s pour 1,23 M d'instances).
> ⚠️ Le VENT ne souffle plus (WPO non branché — à rebrancher avec re-mesure VSM).
> Détails : `Doc/Reprise-Etat-Projet.md` §3 et Playbook §11-12.

> Écrit le 2026-07-30. **Raison d'être : tuer définitivement le flottement des arbres/haies/touffes.**
> Le placement de la végé passe désormais par le **générateur C++**, exactement comme les
> bâtiments (qui, eux, n'ont jamais flotté). La logique *data* (où va la végé) reste en prep JSON.

## 1. Le problème qu'on résout

Pendant 4 itérations, les arbres et l'herbe **flottaient** (posés près de l'origine, décrochés du
sol de plus en plus loin — jusqu'à des rangées entières flottant dans le ciel à ~500 m).

**Cause racine :** la végé était placée par un pipeline **Python** qui :
1. **ré-implémentait** le calcul de hauteur du sol (un modèle du MNT) — et cette ré-implémentation
   **diverge** du `TerrainSampler` C++ qui a réellement posé le sol (rebase, offset −0,5 px,
   indexation des dalles, flip Y : la moindre différence se **cumule avec le relief**) ;
2. ajoutait un **offset `min_Z` deviné** (le « point le plus bas du mesh »), faux pour certains
   meshes (ex. `EuropeanBeech_Forest_03`, bounds à −82 cm sous le pied visible → arbre soulevé).
3. « vérifiait » l'arbre contre **le modèle qui l'avait posé** → tautologie (« 0,00 cm » faux).

**Leçon durable :** quand un fix rate en boucle, suspecter une **vérif tautologique** (on mesure
contre le modèle qui a produit le résultat) et une **logique dupliquée** (Python qui refait le C++).

## 2. Pourquoi les bâtiments ne flottent jamais

Le C++ n'a **qu'une** autorité de hauteur, correcte :

```
GroundZ(Xcm, Ycm) = FTerrainSampler::AltCmAt(Xcm, Ycm) − AltCapitoleCm   // MNT bilinéaire, rebasé Capitole
```

Et les meshes sont **conçus base au Z local = 0** (bâtiments extrudés depuis 0 ; routes : `GroundZ`
cuit par sommet). **Base-à-0 + pose à `GroundZ` = posé pile sur le terrain, partout, sans snap ni
offset.** La hauteur terrain fait partie de la géométrie, elle n'est jamais re-calculée après coup.

## 3. L'architecture (deux temps, comme les bâtiments)

```
   [Prep DATA hors-ligne]                    [Import C++ dans l'éditeur]
   BD TOPO zones + LiDAR MNH      ─────►      UCityImportTools::ImportVegetation
   choix essence / échelle / Voronoi         lit veg.json → instancie à GroundZ, base-à-0
        │                                            │
        └──► veg.json  ───────────────────────────►  HISM par mesh (matériaux d'origine)
```

- **Prep (Python, hors C++)** : tout le crunching géospatial (fetch BD TOPO, parse LiDAR, Voronoi
  des haies, choix d'essence/échelle) — inhérent au hors-ligne, **pas la source du bug**. Sort un
  `veg.json` propre. (Même patron que les bâtiments : `Fetch-*.ps1` → `toulouse10_bati.json`.)
- **Placement (C++)** : `UCityImportTools::ImportVegetation` lit `veg.json` et pose les instances
  via `GroundZ`. **C'est ici, et seulement ici, que vit la correction du flottement.**

## 4. La règle de placement (le cœur du fix — version FINALE, 30/07 soir)

**⚠️ Historique important : la v1 posait à `GroundZ(X,Y)` analytique — INSUFFISANT.** Mesuré : le
`GroundZ` bilinéaire fin diverge de la **dalle telle que l'œil la voit** (rendu 64×64, ~7,8 m/sommet,
triangles linéaires ; collision 16×16 encore plus grossière) jusqu'à **±3,5 m** en zone pentue. Les
bâtiments encaissent cet écart grâce à leur **socle enterré** (`SocleCm`) ; un arbre n'a pas de partie
enterrée → queue de 5 % flottants/enterrés. Il existe en réalité **3 « sols » divergents** : GroundZ
analytique / dalle `SM_Slab` (rendu ≠ collision) / films `SM_Surface` (herbe-pavé, drapés fin).
**L'œil juge par rapport aux films et à la dalle RENDUE** → c'est ça, la référence.

**Règle finale : poser sur la SURFACE VISIBLE, mesurée par line-trace dans la passe C++ :**

1. **Proxys de trace jetables** : duplicata transient de chaque film `SM_Surface_*` **et** chaque dalle
   `SM_Slab_*` (Nanite off, `ComplexCollisionMesh` nul, `CTF_UseComplexAsSimple` posé **avant** tout
   usage physique, puis `Build()`), portés par des acteurs proxy détruits en fin de passe. Les
   originaux ne sont **jamais modifiés**. Les proxys dalle exposent le Z du **rendu 64×64** (celui
   que l'œil voit), pas le 16×16 de collision.
2. **Trace multi-hit** : descendre TOUTE la colonne (traverser toits/mobilier) et prendre le hit
   **sol** (`SM_Surface_`/`SM_Slab_`) le plus haut. `AddIgnoredActor` sur les acteurs de végé déjà
   posés (sinon les canopées absorbent les traces).
3. **Couronne de fosse** : si le hit central = dalle et ≥3 hits **film** en couronne au rayon fosse
   (`clamp(4;6, canopée×scale)+50 cm`, bounds du `UStaticMesh` — même formule que
   `gen_surfaces_v5.py`) → base à la **médiane des Z film**, bornée par la dalle (jamais sous le fond
   de fosse visible). Exclu pour les touffes (`Clump`).
4. Base-à-0 (pivot Megascans au pied), **zéro offset `min_Z`**. Repli `GroundZ` uniquement si aucune
   colonne de sol (position sous un bâtiment) — compté et logué.

**Interdits (ce qui causait les bugs) :** aucun modèle MNT ré-implémenté hors C++, aucun offset
`min_Z` deviné, aucune vérif contre le modèle qui a produit le placement (tautologie).
**Bonus :** meshes instanciés **tels quels** → matériaux d'origine conservés → zéro risque de damier
Virtual Texture.

Exposée en `UFUNCTION(meta = (AICallable))` → outil **MCP** `CityImportTools.ImportVegetation`.

**Résultat mesuré (30/07)** : enterrement médian +11,6 cm → **−0,1 cm**, P90 +35,4 → **+9,7 cm**,
>25 cm : 17,8 % → **2,3 %**, replis 9,8 % → **3,1 %** (les 396 restants = positions sous un bâtiment,
invisibles). Queue résiduelle >1 m (~12 arbres) = **authoring du parc en pente** (le film y passe SOUS
la dalle par bandes), pas un défaut de pose.

## 5. Le format `veg.json`

```json
{ "instances": [
  { "mesh": "/Game/NorwayMaple/Geometry/SimpleWind/SM_NorwayMaple_Field_01",
    "x": 60.2, "y": -21.3, "scale": 1.1, "yaw": 137.0 },
  { "mesh": "/Game/Megascans/3D_Plants/Elderberry/SM_Elderberry_03",
    "x": 88.4, "y": -12.0, "scale": 0.9, "yaw": 42.0 }
] }
```

- `x`, `y` en **mètres**, origine locale (Capitole). Le C++ multiplie par 100 et applique `GroundZ`.
- `mesh` = chemin d'asset complet. `type` implicite (arbre/haie/touffe = le mesh). Pas de `z` :
  **c'est le C++ qui le calcule** (c'est tout l'intérêt).

## 6. Itérer vite (le point clé pour ne pas repayer le cycle lent du C++)

Une fois le module **buildé une seule fois**, on ne rebuild presque jamais :

| Changement | Comment | Vitesse |
|---|---|---|
| **Data / paramètres** (densité, essences, échelle, positions) | régénérer `veg.json` + **rappeler `ImportVegetation` via MCP** sur l'éditeur ouvert | **secondes** |
| **Logique C++** (corps de fonction) | **Live Coding** (Ctrl+Alt+F11), éditeur ouvert | **secondes** |
| **Signature / struct reflété** (nouveau champ, nouvelle UFUNCTION) | **build complet** (fermer l'éditeur → `Build.bat`) | ~1 min, rare |

Clés : `ImportVegetation` est une **passe végé-seule** (efface + ré-instancie *juste* la végé, pas
les bâtiments/routes) et tourne sur le **proto** (`L_ProtoSols`), pas la ville entière. Même un
« tout refaire » = secondes. **Fini les agents Python qui snappent 35 min.**

## 7. Commandes

- **Build C++** (fermer l'éditeur CityLab d'abord — nouvelle UFUNCTION = pas de Live Coding) :
  `Build.bat CityLabEditor Win64 Development -project="…\CityLab.uproject" -WaitMutex -NoHotReloadFromIDE`
- **Rejouer la végé** (éditeur ouvert) : outil MCP `CityImportTools.ImportVegetation(VegJsonPath,
  AssetFolder, Location, Profile)` — Profile Desktop, `bDrapeToTerrain=true` (défauts MNT
  `SourceData/toulouse10_mnt.png/.json` = ceux du proto → `GroundZ` identique aux bâtiments).

## 8. Pièges

- **Nouvelle UFUNCTION ⇒ build complet** (Live Coding ne gère pas les fonctions reflétées ajoutées).
- **Cibler l'éditeur par titre/cmdline `CityLab`** pour le fermer — jamais HELIOS/EnvolFlight/Survol/DroneFPV.
- **`MATUSAGE_InstancedStaticMeshes`** : appeler `CheckMaterialUsage` sur les matériaux instanciés
  sinon rendu en défaut (piège F.39, déjà payé sur les arbres C++).
- **Ne jamais réintroduire d'offset `min_Z` ni de snap Python.** Si un mesh flotte, c'est son
  **pivot** qui n'est pas au pied — corriger l'asset, pas ajouter un offset.
- **Cuisson de collision (piège UE payé cher)** : sur un mesh DÉJÀ construit,
  `CollisionTraceFlag + InvalidatePhysicsData + CreatePhysicsMeshes` ne re-cuit **PAS** le trimesh
  complex-as-simple (échec **silencieux** : 0 hit). Il faut un **`Build()`** du mesh — d'où les
  duplicatas transient construits avec le bon flag (les originaux restent intacts). Idem côté
  Python : `set_editor_property('collision_trace_flag')` seul ne suffit pas.
- **`GroundZ` analytique ≠ dalle rendue** (±3,5 m en pente) : ne JAMAIS poser un objet sans socle
  enterré à `GroundZ` — tracer la surface visible.

## Voir aussi
`CityImportTools.cpp` (`ImportCityStreamed`/`ImportCitySurfaces` = mêmes conventions ;
`FDrapeContext::GroundZ`) · `Doc/Reprise-Sol-Vegetation-Sol2.md` (état végé) ·
`Doc/Donnees-Sources-Catalogue.md` (sources BD TOPO/LiDAR).
