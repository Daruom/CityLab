# Reprise — État du projet CityLab/Survol (2026-08-01)

> **LE document de reprise.** Écrit après le grand cycle du 30/07–01/08 (arbres → sol → ferme →
> T10). Tout ce qu'il faut pour reprendre « comme si de rien n'était » : ce qui est fait, les
> doctrines, le process agents, les next steps. Les docs archivés sont dans `Doc/Archive/`
> (horodatés — rien n'est supprimé). **Carte des docs actifs en fin de fichier.**

## 1. Où en est le jeu (vision)

**Survol** = exploration aérienne photoréaliste de la France (UE 5.8, desktop/Steam). **CityLab** =
l'usine de génération (Toulouse d'abord). Proto de travail : `/Game/Dev/ProtoE2Sol2/L_ProtoSols_E2_Sol2`
(1 km² Capitole). La doctrine produit qui gouverne TOUT : **« cohérence globale et harmonie PRIMENT
sur le réalisme des données »** — établie par l'utilisateur, payée par trois itérations de sol.

## 2. État par chantier

### ✅ ARBRES / VÉGÉTATION — CHAPITRE CLOS (validé utilisateur + à l'échelle 10 km)
- **Autorité de pose unique** : `UCityImportTools::ImportVegetation(veg.json)` (C++). Trace de la
  **surface RENDUE** (proxys dupliqués+`Build()`, collision 16×16 EXCLUE), multi-hit, base-à-0 sans
  offset, skip sans-sol (396) et pente minérale >15° (19), **rétraction bord de chaussée** par
  gradient du SDF (canal G), fosses carrées proportionnelles (1,0-2,2 m, plan local 9 sondages,
  terre toujours visible) + fosses **rondes** arbustes (Ø0,6-0,9 m). 55 821 instances / 1 234 fosses
  sur le proto en ~5 s. **Flottement : 0 instance >30 cm sur 7 295 échantillonnées à 3 zones du
  10 km** — le mécanisme est validé à l'échelle.
- **Vent** : amplitudes validées ×0,65 dans 33 MI + **désynchronisation par `PerInstanceRandom`**
  dans `MF_SimpleWind` (corrélation entre couronnes 0,713→0,001). Touffes/haies : PAS de vent
  (SimpleGrassWind ne compile pas en SM6 sur ces matériaux — reverté, à reprendre avec l'erreur GUI).
- **Herbe du sol** : canal R du masque (SDF OCS GE CS2.*), 4K, macro-variation — **validée**.
- Doc active : `Doc/Vegetation-Pipeline-Cpp.md`.

### ⛔ SOL/ROUTES — DIFFÉRENCIATION VECTORIELLE ABANDONNÉE (01/08, décision utilisateur)
- **3 itérations loyales et mesurées (lots A, A-bis charte, A-ter, V2 cœur piéton) : toutes KO au
  verdict visuel.** Cause structurelle : le sol est peint par 4 systèmes indépendants (masques,
  bordures 3D, tirets, herbe) — les artefacts vivent aux JOINTURES, et chaque zone vectorielle crée
  des km de frontières que ni le texel (48,83 cm — rien de plus fin que ~55 cm n'est représentable),
  ni les bordures, ni le cadastre n'honorent.
- **Le sol du jeu = 3 matériaux** : dalle + asphalte + herringbone (+ herbe masque). Restauré,
  vérifié aux 6 endroits des griefs (« une seule grammaire »), committé (`a2093a9`).
- **Seule évolution autorisée à l'avenir : fondu continu sans frontière** (gradient d'influence
  20-40 m — le polygone piéton V2 redressé est archivé pour ça : 22 polys, sommets ÷21, dans
  `C:\LidarPoC\work\SOLROUTES\v2_archive_final\`). L'identité de ville viendra des TOITS et des PROPS.
- **Gardiens conservés** (committés, invisibles, précieux) : le **graphe routier side-car**
  (`SourceData/Reseau/`, 41 378 arêtes classées 10 km, `cleabs` pérennes, sens/voies/largeurs — le
  socle du futur trafic ; doc `Doc/Reseau-Sidecar.md`), le fetch enrichi GPKG (76/88 colonnes),
  le fetch OSM piéton, la décision BC7 des masques (19,2→4,8 Go agglo), les lignes C++
  anti-régénération. Saga complète + leçons : `Doc/Archive/2026-08-01_Sols-Masques-LotA.md`.

### 🏛️ TOITS — LIDAR_C jugé CONVAINCANT, 2 verrous avant l'arbitrage final
- `Dev/ProtoLIDAR/C/L_ProtoSols_LIDAR_C` : toits LiDAR réels **colorés par PENTE dans le matériau**
  (zéro devinette de clutter ; cheminées = masses en relief ; 3ᵉ zone zinc/terrasse = échec Roofer
  gracieux). Bande cruciale : tuiles canal = 8,5-19° → coupure haute ~5-9°. MI paramétrique
  (`setp.py`, 2 s).
- **Verrous restants** : ① rangées de tuiles alignées sur l'ÉGOUT (UV dérivées de la normale, piège
  LWC à valider en bac à sable) ; ② **teinte ortho réelle = régé LiDAR avec `bMarbleWhite=false`**
  (un flag !). **Arbitrage final « E2 informé LiDAR vs LIDAR_C raffiné » À L'ÉCHELLE** : agglo en E2
  socle + LIDAR_C sur 3-4 districts représentatifs (JAMAIS Roofer sur 508k bâtiments).
- E2 amélioré possible : skeleton piloté par archétype LiDAR (classification statistique des toits —
  plans dominants/orientation/pente/faîtage — PAS de reconstruction).

### 🏭 FERME `CityLab_Batch` — opérationnelle, EN PAUSE (focus maître demandé)
- Second projet persistant (~21 Go), **MCP 8107** (maître 8101), générations versionnées
  `Content/Generations/T10_vN`, sync sens unique `sync_from_master.ps1`. **⚠️ ciblage par CHEMIN**
  (les deux éditeurs s'appellent CityLab.uproject).
- **T10_v1 générée** (~4 h : ville 12 min, surfaces 34, végé 131) = 131 357 bâtiments, 2,8 M
  instances — mais **divergée du proto** (le proto avançait pendant la campagne) → **règle de GEL** :
  md5 de l'outillage dans l'ETAT.md avant toute campagne. v1 = témoin A/B ; **T10 v2** attend le GO
  (recette : `C:\LidarPoC\work\FERME\T10V1\RECETTE_SOL2_VERS_T10.md`).
- **Avant l'agglo (461 km², données 100 % prêtes dans `SourceData/Agglo/` + README)** : optimiser la
  passe végé (131 min → partitionnement spatial), trancher les touffes (2 M cuites = proto ÷20 ;
  reco : **streaming de proximité** autour du joueur), fix eau Garonne (**biefs** — lit à sec par
  tronçons), bascule BC7.

## 3. Next steps (dans l'ordre de valeur estimé)
1. **Passages piétons OSM** — pipeline C++ complet existant (`CROSSINGS_ON`), donnée réelle
   (10 236 nœuds/10 km, `markings=no` respecté), posés SUR l'asphalte = zéro frontière nouvelle.
   Décision utilisateur : GO en test, rollback = le flag.
2. **Verrous LIDAR_C** (①UV égout, ②teinte ortho) puis arbitrage toits à l'échelle.
3. **T10 v2** sur la ferme (gel + sync + re-run ; ~3 h machine) quand l'utilisateur rouvre la ferme.
4. **Lot D qualité/perf** : ombres (**VSM vs CSM mesuré** — VSM = l'outil UE prévu avec nos bâtiments
   Nanite ; le vent WPO invalide le cache → **bornes de distance WPO**, pas UDS qui est aussi du
   WPO), cull des touffes (l'apparition à 45-60 m se voit), ProfileGPU, Scalability.
5. **Eau** : biefs + matériau `SingleLayerWater` (flow par polyligne, écume aux berges) — optimisé,
   data-driven. Le plugin Water d'Epic = plus tard, si besoin.
6. **Props/vie** (chantier futur) : gares/POI BD TOPO, signalisation procédurale aux intersections
   (graphe side-car prêt), mobilier — c'est LÀ que « rue piétonne » se lira, pas au sol. Assets 3D :
   Fab en 1-clic utilisateur (⚠️ JAMAIS de CC-BY — Standard Fab/CC0 uniquement).
7. Divers en attente : UDS (cohérence météo, chantier ambiance), habillage lumière du proto **non
   persisté dans la map** (respawn scripté `v2_habillage.py` — à persister un jour), touffes/haies
   sans vent, texture ponts 128², chaussées appariées des boulevards.

## 4. Le process agents (rodé sur ~30 lots — NE PAS réinventer)
- **Modèles** : **Opus 5 PAR DÉFAUT, bien briefé AVANT** ; Fable 5 = exception argumentée
  (architecture inédite). La qualité vient du BRIEF, pas du modèle.
- **Brief type** : mission + décisions verrouillées + contexte spécifique + **« lis
  `Doc/Agent-Playbook.md` »** (LE doc opérationnel : accès MCP+skill, guetteur de fichiers, captures,
  builds, checklist post-régé, pièges payés — maintenu à chaque nouveau piège). Les prémisses du
  brief sont des **HYPOTHÈSES à falsifier par la mesure** (3 briefs/20 en contenaient une fausse).
- **Supervision** : heartbeat horodaté (progression interne) + `progress.log` + `ETAT.md` de reprise
  pour les campagnes ; **Monitor/watchdog côté coordinateur** (silence 25 min, crash réel vs
  `IsEnsure`) ; **le vrai signal de vie = la PRODUCTION de fichiers**, pas le heartbeat ; les agents
  qui attendent un processus long terminent leur tour → **le coordinateur les réveille** (sonde PID).
- **Boucle de validation** : captures aux MÊMES poses (`.meta.txt`), auto-rejet (« est-ce qu'une
  ville ferait ça ? ») — et depuis le 01/08 : **l'utilisateur juge une liste FIGÉE de captures AVANT
  toute sauvegarde** ; plus d'auto-déclaration de cohérence.
- **Atelier permanent** : éditeur jamais fermé sauf build (1 max/lot), MCP live, un processus UE par
  passe lourde. **Tout réglage sur objets générés va DANS le générateur C++** (posé après coup = 
  effacé à la régé suivante — payé 3 fois).

## 5. Doctrines gravées (l'ordre des leçons compte)
1. **Cohérence globale > exactitude locale** (arbres du talus, fosses inventées, micro-motifs, et
   finalement tout le sol vectoriel).
2. **Mesurer avant de coder ; ne JAMAIS vérifier un placement contre le modèle qui l'a produit**
   (la tautologie a coûté 6 itérations sur le flottement).
3. **Un fix qui rate en boucle = mauvaise question** (le déclutter LiDAR → coloration par pente ;
   les zones vectorielles → fondu continu ou rien).
4. **Validation en 3 étages** : proto (itération) → T10 (validation de chapitre) → agglo (production).
5. **Règle de gel des campagnes** (le proto bouge sous la campagne sinon).
6. Données : OSM = seule source nationale des passages piétons ; OCS GE = partition nationale sans
   trou (**CS2.\* entier**, pas CS2.2 seul — les pelouses sous canopée) ; « BD ORTHO 5 m » N'EXISTE
   PAS ; `vitesse_moyenne_vl` = vitesse de parcours, pas limitation ; le « moignon » du Capitole =
   vraie entrée de parking.

## 6. Carte des documents
| Doc | Statut | Rôle |
|---|---|---|
| **`Doc/Reprise-Etat-Projet.md`** | ⭐ actif | CE doc — le point d'entrée de reprise |
| `Doc/Agent-Playbook.md` | actif | opérations agents (MCP, pièges, checklists) — màj continue |
| `Doc/Vegetation-Pipeline-Cpp.md` | actif | recette végétation (chapitre clos) |
| `Doc/Reseau-Sidecar.md` | actif | graphe routier (socle trafic futur) |
| `Doc/Inventaire-Prototypes-Toits.md` | référence | familles de protos toits (+ LIDAR_C depuis) |
| `Doc/J2-ProfilDesktop.md` | référence | spec du profil desktop du générateur |
| `Doc/Archive/2026-08-01_*` | archives | Sols-Masques (saga sol + charte), Reprise-Sol2, Catalogue données (⚠️ partiellement périmé), Etat 25/07 |
| `SourceData/Agglo/README_Agglo.md` | actif | données métropole complètes + emprise + pièges |
| `CityLab_Batch/README_Batch.md` | actif | règles de la ferme |
| `C:\LidarPoC\work\FERME\T10V1\RECETTE_SOL2_VERS_T10.md` | actif | brief de la prochaine campagne T10 |

*Rappels transverses : commits = coordinateur uniquement, sur validation utilisateur ; ferme hors
git ; maps protégées par md5 (`_E2` 6942C9D6, `_Sol1` 4C67BBD8, LIDAR blanc B71B0E40) ; JAMAIS
toucher aux éditeurs HELIOS/EnvolFlight/Survol/DroneFPV.*
