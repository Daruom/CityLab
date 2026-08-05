# Agent Playbook — CityLab / CityLab_Batch

> **LECTURE OBLIGATOIRE avant toute action.** Ce doc remplace les listes de pièges retapées dans
> chaque brief. Tout y est : accès MCP, méthodes validées, pièges payés, conventions de supervision.
> Un brief d'agent = mission + contexte spécifique + « lis Agent-Playbook.md » — rien de plus.
> (Écrit le 2026-07-31, consolidé depuis ~20 lots d'agents. Màj par le coordinateur.)

## 0. Le skill Claude Code
Si ton environnement expose le skill **`unreal-engine-skills-for-claude-code:unreal-mcp`** (vérifie
ta liste de skills), **charge-le en premier** pour piloter l'éditeur : il documente l'usage canonique
du serveur MCP Unreal. Les helpers ci-dessous restent la référence pour ce qui est spécifique projet.

## 1. Environnement — DEUX projets, DEUX éditeurs, ciblage par CHEMIN
| | MAÎTRE | FERME |
|---|---|---|
| Chemin | `C:\Users\User\Documents\Unreal Projects\CityLab` | `...\CityLab_Batch` |
| Port MCP | **8101** | **8107** |
| Rôle | protos, itérations, la seule vérité (git) | générations longues, `Content/Generations/T10_vN` versionnées, JAMAIS de commit |

⚠️ **Les deux éditeurs s'appellent `CityLab.uproject`.** Avant tout kill/action process : filtre la
cmdline — `CityLab_Batch` = ferme ; `CityLab\` sans `_Batch` = maître. **NE JAMAIS toucher** aux
éditeurs HELIOS / EnvolFlight / Survol / DroneFPV. Sync ferme : `CityLab_Batch\sync_from_master.ps1`
(sens unique, refuse si éditeur ferme ouvert, préserve `Generations\`).

## 2. Accès MCP (mode tool-search)
Le serveur n'expose que 3 méta-outils : `list_toolsets` / `describe_toolset` / `call_tool`
(`{toolset_name, tool_name, arguments}`). Protocole HTTP : `initialize` → header `Mcp-Session-Id` →
`notifications/initialized` → `tools/call`. Réponses = SSE `data:` avec JSON **échappé**.
**Helpers prêts (ne pas réécrire)** : `C:\LidarPoC\work\SOLVERT\lot2_call.ps1` + `lot2_py.ps1`
(vérifie le mode de console avant soumission) · `C:\LidarPoC\work\FERME\mcp_call/desc/tool.ps1` ·
extraction d'image = plus long run base64 de la réponse.
**Pièges PowerShell 5.1** : `ConvertTo-Json` explose (13,6 Go RAM) sur gros payloads → concaténation
manuelle ; `$Args` est une variable AUTOMATIQUE (renomme tes paramètres) ; pas de `&&`.

## 3. Exécuter du Python dans l'éditeur ouvert
- **Méthode fiable : le guetteur de fichiers** (`C:\LidarPoC\work\LIDARC\install_watcher.py` +
  `run.ps1`/`lidarc_submit.ps1`) : dépose `queue/*.py`, exécution au tick, `.done` en retour.
- ⚠️ **Le guetteur NE SURVIT PAS à un redémarrage d'éditeur.** Il se réinstalle par MCP :
  `& C:\LidarPoC\work\SOLVERT\lot2_py.ps1 -PyFile C:\LidarPoC\work\LIDARC\install_watcher.py`.
  Sans ça, `run.ps1` sort en TIMEOUT sans aucun autre symptôme (payé le 01/08).
- Console de barre d'état : possible MAIS vérifier qu'elle est en mode **Python** (mode « Cmd » →
  dialogue MODAL invisible qui gèle le thread de jeu ET le MCP ; diagnostic : capture PrintWindow).
- **JAMAIS `time.sleep()`** dans la console (gèle le jeu → mesures de streaming fausses).
- **StopPIE avant toute sauvegarde** (PIE fait échouer `exists`/`save` en silence).
- Overrides Python (`set_material`…) ne salissent PAS le paquet → `component.modify(True)` +
  `actor.modify(True)` avant save. Un paquet sali ne se dé-salit pas → **dupliquer AVANT** d'expérimenter.
- Python UE 5.8 : indexer un tableau de structs rend une COPIE ; pas de `mark_render_state_dirty` ;
  HISM persistant via `SubobjectDataSubsystem` ; PIL vendorisée = cp313 ≠ py3.11 → stdlib.

## 4. Captures & validation (pack standard)
- `EditorAppToolset.CaptureViewport` : `captureTransform` ET `annotations` **obligatoires** même si
  « optionnels » (grille/labels à 0 pour une capture propre).
- ⚠️ **`CaptureViewport` NE DÉPLACE PAS la caméra du viewport** (payé le 01/08, lot v3) : il rend
  hors champ avec un transform temporaire. Toute mesure « à la pose courante » (`stat`,
  `ListStreamingTextures`, `NaniteStats`) doit donc passer par `set_level_viewport_camera_info`
  d'abord — sinon on mesure la pose PRÉCÉDENTE.
- ⚠️ **Après une régé, la première capture est FLOUE** (TAA non convergé, streaming non stabilisé)
  → toujours une passe `_WARM` jetable avant la passe `APRES`, supprimée après usage.
- **Mesurer un pop-in / un streaming se fait EN MOUVEMENT**, jamais sur pose statique : une pose
  statique laisse converger tous les systèmes à latence et ne montre plus rien. Rig prêt à
  réutiliser : `work/FINITION_SOL/v4_vol.py` — un callback `register_slate_post_tick_callback`
  pilote `set_level_viewport_camera_info` le long d'un axe, `HighResShot 1` aux distances voulues,
  et fait DEUX passes (STAT parkée + convergence, VOL à 20 m/s). L'écart STAT/VOL à distance égale
  isole les systèmes à latence ; l'absence d'écart prouve que le phénomène est purement fonction
  de la distance (mips / LOD / cull).
- Caméras de référence sauvegardées dans les `.meta.txt` de `C:\LidarPoC\work\SOLVERT\` — réutilise
  les MÊMES poses pour tout avant/après.
- Mouvement (vent…) : séries de ≥8 images à intervalles IRRÉGULIERS (l'intervalle régulier fait de
  l'aliasing) + série de contrôle ; plancher de bruit TAA ≈ 30/255.
- Matériaux : le headless ne compile PAS SM6 → valider en GUI + grep log (`Failed to compile`,
  `Sampler type`). Cohérence VT↔samplers obligatoire. En cas d'échec sans texte d'erreur : ouvrir le
  matériau EN GUI et lire l'erreur (ne pas bisecter en aveugle >15 min).
- **AUTO-VALIDATION : regarde tes captures et REJETTE ta propre version si elle ne convainc pas.**
- ⚠️ **MESURER LE COÛT GPU : une seule méthode marche** (établie le 01/08, lot V6).
  Le temps de frame relevé dans un callback de tick est **inutilisable** : il vaut
  **8,333 ms exactement** (120 Hz) à *toutes* les poses, du ras du sol au zénith, et
  `r.VSync 0` / `t.MaxFPS 0` n'y changent rien — c'est un **plafond**, pas un coût.
  Ce qui marche : `ProfileGPU` **avec l'éditeur au premier plan** (`work/FINITION_SOL/v6_focus.ps1`,
  `AttachThreadInput` + `SetForegroundWindow`), puis lire la ligne **« Frame Time »
  du pipeline GRAPHIQUE** dans `Saved/Logs/CityLab.log` (parseur prêt :
  `work/FINITION_SOL/v6_parse_gpu.py`). Toujours ajouter une **pose témoin** qui ne
  dessine presque rien (caméra au zénith à 3 km) : elle donne le plancher de bruit
  (mesuré ±0,3 ms) et prouve que la mesure discrimine. Sans focus, `ProfileGPU`
  profile une frame d'interface — c'est le piège v4 ci-dessous.
- ⚠️ **UN ÉDITEUR NON FOCALISÉ NE REND PAS** (payé trois fois le 01/08). Symptômes :
  delta-time clampé à **125,000 ms** exactement ; `ProfileGPU` profile une frame
  d'**INTERFACE** (mêmes draws/primitives au ras du sol et à 150 m d'altitude) ; la spec
  d'automation reste bloquée sur `FWaitForInteractiveFrameRate` (« Current FPS=3 »,
  timeout 600 s). `t.IdleWhenNotForeground 0` et `EditorPerformanceSettings` **ne
  suffisent pas**. Remède : remettre la fenêtre au premier plan (`ShowWindow` +
  `SetForegroundWindow` par P/Invoke) avant toute mesure de perf ou de rendu.
  Corollaire : préférer quand c'est possible une métrique qui ne dépend pas du rendu
  (comptage d'instances dans la portée × triangles, plutôt que des ms).
- ⚠️ **`unreal.AutomationLibrary.take_high_res_screenshot` ne délivre AUCUN fichier**
  appelé depuis un callback de tick (26 requêtes, 0 PNG, y compris en Simulate). Pour
  capturer la pose courante sans transform d'override : `CaptureViewport` accepte
  d'**omettre** `captureTransform` (il capture alors la caméra du hublot).
- ⚠️ **Les noms de COMPOSANT ne sont uniques que DANS un acteur.** Indexer des
  composants par `c.get_name()` dans un dict global l'effondre silencieusement (payé le
  01/08 : les 12 HISM de touffes se sont retrouvés sur le même maillage). Indexer par
  label d'acteur, ou par `(acteur, composant)`.

## 5. Builds & cycle éditeur (atelier permanent)
> ⭐ **Lis d'abord le §11 « LA BOUCLE D'ITÉRATION PAR DÉFAUT »** : mode district, règles Live
> Coding et chronos attendus. Ce §5 reste la référence des pièges de cycle.
- Objectif : **1 redémarrage d'éditeur max par lot.** Données/matériaux/config → MCP live.
- Build : fermer l'éditeur (par CHEMIN), `Build.bat CityLabEditor Win64 Development -project="…\CityLab.uproject" -WaitMutex -NoHotReloadFromIDE` (~12-20 s), rouvrir via **PowerShell**
  (Git Bash mangle `/Game/...`). Corps de fonction → Live Coding OK pour itérer, mais **JAMAIS
  exécuter une passe lourde sous patch Live Coding** (crash D3D12) : build complet avant le run.
- ⚠️ **La spec d'automation `CityLab` RÉGÉNÈRE LE MONDE COURANT** (payé le 01/08, lot V5) :
  ses tests appellent `ImportCityStreamed`, qui **purge tous les acteurs `SM_Ground_` /
  `SM_Slab_` / `SM_Proxy_` / `SM_Bldg_` du monde ouvert** avant de poser les siens. Lancée
  avec une map de production ouverte, elle la remplace **en mémoire** par la maquette de test.
  Ce n'est sans conséquence que si l'on **ne sauvegarde pas** ensuite. Règle : ouvrir une map
  jetable **avant** `Automation RunTests CityLab`, puis recharger la map de travail sans
  sauvegarder. Idem pour toute sonde qui rejoue un import.
  **V6** : la spec écrit ses assets dans `/Game/Dev/Test/` — dont les noms sont
  **homonymes** des assets réels (`SM_Bldg_0_0`, `SM_Ground_0_0`, `SM_Slab_0_0`, et même
  les sous-niveaux `L_T10_B_0_0`), ce qui a déjà brouillé des diagnostics. Ce dossier a
  été **supprimé** ; la spec le recrée. Donc : **session dédiée, OU purge de
  `/Game/Dev/Test` obligatoire après la spec**, et jamais de sauvegarde de la map de
  travail entre les deux. Purge en deux temps (payé le 01/08) : les assets de la spec
  restent **référencés par son monde** tant qu'il est ouvert — recharger d'abord la map
  de travail, **puis** supprimer, puis vérifier **sur le disque** (le registre peut
  annoncer 0 asset alors que des `.umap` orphelins subsistent).
  ⚠️ **Ouvrir la map jetable DEPUIS une map lourde coûte le teardown de la map lourde**
  (10 min mesurées sur le proto 3×3 à 1,2 M d'instances) : lancer la spec dans une
  session **démarrée** sur une map légère, pas par bascule depuis la production.
- **Un processus UE par passe de génération lourde** (commandlet), reprenable par lot de cellules.
- Crash : `Saved/Crashes/*/CrashContext.runtime-xml` → `IsEnsure=true` = NON-fatal (l'éditeur vit).
- **REDÉMARRER L'ÉDITEUR : trois pièges payés le 2026-08-01, dans cet ordre.**
  1. `Start-Process -ArgumentList @($uproject, $map)` **ouvre le PROJECT BROWSER** : le chemin
     contient des espaces et le tableau ne le requote pas. Passer **UNE seule chaîne** avec le
     `.uproject` entre guillemets internes. Symptôme : 3,5 Go de RAM, aucun `Saved/Logs/*.log`
     neuf, titre de fenêtre « Unreal Engine 5.8 ».
  2. Après un `Stop-Process`, la modale **« Restaurer les paquets »** bloque le démarrage AVANT
     le MCP (donc injoignable en MCP). Diagnostic : titre de fenêtre = `Restaurer les paquets` ;
     remède : `PrintWindow` pour lire le dialogue puis clic natif sur **« Ne pas restaurer »**.
     Précisions payées le 01/08 : `FindWindow(null, titre)` **ne la trouve pas** (titre Slate non
     indexé) → passer par `Get-Process | MainWindowTitle` puis `MainWindowHandle` ; et le bouton
     « Ne pas restaurer » n'est **pas une fenêtre enfant** (Slate) → lire la capture `PrintWindow`
     et cliquer aux coordonnées. Outillage prêt : `work/FINITION_SOL/clic_dialogue.ps1` + `clic_xy.ps1`.
  3. **L'HABILLAGE DESKTOP N'EST PAS PERSISTÉ** (`CitySun`/`CitySkyLight`/`CitySkyAtmosphere`/
     `CityFog`, cf. `work/SOL2/gen_sol2.py`) : il disparaît à chaque rechargement, le monde
     rend alors **entièrement NOIR** et toute capture est un carré noir de ~15 ko. Vérifier
     `get_all_actors_of_class(world, unreal.Light)` **avant** toute campagne de captures ;
     respawn = `work/SOLROUTES/v2_habillage.py` (soleil pitch −35 / yaw 45 — mêmes valeurs que
     toutes les captures de référence, sinon l'A/B est faussé).

## 6. Règles de génération (résumé — détails dans `Doc/Vegetation-Pipeline-Cpp.md` & `Doc/Reseau-Sidecar.md`)
- 🔒 **UNE GÉNÉRATION SE VALIDE PAR SES GÉOMÉTRIES — aires, boucles, retraits — JAMAIS
  par ses comptes ou ses positions** (doctrine du 01/08, lot V5, payée deux fois).
  Un compte d'acteurs, un `Buildings=1657` ou un « bâtiments identiques à 0,000 m » sont
  aveugles **par construction** aux deux défauts qui coûtent le plus cher : une **cour
  bouchée** et une **emprise gonflée** ne changent ni le nombre, ni la position. Les
  métriques qui, elles, les voient : **aire d'emprise projetée** (somme des aires XY des
  faces non verticales du mesh — comparée à l'aire des contours MOINS les trous du JSON),
  **nombre de boucles intérieures percées** (une sonde au centre de chaque cour : rien
  au-dessus = percée), **retraits** (distance mesurée au tracé d'origine).
  Outillage prêt : `Tools/verrou_batiments_sol2.py` (à rejouer après CHAQUE régé du proto ;
  valeurs de référence en dur, mesurées sur le témoin gelé Sol1) et les deux specs
  `CityLab.CityImportTools` « V5 VERROU GÉOMÉTRIQUE » / « V5 VERROU PROXY ».
  > **Post-mortem (comment ça a survécu à N lots).** Le 01/08 l'utilisateur signale
  > « cours intérieures bouchées, emprises gonflées qui débordent sur le sol, c'est une
  > ancienne construction ». La cause n'était **pas** la géométrie des bâtiments (mesurée
  > depuis : les `SM_Bldg_*_Wall` de Sol2 sont **bit-identiques** à ceux du témoin gelé
  > du 29/07, même hash de sommets, et leur emprise projetée vaut **exactement** l'aire
  > nette du JSON). C'étaient les **proxys** `SM_Proxy_*` — blocs grossiers de la couche
  > résidente, cours pleines par construction — **sauvés VISIBLES** et superposés au
  > détail : 96 % de la surface des cours remplie, 24 858 m² débordant hors de toute
  > emprise. Pourquoi personne ne l'a vu : (1) une douzaine de scripts de génération les
  > cachaient **en Python après la passe**, donc **après** la sauvegarde que le C++ fait
  > lui-même — la correction ne vivait qu'en mémoire et mourait à la fermeture de
  > l'éditeur ; (2) toutes les captures de validation étaient prises **dans la session
  > qui venait de générer**, où les proxys étaient cachés : l'agent voyait juste, le
  > disque était faux, et l'utilisateur — qui rouvre le projet — voyait le disque ;
  > (3) le seul contrôle de non-régression bâtiments comparait des **positions**.
  > Correctif : `FCityGenProfile::bProxyVisible` (défaut **false**) posé **à la création
  > de l'acteur** dans `ImportCityStreamed`. Corollaire général : **tout ce qu'un script
  > rejoue à la main après une passe doit migrer dans le C++** — une propriété par objet
  > ne survit ni à sa recréation, ni à une sauvegarde faite avant elle.
- 🔒 **DOCTRINE DU SOL RENDU (généralisée le 01/08, lot v4)** : **toute géométrie posée sur le sol
  échantillonne la surface RENDUE, jamais le terrain abstrait.** C'est la leçon des arbres,
  industrialisée : la dalle est drapée sur une grille (`GroundGridN`, interpolation triangulée) et
  tout ce qui prétend s'y poser en lisant directement le MNT (ou pire, un Z constant) s'enterre ou
  lévite dès qu'il y a du relief. Un seul helper C++ fait foi (`SampleRenderedGroundZ`, cf.
  `CityImportTools.cpp`) et TOUS les poseurs l'appellent (bordures de chaussée, bordurettes
  d'herbe, marquages, fosses…). Symptôme quand la règle est violée : pierres à moitié enterrées
  sur les rues en pente, alors que la même pierre est correcte sur le plat.
- Pose végé : **trace de la surface RENDUE uniquement** (proxys dupliqués+Build() ; les meshes de
  collision 16×16 d'origine sont EXCLUS du trace) ; base-à-0, zéro offset `min_Z` ; pas de sol → skip
  compté ; jamais un modèle analytique (GroundZ) pour un objet sans socle enterré.
- ⚠️ **`shapely` peut rendre une `GeometryCollection`** là où on attend un polygone (deux polygones
  qui se touchent par un seul point) : `.boundary` y est alors VIDE. Tout parcours de contour passe
  par `polygones()` / `contour_de()` de `j3c_sols_masks.py`. Symptôme payé le 01/08 : une cellule
  de 10 417 m² d'herbe mesurée avec « 0 m de contour ».
- ⚠️ **Une opération morphologique se BORNE au voisinage de ce qu'on garde.** L'accostage des
  façades (v2) coûtait 6,5 s pour UNE cellule parce que la fermeture portait sur l'union des 2 611
  emprises bâties de la fenêtre ; bornée au voisinage de l'herbe (4 m) elle est exacte pour ce
  qu'on garde et le bake retombe de 30,5 s à 14,7 s. Corollaire de règle : un accostage annoncé
  à 2 m ne doit pas produire de langue à 50 m.
- Cuisson collision : `InvalidatePhysicsData+CreatePhysicsMeshes` ne re-cuit PAS → `Build()`.
- **CHECKLIST POST-RÉGÉ streamed (`ImportCityStreamed`)** — ALLÉGÉE le 31/07 (lot A-ter) :
  les deux points qu'on rejouait à la main sont désormais posés **par le C++ à la CRÉATION**
  (`CityImportTools.cpp` : `bInitiallyLoaded/bInitiallyVisible = true` sur les streaming levels
  — le piège PIE payé 3 fois ; `ApplyGroundTextureStreaming()` = `StreamingDistanceMultiplier
  = 100` sur `SM_Slab_*`/`SM_Ground_*`/`SM_Proxy_*`/`SM_Surface_*`). Ce sont des propriétés
  PAR OBJET : posées après coup elles ne survivent pas à la régé suivante — d'où le C++.
  **Ne plus lancer `fix_pie_sol2.py` ni le script SDM du lot 3** ; les VÉRIFIER suffit
  (`work/SOLROUTES/ater_regen.py` fait la régé et les deux vérifications, `flags_ok`/`sdm_ok`).
  **V6 (01/08, décision utilisateur) : LA COUCHE PROXY EST SUPPRIMÉE.** Elle est
  obsolète sur desktop — Nanite rend le détail à la densité de l'écran et le streame à
  la demande, et nos maillages sont fusionnés par cellule (surcoût en composants
  trivial). `FCityGenProfile::bProxyLayer` (défaut **false**) : la géométrie proxy n'est
  plus **construite** du tout ; `true` restitue l'ancienne couche, visible. Si des
  silhouettes lointaines redeviennent nécessaires à l'échelle de l'agglo, la réponse est
  le **HLOD d'UE**, qui les génère correctes (toits compris) au lieu de blocs à cours
  pleines. Le verrou n'est donc plus « les proxys sont cachés » mais **« aucun proxy
  n'existe »** (spec `V6 VERROU PROXY` + `Tools/verrou_batiments_sol2.py`). *(V5 avait
  posé `bProxyVisible=false` à la création, après que douze scripts les aient cachés en
  Python APRÈS la sauvegarde faite par la passe : le disque gardait des proxys visibles.)*
  Le streaming de **collision** par distance reste un sujet ouvert du lot perf, sans rapport.
  Restent obligatoires après chaque régé : `ImportCitySurfaces` avec le **NOGREEN**
  (`SOLVERT/proto_capitole_surfaces_nogreen.json`, JAMAIS `grass_v3` — 1 407 films verts
  revenus le 31/07), re-run `ImportVegetation`, re-masquer l'ancienne végé `Sol2Veg_*`/
  `Sol2Grass_*`, comparer les compteurs (CurbQuads / AxialDashes / MaskedCells / instances
  / fosses) au lot précédent, et **jouer le verrou géométrique
  `Tools/verrou_batiments_sol2.py` (doit sortir `VERROU: PASS`)**.
- Masques de sol : canal G = SDF chaussée (sert aussi de champ de rétraction) ; l'herbe = complément
  du peint. `SaveStringToFile` bascule UTF-16 sur tiret cadratin → `ForceUTF8WithoutBOM`.
- **La CHARTE du sol (lot A-ter, `Doc/Sols-Masques-LotA.md` §6)** : la composition est un ARGMAX
  sur une carte de matériaux (zéro chevauchement par construction), plancher de motif 5 m²,
  catalogue FERMÉ de transitions (fil d'eau, rive de façade, bordure 3D), budget ≤ 3 matériaux
  par disque de 20 m. Ne pas ré-ouvrir ces choix sans mesurer. **Le masque cuit fait 48,83 cm
  par texel : aucun objet plus fin que ~55 cm n'est représentable** (il sort en pointillé) ;
  tout objet fin exige la compensation +w/4 avant la réduction 2×2.
- Données : tout est SUR DISQUE (`SourceData\`, `SourceData\Agglo\`, LiDAR `C:\LidarPoC\`) — zéro
  fetch réseau en lot. `data.geopf.fr` : 1 req/s, 429 = page HTML silencieuse de 134 octets.
- ⚠️ **Cuire des cellules à indice NÉGATIF ne passe pas par la ligne de commande**
  (payé le 01/08) : `j3c_sols_masks.py --cells -3,-2` fait échouer argparse (son
  détecteur de nombre négatif est `^-\d+$`, et « -3,-2 » n'y répond pas), et le chemin
  du projet contient des espaces (`Start-Process -ArgumentList @(…)` ne les requote pas
  — même piège que la relance de l'éditeur). Passer par un **wrapper** qui fournit la
  liste de cellules directement à `main()` : `work/FINITION_SOL/v6_bake.py`.
- ⚠️ **Un verrou à valeurs de référence EN DUR meurt au changement d'échelle.**
  `Tools/verrou_batiments_sol2.py` compare désormais, cellule par cellule, l'emprise
  **mesurée** à l'aire nette **du JSON qui a servi à générer** (contours moins trous) :
  ce critère est vrai à toutes les emprises. Les valeurs en dur du témoin gelé Sol1 ne
  sont vérifiées que si c'est bien son extrait de 550 m qui a servi — sinon les mêmes
  cellules reçoivent en plus les bâtiments qui tombaient hors du rayon d'extraction, et
  le verrou crierait sur une différence LÉGITIME.

## 7. Supervision (OBLIGATOIRE)
- **Heartbeat** horodaté dans le dossier de travail du lot, à CHAQUE étape **y compris la prep**
  (progression interne : « cellules 132/400 ») + `progress.log` + pour les campagnes un `ETAT.md`
  de reprise (fait/restant/procédure).
- Watchdog interne : run figé >10-15 min sans progression → tue + autopsie du log, PAS de relance aveugle.
- Si tu dois attendre un processus détaché long : termine ton tour proprement (ETAT.md à jour) —
  le coordinateur te réveillera à la fin du processus.
- **RÈGLE DE GEL DES CAMPAGNES (leçon T10 v1)** : une campagne sur la ferme photographie le maître
  à l'instant du sync — si un lot itère sur le proto en parallèle, la campagne diverge PAR
  CONSTRUCTION. Avant toute campagne : consigner dans l'ETAT.md les **md5 de l'outillage**
  (scripts de prep, cpp/DLL, masques) et vérifier qu'aucun lot éditeur maître ne modifiera ces
  fichiers pendant le run — sinon, séquencer (lot d'abord, campagne ensuite).

## 8. Périmètre & sécurité
- Maps protégées (md5 à vérifier avant/après) : `_E2` `6942C9D6…`, `_Sol1` `4C67BBD8…`,
  `L_ProtoSols_LIDAR` `B71B0E40…`. Ancienne végé `Sol2Veg_*`/`Sol2Grass_*` : masquée, NE PAS supprimer.
- Backup `.bak` daté avant TOUTE modif de fichier (cpp, ini, masters matériaux, veg.json).
- **Aucun commit git** (seul le coordinateur committe). La ferme est hors git.
- N'écris que dans ton périmètre déclaré + `C:\LidarPoC\work\<TONLOT>\`.

## 9. Rédaction des rapports
Chiffré, avant/après, captures commentées, incidents+causes, intégrité confirmée. **Les prémisses du
brief sont des HYPOTHÈSES : falsifie-les par la mesure avant d'implémenter** — 3 briefs sur 20 ont
contenu une prémisse fausse ; les agents qui l'ont détectée ont évité des correctifs erronés.

## 10. Pièges du 02/08 (lots OMBRES + DISCONT C1)
- **Pool Nanite : STRICTEMENT < 2048 Mo.** `r.Nanite.Streaming.StreamingPoolSize 2048` = assert
  FATAL (le plafond matériel EST 2048 et la valeur doit lui être inférieure).
- **La file du guetteur SURVIT au redémarrage d'éditeur** : vider `C:\LidarPoC\work\LIDARC\queue`
  avant de réinstaller, sinon une vieille campagne se rejoue seule (sans focus → frames d'interface).
- **Un marqueur de log doit porter son TAG** dans le parseur — sinon on dépouille la mauvaise
  campagne. Dépouiller chaque campagne immédiatement après son run.
- **Meshes Nanite invisibles aux mesures Python** : `get_num_triangles(0)` rend le repli décimé,
  `get_section_from_static_mesh` rend vide. Contrôle géométrique fin = spec d'automation C++
  (maillage source) + captures reprises après redémarrage pour la preuve disque.
- **Teardown de la map lourde (196 Mo)** : `LogExit: Editor shut down` puis processus qui tourne
  (cœur 100 %, RAM plate) — le `.umap` n'est PAS réécrit, kill sans risque après 90 s de grâce
  (`c1_cycle.ps1` : 300 s → 108 s). **Astuce : basculer sur une map LÉGÈRE avant tout cycle
  d'éditeur** (fermeture en 11 s).
- **`RaiseError` dans une passe = échec d'automation** pour toute spec qui importe avec d'autres
  paramètres : les refus non-fatals se journalisent en **Display**, une ligne par passe.

---

## 11. ⭐ LA BOUCLE D'ITÉRATION PAR DÉFAUT (lot VÉLOCITÉ, 02/08 — **tout est mesuré**)

> **C'est la nouvelle façon de faire par défaut** (décision utilisateur). Une régé complète du
> proto 3×3 coûtait ~32 min ; elle en coûte **3** — et l'itération courante, celle qu'on rejoue
> vingt fois par après-midi, coûte **≈ 1 à 2 min**. Ne régénère plus la ville entière pour
> regarder un quartier.

### 11.1 Le mode district — `FCityGenProfile::CellFilter`
```
CellFilter="-2_0,-2_1,-1_0,-1_1"   CellFilterSizeM=500.0
```
Vide (défaut) = comportement historique, ville entière, **bit pour bit**. Renseigné, les trois
passes (`ImportCityStreamed`, `ImportCitySurfaces`, `ImportVegetation`) ne traitent que ces
cellules : purge d'idempotence **bornée** aux acteurs de ces cellules, masques/murs/bâtiments/
routes filtrés, et **seuls les sous-niveaux qui les portent** sont chargés, remplis et sauvés.
* **Choisis un district qui tient dans UN bloc de streaming** (`BlockSizeM`, 1 km = 2×2 cellules
  de 500 m) : un seul `L_T10_B_*` à charger et à resauver.
* `ImportVegetation` n'a pas de paramètre `CellSizeM` : elle **exige `CellFilterSizeM`**, sinon
  elle ignore le filtre et le dit en Display (une passe qui se croirait filtrée coûterait 30 min).
* Les compteurs du résumé portent alors sur les **cellules visées**, pas sur la ville : une
  comparaison de non-régression se fait sur une régé **complète**.
* Incompatible avec `bProxyLayer` (autre maille) : le filtre l'emporte, avec une ligne de log.

### 11.2 ⚠️ NE REJOUE PAS LA VÉGÉTATION SI LE SOL N'A PAS BOUGÉ — c'est LE poste dominant
La végé trace `SM_Surface_`/`SM_Slab_`, que la passe streamed reconstruit **à l'identique** :
tant que les masques, le MNT et le JSON végétal n'ont pas changé, les instances déjà posées
restent valides (vérifié : `veg_delta = 0` à chaque run filtré). Mesure sur le district de
4 cellules :

| | passes | queue de travail **différé** de l'éditeur ensuite |
|---|---:|---:|
| district **sans** végétation | **14,6 s** | **2,0 s** |
| district **avec** végétation | 39,2 s | **~640 s** (10 min 40) |

La queue n'est pas un artefact : le log moteur montre **31 ticks en 10,5 min**. Elle vient de la
reconstruction des arbres spatiaux HISM et du ré-upload des 1,25 M d'instances (le mode district
vide puis repose chaque HISM touché — et l'herbe est partout). C'est ce que la passe `_WARM` des
captures absorbe, d'où des captures à **353 s** juste après une régé végétation contre **61 s**
sur un éditeur stabilisé.
→ **Règle** : `ImportVegetation` seulement quand le sol a bougé (masques recuits, MNT, veg.json).
→ Levier futur si ça gêne : découper les HISM de végétation **par cellule** (ou par bloc) au lieu
d'un HISM global par mesh — le district ne rebâtirait alors que sa part.

### 11.3 Live Coding — le cadre, et ce qui est vraiment disponible
* **Corps de fonction / `.cpp` uniquement.** Tout changement de LAYOUT (UCLASS/USTRUCT, headers,
  UHT) reste interdit : cycle complet scripté.
* ⚠️ **Le toolset MCP `LiveCodingToolset.CompileLiveCoding` N'EXISTE PAS** sur ce serveur
  (`list_toolsets` fait foi). La voie qui marche : la **commande console `LiveCoding.Compile`**,
  jouée par le guetteur — **asynchrone**, verdict à lire dans `Saved/Logs/CityLab.log` sous
  `LogLiveCoding` (`Live coding succeeded` / `failed`). Outil prêt : `work/VELOCITE/vel_lc.ps1`.
* ⛔ **N'émets JAMAIS `LiveCoding.Enable` ni `LiveCoding.Console`** en session pilotée :
  `LiveCodingConsole.exe` tourne **déjà** depuis le démarrage de l'éditeur (Live Coding est actif
  par la config). Toggler une console vivante a **gelé l'éditeur** le 02/08. Signature du piège :
  **un cœur à 100 %, RAM parfaitement plate, log moteur muet, AUCUNE modale, `Responding=False`**
  — kill + relance, rien n'est perdu si la dernière passe a sauvé.
* Chronos mesurés : **compile 15,6 à 18,9 s**, éditeur jamais fermé, contre **139 s** pour le
  cycle complet (fermeture 110 s + build 10-12 s + réouverture 16 s) — plus le rechargement de la
  map lourde.
* **Rebuild complet propre périodique** : les patchs s'accumulent. Après une série de patchs, ou
  avant toute mesure qui compte (spec d'automation, perf, campagne), refais un vrai build.
* ⚠️ La règle « jamais de passe LOURDE sous patch Live Coding » (crash D3D12) **tient toujours**.
  Une passe **district** (14-40 s) a tourné sans incident sous patch ; une régé complète, non
  testée sous patch — ne la tente pas.

### 11.3 bis Quand un VRAI build s'impose
Changement de layout (`UPROPERTY`, `USTRUCT`, `.h`) · ajout/retrait d'un champ de profil ·
avant une spec d'automation · avant toute campagne de mesure · après une série de patchs LC ·
dès qu'un comportement devient inexplicable. Cycle scripté : `work/VELOCITE/vel_cycle.ps1`
(`-Fermer -Builder -Ouvrir`, chronos écrits dans `chronos.log`).

### 11.3 ter ⭐ LA VOIE LA PLUS RAPIDE : ne pas toucher au C++ du tout
**Doctrine (chantiers PONTS/QUAI/BLOC, 03-05/08) : la CUISSON livre la géométrie, le
MOTEUR ne fait que POSER.** Le side-car porte les triangles/quads/cotes déjà calculés ;
le C++ lit, valide, instancie. Conséquence mesurée : les itérations 1 à 3 du chantier BLOC
ont tourné **sans aucun build ET sans Live Coding** — boucle complète **1 min 36**
(cuisson 3-11 s + bake des masques 22-24 s + régé district 21-23 s).

| type de changement | voie | coût |
|---|---|---|
| **règle géométrique, seuil, profil, emprise, matériau d'ouvrage** | **cuisson Python + régé district** | **~1 min 30** |
| corps de fonction C++ | Live Coding (§11.3) | ~2 min |
| layout / `.h` / champ de profil | cycle complet (§11.3 bis) | ~2 min 20 |

**À la CONCEPTION d'un chantier, pousser le maximum de logique dans la cuisson** : c'est ce
qui transforme un réglage en itération de 90 s au lieu d'un cycle éditeur. Corollaire de
sûreté : le C++ reste un poseur bête (lecture + validation + instanciation), donc beaucoup
plus difficile à casser — et la loi §13.1 (aucune nappe lissée) se vérifie par relecture
d'un seul fichier Python.

### 11.4 Les chronos à attendre (proto 3×3, 36 cellules, 1,25 M d'instances)

| itération | chrono mesuré |
|---|---:|
| **Boucle A — donnée/side-car** : re-cuisson (18,7 s) + verrou SENS (1,1 s) + régé district sans végé (16,1 s) + 2 captures (68,3 s) | **1 min 44** |
| **Boucle B — C++ corps de fonction** : édition (~15 s) + Live Coding (18,9 s) + régé district sans végé (16,1 s) + mesure de l'effet (~15 s) + 2 captures (61,5 s) | **≈ 2 min 05** |
| régé **district** seule, sans végétation | **14,6 s** |
| régé **district** seule, avec végétation | 39,2 s **+ ~640 s de queue** |
| régé **3×3 complète** (streamed 120 + surfaces 14 + végé 42 + verrou 10,5) | **3 min 10** |
| cycle éditeur complet (fermeture + build + réouverture) | 2 min 19 |
| captures 2 poses sur éditeur stabilisé (dont passe `_WARM`) | 61 s |

*(Référence d'avant le lot : régé 3×3 complète ~32 min, dont **1 762 s** rien que pour la
végétation.)*

### 11.5 Vérifier qu'un changement a PRIS EFFET
* **La capture ne tranche pas les petites amplitudes.** 20 cm sur un mur de 6 m vu à 190 m :
  les deux images sont indiscernables. C'est la **mesure** qui juge.
* Sur du Nanite, les triangles sont invisibles à Python (§10) — mais **les BORNES du maillage
  le sont** (`StaticMesh.get_bounds()`, elles viennent de `UStaticMesh::GetBounds`, pas des
  données de rendu). Outil prêt : `work/VELOCITE/py/v6_bounds.py`. Exemple mesuré : un pied de
  mur enterré 20 cm plus bas se lit **−20,00 cm exactement sur 4 cellules sur 4**, `zmax`
  inchangé (preuve que la mesure discrimine).
* Un **compteur du résumé** est le discriminant le plus simple quand il existe : la boucle A
  a fait passer `RetainingWalls` de **24/1944 quads → 23/1771 → 24/1944** au retour.

### 11.6 Le discriminant obligatoire d'une régé filtrée
Rejouer une régé district **sans rien changer** doit rendre un état **strictement identique** :
nombre d'acteurs par préfixe inchangé, `veg_total` inchangé au **delta 0**, zéro écart par
acteur. Script prêt : `work/VELOCITE/py/v2_regen_district.py` (il photographie avant/après).
⚠️ **Le défaut que ce discriminant a attrapé** : bordures, bordurettes, tirets et murs sont
découpés à la cellule au prep, mais le PREMIER sommet d'une polyligne peut tomber exactement sur
la frontière **est ou sud** ; `floor` le range alors dans la cellule suivante — **non purgée et
non visée**. Résultat : 4 `SM_Ground_` en double et **l'asset du voisin écrasé** par le fragment
débordé. Garde posée dans `CityImportTools.cpp` (`CleSol` : en mode district, la géométrie va
dans sa cellule **propriétaire**) + un compteur de sortie « cellules hors filtre écartées » qui
**doit valoir 0**.

---

## 12. ⭐ TESTER EN VOL AU QUOTIDIEN — « Play » et « Stop » (lot PIE, 02/08)

> **Le Play est redevenu instantané.** Avant ce lot : démarrage 10,3 s et **arrêt 604,5 s**
> (l'utilisateur tuait l'éditeur au gestionnaire des tâches — ce kill a cassé deux sessions).
> Après : **démarrage 2,1 s, arrêt 2,0 s.** Zéro kill.

### 12.1 La procédure quotidienne
1. **Démarrer** : `Play` dans le hublot sélectionné (ou `Alt+P`). Attendu : **~2 s**.
   En script : `unreal.get_editor_subsystem(unreal.LevelEditorSubsystem).editor_request_begin_play()`.
2. **Voler**, puis **`Échap`** (ou `editor_request_end_play()`). Attendu : **~2 s**, éditeur vivant.
3. **Si ça dépasse 30 s : NE PAS TUER L'ÉDITEUR.** C'est une régression connue et elle a une
   seule cause (§12.3) : la collision de la végétation est revenue. Diagnostic en 2 s avec
   `work/PIE/py/p3_verrou_collision.py`. L'éditeur ressort TOUJOURS seul (mesuré 604 s et 617 s).
4. Chrono automatique d'un cycle complet :
   `work/PIE/pie_cycle.ps1 -Tag XXX -SejourS 20 -SansFocus` (écrit `chronos.log` + l'extrait de log).

### 12.2 Le banc des options, mesuré (proto 3×3, 1,25 M d'instances)
| option | démarrage | arrêt | l'éditeur gèle ? |
|---|---:|---:|---|
| **Play in editor** (état du lot PIE) | **2,1 s** | **2,0 s** | non |
| Play in editor (avant le lot) | 10,3 s | **604,5 s** | **oui, 10 min** |
| **Standalone Game** (processus séparé) | 18,2 s de chargement | fenêtre fermée en 3 s | jamais |

**Standalone Game** reste utile pour juger le jeu **hors éditeur** (vrai plein écran, pas de
surcoût d'outillage) : `UnrealEditor.exe "<uproject>" <map> -game -windowed` — une seule chaîne
d'arguments (piège §5). Outil prêt : `work/PIE/p2_standalone.ps1`. Ce n'est PAS la boucle par
défaut : 18 s de chargement contre 2 s, et il ne partage pas l'état non sauvegardé de l'éditeur.

### 12.3 ⛔ LE PIÈGE, ET IL EST STRUCTUREL : **un ISM crée UN CORPS PHYSIQUE PAR INSTANCE**
Et comme nos HISM sont `Movable`, le moteur les crée **UN PAR UN**
(`FBodyInstance::InitBody`, `InstancedMeshComponentBodies.cpp` l. 111) au lieu du chemin par lot
`InitStaticBodies` réservé au statique. Sur le proto : **1 253 686 corps Chaos** créés à chaque
Play et détruits à chaque Stop.

**Le profil, mesuré** (`gc.DumpAnalyticsToLog 1` + `log LogGarbage Verbose`) :
```
GC purged 862 objects (75587 -> 74725) in 602392.732 ms      <- AVANT
GC purged 862 objects (75587 -> 74725) in      5.233 ms      <- APRÈS
```
**Mêmes 862 objets, mêmes totaux** : seuls les corps changent. `DestroyGarbage` = **99,97 %**
du temps d'arrêt. Le GC lui-même n'a jamais été en cause (reachability 51 ms) — **aucun réglage
`gc.*` n'aurait aidé**, et la duplication du monde PIE ne coûte que **0,4 s**
(`StaticDuplicateObject`), pas les 10 s qu'on lui prêtait.

**La règle posée, et elle est lue dans la donnée** : un mesh de végétation **sans aucune
primitive de collision SIMPLE** (`AggGeom` vide) n'a pas de collision voulue — son
`CTF_USE_DEFAULT` retombe sur la collision **COMPLEXE**, donc sur le repli décimé du Nanite.
`CityImportTools.cpp` pose donc `NoCollision` sur ces composants-là, **avant
`RegisterComponent()`** (doctrine « propriétés à la CRÉATION »). Les meshes qui portent un vrai
volume (érables, hêtres : 2-3 convexes) **gardent leur collision** — aucun arbitrage de gameplay
n'a été demandé. Rollback **sans rebuild** : `FCityGenProfile::bVegCollisionHistorique=true`.

### 12.4 Corollaires mesurés, à ne pas re-déboguer
* **Ne bascule JAMAIS la collision d'un HISM sur une map déjà chargée** : `SetCollisionEnabled`
  appelle `MarkRenderStateDirty` → les 1,25 M d'instances sont ré-uploadées et les 31 arbres
  spatiaux rebâtis. **611 s mesurés**, éditeur muet, `Responding=False`. Signature à ne pas
  confondre avec un hang : le thread chaud est le **RENDER thread** (pas le principal) et la RAM
  est **plate**. Le correctif C++ (à la création) ne paie jamais ça.
* **HISM par bloc : écarté PAR LA MESURE** *pour le Play*. Le coût du Play est strictement **par
  INSTANCE**, pas par composant : à composants identiques (31) et objets identiques (862),
  retirer les corps divise l'arrêt par **302**. Découper 12 composants en ~192 ne changerait
  aucun des 1,18 M de corps. Il lui reste **un seul terrain**, celui du point suivant.
* ⚠️ **CE QUE LE REMÈDE NE RÈGLE PAS : le changement de MAP.** Basculer de la map lourde vers
  une map jetable coûte toujours **650,9 s** (mesuré après le correctif ; référence C1 : 605 s).
  Ce teardown-là n'est pas la physique mais la **libération des données d'instance / GPUScene**
  des 1,25 M d'instances (render thread chaud, RAM plate). **La règle du §5 tient donc
  toujours : lancer la spec d'automation depuis une session DÉMARRÉE sur une map légère**, pas
  par bascule depuis la production. C'est là — et seulement là — que « HISM par bloc » ou le
  streaming ont encore quelque chose à prouver.
* Le **streaming par distance** reste la cible AGGLO, **avec HLOD** pour le lointain (sans lui,
  les blocs lointains disparaissent en vol haut). Ce que le lot PIE lui apprend : le budget de
  streaming se compte en **instances physiques**, pas en acteurs ni en sous-niveaux — 16
  sous-niveaux chargés en permanence ne coûtent rien par eux-mêmes.

---

## 13. ⭐⭐ DOCTRINE DE CONSTRUCTION GÉOMÉTRIQUE (04-05/08 — payée par 14 tentatives sur la berge)

> Contexte : la bande de berge Saint-Pierre → Daurade a demandé **14 chantiers en 36 h**
> alors que bâtiments (20 780), sols (9 km²) et arbres n'ont eu **qu'une règle chacun**.
> Les six lois ci-dessous sont l'écart entre les deux. **Les lire AVANT tout chantier
> de géométrie.**

### 13.1 ⛔ LOI DES NAPPES — aucune nappe lissée ne raccorde deux choses. JAMAIS.
Une nappe tendue entre deux courbes non parallèles est une **surface réglée gauche** :
courbe **par construction**, quels que soient les réglages. C'est la cause n°1 des
« bosses », « terrain curvé », « ondulations » signalés 6 fois par l'utilisateur.
Deux coupables identifiés et supprimés : la région *esplanade* (sol de ville régénéré,
41 ha) et la *pente lissée* `berge_naturelle`.
- **Interdit** : interpoler/lisser **en Z** entre deux valeurs mesurées.
- **Permis** : des **CONSTANTES par zone** — promenade plate **par bief**, **un seul jeu
  de marches par emprise**, niveau 0 constant par emprise. Une constante n'est pas une nappe.
- **Permis** : régulariser le **contour EN PLAN** (patron `q1_regul`) — ça ne crée aucune
  surface gauche. *Le plan oui, le Z non.*
- **Acceptation structurelle** : dans la zone construite, **toute face est HORIZONTALE ou
  VERTICALE** (mesuré sur le maillage exporté) + **0 appel de lissage en Z** (relecture).
  La courbure devient *impossible*, pas *corrigée*.

### 13.2 ⛔ LOI DE COMPOSITION — le sol ne cède jamais ; les objets se POSENT
Deux façons de composer un objet avec le sol :
- **SOUSTRACTIVE** (ce qui a échoué 10 fois) : l'objet remplace un morceau de sol (quads
  masqués, découpes, zones cédées). Chaque frontière de responsabilité est une couture
  vivante → 1 cm de désaccord = trou (on voit le ciel) ou chevauchement.
- **ADDITIVE** (ce qui n'a jamais failli : les bâtiments) : le sol reste **complet dessous**,
  l'objet se pose et s'enterre un peu (patron `SocleCm`). **Le recouvrement est bénin,
  le vide est fatal.**
**Exception unique** : ce qui passe SOUS le sol (tunnels, trémie Saint-Cyprien) exige une
vraie découpe → chantier à part entière, avec cette conscience-là.

### 13.3 ⛔ LOI D'ORIENTATION — le côté se décide par POINT-DANS-POLYGONE
Sur une **emprise FERMÉE** (contour d'eau, cour de bâtiment), jamais par une **normale de
segment** : une normale se retourne dans les virages serrés (3 occurrences payées, dont
73 murs/239 et « la promenade construite dans le sens contraire »).
**Acceptation** : 0 test de normale directionnelle dans le code de côté (prouvé par
relecture) + échantillonnage point-in-polygon des faces (0 à l'envers).

### 13.4 ⛔ LOI DES BORNES — ne régénérer QUE ce qui n'existe pas
Le sol de ville et l'eau **existent déjà et fonctionnent** : on **borne dessus**, on ne les
refait pas. Inventer un niveau intermédiaire (« esplanade ») a coûté 5 lots.
Formulation utilisateur, qui fait autorité : *« Niveau 0 = le sol habituel, il existe déjà.
Niveau −1 = la promenade, un long bloc plat. Niveau −2 = l'eau. »* → **un seul objet à
fabriquer**, borné en haut et en bas par de l'existant.

### 13.5 ⛔ LOI DES EXTRÉMITÉS — toute extrémité libre reçoit une face de fermeture
Sinon on voit l'intérieur (« espaces ouverts, pas de mur »). Bouchons aux **VRAIS bouts**,
jamais aux coupes de cellule (piège déjà payé sur les murs : 4 bouchons au lieu de 2, dont
deux dos à dos au milieu d'un mur).

### 13.6 ⛔ RÈGLE D'EXISTENCE — « pas de donnée, pas d'objet », appliquée aussi au RELIEF
Le bloc de quai n'existe que là où le sol de ville domine le niveau −1 d'au moins 0,50 m
(**52,8 % du linéaire n'a rien à générer** : la berge y est naturellement basse).
⚠️ Ne se retourne PAS en « ouvrage présent, plus d'objet » : le chevauchement d'une fosse
d'arbre par un couronnement se corrige en **rétrécissant l'ouvrage**, pas en supprimant
l'arbre (version implémentée, mesurée, puis **rejetée sur capture** — elle vidait la
promenade de ses platanes).

---

## 14. ⭐⭐ ITÉRATION VISUELLE — le workflow avec l'utilisateur dans la boucle (05/08)

### 14.1 L'acceptation est l'ŒIL de l'utilisateur, jamais une métrique
Les métriques servent à **ne rien casser** (verrous), pas à déclarer un succès. Six lots
d'affilée ont livré « toutes acceptations vertes » sur des scènes que l'utilisateur a
jugées chaotiques.

### 14.2 ⭐ PÉRIMÈTRE D'ACCEPTATION = PÉRIMÈTRE DE JUGEMENT
Mesurer dans le champ des **caméras de jugement** (FOV 90°, 150 m), **jamais** dans le
périmètre que le système couvre. Défaut découvert le 05/08 : le système ne produisait que
**2,1 %** de ce que voyait la caméra (3 poses sur 6 à **0 %**) pendant que tous les
rapports étaient verts. **Un chantier ne se déclare pas vert sur un sous-ensemble qu'il a
choisi lui-même.**

### 14.3 Le coordinateur OUVRE les captures lui-même
Avant toute transmission à l'utilisateur, et il dit ce qu'il voit — pas ce que le rapport
en dit. Les 2 fois où ça a été fait, l'auto-validation « aucune image chaotique » de
l'agent a été **démentie**.

### 14.4 L'agent ne rend AUCUN verdict
Description **factuelle** par capture + **signalement explicite** des images douteuses
(consigne de brief). Vertu à exiger : l'auto-rejet sur capture *avant* livraison.

### 14.5 ⏱️ LES COÛTS RÉELS D'UNE ITÉRATION (mesurés, 05/08) — ne plus les confondre
| poste | coût | verdict |
|---|---|---|
| **boucle machine** (cuisson → bake → régé district) | **1 min 36** | ✅ |
| **démarrage à froid d'un agent NEUF** (relire brief/rapports/code) | **10-15 min** | ❌ **ÉVITABLE** |
| travelling 50 captures | 11-23 min | ❌ réservé à la validation finale |
| batterie complète verrous + exports | ~8 min | ⚠️ fin de série seulement |
| **itération de bout en bout, agent neuf** | **~40 min** | mesuré |

**Correctifs** : ① **reprendre le MÊME agent par `SendMessage`** (doctrine du 30/07,
non appliquée le 05/08 — d'où la régression) ; ② une itération = **3 captures aux poses du
grief**, pas le travelling ; ③ **ne jamais annoncer la boucle machine comme si c'était le
tour complet** (« 2 min » annoncé, 1 h vécue → l'utilisateur l'a relevé, à juste titre).
**Cible : 10-15 min de bout en bout.**

### 14.6 Piège du watchdog : `progress.log` partagé entre itérations
Le compteur de silence part du dernier écrit — donc d'une **ligne héritée de l'itération
précédente** → fausse alerte à la minute 0. Et un motif de fin trop large (`TERMINEE`)
coupe le guetteur sur un jalon interne : **exiger `TERMINE:` avec les deux-points**.
