# J2 desktop — PASSE D (correctif) : PURGE DES ACTEURS MOBILES RESIDUELS dans les
# blocs de streaming. Bug decouvert apres la passe A : en COMMANDLET, les sous-niveaux
# ne sont pas charges a l'ouverture de la map (charges seulement au remplissage par
# FlushLevelStreaming) -> la purge d'idempotence d'ImportCityStreamed (TActorIterator)
# n'a pas vu les anciens acteurs SM_Bldg_<x>_<y> mobiles : chaque bloc contenait la
# ville EN DOUBLE (mobile + desktop _Wall/_Glass). En editeur interactif le bug
# n'existe pas (sous-niveaux charges a l'ouverture).
# Traitement : un load_map PAR BLOC (memoire bornee), destruction des acteurs de
# labels mobiles exacts (SM_Bldg_x_y sans suffixe, SM_City_*), sauvegarde ; puis
# manifeste strict recalcule depuis la map persistante, et suppression des surfaces
# orphelines (referenceurs verifies) — sanctionnee par le coordinateur.
# Execution : UnrealEditor-Cmd CityLab.uproject -run=pythonscript -script=<ce fichier>
#             -nullrhi -unattended -stdout -FullStdOutLogOutput
import os
import re
import time
import unreal

PROJ = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
BLOCKS = '/Game/Maps/T10Blocks'
MAP_PATH = '/Game/Maps/L_Toulouse10'
ASSETS = '/Game/City/Toulouse10'
MANIFEST = os.path.join(PROJ, 'Saved', 'desktop_manifest_strict.txt')


def log(msg):
    unreal.log_warning('GEN10D: ' + msg)


t_all = time.time()
ar = unreal.AssetRegistryHelpers.get_asset_registry()
blocks = sorted(set(str(ad.package_name)
                    for ad in ar.get_assets_by_path(BLOCKS, recursive=True)))
log('%d blocs a purger' % len(blocks))

MOBILE = re.compile(r'^SM_(Bldg_-?\d+_-?\d+|City_-?\d+_-?\d+)$')
total = 0
dirty_blocks = 0
for i, pkg in enumerate(blocks):
    unreal.EditorLoadingAndSavingUtils.load_map(pkg)
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    removed = 0
    for a in eas.get_all_level_actors():
        if MOBILE.match(a.get_actor_label()):
            eas.destroy_actor(a)
            removed += 1
    if removed:
        unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
        dirty_blocks += 1
        total += removed
    if removed or (i + 1) % 20 == 0:
        log('[%d/%d] %s : %d acteurs mobiles retires' % (i + 1, len(blocks), pkg, removed))
log('PURGE FINIE : %d acteurs mobiles retires dans %d blocs (%.0f s)'
    % (total, dirty_blocks, time.time() - t_all))

# --- Manifeste strict recalcule (seed = map persistante seule) ---
unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)
opts = unreal.AssetRegistryDependencyOptions(
    include_soft_package_references=True, include_hard_package_references=True)
seen = set()
stack = [MAP_PATH]
while stack:
    p = stack.pop()
    if p in seen:
        continue
    seen.add(p)
    for d in (ar.get_dependencies(p, opts) or []):
        ds = str(d)
        if ds.startswith('/Game/') and ds not in seen:
            stack.append(ds)
with open(MANIFEST, 'w') as f:
    f.write('\n'.join(sorted(seen)))
nb_blocks = len([p for p in seen if p.startswith(BLOCKS + '/')])
city = set(p for p in seen if p.startswith(ASSETS + '/'))
other = sorted(seen - set(p for p in seen if p.startswith(BLOCKS + '/')) - city - {MAP_PATH})
log('manifeste strict : %d packages (blocs=%d, city=%d)' % (len(seen), nb_blocks, len(city)))
for o in other:
    log('  hors City/Blocs : ' + o)

on_disk = set(p.split('.')[0] for p in unreal.EditorAssetLibrary.list_assets(ASSETS))
orphans = sorted(on_disk - seen)
log('assets city : %d sur disque, %d references, %d orphelins'
    % (len(on_disk), len(on_disk) - len(orphans), len(orphans)))

# --- Suppression sanctionnee : surfaces orphelines UNIQUEMENT, referenceurs verifies ---
deleted = 0
for pkg in orphans:
    if '/SM_Surface_' not in pkg:
        continue
    refs = [str(r) for r in (ar.get_referencers(pkg, opts) or [])]
    ext_refs = [r for r in refs if r not in orphans]
    if ext_refs:
        log('  surface orpheline GARDEE (referencee par %s) : %s' % (ext_refs, pkg))
        continue
    if unreal.EditorAssetLibrary.delete_asset(pkg):
        deleted += 1
    else:
        log('  ECHEC suppression : ' + pkg)
log('surfaces orphelines supprimees : %d' % deleted)

# Bilan final des familles d'orphelins restants (gardes, a signaler).
fam = {}
for pkg in sorted(set(p.split('.')[0] for p in unreal.EditorAssetLibrary.list_assets(ASSETS)) - seen):
    name = pkg.rsplit('/', 1)[-1]
    key = re.sub(r'_?-?\d+.*$', '', name)
    fam[key] = fam.get(key, 0) + 1
log('orphelins restants par famille : %s' % fam)
log('PASSE D TERMINEE en %.0f min' % ((time.time() - t_all) / 60.0))
