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
- Caméras de référence sauvegardées dans les `.meta.txt` de `C:\LidarPoC\work\SOLVERT\` — réutilise
  les MÊMES poses pour tout avant/après.
- Mouvement (vent…) : séries de ≥8 images à intervalles IRRÉGULIERS (l'intervalle régulier fait de
  l'aliasing) + série de contrôle ; plancher de bruit TAA ≈ 30/255.
- Matériaux : le headless ne compile PAS SM6 → valider en GUI + grep log (`Failed to compile`,
  `Sampler type`). Cohérence VT↔samplers obligatoire. En cas d'échec sans texte d'erreur : ouvrir le
  matériau EN GUI et lire l'erreur (ne pas bisecter en aveugle >15 min).
- **AUTO-VALIDATION : regarde tes captures et REJETTE ta propre version si elle ne convainc pas.**

## 5. Builds & cycle éditeur (atelier permanent)
- Objectif : **1 redémarrage d'éditeur max par lot.** Données/matériaux/config → MCP live.
- Build : fermer l'éditeur (par CHEMIN), `Build.bat CityLabEditor Win64 Development -project="…\CityLab.uproject" -WaitMutex -NoHotReloadFromIDE` (~12-20 s), rouvrir via **PowerShell**
  (Git Bash mangle `/Game/...`). Corps de fonction → Live Coding OK pour itérer, mais **JAMAIS
  exécuter une passe lourde sous patch Live Coding** (crash D3D12) : build complet avant le run.
- **Un processus UE par passe de génération lourde** (commandlet), reprenable par lot de cellules.
- Crash : `Saved/Crashes/*/CrashContext.runtime-xml` → `IsEnsure=true` = NON-fatal (l'éditeur vit).

## 6. Règles de génération (résumé — détails dans `Doc/Vegetation-Pipeline-Cpp.md` & `Doc/Reseau-Sidecar.md`)
- Pose végé : **trace de la surface RENDUE uniquement** (proxys dupliqués+Build() ; les meshes de
  collision 16×16 d'origine sont EXCLUS du trace) ; base-à-0, zéro offset `min_Z` ; pas de sol → skip
  compté ; jamais un modèle analytique (GroundZ) pour un objet sans socle enterré.
- Cuisson collision : `InvalidatePhysicsData+CreatePhysicsMeshes` ne re-cuit PAS → `Build()`.
- **CHECKLIST POST-RÉGÉ streamed (`ImportCityStreamed`)** — ALLÉGÉE le 31/07 (lot A-ter) :
  les deux points qu'on rejouait à la main sont désormais posés **par le C++ à la CRÉATION**
  (`CityImportTools.cpp` : `bInitiallyLoaded/bInitiallyVisible = true` sur les streaming levels
  — le piège PIE payé 3 fois ; `ApplyGroundTextureStreaming()` = `StreamingDistanceMultiplier
  = 100` sur `SM_Slab_*`/`SM_Ground_*`/`SM_Proxy_*`/`SM_Surface_*`). Ce sont des propriétés
  PAR OBJET : posées après coup elles ne survivent pas à la régé suivante — d'où le C++.
  **Ne plus lancer `fix_pie_sol2.py` ni le script SDM du lot 3** ; les VÉRIFIER suffit
  (`work/SOLROUTES/ater_regen.py` fait la régé et les deux vérifications, `flags_ok`/`sdm_ok`).
  Restent obligatoires après chaque régé : `ImportCitySurfaces` avec le **NOGREEN**
  (`SOLVERT/proto_capitole_surfaces_nogreen.json`, JAMAIS `grass_v3` — 1 407 films verts
  revenus le 31/07), re-run `ImportVegetation`, re-masquer l'ancienne végé `Sol2Veg_*`/
  `Sol2Grass_*`, cacher les `SM_Proxy`, et comparer les compteurs (CurbQuads / AxialDashes /
  MaskedCells / instances / fosses) au lot précédent.
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
