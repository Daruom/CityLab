# CONCEPTION — LA VILLE PLAN-NATIVE (le plan complet du chantier)

> **Rédigé le 2026-08-08, arbitré avec l'utilisateur.** Succède à l'exécution
> « greffe sur l'existant » de l'étage 2 (post-mortem au §12 de
> `Chantier-Plan-de-Ville.md`). **Principe fondateur : on construit EN
> CONNAISSANCE DE CAUSE** — on SAIT tout ce qui ira sur le terrain (la donnée est
> complète) ; le nivellement est donc résolu GLOBALEMENT, tous éléments connus,
> avant le premier triangle. La cohérence vit dans le PLAN (prouvable), plus dans
> l'ordre de construction (fragile).

## ⛔ LA RÈGLE D'OR (utilisateur, non négociable, systématique)
« ON VEUT LA COHÉRENCE GLOBALE — on ne se perd pas dans les détails, on prend à
chaque fois du recul pour comprendre POURQUOI on a une anomalie et on corrige À LA
SOURCE. » Opérationnalisée : ① cause STRUCTURELLE avant tout lot ; ② toute
livraison = A/B contre le TÉMOIN aux mêmes poses + verdict visuel GLOBAL écrit du
coordinateur (y compris « moins bien ») ; ③ mesures d'agents confrontées avant
relais ; ④ un cas mal emboîté = un bug du SOLVEUR corrigé pour tous ses
semblables, jamais une rustine locale.

## Les 3 étapes

### ÉTAPE 0 — LE TERRITOIRE EN DONNÉES (le recensement exhaustif)

**Objectif : rendre l'oubli IMPOSSIBLE.** L'exhaustivité ne sort pas d'un
brainstorm mais 1) des NOMENCLATURES officielles des données et 2) d'une grille
des MILIEUX nationale. Rien ne peut être ignoré silencieusement.

- **Le recensement** : énumérer TOUTES les natures présentes dans la donnée de
  l'agglo (BD TOPO : ~9 thèmes, toutes classes/natures ; OSM en complément ;
  OCS GE ; hydro ; RGE ALTI/LiDAR) sur les 461 km² de `SourceData/Agglo` —
  comptes, surfaces/longueurs, exemples localisés.
- **La table de statut** (le livrable central) : chaque nature → UNE mécanique
  (voir §Mécaniques) → un statut parmi : `couvert` / `hors-v1 déclaré` (avec
  raison) / **`NON COUVERT` (liste bornée = arbitrage)**. Invariant : 100 % des
  objets de la donnée ont un statut ; le compilateur REFUSE le silence.
- **La grille des MILIEUX (anti-« on a oublié les côtes »)** : la nomenclature est
  NATIONALE par construction — chaque milieu a sa ligne MÊME ABSENT de Toulouse,
  avec le statut `famille prête, hors périmètre Toulouse` :
  littoral (plages, falaises, digues, ports, marais salants) · montagne (pentes
  extrêmes, falaises, stations, remontées) · rural (champs, haies, hameaux,
  chemins) · fleuves majeurs & estuaires · îles · zones industrielles/portuaires/
  aéroportuaires · frontière ville→campagne · voies ferrées & gares · réseaux
  aériens (lignes HT, pylônes). Quand un nouveau territoire arrive, on remplit des
  lignes EXISTANTES — on ne découvre pas de catégorie.
- **Revue utilisateur** : la table entière en UNE passe (visualiseur/tableau),
  arbitrage des statuts. C'est LA garantie contre les surprises d'échelle.

### Les 5 MÉCANIQUES (tout élément se range dans une, sinon NON COUVERT)
| # | Mécanique | Traite |
|---|---|---|
| ① | **Gabarit linéaire** (profil en travers extrudé sur l'axe, à la BTP : bordures/caniveaux/trottoirs INCLUS dans le gabarit) | rues par classe, autoroutes (TPC/glissières/BAU), voies ferrées, canaux (cuvette+berges+halage), pistes cyclables, pistes d'aéroport |
| ② | **Pièce nodale** (aux nœuds du graphe) | carrefours ✔, ronds-points (plateau+îlot), échangeurs (composition de bretelles) |
| ③ | **Objet d'ouvrage** (constructeur side-car) | ponts ✔, gradins/escaliers ✔, écluses, trémies, chaussées d'eau (Bazacle), édicules (émergences métro/parkings) |
| ④ | **Surface de plan** (parcelle + loi de Z + matière) | parcs ✔, places ✔, parkings, terrains de sport, cimetières, zones d'activité |
| ⑤ | **Instance ponctuelle** (semis réglé) | arbres ✔ ; plus tard : mobilier, fontaines, feux, candélabres |
+ transversales : **TERRASSEMENT** (raccordement au terrain : talus normalisés /
soutènements calculés) · **règle du SOUS-SOL** (n'existe que par ses émergences).

### ÉTAPE 1 — LE PLAN (enrichi : le nivellement en connaissance de cause
### + LA MATRICE DE COHÉRENCE)

**⭐ LA MATRICE DE COHÉRENCE (exigence utilisateur 08/08 : « on veut savoir la
cohérence entre TOUS les éléments — absolument tout »)** : les familles issues du
recensement (L0) croisées TOUTES CONTRE TOUTES. Chaque case = la relation entre
deux familles, avec obligatoirement : un CONTRAT déclaré (ancrage, exclusion,
appui, dégagement, pièce de jonction) + un INVARIANT chiffré cible 0, mesuré par
le compilateur + ses pires cas dans la tournée — OU un statut explicite (« aucune
interaction possible » justifié / « NON COUVERT » en liste bornée d'arbitrage).
**Une paire sans statut = compilation refusée.** Exemples : bâtiment×sol (0 base
hors pad, 0 double dalle) · mur×sol (0 flottement) · pont×terrain (0 vide de
culée) · pont×dessous (0 violation de gabarit/tirant d'air) · escalier×terrain
(0 vide sous volée) · arbre×bâti (0 collision) · semis×dur (0 — existe déjà).
Les invariants d'intégration historiques étaient des cases isolées de cette
matrice ; chaque surprise passée venait d'une case manquante. **Voir n'est pas
savoir : la maquette 3D échantillonne pour l'œil, la matrice PROUVE pour tout.**

**LA MAQUETTE BLANCHE 3D (organe de revue humain de l'étape 1)** : maquette
d'urbanisme générée du plan — terrain harmonisé + gabarits + volumes bâtis +
ouvrages en masses — dans le NAVIGATEUR (three.js dans le visualiseur : tournée
des pires cas EN 3D, fiches, chargement par cellule) ; export Blender (.blend,
bpy — Blender 5.2 headless dispo) en appoint pour inspection fine. L'utilisateur
survole la ville harmonisée avant tout triangle Unreal.

Acquis conservés : QUI (partition 100 %) · MATIÈRE · INTERFACES (catalogue fermé)
· semis · juges/empreintes/idempotence · contrat par cellule · visualiseur.
**Quatre enrichissements :**
1. **LES ASSIETTES (règle de l'assiette)** : tout objet déclaré (pont, escalier,
   gradins, bâtiment, écluse…) déclare son emprise d'appui ; le nivellement RÈGLE
   le terrain à la cote de l'objet sur cette assiette (+ marge de seuil pour les
   bâtiments) ; le raccord au naturel est une pièce déclarée. Le terrain s'adapte
   à l'objet, jamais l'inverse.
2. **LE TERRASSEMENT** : partout où un élément réglé rencontre le terrain,
   l'emprise de raccordement est CALCULÉE (talus à pente normalisée ; au-delà d'un
   seuil → mur de soutènement ; contre l'eau → mur de quai, pièce nouvelle du
   catalogue qui résoudra les 11 arbitrages minéral|eau).
3. **LE SOLVEUR D'HARMONISATION** : le nivellement = solution GLOBALE sous
   contraintes, tous éléments présents (biefs, tabliers, assiettes, profils,
   plateaux, pads, communautés). Règles : le solveur ajuste les VALEURS des lois,
   jamais leur forme (anti-nappe intact, contrat lisible par cellule) ;
   propagation hiérarchique + relaxation DÉTERMINISTE (idempotence exigée) ;
   **invariant de fidélité** : |z_projet − z_relevé| borné hors emprises déclarées
   (« lisser sans dénaturer », chiffré) ; conflits insolubles → liste d'arbitrage
   bornée.
4. **LES GABARITS** : bibliothèque nationale de profils en travers par classe
   (largeurs de la donnée + normes CEREMA paramétrées), pièces nodales, pièces du
   catalogue étendu.
**Pré-validation navigateur (une passe)** : cartes nouvelles — écarts au relevé,
terrassements, assiettes — s'ajoutent à la carte des marches ; tournée des pires
cas et compteurs de revue étendus. TOUT se juge au visualiseur avant la 3D.

### ÉTAPE 2 — LA CONSTRUCTION (`L_PlanVille_A`, map fraîche)

**Trois maps, rôles étanches** : `L_PlanVille_A` = le chantier (100 % généré,
rien d'importé) · `L_ProtoSols_E2_Sol2` = la map utilisateur (intouchée) ·
`Temoin` = la référence des A/B (lecture seule).
**Ordre d'exécution** (la cohérence étant déjà dans le plan, l'ordre ne sert que
les dépendances) : lecteur-validateur (refuse un plan incomplet) → terrassements +
assiettes (le sol qui sait tout) → gabarits de voirie (bordures incluses) + pièces
nodales → surfaces → objets posés sur leurs assiettes (ouvrages via constructeurs
existants ; bâtiments recette E2 ANCRÉE aux pads — une dalle = une vérité) →
pièces restantes (murs, emmarchements, quais) → semis exécutant. Plus tard :
mobilier/marquages (couche ⑤) et lot RENDU.
**Machinerie réutilisée** : lecteur PlanVille, PlanSol (évolue en gabarits),
jupes, matériaux mesurés, re-pose murets, exécuteur de semis.
**Verrous par lot** : idempotence · comptes = manifeste · A/B vs Témoin aux mêmes
poses avec verdict global écrit · fourchettes honnêtes (calibrées sur l'historique
mesuré, pas sur l'optimisme).

## ⚠️ PROVENANCE OBLIGATOIRE DES RÈGLES (garde-fou utilisateur 08/08)
« National » = une exigence de FORME (règle générique, zéro cas particulier local
— c'est la règle d'or elle-même), PAS un blanc-seing pour le contenu hérité.
**Toute règle du compilateur porte sa PROVENANCE** : `réel` (norme BTP/CEREMA/
accessibilité) / `donnée` / `arbitrage utilisateur` (tracé) / `héritée` (ancien
pipeline). Le registre des règles affiche la provenance ; une règle `héritée` ne
peut entrer dans la ville native SANS re-dérivation justifiée dans la nouvelle
architecture — sinon elle est supprimée. Déjà tranché : le p50 par parcelle
indépendante est MORT (remplacé par solveur+communautés+fidélité) ; le « plafond
de mur 12 m » ne survit que comme seuil de DÉTECTION, plus comme règle de
construction.

## Arbitrages utilisateur en attente (tous en 3D, sur place, sur la map native)
① mur de quai (résout 11/12 arbitrages — pièce à valider visuellement) ·
② aplanir-vs-épouser les grandes communautés en relief (le solveur + la borne de
fidélité devraient en résoudre l'essentiel — le reste en A/B) · ③ verdict berge /
escaliers Saint-Pierre (assiettes) · ④ statuts de la table de recensement.

## Ordre des lots (chacun avec critère d'arrêt, jugés un par un)
- **L0 — RECENSEMENT** (compilateur, zéro Unreal) : la table nature→mécanique→
  statut sur l'agglo + grille des milieux → TA revue en une passe.
- **L1 — SOLVEUR + ASSIETTES + TERRASSEMENTS** (compilateur) : nivellement global
  + cartes de pré-validation → TA revue navigateur.
- **L2 — CONTRAT v3** : export + lecteur mis à jour (garde).
- **L3+ — CONSTRUCTION** sur `L_PlanVille_A` par lots (terrassements/sol →
  gabarits → objets → pièces), chaque lot A/B vs Témoin, TES yeux.
- **L final — arbitrages 3D** sur place.
