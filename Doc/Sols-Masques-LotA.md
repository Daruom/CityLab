# LOT A — architecture des masques de sol & peinture par classe de voie

> 2026-07-31. Décisions D3/D4 du chantier SOL (cf. `C:\LidarPoC\work\SOLROUTES\analyse_sol_routes.md`),
> tranchées PAR MESURE sur le proto (`L_ProtoSols_E2_Sol2`). Captures et chiffres :
> `C:\LidarPoC\work\SOLROUTES\captures\` + `lotA_progress.log`.

## 1. DÉCISION A1a — architecture : un 2ᵉ masque SDF (candidate a), PAS d'ID+LUT

Les deux candidates ont été prototypées en éditeur sur les 4 cellules réelles et
capturées AUX MÊMES poses (`P8_seam_embouchure_A.png` vs `_B3.png`) :

| | (a) mask2 SDF | (b) ID entier + LUT |
|---|---|---|
| Bord pavé/dalle/asphalte | **lisse, continu** (l'AA par footprint écran s'applique au SDF) | **escalier de 48,8 cm**, net à 6 m de distance — rédhibitoire |
| Coût shader mesuré | master : 552 instr. PS, 32 samples, 6 samplers | identique à ~5 instr. près (mêmes 9 samples de packs ; 1 sample RGBA vs 1 sample R8) |
| Volume (BC7, mips incl.) | 2,67 Mo/cellule | 2,67 Mo/cellule (l'ID 8 bits n'est PAS compressible en bloc et exige en plus un sampler Grayscale dédié — piège payé : mismatch sampler → MI qui ne compilent plus) |
| Marge d'avenir | canal A du mask2 en réserve (plancher 1) | 252 valeurs d'ID libres mais bords toujours en escalier |

→ **T_GroundMask2_<cx>_<cy>**, mêmes conventions que le masque 1 (SDF ±2 m,
1,57 cm/niveau, 128 = frontière) : `R = pavé piéton (façade à façade)`,
`G = fine1 (rue)`, `B = fine2 (ruelle)`, `A = RÉSERVE` (plancher 1).
Le masque 1 est INCHANGÉ (R herbe, G chaussée circulée, B privée, A gravier) →
**zéro modification du C++ `FGrassMaskSampler`** (fosses/rétraction identiques).

## 2. DÉCISION A1b — compression : BC7 sur les DEUX masques (débloque l'agglo)

Mesuré par relecture GPU exacte (draw → RT → export, pipeline validé bit-à-bit sur
le non-compressé) sur la cellule 0,0, erreur DANS la bande de frontière (±50 cm) :

| | moyenne | p99 | max | volume/cellule (mips) | agglo 1845 c. |
|---|---|---|---|---|---|
| non compressé (statu quo ×2 masques) | 0 | 0 | 0 | 10,67 Mo | **19,2 Go** ❌ |
| **BC7 (retenu)** | **1,8–3,5 cm** | 6–16 cm | 28–31 cm | **2,67 Mo** | **4,8 Go** |
| BC4 par canal (8 textures) | 4,0–4,4 cm | 12,5 cm | 17–19 cm | 5,3 Mo + 8 samplers | 9,8 Go ❌ |
| variante mask2 à 512² BC7 | 2,7–4,3 cm | 22–33 cm | 40–55 cm | 1,67 Mo | 3,0 Go (repli si besoin) |

Contrôle visuel `P7_bc7_frontiere_A.png` vs `_BC7.png` : **71 pixels sur 664 k
diffèrent de > 8 niveaux (0,01 %)** — frontière indiscernable. Le commentaire
historique « pas de DXT » visait BC1/BC3 ; BC7 est validé par la mesure.
⚠ Non encore appliqué en production (import_ground_masks reste en
TC_VECTOR_DISPLACEMENTMAP) : à basculer AVANT la régé T10 v2 — une ligne
(`TC_BC7`) dans `import_mask_texture()`.

## 3. Mapping classe → revêtement (décisions utilisateur, verrouillées)

`classe_rendu` vient du **side-car réseau** (`SourceData/Reseau/`, jointure par
`cleabs` avec la géométrie du fetch riche GPKG) — plus AUCUN tri local dans le
cuiseur (`CLASSE_PAINT` de `Tools/j3c_sols_masks.py`) :

| classe | peinture | canal | bordures | tirets |
|---|---|---|---|---|
| autoroute | asphalte | mask1.G | non (accotement) | oui |
| artère / bus | asphalte | mask1.G | oui | oui |
| rue | fine_road_vgdlejpew 4K | mask1.G + mask2.G | oui | **non** (ligne cuite dans le scan) |
| ruelle | fine_road_viciaalew 4K | mask1.G + mask2.B | oui | non |
| pietonne_pavee | **cobblestone 4K, FAÇADE À FAÇADE** | mask2.R | **non** + disque de silence à chaque embouchure | non |
| chemin | gravier | mask1.A | non | non |
| pieton (sentier/escalier) | la dalle | — | — | — |

- Zone pavée = compétition de Voronoï pondérée par la largeur, au raster
  (24,4 cm/px, rayon 15,6 m) : corridor attribué à la rue piétonne la plus proche ;
  l'herbe gagne sur le pavé, le circulé gagne sur tout. Ordre shader :
  dalle → pavé → herbe → gravier → privée → asphalte → fine1 → fine2.
- Critère piéton HYBRIDE : strict BD TOPO (`acces_vl = Physiquement impossible`)
  ∪ OSM `highway=pedestrian` (cache disque `Tools/fetch_osm_pieton.py`, matching
  spatial 6 m / 60 % dans `build_reseau.py`, ways linéaires seuls — les anneaux
  de places ne votent pas). Table `Tools/reseau_classes.json` v2.
- TUNNELS (`niveau < 0`) exclus de la peinture, bordures et tirets (comptés).

## 4. Pièges payés ce lot

- **Nanite ignore l'override de matériau posé sur le composant** : pour un A/B
  visuel, changer les PARAMS de la MI de production (et les restaurer), jamais
  `set_material`.
- **Ne jamais écrire un .py directement dans la queue du guetteur** (lecture
  partielle) : écrire ailleurs puis `Move-Item`.
- **PIL prémultiplie l'alpha au resize RGBA** : mesurer les SDF canal par canal.
- Un masque d'ID sous le sampler LinearColor du master ne compile pas
  (TC_Grayscale ≠ sampler) — l'« économie » du candidat (b) coûte un sampler dédié.
- `toggle de visibilité de niveau` réinitialise les overrides non transactés.

## 5. LOT A-bis — COHÉRENCE VISUELLE DU SOL (2026-07-31)

> Verdict utilisateur sur le lot A : « des bouts de sols collés à d'autres,
> séparations brutales, aucune cohérence » (la texture pavé elle-même validée).
> Cinq correctifs, tranchés par la mesure. Captures : séries `AB1`/`AB2`/`FIN`
> + vues-griefs `G1..G4` (`abis_vues_griefs.json`), chiffres :
> `work/SOLROUTES/abis_mesure_*.txt` + `lotAbis_progress.log`.

### 5.1 Continuité de classe le long des rues (correctif 1 — `build_reseau.py`)

La classe était décidée TRONÇON par tronçon : une même rue changeait de
revêtement en plein bloc. Le LISSAGE à 3 niveaux (dans la CLASSIFICATION, pas
la cuisson) impose la règle de ville « une rue garde UNE classe entre deux
carrefours » :
1. **RUN** (chaîne d'arêtes entre carrefours, continuité deg 2) : vote
   majoritaire pondéré par la longueur — c'est aussi l'hystérésis (un
   micro-tronçon ne renverse jamais un bloc) ;
2. **RUE NOMMÉE** (sandwich) : un run court encadré par deux runs de la même
   voie nommée et d'une même autre classe l'adopte ;
3. **ABSORPTION DES POCHES** (correctif 3) : groupe CONNEXE de runs d'une même
   classe, cerné d'une seule autre classe D avec ≥ 2 contacts → absorbé si
   court (≤ 55 m), si ses contacts débouchent dans la même zone D (BFS ≤ 250 m),
   ou — **îlot injoignable** — si D est le piéton pavé (une voirie circulée
   entièrement cernée de piéton est injoignable en voiture, la donnée se
   contredit ; plafond 400 m). Une `pietonne_pavee` (donnée réelle) ne
   s'absorbe que ≤ 20 m ; les classes de NATURE (autoroute, chemin, pieton)
   sont verrouillées ; une impasse (1 seul contact) n'est jamais une enclave.

Mesuré sur le proto (4 cellules) : **bascules en pleine rue votables 4 → 0**
(reste 1 transition de nature pavée↔escalier, légitime), carrefours
multi-classes 143 → 136, **10 fragments absorbés** (17 arêtes) — dont Place de
la Bourse (16 m pavée→ruelle), Place de la Trinité (47 m→pavée), Place de
Bologne (81 m→pavée) — et la Place Saint Georges unifiée par rue nommée (le
« bout courbe avec tirets au milieu de la place » du grief). Rue de la Pomme /
St Pantaléon : PAS une enclave (pointe du réseau circulable du quartier SE,
11,5 km connexes) → rue continue qui s'arrête où le plateau commence, embouchure
cousue. Traces persistées dans le side-car : `classe_brute` + `lissage`
(`run_vote`/`rue_nommee`/`absorption`) ; métrique `bascules_pleine_rue_*` dans
l'index. Selftest : 9 verrous lissage ajoutés, PASS.

### 5.2 Bande de couture aux frontières (correctif 2 — `j3c_sols_masks.py` + master)

Le canal **A du masque 2** (la réserve) porte désormais le SDF d'une **bande de
50 cm** cuite là où deux champs de distance minéraux se croisent
(`|d1−d2| < 0,5 ∧ max(d1,d2) > −0,5`), pour les 7 paires {pavé, fine1, fine2,
asphalte-nu, dalle} de chaussée/piéton — jamais sur une frontière déjà porteuse
d'une bordure 3D (chaussée↔trottoir). À l'embouchure, la compétition laisse un
coin de dalle : la couture y trace le **seuil d'entrée** du plateau (verrou 14).
Shader : la couche couture réutilise les échantillons du pavé (0 sampler en
plus) × `Tint_seam` (pavé × `SEAM_TWEAK` 0,78/0,74/0,72) — le rang de pierre
sombre du fil d'eau. 1 527 m² sur le proto. Rétro-compatibilité structurelle :
cellule sans couture = canal plancher, exactement l'ancien contenu.

### 5.3 Marquages en zone piétonne (correctif 3 bis — cuisson)

`axial_dashes` voit la zone pavée par une distance NON signée à grand rayon
(15,6 m — le SDF du masque sature à 2,44 m, piège évité) : un tiret dont un
point voit le pavé à moins de `largeur/2 + 1 m` est supprimé, quelle que soit
la classe du tronçon. 0 supprimé sur le proto (l'absorption a déjà nettoyé les
porteurs) — le garde-fou vaut pour le carré 10 km.

### 5.4 Harmonisation de la palette (correctif 4 — master paramétré)

Les teintes des couches sont des **paramètres vectoriels** du master
(`Tint_<couche>`, défaut = harmonisation v4b × `TINT_TWEAK`) — calibrables en
LIVE sur les MI, valeurs consignées dans `import_ground_masks.py`. Pavé
calibré **gris-rose chaud** sur captures soleil+ombre (AB1 1,18/1,06/0,96
mesuré trop clair — lum 184 vs dalle 173 ; **AB2 retenu 1,10/0,99/0,89** :
lum 181, R/B 1,27 vs dalle 1,12 ; à l'ombre pavé 55/R-B 0,63 vs dalle 50/0,45 —
fini le « bleu nuit »). Dalle = référence intouchée, asphalte neutre v4b gardé,
fine1/fine2 déjà intermédiaires (R/B 1,17/1,25). Mesures :
`work/SOLROUTES/abis_palette_mesure.py`.

### 5.5 Ligne axiale cuite des fine_road : supprimée (correctif 5 — master)

Positions mesurées sur les BaseColor 2K (`abis_ligne_scan.py`) : vgdlejpew
bande v [0,4883..0,5078] (+37 niveaux), viciaalew bande u [0,4912..0,5073]
(+15). Le master remplace la bande (frac de l'UV monde, ± feather) par un
échantillon décalé de 0,31 — la ligne en biais disparaît (`P3_fine1_rue_AB1`),
nos tirets restent la seule signalisation. Coût : +2 samples BaseColor.

### 5.6 Régé finale & intégrité

Recuisson proto 44 s, réimport masques + master + 4 MI (6 s), régé streamée
13,4 s : CurbQuads 5124→**5133**, AxialDashes 62 (=), MaskedCells 4 (=) ;
végé re-run : **55 821 instances / 1 235 fosses / 397 skips — identiques au
lot A au compteur près** (zéro régression du couplage masque↔C++).
⚠ PIÈGE payé : la régé q3 avait repassé `proto_capitole_surfaces_grass_v3.json`
— les 1 407 films verts abandonnés par SOLVERT sont revenus et les fosses sont
tombées à 342 (arbres jugés « sur film ») ; correctif q4 = **toujours
`SOLVERT/proto_capitole_surfaces_nogreen.json`** pour les surfaces du proto.
md5 protégés intacts (_E2 6942C9D6, _Sol1 4C67BBD8, LIDAR B71B0E40), 0 commit,
0 build C++, ferme intouchée.

### 5.7 Reste pour les lots B/C (en plus du §6)

- Boulevards à 2 chaussées (Strasbourg) : tronçons parallèles de la même voie
  classés rue/ruelle en alternance — le lissage par continuité ne les voit pas
  (chaussées séparées). Règle « chaussées appariées » (même nom, parallèles
  < 25 m, vote commun) à envisager au lot B.
- Fragments restants assumés : Rue Pierre Baudis (59,6 m pavée dans ruelle,
  donnée réelle conservée), impasses pavées (St Geraud, Yersin), 41 m ruelle
  impasse dans Jean Jaurès.
- Le juge visuel de la couture sur le carré 10 km (le proto n'a que ~1,5 km de
  coutures).

## 6. LOT A-ter — LA CHARTE DE COHÉRENCE DU SOL (2026-07-31)

> Décision utilisateur : « on veut surtout de la cohérence globale et de
> l'harmonie — éviter les chevauchements de sol et les bouts qui apparaissent de
> manière random ». Troc assumé : **la cohérence globale prime sur l'exactitude
> locale**. Ce lot n'est pas une liste de correctifs, c'est **quatre LOIS** dans
> `Tools/j3c_sols_masks.py`, chacune avec son verrou de selftest et sa mesure.
> Chiffres : `work/SOLROUTES/ater_mesure_AVANT.json` / `_APRES.json` +
> `lotAter_progress.log`. Captures : suffixes `ATER*` dans `captures/`.

### 6.1 LOI 1 — pile de priorité UNIQUE (argmax hiérarchique)

La composition n'est plus une superposition de booléens rendus disjoints à la
main (7 lignes de `&= ~…` qu'il fallait maintenir cohérentes avec l'ordre du
shader) : c'est un **argmax** sur une carte de MATÉRIAUX (`mat`, uint8), peinte
du plus faible au plus fort. Un texel appartient à un matériau, **par
construction** — le chevauchement n'est plus improbable, il est impossible.
Les masques SDF (et le canal G = chaussée entière, que lit `FGrassMaskSampler`)
sont ensuite RE-DÉRIVÉS de cette carte, donc le cuit et le rendu ne peuvent plus
diverger. Ordre : `voie (side-car) > pavé piéton > herbe > dalle`, puis les
mécanismes **carrefour dominant** et **seuil de façade** par-dessus.

*Preuve de non-régression* : avec `--sans-charte` (l'ordre lot A-bis), les 8
masques cuits sont **bit-à-bit identiques** à ceux du lot A-bis (8/8 md5) — la
refonte seule ne change RIEN, tout le delta vient des lois.

**Pavé > herbe (changement d'ordre assumé).** Au lot A-bis l'herbe gagnait. Or
le canal végétal vient d'OCS GE CS2.\* qui inclut le LIGNEUX : une rangée
d'arbres AU-DESSUS d'un plateau pavé y est « végétale ». C'est la source de la
« soupe verte + pavé + dalle » des captures utilisateur (`G3_stgeorges_FIN` :
tache verte en plein milieu de la place). Coût mesuré : **2 853 m² d'herbe
passés au pavé sur le proto (5,8 %)**, gain : place Saint-Georges cohérente
(`G3_stgeorges_ATERFIN`). Réversible : constante `COB_SUR_HERBE`.

**Carrefour dominant** : dans le disque d'un carrefour (≥ 3 tronçons ; rayon =
demi-largeur max des voies circulées + 75 cm), la CHAUSSEE ne porte qu'un
revêtement, celui de la voie entrante la plus prioritaire (asphalte > rue >
ruelle). **197 carrefours unifiés** sur le proto. Zéro fil d'eau à l'intérieur.

### 6.2 LOI 2 — taille minimale de motif : 5 m²

Après composition, toute composante connexe < 5 m² est absorbée **entière**
(jamais un demi-motif) par sa voisine dominante — vote sur son contour 4-voisins.
Étiquetage par union-find sur les runs de lignes, pur numpy (~0,15 s pour
2 080², zéro dépendance nouvelle). Exemptions : composante qui touche le bord de
la fenêtre de calcul (sa vraie taille dépasse ce qu'on voit, et la décision doit
rester la même d'une cellule à l'autre) et composante **majoritairement** sous le
bâti (un édicule n'est pas un motif de sol — le critère « touche le bâti », plus
laxiste, exemptait 26 vrais motifs par cellule, exactement les contours rongés).

| | motifs < 5 m² dans le cuit | absorbés | aire |
|---|---|---|---|
| lot A-bis | **154** | — | — |
| A-ter | **0** | 132 | 37 m² |

### 6.3 LOI 3 — catalogue FERMÉ de transitions (3 objets, rien d'émergent)

**Fil d'eau (~50 cm).** L'ancienne couture naissait de `|d1−d2| < 0,5` : le lieu
des points ÉQUIDISTANTS. Près d'un coin ou d'une embouchure il s'évase en
AMIBE — les taches beiges des captures utilisateur. Refondu sous trois
contraintes : (a) largeur plafonnée `|d1| < 0,25 ∧ |d2| < 0,25` (le texel est
près des DEUX bords, donc sur leur bord commun) ; (b) **gradient**
`|∇(d1−d2)| ≥ 1,2` (≈ 2 pour un vrai croisement transversal) ; (c) jamais à
moins de 40 cm d'une frontière d'herbe, jamais dans un carrefour.

Mesure « zéro amibe » = **plus grand disque inscrit dans le fil d'eau** (une
ligne de 50 cm plafonne à 0,25 m ; une amibe monte à plusieurs mètres) :
**0,88 m → 0,49 m**, et **0,13 % seulement** de son aire dépasse le ruban
(rayon > 35 cm) — les coins et les croisements de deux fils, c'est géométrique
et borné. Aire : 1 527 → **982 m²**.

**Seuil de façade / rive du sol public (55 cm).** Deux mesures ont refondu
l'objet demandé au brief :
- il suit **le bord du CORRIDOR**, pas le contour du bâti : le contour bâti
  traverse alternativement du corridor et de la parcelle cadastrale, la bande y
  sortait EN POINTILLÉ (carte `Z1_malcousinat`, capture `T1_..._ATER3`) ;
- **55 cm et non 35** : le masque fait 48,83 cm par texel et son SDF est
  quantifié au pixel de calcul (24,41 cm) — un objet de 35 cm tient sur UN pixel
  et la moyenne 2×2 de la réduction le fait passer/rater un texel sur deux.
  55 cm = un texel plein, c'est **la largeur minimale représentable** par ce
  masque (35 cm exigerait un masque 2048, arbitrage volume tranché au lot A) ;
- la restriction « seulement près d'un bâti » a été mesurée (88,8 % du bord de
  corridor est à moins de 1,20 m d'un bâti, ça sature à 90 % dès 2,40 m) puis
  **abandonnée** : les 10 % restants coupaient la ligne en plein milieu. Une
  rive FERMÉE vaut mieux qu'une rive exacte à 89 %.
- **Compensation de la réduction 2×2** (+w/4 = 12,2 cm sur tout le champ de
  transitions) : sans elle, tout objet fin est rentré de 12,2 cm de chaque côté
  et se hachure. Elle est symétrique, donc l'objet ne bouge pas.

18 249 m² sur le proto. **Zéro sampler et zéro recompilation du master** : le
seuil et le fil d'eau partagent le canal A du masque 2 et le même matériau de
transition (`Tint_seam`). La **bordure 3D** (générateur) est le 3ᵉ objet du
catalogue, inchangée.

### 6.4 LOI 4 — budget de simplicité : ≤ 3 matériaux par disque de 20 m

Verrou : échantillonnage de disques de 20 m (pas de 10 m, 10 000 disques sur le
proto) ; un matériau « compte » s'il couvre ≥ 1 % du disque. En cas d'excès, les
matériaux au-delà du budget voient leurs composantes absorbées — **plafonnées à
250 m²** : au-delà, la composante est un objet réel de la ville, on ne l'efface
pas, on le CONSTATE.

| | ≤ 3 matériaux | 4 | 5 |
|---|---|---|---|
| lot A-bis | 95,89 % | 383 | 33 |
| A-ter | **97,23 %** | 258 | 19 |

**Calibration du plafond, mesurée** : à 800 m² on monte à 98,49 % (et 0 disque à
5 matériaux) MAIS on efface **5 036 m² d'herbe** — de vrais parcs. À 250 m² le
coût est de 263 m². Le plafond de 250 est retenu : +1,3 pt ne vaut pas dix fois
plus de parc supprimé. Résidu assumé (2,77 %) = endroits où 4 matériaux réels
coexistent vraiment ; surplus par matériau : fine2 123, pavé 80, herbe 62.

### 6.5 Domaine de définition des lois

Les lois 2 et 4 sont écrites EN MÈTRES : sous leur propre échelle (verrous à
résolution réduite, où un pixel de calcul pèse 15 m² et où le disque de 20 m ne
fait que 5 px) elles **se taisent** (`charte["echelle_ok"]`). Ce n'est pas une
exception, c'est leur domaine de définition — en production : 0,06 m²/px et 82 px
de rayon. Sans cette garde, la loi 4 déformait les scènes synthétiques des
verrous 11/13.

### 6.6 Streaming — le fix DURABLE en C++ (2 ajouts, 1 build)

`CityImportTools.cpp`, à la CRÉATION (ce sont des propriétés PAR OBJET : posées
après coup, elles ne survivent pas à la régénération suivante) :
1. `Dyn->bInitiallyLoaded = true; bInitiallyVisible = true;` sur les streaming
   levels — le piège PIE payé trois fois, rattrapé jusqu'ici par
   `SOL2/fix_pie_sol2.py` ;
2. `ApplyGroundTextureStreaming()` (helper + constante `100.f` documentée) sur
   les composants `SM_Slab_*`, `SM_Ground_*`, `SM_Proxy_*`, `SM_Surface_*` —
   l'UV0 du sol est en MÈTRES quand le monde est en CENTIMÈTRES, la métrique de
   streaming croit la texture étirée 100 fois.

**Vérifié APRÈS la régé finale, sans aucun correctif manuel** : `flags_ok=true`
(4/4 sous-niveaux) et `sdm_ok=true` (17/17 composants à 100).
⚠ L'A/B de netteté en session chaude (SDM 1 vs 100, mêmes poses, énergie de
gradient) donne **−0,1 % / +0,0 %** : sur le proto, `r.Streaming.PoolSize=3000`
suffit à garder toutes les textures résidentes. L'effet mesuré au lot 3 (masques
visés à 64×64 → 512/1024) ne se voit **qu'après un rechargement à froid** et sur
une ville saturée. Le fix est donc structurel/préventif — pas un gain visible
sur 4 cellules.

### 6.7 Régé finale & intégrité

`ImportCityStreamed` 13,5 s : **CurbQuads 5133 (=), AxialDashes 62 (=),
MaskedCells 4 (=)** ; surfaces NOGREEN (Green=0) ; végé **55 821 instances /
397 skips (identiques au lot A) / 1 292 fosses** (1 235 au lot A-bis, +57 :
l'herbe absorbée rend au minéral des arbres jusque-là jugés « sur pelouse » —
direction attendue). Selftest `j3c_sols_masks --selftest` : **PASS** (verrous
historiques + 9 lissage + ~20 charte). md5 protégés intacts (`_E2` 6942C9D6,
`_Sol1` 4C67BBD8, LIDAR B71B0E40). 1 build C++, 0 commit, ferme intouchée.

### 6.8 Reste pour le lot B

- Résidu du budget (2,77 % de disques à 4 matériaux) : le vrai levier est la
  **règle des chaussées appariées** (§5.7) qui supprimerait des couples
  rue/ruelle voisins, pas une absorption plus agressive.
- Juger le fil d'eau et la rive de façade **à l'échelle du carré 10 km**
  (le proto n'a que ~1,5 km de coutures).
- `sidewalk_trim` sur le chant de bordure : la rive du sol public a désormais
  son matériau, le chant 3D pourrait le reprendre.
- Masque 2048 si l'on veut vraiment un seuil de façade de 30-40 cm.

## 7. Reste à faire (lots B/C)

- Lot B : passages piétons OSM (`CROSSINGS_ON`, D5), macro-variation du minéral
  (D6), lignes de rive/couloir bus peints, `sidewalk_trim` sur le chant de
  bordure, ronds-points ; option : redresser la couture de Voronoï aux embouchures
  (coupe perpendiculaire) ; JUGER sur captures la ligne axiale cuite des
  fine_road en UV monde (orientation monde, pas rue — `P3_fine1_rue_A2.png`).
- Lot C : `URoadGraphCell` + ancre de cellule (le side-car dans le pak), compteurs
  `RoadEdges` dans `FCityStreamedSummary`, refus de générer sans réseau.
- Appliquer BC7 en production + purger `/Game/City/Ground/A1Proto` (assets de
  mesure conservés pour audit).
