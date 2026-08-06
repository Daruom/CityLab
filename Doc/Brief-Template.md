# Modèle de brief d'agent + checklist du coordinateur

> **Écrit le 2026-08-06, après ~15 lots d'agents en 48 h.** Cause du problème : le
> coordinateur réécrivait chaque brief **de mémoire**, et oubliait à chaque fois un
> point différent (le skill, le guetteur, la boucle district, l'annonce du temps…).
> **Ce fichier se COPIE, il ne se retient pas.** Le lire prend 30 s ; l'oublier a
> coûté ~18 min sur une seule heure.

---

## PARTIE A — LE BLOC INVARIANT (à coller VERBATIM dans tout brief d'agent)

```
## ⚠️ OUTILLAGE (obligatoire — ne se déduit pas)
① Charge le skill `unreal-engine-skills-for-claude-code:unreal-mcp` s'il est exposé.
② Lis `CityLab\Doc\Agent-Playbook.md` §0-§4 (2 min) — notamment §3 : Python par le
   GUETTEUR (`LIDARC/run.ps1`, ~2 s/op), JAMAIS la console MCP (~90 s/op).
③ `v6_focus.ps1` AVANT toute série de captures : un éditeur non focalisé bloque
   TOUT le serveur MCP (9 min perdues le 06/08, ça ressemble à un agent mort).
④ Vérifie map ET `pie=false` au démarrage (PIE fantôme : 5 occurrences).

## ⚠️ BOUCLE (Playbook §11)
- Itérations : **régé DISTRICT** (`CellFilter`, ~20 s) + mesures **sur le district**.
- **UNE régé 3×3 complète À LA FIN** seulement (validation + idempotence).
- Végétation rejouée **seulement si le sol a bougé**.
- Le plus rapide : **ne pas toucher au C++** — la cuisson livre, le moteur pose (§11.3 ter).

## ⚠️ SUPERVISION
`JALON:` ≤ 10 min dans progress.log, **y compris au démarrage de chaque phase longue**
(« j'écris la mesure », « je lance l'export ») : le silence doit rester un signal.
`BLOQUE:` dès qu'un arbitrage NOUVEAU apparaît — ne jamais trancher seul.

## ⚠️ PÉRIMÈTRE & DOCTRINE
- **Correctifs NATIONAUX** : zéro cleabs, zéro identifiant, zéro coordonnée en dur
  dans une RÈGLE. La vérité locale ne vit que dans les VERROUS. Si un correctif ne
  s'exprime que comme cas particulier → `BLOQUE:`.
- Prémisses du brief = **HYPOTHÈSES à falsifier**, pas des faits.
- Lois de construction : Playbook §13 (nappes / composition / orientation / bornes /
  extrémités / existence).
- AUCUN commit (le coordinateur seul) · jamais `L_ProtoSols_E2_Sol1` · `.bak` avant
  toute réécriture · aucune nouvelle classe de sol ni matériau.

## ⚠️ ACCEPTATIONS
- Formulées **à l'échelle du JUGEMENT** (le champ des caméras), jamais du périmètre
  d'implémentation (Playbook §14.2).
- Au moins une **structurelle** qui rend la classe de bug impossible (ex. « 0 face
  oblique », « 0 normale rentrante », « 0 test de normale dans le code »).
- Mesurées sur le **maillage SORTI DU MOTEUR**, jamais sur la cuisson qui l'a produit.

## ⚠️ LIVRAISON
- **3-4 captures aux poses du grief**. Pas de travelling (réservé à la validation finale).
- Description **FACTUELLE**, **aucun verdict** ; signaler explicitement les images
  douteuses. Auto-rejet d'une version ratée AVANT livraison.
- Message final COURT : mesures chiffrées, captures, chronos.
```

---

## PARTIE B — CHECKLIST DU COORDINATEUR (avant / pendant / après)

**AVANT de lancer**
- [ ] ⭐ **REPRENDRE l'agent du lot précédent plutôt que d'en créer un neuf** :
      `SendMessage` avec son `agentId` fonctionne **même après sa fin**
      (« *had no active task; resumed from transcript* ») — il garde son contexte,
      son code et ses pièges. Économie mesurée : **10-15 min de démarrage à froid
      par lot**. ⚠️ Ne marche PAS au travers d'un changement de session (transcript
      introuvable) : dans ce cas seulement, agent neuf + brief dense.
- [ ] Le bloc invariant (partie A) est **collé** dans le brief.
- [ ] La mission tient en 5-10 lignes : **un seul sujet** si c'est du visuel.
- [ ] Les faits déjà connus sont **dans le brief** (pas « relis la saga ») — mais
      l'outillage n'est JAMAIS coupé, même en itération rapide.
- [ ] Les acceptations sont **chiffrées** et à l'échelle du jugement.

**AU LANCEMENT**
- [ ] **Watchdog armé** sur le progress.log (motif de fin strict `TERMINE:`, alerte
      silence ≥ 12-14 min, compteur armé après le 1ᵉʳ écrit de CET agent).
- [ ] Annoncer à l'utilisateur une **fourchette de bout en bout**, **jamais** la boucle
      machine (« 20-40 min », pas « 90 secondes »).

**PENDANT**
- [ ] Sur alerte : diagnostiquer (progress.log + **journal de l'éditeur** = vraie vie),
      ne pas attendre passivement.
- [ ] Ne rien relayer d'un rapport sans l'avoir confronté aux faits disponibles.

**À LA LIVRAISON**
- [ ] **Ouvrir les captures moi-même** avant de les transmettre, et dire ce que j'y vois.
- [ ] Distinguer ce qui est **prouvé** de ce qui est **supposé** (une mesure prouve ce
      qu'elle mesure : un rayon prouve l'existence, pas la visibilité).
- [ ] **Commit + push** dès qu'un état est meilleur que le précédent, avec la leçon
      dans le message.
- [ ] Mettre à jour Playbook/mémoire si un piège neuf a été payé.
