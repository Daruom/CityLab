# Reprise — État du projet CityLab/Survol (2026-08-02, v2)

> **LE document de reprise.** Réécrit après la grande session du 01-02/08 (élargissement
> 3×3, VSM, murs/escaliers/promenade, vélocité ×20, PIE réparé). La v1 du 01/08 est
> archivée (`Doc/Archive/2026-08-02_Reprise-Etat-Projet-v1.md`). Compagnon opérationnel :
> **`Doc/Agent-Playbook.md`** (§11 = LA boucle d'itération, §12 = tester en vol).
> Carte des docs en fin de fichier.

## 1. Vision et doctrine produit
**Survol** = exploration aérienne photoréaliste de la France (UE 5.8, desktop/Steam).
**CityLab** = l'usine de génération (Toulouse d'abord). Doctrines produit :
- **« Cohérence globale et harmonie PRIMENT sur le réalisme des données »** ;
- Règles **NATIONALES** uniquement, zéro cas particulier toulousain — la vérité locale
  vit dans les VERROUS de test (Alsace-Lorraine, volées du Pont Saint-Pierre), jamais
  dans les règles ;
- **Pas de donnée → pas d'objet** (leçon gradins : sans source qui dit « ici », on ne
  décore pas) ; l'identité de ville = TOITS + PROPS, pas le pavage ni les façades ;
- **⛔ Façades : PAS de fenêtres** (décision ferme 01/08 : pas de données réelles →
  résultats approximatifs → KO ; `WindowMode=1` marbre nu reste). Ne pas re-proposer.

## 2. LE TERRAIN DE JEU ACTUEL (ce que l'utilisateur ouvre)
- **Proto = `L_ProtoSols_E2_Sol2` en 3×3 km** (36 cellules ±1500 m autour du Capitole,
  indices des 4 cellules historiques conservés → captures comparables). 20 780 bâtiments,
  1,25 M d'instances de végétation (touffes **Nanite, sans cull, sans ombre, SANS
  collision**), Garonne EN EAU (placeholder surfaces), Prairie des Filtres en herbe.
- **Ombres : VSM actif depuis la config** (`r.Shadow.Virtual.Enable=1`) — ombres pleines
  à toute distance, moins chères que l'ancien CSM. ⚠️ AA=0 (aucun anti-aliasing) : si
  scintillement en vol → lot TSR (identifié, pas fait).
- **Play : 2 s pour entrer, 2 s pour sortir, zéro kill** (Playbook §12). Si >30 s : NE
  PAS tuer, diagnostiquer (`work/PIE/py/p3_verrou_collision.py`).
- **Régé complète 3×3 : ~3 min** ; district 4 cellules : ~15-40 s ; itération type :
  **1 min 30 - 2 min 30** (chronos réels, Playbook §11).
- Sauvegardes : le générateur sauve en fin de passe (doctrine : garantie = rollback par
  re-bake + git, pas la non-sauvegarde).

## 3. État par chantier (ce qui est VALIDÉ / OUVERT / EN RÉSERVE)

### Sol & peinture — stable
3 matériaux (dalle+asphalte+herringbone) + herbe masque. **Bordurette 3D** sur contour
d'herbe régularisé (validée : « le bon deal »), peinture rétractée 10 cm sous le chant,
méandre de bruit supprimé. **Axiale par RUE** (Alsace-Lorraine muette, verrou).
**Passages piétons : RETIRÉS, mécanisme complet EN RÉSERVE** (`CROSSINGS_ON=False` ;
conditions de retour au flag : peinture blanche matériau marking, pleine largeur
bordure-à-bordure, chaussée roulable non ambiguë — chantier props/marquages).

### Végétation — régime 02/08 (⚠️ change de l'ancienne doc)
Touffes : **Nanite, cull 0/0** (le « sous-marin » est mort — l'herbe existe à toute
altitude pour MOINS cher), **no-shadow**, **NoCollision** (règle data-driven : mesh sans
primitive simple = pas de collision voulue ; les ARBRES gardent la leur, 336 ms d'arrêt).
Semis par `AddInstances` par lots (passe 3×3 : 30-42 s). **⚠️ Le VENT ne souffle plus**
(constat mesuré : WPO non branché dans les 5 matériaux de base — le vent validé au
chapitre arbres s'est perdu quelque part ; à REBRANCHER un jour en connaissance de cause
VSM, avec re-mesure du cache). `Vegetation-Pipeline-Cpp.md` à lire avec ces amendements.

### Bâtiments — sains et verrouillés
E2 socle (marbre + toits skeleton/terracotta), cours intérieures percées (vérifiées au
m² contre le JSON), **proxys SUPPRIMÉS** (Nanite les rend obsolètes ; silhouettes
lointaines futures = HLOD UE si besoin). Verrou géométrique permanent (bâtiment-témoin
avec cour + valeurs de référence) + `Tools/verrou_batiments_sol2.py`.

### Discontinuités (le sol n'est plus 2,5D) — C1+V2+V3 livrés, C2 SUIVANT
- **C1 murs de soutènement** : breaklines MNT (seuil relevé au-dessus des témoins
  urbains), 239 murs/15,5 km, polylignes **régularisées** (le zigzag était l'escalier de
  pixels du raster 1 m), helper « Z du sol RENDU » partagé, VERROU SENS 716/716.
- **V2 escaliers** : source **BD TOPO SEULE** (`nature='Escalier'` ; OSM steps ÉCARTÉ
  derrière `INCLURE_OSM=False` — retour futur : corroboration au pied d'un mur
  `borde_pieton`). Seuil de **pente ≥ 11 %** (vide [9,1;12,7] — ni ΔZ ni nb de marches
  ne séparent). Les **2 volées du Pont Saint-Pierre** (46+38 marches, paliers de repos)
  sous **VERROU NOMINATIF** (`q_verrou_escaliers.py`) ; le C++ journalise CHAQUE volée
  posée/écartée avec sa cause.
- **V3 promenade de berge** : classe gravier EXISTANTE, règle « chemin au pied d'un mur
  de quai, mesuré côté BAS » — 100 % côté bas après découpe par échantillon (0 m² au-
  dessus du couronnement). Sources : **BD TOPO d'abord** (Promenade Henri-Martin y est !),
  OSM complément. OUVERT : lisibilité à distance (remède identifié : la border de
  bordurette, comme l'herbe).
- **V4 gradins : OFF par défaut** (`bQuayTiers=false` — la réalité des berges = mur
  lisse + promenade ; les photos de référence utilisateur font foi). Mécanisme conservé.
- **La trémie Saint-Cyprien n'est PAS dans le MNT** (sol nu qui enjambe les ouvrages) :
  son traitement = re-maillage piloté par le réseau `niveau<0` (documenté, pas fait).

### Ombres / PIE / perf — réparés, avec une LOI pour l'agglo
VSM (cf. §2). PIE 2 s/2 s. Fermeture d'éditeur 13 s (le zombie de teardown avait la même
racine physique). Queue post-végétation 640 s → 2 s. **LOI MESURÉE : la destruction des
corps physiques est QUADRATIQUE (n^2,14), leur création quasi linéaire — le budget agglo
se compte en CORPS PHYSIQUES.** Reste UN teardown lourd : la bascule de map depuis la
map lourde (~650 s, libération GPUScene/instances) → terrain du futur lot streaming/HLOD ;
invisible au quotidien (on ne change pas de map, on vole). Standalone Game documenté en
option de secours (l'éditeur n'est jamais touché).

### Visuels PARQUÉS (constats mesurés, lots à ouvrir sur demande)
- **AA=0** → « vibration » en vol haut : lot **TSR** dédié et mesuré.
- **« Qualité moyenne constante, pas du vrai 4K »** : constat STRUCTUREL — packs 4096
  tuilés à 2 m = 0,49 mm/texel, le mip 0 est hors d'atteinte au-delà d'~1 m ; AUCUN
  réglage de streaming n'y peut rien ; le levier = densité de motif / couche de détail
  matériau (chantier séparé).
- **Teinte des touffes pâle/blanchâtre** (pré-existant, visible partout depuis Nanite).
- Coutures de bordurette aux limites de cellules : max 2,40 m (médiane 0,30).

## 4. LE PROCESS (les décisions de workflow qui font la différence)
1. **Le coordinateur est le MÉTRONOME et n'exécute JAMAIS lui-même** (directive
   utilisateur explicite : préserver le contexte) — tout passe par des agents Opus 5,
   même une micro-itération de 2 réglages.
2. **Micro-lots** : brief = mission + verdicts utilisateur + faits vérifiés + pointeur
   Playbook (les prémisses du brief sont des HYPOTHÈSES — cette session en a falsifié
   ~10, dont 3 mesures erronées du coordinateur : personne n'est au-dessus de la mesure).
3. **La boucle d'itération** (Playbook §11) : mode district (`CellFilter`), végétation
   seulement si le sol a bougé, `LiveCoding.Compile` SEUL (jamais Enable/Console — hang
   payé), un vrai build seulement pour la structure/les consolidations (cycle map-légère).
   **Chronos réels : donnée 1 min 25 - 2 min ; C++ corps ~2 min ; les chronos se
   JOURNALISENT à chaque itération** (`iteration_user_N.log`) — c'est le contrat.
4. **Les CAPTURES sont l'organe de contrôle et restent TELLES QUELLES** (décision
   utilisateur : elles ont attrapé tous les défauts sérieux — murs inversés, panneaux
   disjoints, gradins — là où les compteurs mentaient). L'utilisateur, lui, juge
   directement en éditeur.
5. **Vérification NOMINALE, jamais agrégée** : on vérifie LE grief (le cleabs, la
   cellule, le m²), pas un compte (« Stairs=6 » a caché deux fois la vraie question).
6. **Supervision** : un watchdog par lot (préfixe [LOT]), jalons ≤10 min, BLOQUE: propre
   + réveil coordinateur ; compléments en vol par SendMessage ; à un silence, vérifier
   LA PRODUCTION (logs éditeur, fichiers) avant toute conclusion. Signatures de hang :
   attente OK pour sortie de PIE/teardown (RAM plate ou croissante, ça FINIT) ; les
   corps physiques étant retirés, tout gel >30 s redevient ANORMAL.
7. **Commits** : coordinateur uniquement, après validation, messages-chronique détaillés.
   ⚠️ Piège PowerShell 5.1 : AUCUN guillemet double dans les messages git (argv natif
   éclaté — payé 2×) ; utiliser « » ou <<>>.
8. **Cohabitation éditeur** : UN seul agent sur 8101 ; le second travaille hors-éditeur
   et s'endort (`BLOQUE: prêt pour fenêtre éditeur`) ; fenêtres de build déclarées.
9. Après un kill utilisateur de l'éditeur : il rouvre N'IMPORTE QUOI — toujours vérifier
   la map courante avant de juger (un faux « escaliers disparus » vient de là).

## 5. Doctrines gravées (liste à jour — l'ordre des leçons compte)
1. **Cohérence globale > exactitude locale** ; pas de donnée → pas d'objet.
2. **Toute propriété d'objets générés se pose À LA CRÉATION dans le générateur** —
   occurrences payées : flags PIE (3 têtes : graines / état runtime / visibilité
   éditeur), SDM streaming, proxys visibles, **collision végétation**. Un script
   après-coup = effacé à la régé suivante.
3. **Validation par GÉOMÉTRIE, par le DISQUE (après redémarrage), et NOMINATIVE.**
   Jamais comptes/positions/objets en mémoire (T10 v1, flags_ok, proxys, Stairs=6).
4. **Témoin gelé** (`_Sol1` md5 4C67BBD8, jamais ouverte pendant un import) = l'A/B
   parfait quand « ça ressemble à une ancienne version ». L'œil utilisateur est une
   DONNÉE.
5. **Mesurer avant de coder** ; un fix qui rate en boucle = mauvaise question ;
   les seuils se posent dans les VIDES des distributions (pente 11 %, miettes 15 m²).
6. **Budget agglo = corps physiques (quadratique)** + passes par cellule + AddInstances
   par lots. La 5090 n'était jamais le problème ; l'orchestration l'était.
7. Le développement d'une capacité se paie UNE fois (~heures) ; ensuite chaque réglage
   coûte 2 min et chaque application à la France entière est gratuite (murs 15 km/2,3 s).

## 6. NEXT STEPS (énoncés par l'utilisateur avant compact)
1. **Itérations quais restantes** — directement LIÉES au C2 : lisibilité promenade
   (bordurette), largeur/matériau des volées, les 4 volées BD TOPO hors pont à examiner,
   teinte brique des murs (arbitrage matériau ouvert), pied d'escalier sur le sable.
2. **⭐ CHANTIER C2 PONTS** (le suivant, acté) : tablier à SON Z (BD TOPO
   `position_par_rapport_au_sol=+1` + Z 3D), culées, parapets, **le sol/murs/escaliers
   s'effacent dessous** — cas d'acceptation : **Pont Saint-Pierre à sa vraie hauteur,
   passage piéton CONTINU dessous reliant les 2 escaliers verrouillés**. Développement
   ~demi-journée UNE fois → les 407 ponts du carré (et la France) gratuits ensuite.
3. Lot **TSR/AA** (vibration) + teinte touffes + couche de détail matériau (« vrai 4K »).
4. **Vent végétation** à rebrancher (avec re-mesure cache VSM).
5. **Streaming/HLOD agglo** (le teardown de bascule de map restant + silhouettes) —
   budget en corps physiques.
6. **Eau** SingleLayerWater (la Garonne placeholder appelle son matériau).
7. LIDAR_C : 2 verrous (UV égout, teinte ortho) puis arbitrage à l'échelle.
8. Props/marquages : rebranchement passages piétons (blanc, pleine largeur), OSM steps
   corroborés, feux/stops (graphe side-car prêt), bordurette de promenade.

## 7. Carte des documents et des chantiers
| Doc | Statut | Rôle |
|---|---|---|
| **`Doc/Reprise-Etat-Projet.md`** | ⭐ actif (v2, 02/08) | CE doc |
| **`Doc/Agent-Playbook.md`** | actif, §1-12 | LA bible opérationnelle (boucle §11, vol §12, pièges §10) — maintenu à chaque piège payé |
| `Doc/Vegetation-Pipeline-Cpp.md` | actif + bannière | recette végétation (amendée 02/08 : Nanite/collision/AddInstances) |
| `Doc/Reseau-Sidecar.md` | actif | graphe routier (socle trafic futur) |
| `Doc/Inventaire-Prototypes-Toits.md` / `J2-ProfilDesktop.md` | référence | toits / profil desktop |
| `Doc/Archive/2026-08-0*_*` | archives | v1 des docs supplantés (rien n'est supprimé) |
| `C:\LidarPoC\work\<LOT>\RAPPORT_*.md` + `ETAT.md` + `chronos.log` | par lot | FINITION_SOL (v1-v6), OMBRES, DISCONT (C1), VELOCITE, QUAIS (+iter 1-2), PIE |
| `SourceData/` | données | Murs/, Escaliers/, Promenade/ (side-cars committés), Reseau/ (+caches OSM), Sols/, ocsge_verts |

**Commits de la session** (chronique complète dans les messages) : `d189d15` (checkpoint
v1+v2 crossings/bordures) → `9ece765` (v3 bordurette+axiale) → `2663eab` (v4 helper Z
rendu+flags PIE) → `e5a95ed` (V5 autopsie proxys+verrous) → `cebf44a` (V6 Nanite+3×3) →
`be7de6a` (VSM) → `b57dcba` (C1 murs) → `b2a5ceb` (vélocité) → `5e6868f` (quais 4 volets)
→ `f1060cd` (iter1) → `d970483` (iter2) → `5588fdd` (PIE).

*Rappels transverses : maps protégées md5 (`_E2` 6942C9D6, `_Sol1` 4C67BBD8, LIDAR blanc
B71B0E40) ; jamais Sol1 ouverte pendant un import ; jamais toucher aux éditeurs
HELIOS/EnvolFlight/Survol/DroneFPV ; ferme `CityLab_Batch` en pause (hors git) ;
CC-BY interdit ; commits = coordinateur seul, trailer Claude Fable 5.*
