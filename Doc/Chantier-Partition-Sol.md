# Chantier PARTITION DU SOL — brief de conception (soumis à arbitrage)

> ⚠️ **2026-08-07 soir : chantier ABSORBÉ par `Chantier-Plan-de-Ville.md`** — le nouveau
> point d'entrée. Ses acquis (carte v2.1, contrat de Z, leçons, machinerie de preuve)
> deviennent les couches/entrées du PLAN DE VILLE ; É2-a est ÉTEINTE PAR DÉFAUT dans le
> code (`bPartition=false`, commit `ddfdb6cb`, prouvée inerte) ; É2-b/É3 et les
> arbitrages « É2-a-bis » sont SUPERSÉDÉS par le plan (matière + catalogue d'interfaces).
> Ce document reste la référence historique des verdicts É0→É2-a.

> **Statut : CONCEPTION — aucune exécution avant arbitrage utilisateur** (§8).
> Rédigé le 2026-08-07 après la saga berge (voir `Reprise-Etat-Projet.md` et les
> commits `6f5c3414..a8671110`). Objectif produit inchangé : *un rendu sans défauts,
> basé sur les données, qui fonctionne au niveau national — la cohérence globale.*

## 1. Le problème de fond (établi par ~15 nommages mesurés)

Tous les griefs de la berge ont vécu **aux coutures entre deux propriétaires du sol**,
jamais au cœur d'un objet :

| Grief (nommé au pixel) | Couture entre… |
|---|---|
| Vide au pied de l'escalier | objet C++ (escalier sans joues) ↔ sol de la cuisson |
| Lames pâles (« triangle ») | famille `tris` (dalle) ↔ famille `objets` (gradins, sans slot) |
| Bande « chantier » | régime zone (plan) ↔ régime dur (voirie/ouvrage) |
| Cratère / crête serpentante | monde organique (drapage MNT) ↔ monde architectural (plans) |
| Incident végétation (+75 160) | semis ↔ emprise d'ouvrage (repli silencieux sur le brut) |

Cause unique : **le sol n'a pas de carte de propriété**. Il est « le reste » — tout ce
que personne ne réclame retombe dans le drapage du MNT, et les ouvrages se posent
par-dessus sans se déclarer. Les frontières sont émergentes, donc défectueuses.
Trois rustines locales (palier, 3 formes de finition) ont été essayées et **toutes
contredites par la mesure** : on réparait au mauvais niveau.

## 2. Le précédent qui prouve le modèle

Les trois mondes cohérents du proto suivent DÉJÀ le modèle visé :
- **Bâtiments** : emprise de donnée (BD TOPO) = frontière autoritaire, un constructeur,
  propriétés à la création. Le sol s'arrête à l'emprise (`GroundRibbonsSkipped`).
- **Végétation** : semis global + loi vectorielle unique (« rien n'est semé sur un
  ouvrage », l'emprise fait autorité).
- **Routes** : le graphe est l'autorité (side-car gardien existant), un constructeur.

Le chantier = **étendre au sol le modèle qui marche** : fermer la dernière exception.

## 3. Le principe

> **P1 — Partition** : chaque m² de sol appartient à exactement UNE cellule, qui a un
> propriétaire et un régime. Couverture 100 %, zéro interstice émergent.
> **P2 — Frontières de donnée** : toute frontière de cellule est une LIGNE DE DONNÉE
> (bord de voirie du graphe, emprise de bâtiment, polygone de zone, emprise d'ouvrage
> déclarée) — jamais une limite émergente de calcul.
> **P3 — Loi d'interface unique** : toute frontière entre deux cellules reçoit sa
> couture par UN SEUL mécanisme (la fermeture systémique, étendue à tous les
> producteurs) ; les matériaux se résolvent à l'interface (slots des deux côtés).
> **P4 — Contrat des objets posés** : tout objet posé sur le sol déclare son emprise
> dans la partition et respecte la loi (joues fermées, slots déclarés).

Les lois existantes restent : anti-nappe (le Z ne s'interpole jamais — constantes par
cellule pour l'architectural, drapage pur pour l'organique), composition additive,
orientation par surface voisine, réglages canoniques en code + `garde_ancrage()`.

## 4. Les autorités et la préséance (⚠️ arbitrage §8-A)

Ordre de préséance proposé en cas de recouvrement (du plus fort au plus faible) :
1. **Emprises d'ouvrages posés** (bloc/berge : `bl2_emprise.wkt` existe ; escaliers,
   ponts/culées C++ : à déclarer — étape É2) ;
2. **Graphe routier** (voirie, bordures — side-car gardien existant) ;
3. **Emprises de bâtiments** (BD TOPO) ;
4. **Polygones de zones** (OCS GE), *clippés/snappés sur 1-3* : un polygone de zone
   ne franchit jamais une autorité supérieure — l'interstice zone/dur disparaît par
   construction (c'est LE remède de fond à la « bande ») ;
5. **Le reste = organique** (drapage MNT d'origine, intouché).

## 5. Les régimes par cellule

- **Architectural** : Z = constante(s) par cellule (plans). Attribution par règles
  nationales mesurables déjà en place ou à généraliser : voirie (régime routier
  existant), zones encaissées (règle « plan dominant p50 », 3 critères validés),
  emprises d'ouvrages (leur constructeur).
- **Organique** : drapage du MNT d'origine, strictement identique à aujourd'hui.
- Une cellule non attribuée est organique par défaut (« pas de donnée → pas d'objet »).

## 6. Les étapes livrables (chacune : verrous + commit + zooms)

- **É0 — LA CARTE (mesure seule, rien ne change au rendu)** : construire le side-car
  de partition (cellules, propriétaires, régimes, frontières) à partir des autorités
  §4. Verrous : couverture 100 % de la 3×3, 0 interstice > seuil, stats par
  propriétaire publiées, empreinte de la carte dans `garde_ancrage()`.
  *Fourchette : 1-2 h agent.*
- **É1 — LE SOL SE RECONSTRUIT DEPUIS LA CARTE** : les cellules organiques redonnent
  un drapage attendu bit-identique à l'existant ; les cellules architecturales
  reprennent les plans déjà validés ; la loi d'interface unique remplace les coutures
  ad hoc. Verrous : g6 sur les 4 poses de référence (≤ 0/0/24/12739), verrous berge
  (79,75 m, C2, veg/corps), idempotence, hors zones litigieuses bit-identique.
  *Fourchette : 2-4 h agent, itérations en district.* **Le gros morceau.**
- **É2 — LE CONTRAT DES OBJETS (build C++ complet, éditeur fermé/rouvert par agents)** :
  emprises déclarées dans la partition + joues fermées (« un escalier a des joues »)
  + slots pour la famille `objets`. Ferme : le VIDE, les LAMES, et toute la classe.
  Verrou structurel : 0 arête de bord verticale non cousue sur les objets posés
  (détecteur existant). *Fourchette : ~1 h build compris.*
- **É3 — LA BASCULE DES GRIEFS** : zooms A/B sur TOUS les points nommés de la saga
  (bande, cratère, vide, lames, liaison incurvée) aux coordonnées exactes —
  fermeture par l'utilisateur, grief par grief. *Fourchette : 30-45 min.*

Ordre É0→É1→É2→É3 ; É0 et É1 sont cuisson pures (Live Coding inutile), É2 est le
seul cycle de build. Chaque étape est commitée avec ses leçons ; retour arrière
possible étape par étape (`.bak` + git).

## 7. Ce que le chantier NE fait PAS

- Aucun nouveau matériau ni classe de sol ; le lot RENDU (bâtonnet, trait blanc,
  girons, patchwork, Fresnel) reste séparé et vient APRÈS.
- Aucune retouche au modèle deux-niveaux validé de la berge (promenade, gradins,
  volées, limons : INTOUCHÉS).
- Aucune « correction » des données : les lignes font autorité telles quelles ;
  on simplifie notre construction, pas la réalité.
- Pas de World Partition Unreal ni de refonte moteur : c'est un side-car + la cuisson.

## 8. ⚠️ Les arbitrages — TRANCHÉS le 2026-08-07

> **A : préséance validée telle quelle** (ouvrages > routes > bâtiments > zones
> clippées > organique) · **B : seuil 5 cm** · **C : toute la 3×3 dès É0** ·
> **D : GO É0→É3 enchaînées**, chaque étape commitée et présentée avant la suivante.
>
> **Amendement utilisateur (même jour) : « pas d'un méga-plan qui finit par échouer —
> tester ! »** → chaque étape est un TEST falsifiable avec critère d'arrêt :
> É0 doit rendre un VERDICT sur les 5 points nommés de la saga (la carte résout-elle,
> en donnée, la bande / le cratère / le vide / les frontières berge ?) — sinon STOP
> et on repense, pour le prix d'une mesure. É1 se teste d'abord sur le DISTRICT
> berge avant toute régé 3×3. Aucune étape ne s'engage sur la foi de la précédente
> sans ses propres verrous.

Formulation d'origine conservée ci-dessous pour mémoire :

- **A. La préséance §4** te convient-elle (ouvrages > routes > bâtiments > zones >
  organique) ? Des cas particuliers te semblent-ils inversés ?
- **B. Le seuil d'interstice** toléré dans la carte avant qu'une frontière soit
  déclarée défectueuse (proposé : 5 cm, le « collé » mesuré sur 72,8 % du tour).
- **C. Périmètre** : la partition s'applique à toute la 3×3 dès É0 (recommandé — les
  lois sont globales, les itérations restent en district) ou d'abord au district berge ?
- **D. Go de principe sur l'ordre É0→É3** et ses fourchettes (une grosse journée de
  travail agent au total, séquencée, interruptible à chaque étape).

## 9. É0 — VERDICT RENDU (2026-08-07, 61 min, mesure seule)

Carte `part/v1` construite sur 49 cellules (12,25 km²), idempotente, empreintes md5 des
4 sources. Répartition : organique 48,4 %, bâtiments 26,1 %, voirie 15,0 %, zones 8,7 %,
ouvrages 1,8 %. Recouvrements résolus par préséance (zone −215 557 m² etc.).
**3 des 5 points de la saga résolus en donnée** : cratère (zone architecturale, plan
141,01), liaison incurvée (à 0,02 m du bord déclaré de l'ouvrage), frontières de la berge
(100 % portées par des lignes de donnée après contrôle d'un artefact de mesure).
**1 dette attendue** : les 36 escaliers n'ont pas d'emprise surfacique (donnée = axe) → É2.
**1 NON RÉSOLU = le critère d'arrêt a fonctionné** : ⛔ la prémisse du §4 (« zones clippées
→ l'interstice disparaît ») est FAUSSE — *le clippage ôte les recouvrements, pas les
vides*. Zone du cratère : bord clippé encore à p90 0,69 / max 2,95 m du dur (25,5 % du
tour > 5 cm) ; global : 4 324 bandes fines > 5 cm (69 238 m², 0,565 % du domaine).
Le remède est un **SNAPPING dans la carte** (avancer le bord de zone jusqu'au dur,
segment par segment sur des lignes de donnée — l'objet qui a échoué 3 fois en cuisson 3D
devient faisable en 2D). → Arbitrage §10.

## 10. ⚠️ Arbitrage SNAPPING (ouvert)

Règle proposée : **une bande organique de largeur ≤ 3 m coincée entre deux cellules
architecturales est annexée par snapping** (au-delà de 3 m : elle reste organique, c'est
du vrai terrain). Côté d'annexion à trancher (dur vs zone vs plus-long-contact).
É0-bis = le snapping DANS LA CARTE, mesure seule, acceptations : bandes > 5 cm
restantes ~0 (parmi les ≤ 3 m), tour de la zone du cratère collé ≥ 99 %, idempotence,
PNG A/B de la carte. STOP possible au même prix qu'É0.

## 11. É0-bis — SNAPPING : verdict rendu (2026-08-07, STOP discipliné)

Règle arbitrée implémentée exactement (carte `part/v2`, idempotente, invariants exacts à
0,000000 m² près, PNG A/B) : **6 589 bandes annexées** (69 241 m² — voirie 58 540,
bâtiment 8 785, ouvrage 1 887, zone 30), 5 859 681 m² de vrai terrain intouchés.
Lecture de la règle validée par le coordinateur : les 3 690 bandes à voisin architectural
UNIQUE sont annexées aussi (aucun choix de côté ne se pose).
⛔ **Acceptation cratère non atteinte (87,24 % contre ≥ 99 %) → STOP appliqué.** Diagnostic
complet : « 0 bande restante » était une tautologie (on retire des plages entières) ; le
vrai résidu = les **APPENDICES** — parties minces (< 3 m) accrochées aux grandes plages
organiques, **208 849 m² = 3× ce que la règle par plage a su annexer**. Simulation chiffrée
(non implémentée) : la variante MORPHOLOGIQUE (annexer les PARTIES minces, ouverture
r=1,5 m) porte le cratère à **97,92 %** et les 712 zones à 69,64 % ; plafond insensible au
seuil (98,18 % à 5-6 m) ; les ~2 % ultimes = lacunes face à une AUTRE ZONE (classe à part) ;
477 m² de vrai terrain mince (talus, berges) ne touchent aucun dur et resteraient organiques.
**L'acceptation « ≥ 99 % » était elle-même mal calée** : la bonne mesure est « 0 lacune
mince (0,05-3 m) restante » — le tour non collé résiduel doit être exclusivement de la
VRAIE frontière (terrain ≥ 3 m).
→ É0-ter proposé : variante morphologique + acceptation re-calée (arbitrage ci-dessous).

## 12. É0-ter & É0-quater — LA CLÔTURE D'É0 (2026-08-07, sous mandat d'autonomie)

**É0-ter (morphologique, 40 min)** : la découpe par tampon ferme les lacunes (cratère 0
lacune organique) MAIS invente **102 987 m (15,3 %) de frontières en arcs** → BLOQUE de
doctrine (P2). Au passage : juge de tour CORRIGÉ (une lacune n'existe que si de
l'organique est réellement posé sur le segment — l'ancien comptait des trajets traversant
le corps des zones concaves), rabotage itératif jeté (se nourrit de ses propres découpes),
forme fermée v4 non livrée (5 474 m² inexpliqués = intégrité d'abord).
**É0-quater (prolongements, ~75 min)** : la voie (c) testée au banc des 10 pires
sandwichs, 3 dessins, 3 causes nommées. Elle **tient P2 à la lettre** (0 m de bord
illégitime) mais **trahit son esprit** : 2,35 m de frontière neuve par m² annexé =
**4,8× pire que la voie (b) refusée pour cette raison même**, rendement plafonné ~43 %
(la lacune a des bords en arcs qu'aucune droite n'épouse). Voie NON généralisée.
**Résultat mesuré trois fois : aucune découpe de carte ne ferme les lacunes sans
inventer de la frontière.**

### La décision de sortie (coordinateur, dans le mandat — le repli (a) déjà arbitré)
> **La carte officielle d'É0 est la v2** (`part/v2`, empreinte `51b88070…`) : complète,
> idempotente, invariants exacts, **0 m de frontière inventée**, 69 241 m² de bandes
> isolées annexées. **Les 106 855 m² de sandwichs restants (0,87 % du domaine) ne sont
> pas des défauts de la carte : ce sont des COUTURES**, transférées au cahier des charges
> d'É1 — la loi d'interface (P3) devra traiter les frontières zone/organique ≤ 3 m comme
> des coutures (11,77 % du tour des zones). La carte dit QUI possède ; la couture dit
> COMMENT deux propriétaires se rencontrent. P2 et P3 restent intacts tous les deux.
**« CARTE PRÊTE POUR É1 : OUI »** sous cette définition — l'engagement d'É1 est chiffré
ci-dessus et devra figurer dans son brief. Fin du mandat d'autonomie : **É1 = GO
utilisateur explicite** (premier changement réel du rendu).

## 13. É1 — verdict (2026-08-07, 2 BLOQUE en 39 min, zéro ligne écrite pour rien)

**BLOQUE 1 (périmètre)** : le sol de la 3×3 est bâti en C++ (`ImportCitySurfaces` :
drapage MNT, rubans de voirie) — la cuisson Python ne cuit que le lot berge. Le
contournement data-driven (forcer une grille de nœuds Z par side-car) existe et a été
REFUSÉ : c'est une nappe. **BLOQUE 2 (le plus structurant)** : *le Z du propriétaire
n'est pas publié* — `routes_3x3.json` est 2D pur (vérifié sur 500 tronçons), le Z
routier naît au cook. 93,9 % des bandes du district sont donc non-constructibles en
Python, et draper la bande sur le MNT reproduirait le défaut même qu'on corrige.
**Décision (coordinateur)** : É1 versée entière dans É2, le **lot-contrat unique**,
dont la dette compte désormais 4 points — le 4ᵉ débloquant les autres :
1. `ImportCitySurfaces` lit la carte v2 (autorité de propriété du drapage) ;
2. les 69 241 m² de bandes en rubans (constructeur Ribbon) ;
3. la loi d'interface au sol de ville (toutes paires, dont zone/organique ≤ 3 m —
   l'engagement des 106 855 m²) ;
4. **LE CONTRAT DE Z : la cuisson publie le Z de chaque propriétaire le long de ses
   lignes de frontière** (side-car « frontière → profil de Z »). La carte dit QUI ;
   ce side-car dit À QUELLE COTE ; la couture dit COMMENT.
+ la dette d'É0 : slots de la famille `objets` (lames), joues d'escaliers, emprises
d'escaliers déclarées (axe → polygone, comme la voirie).
Découpage proposé : **É2-a = le sol** (points 1-4, un build, verrous sol + zooms bande
du cratère) puis **É2-b = les objets** (slots/joues/emprises, un build, zooms lames +
vide). Chacun testable, chacun commité.

## 14. Références

Saga et nommages : `Reprise-Etat-Projet.md` (v3) + commits `6f5c3414..a8671110`.
Doctrine : Playbook §13 (lois), §11 (boucle), Brief-Template.md (workflow agents).
Précédent sol/routes : mémoire `survol-sol-routes-chantier` (différenciation
vectorielle abandonnée ; le graphe side-car gardien = graine de la carte).
