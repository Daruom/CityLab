# REPRISE — État du projet (v3, réécrite le 2026-08-06 après la saga BERGE)

> **LE document d'entrée.** Réécrit après 3 jours de chantier berge (04-06/08,
> ~25 lots d'agents). v2 archivée : `Archive/2026-08-06_Reprise-Etat-Projet-v2.md`.
> Compléments obligatoires : `Agent-Playbook.md` (outillage + doctrine §13-14) et
> `Brief-Template.md` (à COPIER à chaque lancement d'agent).

---

## 1. Vision (inchangée)

Génération de Toulouse **data-driven, règles NATIONALES uniquement** — zéro cas
particulier local dans une règle : la vérité locale ne vit que dans les VERROUS
(tests). Pas de donnée → pas d'objet. Pas de photoréalisme visé : **un rendu sans
défauts, cohérence globale**. Façades sans fenêtres (définitif). L'œil de
l'utilisateur est une DONNÉE — y compris ce qu'il voit en se déplaçant dans
l'éditeur (l'indice « je passe derrière le mur et c'est texturé » a résolu ce que
6 h de mesures rataient).

## 2. État du proto 3×3 (hors berge — stable)

`L_ProtoSols_E2_Sol2`, 36 cellules, ~20 780 bâtiments, ~1,17 M d'instances de
végétation, **Play 2,3 s / Stop 2,2 s**, corps physiques ~91 k (budget = corps).
Tiennent sans grief : **bâtiments** (0 incident depuis toujours), **sols** (3
classes), **arbres/végé** (LiDAR, line-trace sur surface rendue, « rien sur l'eau »
et « rien sur un ouvrage »), **eau** (cote réelle par bief, lit enfoncé à −2,50 m,
Delaunay contrainte avec garde de Z, « le polygone d'eau gagne » sur la peinture),
**ponts** (98 tabliers à leur cote, passage sous Saint-Pierre verrouillé C2),
**gradins OSM bleachers** (66,8-79,7 m selon comptage, 0 ailleurs), VSM, Nanite.

## 3. LA BERGE — le chantier en cours

### 3.1 Le modèle (verbatim utilisateur, fait autorité)
**Niveau 0** = le sol habituel de la map (place Saint-Pierre, ~141,5 m NGF) — il
EXISTE, on n'y touche pas. **Niveau −1** = la promenade : un long bloc PLAT
(eau + 1,20 m), continu jusqu'à la Daurade. **Niveau −2** = l'eau ; rien ne les
sépare, le bloc plonge. Entre 0 et −1 : **un escalier central étroit + gradins**
(les 2 emprises OSM `leisure=bleachers`, chacune contenant sa volée BD TOPO).

### 3.2 Acquis (commits `3dd841fc` → `797a3ee9`)
- **Le bloc à deux niveaux existe** : promenade plate (écart-type 0 cm), faces
  verticales, gradins pleine hauteur avec UN jeu de marches par emprise, volées
  taillées dedans (têtes 141,5 = la place), le bloc passe sous le pont (C2 PASS),
  règle d'existence (bloc seulement si le sol de ville domine de ≥ 0,50 m —
  52,8 % du linéaire n'a rien).
- **Classes de bugs éteintes structurellement** : faces retournées cuisson
  (49 309 → 13, orientation par sondage de surface, indécidable = deux côtés) ;
  parements de PONT posés des deux côtés (C++ `BuildBridge`, 5 sites) ;
  fermeture systémique (0 discontinuité non fermée) ; **coutures soudées sommet
  pour sommet** (fin des quads-enveloppe, 181 soudées, compteur
  `coutures_enveloppe_non_soudees=0`) ; réglages canoniques = **défauts du code**
  (`CAP_M 40 / REGUL_M 9 / SIGMA_M 0 / MINRUN_M 10`) + `garde_ancrage()` ;
  **cuisson idempotente** (2 cuissons = bit-identique).
- **Lois** (Playbook §13) : aucune nappe lissée en Z (constantes par zone, régul
  EN PLAN seulement) ; le sol ne cède jamais, les objets se posent ; orientation
  par point-dans-polygone ; ne régénérer que ce qui n'existe pas ; fermer les
  extrémités ; pas de donnée pas d'objet (relief inclus).

### 3.3 ⛔ LES 4 GRIEFS OUVERTS (nommés au pixel — l'utilisateur les ferme, pas nous)
1. **LIMONS manquants** : la jonction escalier/gradins est une dentelure (rythmes
   17 vs 43 cm). Règle spécifiée jamais implémentée (3 passes mangées par les
   diagnostics) : *une volée taillée dans des gradins est bordée de limons
   continus* — surface PLANE le long de la ligne droite de la volée, UNE largeur
   nationale (~0,4-0,6 m), curb, additif. Spec : `work/BLOC/BRIEF_TROIS.md` §②.
2. **TRAIT BLANC** x=455 de la pose GRADINS_ESCALIER (58 px, luminance ~101) :
   liseré vertical clair sur une surface CONTINUE dessinée (`SM_Slab`, ~8 m).
   Ni trou, ni objet, ni couture à profils croisés (falsifiés). **Piste : normales
   non unifiées à la jonction** (positions soudées depuis `797a3ee9`, normales
   calculées par pan) — sonde des normales = 1 min.
3. **TRIANGLE + son jumeau** : lames verticales (−676,31 ; 133,07) h 7,24 m et
   (−696,95 ; 121,88) h 7,37 m — détectées par le garde-fou v4. Ce sont des
   **faces de bord du BLOC** (PAS du volume en escalier : la rétraction des
   marches, écrite, a raté la cible et créé 8 px de régression → **désactivée au
   bisect**, commentée avec sa cause dans `bl2_bloc.py`). À faire : nommer le
   constructeur exact de ces faces, puis rétracter LE BON objet.
4. **LIAISON INCURVÉE** (nouveau, 06/08 soir) : d'un côté de l'escalier, la
   liaison au niveau 0 est courbe/ondulée (la crête serpente au raccord avec la
   pelouse). Hypothèse à sonder : le bord du bloc suit un contour ORGANIQUE de
   donnée (limite pelouse/emprise) au lieu d'une ligne architecturale droite.

### 3.4 Arbitrages utilisateur en attente
- **La rampe** : 45,9 % du linéaire sans bloc (sol de ville trop bas) → mur si
  écart ≥ 50 cm / raccord à plat sinon ? (recommandé : jamais de rampe.)
- **Lot RENDU** (à froid, toute la ville) : contraste girons/contremarches au
  soleil (×2,6, physique), ombre portée de la promenade (gagnée au fix FACES),
  reflets rasants Fresnel/ciel réfléchi.
- **Garde-fou lames** : indexer les couvertures sur le maillage ENTIER (14
  bâtiments lointains = faux positifs du tronc de vision culé).
- Verrou berges jambe 3 (135 touffes), consolidation générale (régé 3×3 +
  verrous gravés) une fois la berge validée.

### 3.5 Outillage berge (dans `C:\LidarPoC\work\BLOC\`, hors repo)
`bl2_bloc.py` (LA cuisson du bloc : réglages canoniques en défauts, garde
d'ancrage, `retracter_bouts()` désactivée-commentée) · `sb2_sol.py` (couture
soudée + `orienter_verticales()`) · **`g6_visib.py`** = le JUGE DE VISIBILITÉ
(rasterisation double tampon 1-3 s/vue : pixels que l'œil TRAVERSE) + garde-fou
`lames_qui_traversent()` v4 (tranche LIBRE = arête haute non bordée + couvertures
obliques indexées) · `bl22_sens.py` (orientation — ⚠️ ses « indécidables » ont
raté la fente la plus visible) · `bl15_discont.py` (⚠️ NON valide sur du
double-face : parité) · `g3_rayons.py`/`g5_depth.py` (sondes pixel→mesh) ·
poses dans `poses_ancrage.json` + `.meta.txt`. C++ : `CityImportTools.cpp`
(parements pont ; `essai-surplomb-KO` archivé = tentative surplomb à NE PAS
refaire telle quelle : +596 px).

## 4. ⭐⭐ LE WORKFLOW CAPITALISÉ (03-06/08 — vaut pour tout chantier)

1. **`Doc/Brief-Template.md` se COPIE à chaque lancement** (bloc invariant
   verbatim + checklist coordinateur). Cause : les briefs de mémoire oubliaient
   un point différent à chaque fois. Couper « relis la saga » est bon ; couper
   l'OUTILLAGE ne l'est jamais.
2. **REPRENDRE l'agent plutôt qu'en créer un** : `SendMessage` sur son agentId
   marche **même après sa fin** (« resumed from transcript », 6 s, contexte
   intact) — économise 10-15 min de démarrage à froid. ⚠️ Aléatoire (échoue
   parfois « No transcript found », toujours au travers d'un changement de
   session) → repli : agent NEUF + **brief dense** qui porte tous les faits.
   Agents en **Opus 5**.
3. **L'ACCEPTATION D'UN GRIEF UTILISATEUR = UN ZOOM A/B SUR SES COORDONNÉES
   EXACTES, jamais un agrégat.** Le grief reste ouvert tant que l'utilisateur ne
   l'a pas fermé. (Payé : « part pont 637 → 83 » annoncé « corrigé ».)
4. **La chaîne des confusions mesure/perception** (3 occurrences payées) : un
   rayon touche ≠ visible (backface culling) · le juge est content ≠ le grief est
   réglé · l'agrégat ≠ les coordonnées du grief. Le **juge de visibilité** (ce
   que l'œil traverse) comble une partie ; le zoom A/B fait le reste.
5. **Un réglage canonique qui vit dans une DOC est une régression en attente** :
   toute valeur canonique = défaut du CODE + garde d'empreinte (payé : cap 21 au
   lieu de 40 → la « fosse »).
6. **Passes MONO-SUJET, diagnostic d'abord puis EXÉCUTION PURE** ; bisect avant
   de livrer ; auto-rejet sur capture ; le coordinateur OUVRE les captures
   lui-même et annonce une **fourchette de bout en bout** (jamais la boucle
   machine).
7. **Boucles mesurées** : cuisson seule ~50 s + bake ~23 s + régé district
   ~25 s ≈ **2 min** ; C++ corps → **Live Coding** 15-24 s (jamais
   Enable/Console) ; guetteur Python **2 s/op** (jamais la console MCP à
   90 s/op) ; juge 1-3 s/vue ; captures 3-4 poses ~2 min. Régé 3×3 (129-156 s)
   réservée à la validation finale.
8. **Watchdog** : Monitor sur progress.log (motif `TERMINE:` strict, alerte
   ≥ 12-14 min, armé après le 1ᵉʳ écrit de CET agent). Diagnostic d'une alerte =
   jalons + **journal de l'éditeur** + mtime des fichiers de travail (les phases
   de rédaction LLM sont silencieuses partout — la plupart des alertes de la
   session étaient ça).

### Pièges payés 05-06/08 (en plus du Playbook §10-11)
- **`CaptureViewport` sur un éditeur NON FOCALISÉ bloque TOUT le serveur MCP**
  (même `initialize` expire ; ressemble à un agent mort) → `v6_focus.ps1` avant
  toute série. 9 min payées.
- **1ʳᵉ régé post-Live-Coding AVALÉE par le guetteur** (aucun .done, aucun log,
  éditeur vivant) → relancer le même .py, la 2ᵉ passe s'exécute (Playbook §11.3).
- `EditorLoadingAndSavingUtils.reload_packages` **tue la frame Python** du
  guetteur (l'éditeur survit) et peut ouvrir une **modale** qui gèle MCP jusqu'au
  clic utilisateur.
- `sed -i` (Git Bash) convertit un `.cpp` **CRLF → LF** (diff massif fantôme).
- **PIE fantôme** (~6 occurrences) : vérifier `pie=false` au démarrage de chaque
  lot ; `BERGES/py/b0_stop.py` l'arrête en 2 s. Cause jamais élucidée.

## 5. NEXT STEPS (dans l'ordre)

1. **Grief 1 — LIMONS** : passe dédiée, exécution pure, rien d'autre.
2. **Grief 2 — TRAIT BLANC** : sonde des normales à la jonction, puis
   unification si confirmé.
3. **Grief 3 — TRIANGLE** : nommer le constructeur des 2 lames, rétracter le bon
   objet (la version marches est commentée dans `bl2_bloc.py` avec sa cause).
4. **Grief 4 — LIAISON INCURVÉE** : sonder le contour (donnée organique vs ligne
   architecturale) puis règle de redressement.
5. Arbitrages utilisateur (rampe, lot RENDU) → puis **CONSOLIDATION GÉNÉRALE**
   (régé 3×3, verrous gravés, 135 touffes, doc) et la berge est CLOSE.
6. Ensuite, la file d'avant la berge : props/marquages, LIDAR_C, streaming/HLOD
   agglo, trémie Saint-Cyprien.

## 6. Carte des documents

`Doc/Agent-Playbook.md` — LA référence agents (outillage §0-4, boucle §11,
doctrine construction §13, itération visuelle §14) · `Doc/Brief-Template.md` —
à COPIER à chaque lancement · `Doc/Reprise-Etat-Projet.md` — CE fichier ·
`Doc/Archive/` — versions datées (v1 du 01/08, v2 du 02/08) · les briefs
`work/BLOC/BRIEF_*.md` — historiques de la saga (BRIEF_TROIS.md contient la spec
LIMONS encore valable) · mémoire persistante coordinateur — index + fiches
workflow (discipline de lancement, atelier permanent).

### Chaîne des commits de la saga berge (04-06/08)
`6f988d6e` BERGES → `cebdf41c` MUR35 → `a651c709` RIVAGE interim → `22c770fd`
CHECKPOINT → `3dd841fc` **BLOC** (le modèle deux niveaux) → `07cfa96f`+`f11d1d71`
Playbook §13-14 + 11.3ter → `49f3202b` consolidation+végé → `a99ddf3d` **FACES**
(49 309 faces retournées) → `75fcba89` **ANCRAGE** (réglages → code) →
`ebc361a8` Brief-Template → `f2994667` FENTES → `6b19d685` parements pont+girons
infirmés → `ed2558a6` piège régé avalée → `797a3ee9` **SOUDURE**.
