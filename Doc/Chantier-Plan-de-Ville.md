# Chantier PLAN DE VILLE — le document maître

> **Statut : ARBITRÉ, prêt à exécuter (étage 1). Rédigé le 2026-08-07** après la journée
> partition (voir `Chantier-Partition-Sol.md`, désormais ABSORBÉ par ce chantier).
> **LE point d'entrée du travail en cours.** Objectif produit inchangé : *un rendu sans
> défauts, basé sur les données, qui fonctionne au niveau national — la cohérence globale.*

## 1. Pourquoi ce virage — le diagnostic sans complaisance

Constat de l'utilisateur (2026-08-07, verbatim d'esprit) : « les détails créent de
nombreux problèmes qu'on a du mal à corriger… des heures/jours à essayer une approche en
espérant que cela fonctionne… il semble qu'on n'ait pas de base ou de plan ».

**Le diagnostic est structurel, pas un défaut d'exécution** : le pipeline actuel *infère*
le monde construit **pendant la construction, en 3D, dans Unreal** (drapage du relevé
brut, masques, coutures calculées au build). Chaque défaut des derniers jours est un
artefact d'inférence : la bande = l'écart jamais réconcilié entre deux jeux de données ;
le cratère et le « terrain courbé » = le relevé historique drapé tel quel ; les tracés
dans le parc = une frontière de donnée invisible dans le monde ; le triangle = une couture
de notre propre échafaudage. On corrigeait les symptômes un par un, à l'endroit le plus
cher du monde pour corriger quoi que ce soit (cycles build/régé/capture).

**La recherche données (faite, sourcée §12)** : LE « vrai plan » n'existe chez personne.
- BD TOPO = un inventaire d'objets (axes + largeur, emprises, surfaces) — pas un plan
  d'exécution : aucun bord de chaussée levé, aucun trottoir, aucun niveau projet.
- OSM = même nature ; OCS GE = occupation du sol (écarts mesurés jusqu'à 2,95 m au dur).
- MNT / LiDAR HD = le monde TEL QU'IL EST (un relevé, l'inverse d'un plan).
- PCRS = le plus proche d'un vrai plan (bordures ±5-10 cm) mais **raster** en
  Haute-Garonne (une photo 5 cm, pas des lignes) ; ni niveaux projet ni sous-sol.
- Réseaux enterrés = données privées (DT-DICT). Normes de voirie (CEREMA : bordures
  12-14 cm, dévers, pentes) = des documents publics, paramétrables — pas des données.

**La conclusion** : puisque le plan n'existe pas, on ne l'espère plus — **on le COMPILE
d'abord, puis on construit bêtement.**

## 2. L'architecture en deux étages

> **La carte dit QUI possède · le nivellement dit À QUELLE COTE · la matière dit EN QUOI
> · la table des interfaces dit COMMENT deux parcelles se rencontrent — et le
> constructeur EXÉCUTE, sans plus jamais décider.**

- **Étage 1 — LE COMPILATEUR DE PLAN** : Python pur, **zéro Unreal** (ni éditeur, ni MCP,
  ni build). Entrées = les side-cars existants (carte v2.1, graphe voirie, MNT, OCS GE,
  masques/semis). Sortie = `plan_ville/v1`. Boucle de test : modifier → exécuter
  (secondes-minutes) → regarder dans le visualiseur (§4).
- **Étage 2 — LE CONSTRUCTEUR** : C++ **lecteur-exécutant** dans le pipeline d'import
  existant (qui lit déjà des dizaines de side-cars). Il valide (empreintes + complétude),
  puis exécute : parcelle → constructeur selon la loi de Z ; frontière → pièce du
  catalogue. Les constructeurs existants (rubans, murs, drapage, recettes bâtiments)
  sont réutilisés **en exécutants d'ordres explicites** — l'étage 2 consiste surtout à
  SUPPRIMER du code de décision. **Un plan incomplet ou à l'empreinte fausse = build
  refusé, jamais deviné** (garde md5 prototypée, leçon CRLF comprise : double empreinte
  octets + contenu logique LF).

## 3. L'output : `plan_ville/v1` (le contrat)

Un JSON versionné avec empreintes (+ annexes si profils lourds). Pour chaque m² :

- **① QUI** — la parcelle et son propriétaire (ouvrage/voirie/bâtiment/zone/organique).
  ✔ FAIT : la carte v2.1 (`SourceData/Partition/carte_v2.json`, commit `8427e81b`),
  prouvée à l'échelle (12,25 km², couverture 100 %, 0 interstice, 0 frontière inventée).
- **② À QUEL NIVEAU** — la loi de Z par parcelle, TROIS formes seulement (anti-nappe) :
  **CONSTANTE** (place, pelouse encaissée — la règle p50 validée au cratère, généralisée),
  **PROFIL PAR TRONÇON** (rue : profil en long échantillonné sur le MNT puis RÉGULARISÉ
  par les normes de voirie — pente constante par segment, plafonds de pente/ressaut),
  **DRAPAGE** (organique pur : MNT intouché).
  ⚠️ **Le niveau projet n'est JAMAIS le relevé** : le contrat de Z d'É2-a (échantillons
  du monde tel quel) est une ENTRÉE du nivellement, pas une recopie.
- **③ EN QUELLE MATIÈRE** — minéral / végétal / eau. Le champ dont l'absence a causé la
  régression du 07/08 (pierre cousue entre deux herbes, arbres sur rubans). Le conflit
  « le parc visible déborde le polygone de donnée » se résout ICI : le végétal visible
  (masque peint + semis) est une couche du plan, avec préséance nationale — le semis
  obéit au plan, jamais l'inverse.
- **④ COMMENT** — la table des interfaces : CHAQUE frontière reçoit une résolution d'un
  **catalogue fermé national** : `bordure` (h≈14 cm) · `mur` (h=ΔZ) · `affleurement`
  (ΔZ≤2 cm) · `emmarchement` · `talus` · `rien` (végétal/végétal — l'herbe absorbe).
  Résolution calculée depuis (matière A, matière B, ΔZ le long) ; hors catalogue →
  **liste d'arbitrage chiffrée** (bornée). Exemples : pelouse/chaussée du cratère →
  `bordure, chaussée 141,00 / pelouse 141,14` ; pelouse/bois du parc → `rien` ;
  pied du bloc berge → `mur` (déclaré, plus découvert).

**Invariants du plan** (avant tout build ; le validateur livré = le même lint que le
C++ exécutera) : couverture 100 % ; 0 frontière sans résolution ; ΔZ borné par la
résolution choisie ; cohérence des biefs ; **invariants d'INTÉGRATION** (les leçons du
07/08 gravées : 0 objet minéral sur frontière végétal/végétal ; 0 instance semée sur du
dur du plan) ; idempotence ; empreintes.

## 4. Le visualiseur web local — l'outil de revue permanent

Une page HTML locale (Leaflet/MapLibre), sans serveur : couches togglables
(QUI/NIVEAUX/MATIÈRE/INTERFACES), **clic sur une parcelle → sa fiche**, clic sur une
frontière → sa résolution, **fond = orthophoto IGN / PCRS 5 cm servis en tuiles par
data.geopf.fr** → on compare le plan **au réel photographié**, pas seulement à notre 3×3.
**Signets de la tournée des pires cas** (§5) intégrés. Raison d'être arbitrée par
l'utilisateur : les pré-validations se font SUR LE PLAN, sans éditeur ni MCP — l'inverse
de la validation visuelle actuelle, coûteuse et tardive. Cet outil est permanent : c'est
le bureau d'urbanisme du projet, jusqu'à l'échelle nationale.

## 5. Les mécanismes anti-récidive (le doute légitime des « 80 % réutilisés »)

Ce qui était testé à grande échelle : la machinerie de preuve et la carte COMME DONNÉE.
Ce qui ne l'était PAS : **les effets visibles des règles** (jugés à l'œil sur UN lieu —
le cratère — pour 6 589 bandes et 18 zones aplanies). D'où, structurellement :
1. **La tournée d'échantillonnage automatique** : le compilateur génère la liste de revue
   — les N cas LES PLUS EXTRÊMES de chaque règle (plus grande bande, zone au plus fort
   relief, profil le plus pentu, plus longue interface de chaque type…) en signets
   cliquables du visualiseur. On juge une règle sur SES PIRES CAS, systématiquement.
2. **Compteur d'application par règle** (« appliquée 4 313 fois, revue sur 12 cas ») :
   la couverture de revue devient une mesure ; règle massive peu revue = signalée.
3. **Aucune décision de l'ancien pipeline n'entre dans le compilateur** : les règles sont
   réexprimées DE ZÉRO dans les termes du plan. Seules entrent les données prouvées et
   les preuves. L'étage 2 garde interrupteur + district-first (retour arrière en 1 ligne,
   patron prouvé le 07/08).

## 6. Périmètre du plan v1

| Élément | Dans le plan v1 ? |
|---|---|
| Sol complet (routes, trottoirs/bandes, places, pelouses, berge, eau) | ✅ parcelles + niveaux + matières |
| Toutes les frontières (bordures, murs, affleurements…) | ✅ table des interfaces, catalogue fermé |
| Bâtiments | ✅ leurs EMPRISES (le sol s'y arrête) — la 3D garde la recette E2 actuelle |
| Ponts / ouvrages | ✅ emprises + niveaux (constructeurs existants exécutent) |
| Végétation | ✅ comme MATIÈRE + règle de semis (« rien sur du dur du plan ») |
| Mobilier urbain, props, marquages fins | ❌ hors v1 — couche ultérieure |

**Périmètre géographique v1 : les 12,25 km² du domaine, d'un coup** (arbitré — le calcul
scale : É0 a partitionné le tout en 90 s). Zooms de comparaison à l'existant : cratère,
berge, parc.

## 7. Le lot compilateur (étage 1) — étapes et fourchettes

- **A. Partition** ✔ (carte v2.1).
- **B. Matière** : classification des parcelles + couche « végétal visible » + conflits
  donnée/visible chiffrés.
- **C. Nivellement** : voirie par tronçon (MNT sur l'axe → profil régularisé) ; zones →
  plan p50 ; ouvrages → leurs constructeurs ; organique → MNT ; propagation par les
  interfaces (bordure ⇒ « chaussée + 14 cm »…).
- **D. Table des interfaces** : (matière, matière, ΔZ) → résolution ; reste → arbitrages.
- **E. Juges + rendus + VISUALISEUR** : invariants, 4 vues, fiches, signets pires cas,
  rapport chiffré, validateur.
**Fourchette : ~60-90 min d'agent** (zéro Unreal), jalon-image ~toutes les 20 min.
**Revue arbitrée : EN UNE PASSE (option b)** — les 4 couches + la tournée des pires cas.
La 3×3/12 km² se CONSTRUIT (étage 2) seulement après validation utilisateur du plan.

## 8. Étage 2 — après validation du plan (rappel)

Lecteur C++ (patron side-car existant), constructeurs existants en exécutants,
suppression du code de décision, interrupteur `bPlan` + district-first, verrous habituels
(g6, verrous berge, végétation, idempotence) + les invariants d'intégration du plan.

## 9. État des lieux hérité (pour la reprise post-compact)

- **Partition (chantier absorbé)** : carte v2.1 = SOURCE officielle commitée ; contrat de
  Z publié (`SourceData/Partition/profil_z_v1.json`, 276 191 pts — devient une ENTRÉE du
  nivellement) ; **É2-a : code C++ complet mais ÉTEINT PAR DÉFAUT** (`bPartition=false`,
  commit `ddfdb6cb`, prouvé inerte) après régression visuelle utilisateur (végétation sur
  rubans + 52,7 % du linéaire cousu en plein végétal) ; les 4 arbitrages « É2-a-bis »
  sont SUPERSÉDÉS par le plan (matière + catalogue les résolvent en conception).
- **Berge** : map SAINE, état visuel validé du matin du 07/08. Griefs FERMÉS : limons
  (géométrie + apparence), cratère (plan p50). RESTANTS → absorbés par le plan/étage 2 :
  lames du triangle (couture de la face du bloc — le catalogue en décidera), bande
  interstitielle (= les bandes du plan), vide de l'escalier C++ (emprises d'escaliers =
  dette de déclaration au plan), liaison incurvée (frontière = ligne d'emprise, acquis
  carte). Lot RENDU inchangé, plus tard : bâtonnet, trait blanc x=455, contraste girons,
  patchwork du sol.
- **DETTES / ENQUÊTES** : ① g6 rend 91/2794/24/13441 contre la référence 0/0/24/12739 —
  prouvé NEUTRE aux lots partition (A/B triple), cause ANTÉRIEURE à élucider (suspect
  n°1 : le redémarrage de l'éditeur entre les mesures) ; ② 227 bandes « non-ruban »
  (4 058 m², chemin polygone) ; ③ 36 escaliers sans emprise surfacique (axe → polygone).

## 10. Les arbitrages actés le 2026-08-07 (utilisateur)

Périmètre 12,25 km² d'un coup · revue en UNE PASSE (b) via visualiseur (« plus de
facilité à faire les pré-validations que la validation visuelle dans l'éditeur via
MCP ») · output = JSON contrat + lecteur C++ exécutant · visualiseur web local avec fond
ortho/PCRS · les 3 mécanismes anti-récidive §5 · fournitures hors v1 · le plan 2D/2,5D
n'est pas une ville 3D (les volumes restent à l'étage 2).

## 11. Workflow du chantier (inchangé sur le fond)

Coordinateur = métronome (ne code pas les lots) ; agents Opus 5 ; briefs collés depuis
`Brief-Template.md` ; watchdog sur progress.log (`JALON:`/`BLOQUE:`/`TERMINE:` stricts —
et inclure BLOQUE final comme fin de watch) ; jalons-IMAGES pour l'étage 1 ; commits par
le coordinateur avec leçons ; « pas de méga-plan — TESTER » : chaque étape a un critère
d'arrêt. Leçons du 07/08 à ne pas re-payer : un attendu auto-référent ne prouve rien
(`veg_delta 0`) ; les acceptations GÉOMÉTRIQUES ne suffisent pas — il faut des
acceptations d'INTÉGRATION ; le juge final reste l'œil utilisateur — le visualiseur le
fait intervenir AVANT la 3D, plus après.

## 12. ÉTAGE 1 LIVRÉ (2026-08-07 soir) — état pour la reprise

**Le plan est compilé, exporté, installé et commité.** Chiffres finaux (après les 2
corrections de mesure ci-dessous) : 49 cellules / 12,25 km² (domaine = carte v2.1,
les 3 cellules disque hors carte écartées et chiffrées), **46 424 parcelles,
96 986 interfaces, 990 061 instances de semis retenues (81,0 %)** ; les 9 invariants
au vert (couverture 100,000000 %, interstices 0, recouvrements 0, résolutions 100 %,
ΔZ borné, 0 minéral sur végétal|végétal, 0 semis sur dur, idempotence bit-identique).
- **Installé** : `SourceData/PlanVille/` (manifestes + `data/` = 147 side-cars PAR
  CELLULE + `plan_index.json` avec double empreinte de chaque fichier — `data/` est
  gitignoré : 297,6 Mo régénérables bit-identiques par `Tools/PlanVille/c8_export.py`).
- **Compilateur commité** : `Tools/PlanVille/c0..c8` (venv `C:\LidarPoC\venv`) ;
  visualiseur : `Tools/PlanVille/visualiseur/index.html` (données .js régénérées par
  c6 dans `C:\LidarPoC\work\PLAN\visualiseur\`, double-clic, fond ortho IGN).
- **Convention du contrat** : parcelles/frontières DÉCOUPÉES par cellule (lecteur
  district-first : une cellule se construit sans lire les autres) ; `cellule_porteuse`
  + `entiere` + totaux pour reconstituer les entités distinctes.
- **Arbitrage DIFFÉRÉ (décision utilisateur)** : les 5 frontières hors catalogue du
  coteau SE (dZ 12,1-16,6 m, 217,6 m, toutes contre `bnd/8730`) = artefact de la loi
  CONSTANTE p50 appliquée à des parcelles à fort relief (13-16 m sous la parcelle).
  L'utilisateur tranchera **en A/B 3D à É2-1** (variante p50 vs éligibilité au relief
  ~2,5 m) — il ne pouvait pas juger sur la 2D. Le lecteur C++ REFUSE ce secteur tant
  que non résolu ; le premier district d'É2-1 sera un secteur 100 % résolu.
- **Leçons de la passe export** (payées, corrigées à la racine) : ① lecture par
  tranches avec report d'octets = instances comptées 2× aux jointures → lecteur
  unique partagé juges/export ; ② frontière prise sur `A∩B` = 546 polygones dégénérés
  (`.length` = périmètre, longueur ~2×) → `A.boundary ∩ B.boundary`, linéaire par
  définition. Les comptes manifeste↔export se reconstituent désormais exactement.
- **Témoin gelé frais** : `/Game/Dev/ProtoE2Sol2/L_ProtoSols_E2_Sol2_Temoin`
  (183,8 Mo, dupliqué+vérifié le 07/08 20h01 ; Sol1 = archive historique). Les maps
  ne sont PAS dans git — la vérité committée = données + code.
- **Dettes conservées** : queue de contre-preuve Z inexpliquée (p95 2,05 m — hypothèse
  non prouvée : points le long des bandes, là où le monde construit s'écarte du MNT) ;
  787 tronçons > plafond de pente 12 % (signets) ; masques 36/49 cellules (344
  parcelles en règle de repli) ; enquête g6 antérieure.
- **É2-0 FAIT (07/08 21h42)** — lecteur-validateur C++ livré : `PlanVille.h/.cpp` +
  spec `CityLab.PlanVille` 4/4 PASS (plan complet 6,13 s / 152 empreintes, comptes
  reconstitués = manifeste, DISTRICT-FIRST prouvé à 3 fichiers/cellule, SABOTAGE et
  MANQUE refusés explicitement, bac à sable Saved/Tests — SourceData jamais écrit) ;
  `bPlan=false` inerte (1 seule occurrence du symbole). **1 cellule refusée à la
  construction : (2,−2)** — porte les 5 `arbitrage_demande` du coteau. Leçons :
  les interfaces n'ont pas de `cellule_porteuse` (identité = couple (a,b), compte
  distinct GLOBAL, comptes par cellule = pièces) ; UE 5.8 : clés de `FJsonObject`
  en `UE::FSharedString`.
- **Prochaine étape : É2-1** — le SOL d'un district construit depuis le plan
  (district 100 % résolu d'abord), puis l'A/B 3D du coteau pour l'arbitrage
  utilisateur (p50 vs éligibilité au relief).

## 13. Références

`Chantier-Partition-Sol.md` (préhistoire : É0→É2-a, verdicts et leçons) ·
`Reprise-Etat-Projet.md` (v3, la saga berge) · `Agent-Playbook.md` · `Brief-Template.md`.
Commits clés du 07/08 : `6f5c3414` limons → `a2ea7e3a` UV → `482b4661` anti-pli/palier →
`7ca89817` revert palier → `1049b5c6` pelouse p50 → `9e596cfa`/`f59378cd`/`2dd01165`/
`9c78f635` partition É0 → `e56b6449` verdict É1 → `8427e81b` carte source → `ce489fe1`
É2-a → `ddfdb6cb` extinction scellée.
Données (recherche du 07/08) : PCRS data.gouv.fr · standard CNIG PCRS · Géoservices IGN
PCRS · PCRS raster 5 cm (Haute-Garonne = raster) · data.toulouse-metropole.fr (filaire de
voirie ; pas de jeu « trottoirs ») · tuiles data.geopf.fr pour le fond du visualiseur.
