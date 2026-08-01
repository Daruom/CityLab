# Le graphe routier side-car — format, conventions, consommation

> Lot **D1 + D2** du chantier SOL, 2026-07-31. Voie **sans éditeur** : aucun process UE,
> aucun asset, aucune ligne de C++. Tout est produit par des scripts Python stdlib.
> Analyse d'origine : `C:\LidarPoC\work\SOLROUTES\analyse_sol_routes.md` (sections A et C).

## Pourquoi ce lot passe en premier

La géométrie générée est **fusionnée par cellule** : un `SM_Slab_<x>_<y>` et un
`SM_Ground_<x>_<y>` pour *toutes* les routes d'une cellule de 500 m
(`CityImportTools.cpp` l. 6475-6534). Il n'existe **aucun acteur, aucun composant,
aucune section de mesh par route** — donc aucun endroit où accrocher un tag, et en
créer un coûterait des milliers d'acteurs et détruirait le batching.

Conséquence : une fois le mesh cuit, plus rien ne dit qu'un triangle est une chaussée
à double sens de 7 m. **Si la sémantique n'est pas écrite à côté de la géométrie, au
moment où on la connaît encore, elle ne se rattrape qu'en régénérant les 461 km².**
C'est le seul lot du chantier qui a cette propriété.

---

## 1. Ce qui est produit

| Chemin | Rôle |
|---|---|
| `Tools/reseau_classes.json` | **LE seul fichier à éditer** : grille, emprises, table de classification, replis de largeur, normalisation des énumérations, pièges |
| `Tools/geo_local.py` | Lambert 93 ↔ WGS84 ↔ repère local, décodage GPKG, grille, clé de nœud. Stdlib pure. `--selftest` intégré |
| `Tools/build_reseau.py` | D1 : le générateur du side-car. GPKG → cellules |
| `Tools/fetch_routes_gpkg.py` | D2 : le fetch enrichi. GPKG → 2 formats (compatible + riche) |
| `SourceData/Reseau/reseau_<cx>_<cy>.json` | **un fichier par cellule** — le side-car lui-même |
| `SourceData/Reseau/index_<emprise>.json` | inventaire + **tous les contrôles** de l'emprise |
| `SourceData/Reseau/routes_bdtopo_gpkg_<nom>.json` | fetch (a), format compatible pipeline actuel |
| `SourceData/Reseau/routes_riche_<nom>.json` | fetch (b), 76 colonnes BD TOPO, `cleabs` compris |
| `SourceData/Reseau/controle_non_regression_<nom>.txt` | comparaison chiffrée GPKG vs WFS |
| `SourceData/Reseau/*.progress.log` | journaux des deux scripts |

Commandes :

```
python Tools/geo_local.py                              # selftest projection : 9 verrous
python Tools/build_reseau.py --selftest                # 27 verrous
python Tools/fetch_routes_gpkg.py --selftest           # 12 verrous
python Tools/build_reseau.py --emprise proto           # 4 cellules
python Tools/build_reseau.py --emprise carre10         # 400 cellules, ~10 s
python Tools/build_reseau.py --cells 3,-4 3,-3         # cellules explicites
python Tools/build_reseau.py --emprise carre10 --valider-seulement
python Tools/fetch_routes_gpkg.py --half 5000 --nom carre10 --controle
```

---

## 2. Format d'un fichier de cellule

```jsonc
{
  "cell": [0, 0],
  "cellSizeM": 500.0,
  "origin": { "lat": 43.6045, "lon": 1.4442 },
  "crs": "local_citylab_equirect_capitole",
  "axes": "x = est (m), y = sud (m) ; NORD = -Y",
  "bbox": [0.0, 0.0, 500.0, 500.0],
  "source": "BD TOPO 3.5 GPKG LAMB93 D031 ED2026-06-15 (IGN, Licence Ouverte 2.0)",
  "classes": "reseau_classes.json v1 sha1=…",   // trace la table QUI A SERVI
  "ecrit_par": "carre10",                        // informatif, cf. piège n° 1
  "genere": "2026-07-31T13:41:29",
  "nodeKey": "decimetre : k = [round(x*10), round(y*10)] — meme cle que FJunctionMap::Key (C++)",
  "stats": { "edges": 152, "nodes": 126, "longueur_m": 7069.3, "km_par_classe": {…} },

  "nodes": [
    { "id": 0, "k": [2260, 4392], "p": [226.01, 439.19], "z": 144.6,
      "deg": 4, "jonction": true, "bord": true }
  ],

  "edges": [
    { "id": 0, "cleabs": "TRONROUT0000000073501137",
      "a": 0, "b": 1,
      "pts": [[226.01, 439.19], [227.88, 457.76], [237.39, 500.0]],
      "z":   [144.6, 144.6, 144.6],
      "len": 62.07, "abs0": 0.0, "part": 0, "nparts": 2, "cut1": true,
      "classe_rendu": "ruelle",
      "nature": "Route à 1 chaussée", "importance": 5, "lanes": 2, "w": 4.0,
      "sens": "direct", "acces_vl": "libre", "vit_moy": 5,
      "urbain": true, "niveau": 0,
      "nom": "Rue des Arts", "ban": "3cf58fb6-0e28-4745-9e39-40820e3a2b44" }
  ]
}
```

### 2.1 Nœuds

| Champ | Sens |
|---|---|
| `id` | index **local à la cellule** — c'est lui que `a`/`b` référencent |
| `k` | **clé décimétrique** `[round(x*10), round(y*10)]` : l'identité globale du nœud |
| `p` | position locale en mètres, arrondie au cm |
| `z` | altitude BD TOPO (m NGF) si renseignée. La sentinelle `-1000` de l'IGN est convertie en `null` |
| `deg` | degré **dans la cellule** (le degré global se recalcule en recollant les `k`) |
| `jonction` | présent = au moins une vraie extrémité de tronçon BD TOPO tombe ici |
| `bord` | présent = le nœud est sur la frontière de la cellule (il a un jumeau chez le voisin) |

### 2.2 Arêtes

| Champ | Sens | Absent signifie |
|---|---|---|
| `cleabs` | **identifiant IGN pérenne** du tronçon — la seule clé qui survit à un changement de millésime | — |
| `a`, `b` | `id` des nœuds d'extrémité, dans cette cellule | — |
| `pts` | polyligne locale (m, arrondie au cm) | — |
| `z` | altitudes des sommets (`null` par sommet non renseigné) | aucune altitude sur ce tronçon |
| `len` | longueur du **morceau** (m) | — |
| `abs0` | abscisse curviligne du début du morceau dans le tronçon **complet** | `0` |
| `part`/`nparts` | i-ème morceau sur n pour ce `cleabs` dans l'emprise | tronçon non découpé |
| `cut0`/`cut1` | l'extrémité vient d'une **coupe de grille**, pas d'une extrémité de tronçon | vraie extrémité |
| `classe_rendu` | classe issue de `reseau_classes.json` | — |
| `nature`, `importance` | BD TOPO bruts (`importance` converti en entier) | — |
| `lanes` | `nombre_de_voies` | non renseigné (18,6 % à l'agglo) |
| `w` / `w_src` | largeur de chaussée (m) / `"repli"` | `w_src` absent = **largeur MESURÉE par l'IGN** |
| `sens` | `double` / `direct` / `inverse` / `sans_objet` | — |
| `vit_moy` | ⚠ **vitesse de PARCOURS**, pas la limitation — cf. pièges | — |
| `acces_vl` | `libre` / `impossible` / `ayants_droit` / `peage` | — |
| `acces_pieton`, `bus`, `cyclable_g`, `cyclable_d` | idem, normalisés | non renseigné |
| `urbain`, `prive`, `fictif` | booléens | `prive`/`fictif` absent = faux **ou** non renseigné |
| `etat` | `"En projet"`, `"En construction"` | **« En service »** |
| `niveau`, `pont`, `tunnel`, `niveau_txt` | `position_par_rapport_au_sol` en entier + drapeaux | niveau 0 |
| `nom`, `ban`, `num`, `classement` | `nom_voie_ban_gauche`, `id_ban_odonyme_gauche`, `cpx_numero`, `cpx_classement_administratif` | non renseigné |

**Règle générale : un champ nul n'est pas écrit.** C'est ce qui tient la volumétrie
(496 octets par arête, mesuré) ; le tableau ci-dessus donne la valeur implicite de
chaque absence, et il n'y en a que trois qui ne soient pas « non renseigné ».

---

## 3. Conventions

### 3.1 Repère et grille

- Repère **local CityLab** : équirectangulaire autour du Capitole
  (`lat 43,6045 / lon 1,4442`), `x = est`, **`y = sud` (NORD = -Y)**, mètres.
  Convention **gelée** : identique à `GF-Common.ps1` l. 21-28.
- Cellule `(cx, cy)` = `[cx·500, (cx+1)·500] × [cy·500, (cy+1)·500]`.
  Identique à `CELL_M` de `j3c_sols_masks.py` et à `CellSizeM` du générateur C++.
- Emprises livrées : **proto** (4 cellules, `-1..0 × -1..0`) et
  **carre10** (400 cellules, `-10..9 × -10..9`). Toutes deux dans `reseau_classes.json`.

### 3.2 Du Lambert 93 au repère local

Le GPKG BD TOPO est en **EPSG:2154**. `geo_local.py` déprojette en RGF93
géographique (conique conforme sécante, formules IGN NT/G 71) puis applique la même
équirectangulaire que l'ancien chemin WFS. **Vérifié par la mesure**, pas affirmé :

- constantes de la projection recalculées vs les valeurs officielles IGN :
  `n = 0,7256077650` (écart 5·10⁻¹¹), `C = 11 754 255,426` (9,6·10⁻⁵ m),
  `Ys = 12 655 612,050` (1,2·10⁻⁴ m) ;
- aller-retour Lambert 93 sur un semis couvrant le département : **< 4 nm** ;
- **71 722 extrémités** de polylignes comparées à celles produites par l'ancien chemin
  WFS : écart **médian 4,0 mm**, p99 6,7 mm, **max 7,5 mm** — c'est exactement
  l'arrondi centimétrique du WFS, donc zéro écart réel.

### 3.3 Découpe par cellule et recollage

Une polyligne est coupée **exactement** sur les lignes de grille : les points de coupe
sont calés sur le multiple de 500 (pas d'interpolation résiduelle). Les deux cellules
voisines voient donc **rigoureusement le même point**, et le recollage est une
**égalité de clés décimétriques**, jamais une recherche par rayon.

La clé décimétrique est la **même que celle du C++** (`FJunctionMap::Key`,
`CityImportTools.cpp` l. 1230-1233) : le graphe et le générateur parlent des mêmes nœuds.

Un tronçon peut repartir en **diagonale** après une coupe de coin : le contrôle de
recollement ne teste donc pas « le voisin nommé » mais **l'existence de la continuité**
(même clé + même `cleabs` ailleurs dans l'emprise).

---

## 4. La table de classification

Tout est dans `Tools/reseau_classes.json`. `build_reseau.py` **ne contient aucune règle
en dur** : il applique le fichier. Les règles sont évaluées **dans l'ordre, la première
qui matche gagne** — changer l'ordre change la priorité, ajouter une règle est une ligne.
N'importe laquelle des 88 colonnes de `troncon_de_route` peut servir de critère sans
toucher au code. La comparaison est faite en minuscules sans accents
(`Route a 1 chaussee` == `Route à 1 chaussée`).

Table **provisoire** livrée (à valider / ajuster par l'utilisateur) :

| # | classe | critère |
|---|---|---|
| 1 | `autoroute` | `nature ∈ {Type autoroutier, Bretelle}` |
| 2 | `pietonne_pavee` | `acces_vehicule_leger = Physiquement impossible`, **sauf** natures piétonnes/chemin |
| 3 | `bus` | `reserve_aux_bus` renseigné |
| 4 | `pieton` | `nature ∈ {Sentier, Escalier}` |
| 5 | `chemin` | `nature ∈ {Chemin, Route empierrée}` |
| 6 | `artere` | `importance ∈ {1, 2}` |
| 7 | `rue` | `importance ∈ {3, 4}` |
| 8 | `ruelle` | `importance ∈ {5, 6}` — **aussi la classe par défaut** |

`pieton` est un **ajout** aux 7 classes du brief : sans elle, sentiers et escaliers
tombent dans les règles d'importance et faussent tous les kilométrages. Elle ne peint
rien (doctrine v4 : le piéton, c'est la dalle). Une ligne à retirer si l'utilisateur
n'en veut pas.

**Ronds-points et « Route à 2 chaussées » n'ont pas de règle propre** : ils tombent dans
les règles d'importance (un rond-point de boulevard devient `artere`, un rond-point de
lotissement `ruelle`). Ajouter une règle avant la règle `artere` pour les distinguer.

Le **repli de largeur** (quand `largeur_de_chaussee` est absente) reprend **à l'identique**
la table de `j3c_sols_corridor.py` (`FALLBACK_LARGEUR`) : le graphe et la peinture du sol
ne peuvent pas diverger.

---

## 5. Le fetch enrichi (D2) et sa migration

### 5.1 Ce qui est remplacé, et ce qui ne l'est pas

| Ancien chemin | Statut |
|---|---|
| `SourceData/GrandFetch/Fetch-GF-RoutesBDTopo.ps1` | **conservé**, non modifié. 18 attributs sur 88, **pas de `cleabs`** (liste noire `$GFPropBlacklist`, `GF-Common.ps1` l. 235), pas d'aménagement cyclable, pas d'`id_ban_odonyme`. Dépend du WFS (1 000 objets/requête à 1,2 s) |
| `C:\LidarPoC\work\E2SOL1\fetch_routes_bdtopo.py` | **conservé**, non modifié. Même WFS, puis `map_type()` (l. 54-69) **écrase 12 attributs sur 14** pour ne garder qu'un type OSM-like |
| **`Tools/fetch_routes_gpkg.py`** | **le chemin privilégié** : même donnée IGN, même repère, deux sorties, 1,6 s pour le carré 10 km |

Rien n'est supprimé : les deux anciens chemins ne demandent pas les 2,8 Gio du GPKG et
restent utilisables sur une machine qui ne l'a pas.

### 5.2 Les deux sorties

- **(a) compatible** `routes_bdtopo_gpkg_<nom>.json` — les **18 clés de `$Keys`, dans
  leur ordre**, même amincissement (seuil de Manhattan à 1 m, dernier point toujours
  conservé), même arrondi centimétrique, mêmes omissions de valeurs nulles.
  `j3c_sols_corridor.py` le lit **sans une ligne de changement** : soit on le copie
  par-dessus `SourceData/GrandFetch/routes_bdtopo.json`, soit on pointe `ROUTES_PATH`
  (l. 45) dessus. **Ce lot n'a écrasé aucun des deux fichiers existants** — une campagne
  tournait sur la ferme, les sources restent en lecture seule.
- **(b) riche** `routes_riche_<nom>.json` — **76 colonnes sur 88**, `cleabs` en tête.
  Les 12 écartées sont les métadonnées d'acquisition (dates, sources, méthodes,
  précisions). C'est aussi le **repli de `build_reseau.py`** sur une machine sans GPKG
  (`--source-json`).

### 5.3 Non-régression mesurée (fenêtre 10 km)

| | GPKG (2026-06) | WFS (réf. 2026-07-27) | écart |
|---|---:|---:|---:|
| tronçons | **35 851** | 35 861 | **-10 (-0,03 %)** |
| longueur | **2 316,0 km** | 2 316,8 km | -0,8 km (-0,03 %) |
| `largeur_de_chaussee` remplie | 30 282 (moy. 4,46 m) | 30 288 (moy. 4,46 m) | -6 |

Les taux de remplissage des 18 clés sont **identiques au dixième de point**.

**L'écart est entièrement expliqué, et par la mesure** : les 10 tronçons manquants sont
**exactement** les 10 tronçons WFS qui ne mordent pas le carré local exact (leurs bbox
sont toutes collées à `x = ±5000` ou `y = ±5000`). Le WFS filtre sur une bbox
**lon/lat géodésique**, nous sur le **carré métrique local** : au bord, les deux fenêtres
diffèrent de quelques mètres. Leur ventilation par nature (6 `Route à 1 chaussée`,
2 `Sentier`, 1 `Chemin`, 1 `Route à 2 chaussées`) correspond au delta du tableau des natures.

**Aucun effet de millésime sur cette fenêtre** : 0 tronçon franchement à l'intérieur
manque, et 0 tronçon GPKG est absent du WFS. Le millésime 2026-06 du GPKG et le fetch
WFS de juillet décrivent la même voirie ici.

---

## 6. Comment ça se consommera

### 6.1 Le cuiseur de sol (Python, aujourd'hui)

`j3c_sols_corridor.py` / `j3c_sols_masks.py` peuvent lire `classe_rendu` **par cellule**
au lieu de refaire un tri par nature : le rendu et le futur trafic partagent alors
**la même décision de classification**. Le jour où « qu'est-ce qu'une zone piétonne »
change, le pavé au sol et l'interdiction de circuler bougent ensemble, par construction.

Verrous à ajouter à leur `--selftest` (couche C.4 de l'analyse, **pas encore faite**) :
« le fichier réseau de la cellule existe, ses arêtes sont ≥ 1, ses nœuds de bord se
recollent avec ceux du voisin ». Une cellule sans réseau = échec de cuisson, pas un
avertissement.

### 6.2 Le générateur et le trafic (UE — LOT ÉDITEUR ULTÉRIEUR, non fait ici)

> ⚠ **Rien de ce qui suit n'a été implémenté : ce lot est en voie sans-éditeur.**

1. **`URoadGraphCell : UPrimaryDataAsset`** dans `/Game/City/Reseau/DA_Reseau_<x>_<y>`,
   mêmes champs en `TArray<FRoadEdge>` / `TArray<FRoadNode>`.
2. **Il doit être référencé par un acteur de la map pour voyager** : un JSON de
   `SourceData/` ne part pas dans le pak et n'est pas copié vers Survol
   (`Tools/copy_to_survol.py` ne copie que les packages `/Game/` listés dans
   `Saved/desktop_manifest.txt`, lui-même construit depuis les **références de la map**).
   Le plus simple : un `ARoadGraphAnchor` vide par cellule, ou un pointeur sur
   l'acteur `SM_Slab` de la cellule. C'est la seule ligne de C++ vraiment nouvelle.
3. **Compteurs de refus** : ajouter `RoadEdges` / `RoadNodes` à `FCityStreamedSummary`
   (`CityImportTools.h` l. 187-243, qui compte déjà `CurbQuads`, `AxialDashes`,
   `MaskedCells`) et **refuser de générer** sans réseau quand le profil le demande —
   sur le modèle du refus existant quand `cellSizeM` ne correspond pas (l. 744-752).
4. **La question « sur quelle route suis-je ? » se résout par le graphe** (recherche
   spatiale sur les arêtes de la cellule), jamais par le mesh. Pas de NavMesh, pas de
   spline par route, pas de tag d'acteur : tout ça se régénère depuis le graphe.

---

## 7. Pièges

1. **`ecrit_par` n'est pas un filtre.** Les emprises se **chevauchent** (proto ⊂ carre10)
   et un fichier de cellule est indexé par `(cx, cy)` **seul** : le contenu est identique
   quelle que soit l'emprise (même source, même grille, même table), seul ce libellé
   change au dernier build. **Pour itérer sur un lot, utiliser la liste `cellules` de
   `index_<emprise>.json`**, jamais ce champ.
2. **`vit_moy` est une vitesse de PARCOURS moyenne constatée, pas une limitation
   réglementaire.** 14 % des tronçons sont à 0 km/h, 3,5 % à 1 km/h, les pics sont à
   20/25/30. Utilisable pour un temps de trajet ou une densité de trafic ; **jamais**
   pour afficher un panneau ni pour brider un véhicule. La limitation réglementaire
   n'est pas dans BD TOPO.
3. **`importance` et `position_par_rapport_au_sol` sont des colonnes TEXTE**, pas des
   entiers — et `position_par_rapport_au_sol` vaut aussi `"Gué ou radier"` (133 objets
   sur le D031), d'où le champ `niveau_txt`.
4. **L'altitude BD TOPO absente vaut `-1000,00`** : c'est une sentinelle, pas une
   altitude. `geo_local.py` la convertit en `null`.
5. **`nombre_de_voies` code « 2 voies » dès qu'une rue est à double sens** : dans un
   centre ancien, la moitié des ruelles de 4 m sont à « 2 voies ». Croiser avec la
   largeur avant d'en tirer un marquage (le pipeline sol utilise déjà 5,50 m).
6. **`prive` a 1 474 valeurs NULL** sur le D031 (ni vrai ni faux) : tester `is True`,
   jamais `is not False`.
7. **Les tronçons `fictif` portent la topologie** (traversée de place) sans porter de
   revêtement. Le side-car les **conserve** avec leur drapeau : c'est au consommateur
   de décider, pas au producteur de jeter.
8. **Le repli `--source-json` perd deux choses** : la géométrie y est déjà amincie à 1 m
   (sommets intermédiaires fins perdus, connectivité intacte) et il n'y a **pas de Z**.
   Mesuré sur le proto : 603 arêtes identiques, **468 nœuds au lieu de 467**, mêmes km,
   même topologie, 0,25 Mo au lieu de 0,27.

---

## 8. Chiffres mesurés (2026-07-31)

### Volumétrie et contrôles

| | proto | carré 10 km |
|---|---:|---:|
| cellules | 4 | 400 (dont **2 vides**) |
| arêtes | 603 | **41 378** |
| `cleabs` distincts | 571 | 35 851 |
| nœuds recollés | 467 | **32 106** |
| longueur | 26,54 km | **2 274,71 km** |
| volume total | **0,27 Mo** | **19,72 Mo** |
| par cellule | 53 – 71 – 84 Ko | 0,5 – **47,5** – 111,4 Ko (min/médiane/max) |
| arêtes par cellule | — | 0 – **99** – 243 |
| arêtes dégénérées | **0** | **0** |
| coupes testées / orphelines | 64 / **0** | 11 036 / **0** |
| composantes connexes | 4 (la plus grande **98,29 %**) | 65 (la plus grande **99,30 %**) |
| ponts / tunnels / fictifs / hors service | 0 / 0 / 0 / 0 | 525 / 22 / 30 / 24 |

Degrés (carré 10 km) : `1 → 3 426 · 2 → 9 785 · 3 → 15 979 · 4 → 2 773 · 5 → 128 · 6 → 14 · 7 → 1`.
Aucun degré aberrant, aucune arête dégénérée : **BD TOPO est déjà un graphe propre**,
il n'y a pas de nettoyage topologique à faire. Les composantes résiduelles sont du bruit
de bord d'emprise (fragments coupés par la fenêtre).

### Distribution des classes — carré 10 km (400 cellules écrites)

| classe | km | arêtes | ha de chaussée | largeur moy. | largeur mesurée | nommée |
|---|---:|---:|---:|---:|---:|---:|
| `ruelle` | **1 348,5** | 25 802 | 541,0 | 4,01 m | 100 % | 65,1 % |
| `rue` | 294,1 | 6 930 | 161,1 | 5,52 m | 100 % | 96,2 % |
| `pieton` | 248,9 | 4 346 | 87,1 | 3,50 m | **0 %** | 15,6 % |
| `autoroute` | 136,6 | 1 052 | 119,8 | 8,10 m | 97,6 % | 19,2 % |
| `pietonne_pavee` | 132,5 | 1 631 | 46,5 | 3,51 m | **6,0 %** | 9,9 % |
| `chemin` | 65,1 | 763 | 19,5 | 3,00 m | **0 %** | 13,8 % |
| `bus` | 34,2 | 520 | 19,2 | 5,86 m | 100 % | 68,3 % |
| `artere` | 14,7 | 334 | 9,9 | 7,11 m | 100 % | 86,5 % |
| **TOTAL** | **2 274,7** | **41 378** | **1 004,1** | | | |

### Projection métropole (bbox 461 km², mesurée sur les 136 987 tronçons)

| classe | tronçons | km | ha de chaussée | % surface |
|---|---:|---:|---:|---:|
| `ruelle` | 83 016 | 5 358,2 | 2 100,2 | 49,9 % |
| `chemin` | 10 638 | 1 653,6 | 496,1 | 11,8 % |
| `rue` | 24 194 | 1 490,4 | 718,1 | 17,1 % |
| `pieton` | 11 046 | 994,1 | 347,9 | 8,3 % |
| `pietonne_pavee` | 4 059 | 418,7 | 146,9 | 3,5 % |
| `autoroute` | 1 477 | 338,4 | 281,4 | 6,7 % |
| `artere` | 1 750 | 124,9 | 79,3 | 1,9 % |
| `bus` | 807 | 67,0 | 35,7 | 0,8 % |
| **TOTAL** | **136 987** | **10 445,4** | **4 205,5** | |

Coût **mesuré** : **496 octets par arête**. À l'agglo : ~158 000 arêtes après découpe
→ **~75 Mo** de side-car. À comparer aux **9,8 Go de masques** de la même régé (B.6 de
l'analyse) : le graphe pèse **0,8 %** du poste masques. La volumétrie n'est pas un sujet.

---

## 9. Ce qui reste

**Pour l'utilisateur (arbitrage) :**

- **Valider ou corriger la table `reseau_classes.json`.** Deux mesures qui appellent
  une décision :
  - `pietonne_pavee` n'a **que 6,0 % de largeurs mesurées** (98 sur 1 631) : la classe
    tourne presque entièrement sur le repli d'importance (3,50 m). Il lui faut une
    politique de largeur propre si elle doit être peinte.
  - Le millésime **2026-06 code `rue Saint-Rome` et `rue du Taur` en
    `acces_vehicule_leger = 'Libre'`** — l'analyse annonçait que ce critère dessinait
    « exactement » le cœur piétonnisé, la donnée ne le confirme pas sur ces rues-là.
    Sur la fenêtre 10 km il attrape **1 493 tronçons entiers, 153,4 km bruts**
    (132,5 km une fois découpés et clippés au carré), autour de Saint-Georges, Tripière,
    Bédelières, Jules-Chalande. La variante « ajouter `Restreint aux ayants droit` »
    ajouterait 1 140 tronçons / 88,1 km — mais **66 % de ces kilomètres sont sans nom**
    et les plus longs nommés sont des voies de zone aéroportuaire et industrielle
    (Voie des Carènes, Aéropostale, Hubert-Curien) : **ce n'est pas un critère piéton**,
    c'est un critère d'accès restreint. Recommandation : garder le critère strict.

**Pour un lot éditeur ultérieur (UE) :** couche 2 complète — `URoadGraphCell`,
l'acteur d'ancrage qui la fait voyager dans le pak, les compteurs de résumé et le
refus de générer sans réseau (§ 6.2).

**Pour un lot Python ultérieur :** brancher `classe_rendu` dans
`j3c_sols_corridor.py` / `j3c_sols_masks.py` et ajouter les verrous de cuisson (§ 6.1).
