# REPRISE de gen_desktop_toulouse10.py apres interruption EXTERNE du processus.
#
# Contexte : le 26/07, la passe ImportCityStreamed s'est terminee avec succes
# (3519 s, 131357 batiments / 103333 toits en pente / 457 collisions / 136 blocs,
# « Tout est sauve », map + 140 blocs ecrits sur disque a 13:07:42) puis le
# processus a ete tue de l'exterieur (shell parent) AVANT les passes suivantes.
# Le log ne contient ni erreur, ni assert, ni crash.
#
# Ce script rejoue STRICTEMENT les etapes 3 a 7 du script canonique, avec les
# memes parametres, sur la map rechargee depuis le disque. Il ne retouche pas
# la passe streamed (deterministe, deja persistee).
#
# Execution headless : UnrealEditor-Cmd CityLab.uproject -run=pythonscript
#   -script=<ce fichier> -nullrhi -unattended -stdout -FullStdOutLogOutput
import os
import re
import time
import unreal

PROJ = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SD = os.path.join(PROJ, 'SourceData')
MAP_PATH = '/Game/Maps/L_Toulouse10'
ASSETS = '/Game/City/Toulouse10'
BLOCKS = '/Game/Maps/T10Blocks'
WALL = '/Game/Dev/M_BldgWall.M_BldgWall'
GLASS = '/Game/Dev/M_BldgGlass.M_BldgGlass'
MANIFEST = os.path.join(PROJ, 'Saved', 'desktop_manifest.txt')


def log(msg):
    unreal.log_warning('GEN10: ' + msg)


def counts(struct):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', struct.export_text())}


t_all = time.time()

# --- 0. Profil identique au script canonique (source bati J3b) ---
profile = unreal.CityGenProfile()
BATI = os.path.join(SD, 'toulouse10_bati.json').replace('\\', '/')
if not profile.import_text('(bDesktop=True,BuildingsJsonPath="%s")' % BATI):
    raise RuntimeError('GEN10: import_text a echoue sur CityGenProfile')
if 'bDesktop=True' not in profile.export_text():
    raise RuntimeError('GEN10: bDesktop non pose (export: %s)' % profile.export_text())
log('profil desktop OK (reprise etapes 3-7)')

# --- 1. Map ---
t = time.time()
unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)
log('map chargee en %.0f s' % (time.time() - t))

# --- 1b. GARDE : la passe streamed doit etre PRESENTE sur disque, sinon on
#          sauvegarderait une map amputee. Echec immediat plutot que degat.
ar = unreal.AssetRegistryHelpers.get_asset_registry()
n_blocks = len(ar.get_assets_by_path(BLOCKS, recursive=True))
if n_blocks < 100:
    raise RuntimeError('GEN10: GARDE — seulement %d blocs dans %s, la passe streamed '
                       'manque. Relancer le script canonique complet.' % (n_blocks, BLOCKS))
n_col = len([a for a in unreal.EditorAssetLibrary.list_assets(ASSETS)
             if a.split('.')[0].endswith('_Col')])
if n_col < 400:
    raise RuntimeError('GEN10: GARDE — seulement %d meshes _Col, la passe streamed '
                       'manque. Relancer le script canonique complet.' % n_col)
log('garde OK : %d blocs, %d meshes de collision presents' % (n_blocks, n_col))

cdo = unreal.CityImportTools.get_default_object()

# --- 3. Surfaces (eau plane p10, verts/rails drapes) ---
t = time.time()
s2 = counts(cdo.call_method('ImportCitySurfaces', args=(
    os.path.join(SD, 'toulouse10_surfaces.json'), ASSETS, WALL, 500.0,
    unreal.Vector(0, 0, 0), profile)))
log('ImportCitySurfaces %.0f s : %s' % (time.time() - t, s2))
if s2.get('Meshes', 0) == 0:
    raise RuntimeError('GEN10: ImportCitySurfaces a echoue (0 mesh) : %s' % s2)

# --- 4. Marqueurs (drapes MNT) ---
t = time.time()
placed = cdo.call_method('ImportCityMarkers', args=(
    os.path.join(SD, 'toulouse10_markers.json'), ASSETS, WALL,
    unreal.Vector(0, 0, 0), profile))
log('ImportCityMarkers %.0f s : %d marqueurs' % (time.time() - t, placed))
if placed == 0:
    raise RuntimeError('GEN10: ImportCityMarkers a echoue (0 marqueur)')

# --- 5. Habillage desktop (idempotent) ---
eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
OLD_DRESS = ('CitySun', 'CitySkyLight', 'CitySkySphere', 'CitySkyAtmosphere',
             'CityFog', 'CityPlayerStart', 'CityCeiling')
for a in eas.get_all_level_actors():
    if a.get_actor_label() in OLD_DRESS:
        eas.destroy_actor(a)

sun = eas.spawn_actor_from_class(unreal.DirectionalLight, unreal.Vector(0, 0, 500))
sun.set_actor_rotation(unreal.Rotator(roll=0.0, pitch=-35.0, yaw=45.0), False)
sun.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sun.light_component.set_editor_property('atmosphere_sun_light', True)
sun.set_actor_label('CitySun')

sky_l = eas.spawn_actor_from_class(unreal.SkyLight, unreal.Vector(0, 0, 500))
sky_l.light_component.set_mobility(unreal.ComponentMobility.MOVABLE)
sky_l.light_component.set_editor_property('real_time_capture', True)
sky_l.set_actor_label('CitySkyLight')

atmo = eas.spawn_actor_from_class(unreal.SkyAtmosphere, unreal.Vector(0, 0, 0))
atmo.set_actor_label('CitySkyAtmosphere')

fog = eas.spawn_actor_from_class(unreal.ExponentialHeightFog, unreal.Vector(0, 0, 0))
fog.component.set_editor_property('fog_density', 0.005)
fog.set_actor_label('CityFog')

ps = eas.spawn_actor_from_class(unreal.PlayerStart, unreal.Vector(-7000.0, 0.0, 500.0))
ps.set_actor_label('CityPlayerStart')
log('habillage desktop pose (soleil -35, skylight RTC, atmosphere, fog 0.005, PlayerStart)')

# --- 6. Sauvegarde ---
t = time.time()
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
log('sauvegarde en %.0f s' % (time.time() - t))

# --- 7. Manifeste des packages REFERENCES par la map + blocs ---
opts = unreal.AssetRegistryDependencyOptions(
    include_soft_package_references=True, include_hard_package_references=True)
seeds = [MAP_PATH]
for ad in ar.get_assets_by_path(BLOCKS, recursive=True):
    seeds.append(str(ad.package_name))
seen = set()
stack = list(dict.fromkeys(seeds))
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
log('manifeste : %d packages references -> %s' % (len(seen), MANIFEST))

on_disk = set(p.split('.')[0] for p in unreal.EditorAssetLibrary.list_assets(ASSETS))
orphans = sorted(on_disk - seen)
log('assets %s : %d sur disque, %d references, %d orphelins' % (
    ASSETS, len(on_disk), len(on_disk) - len(orphans), len(orphans)))
for o in orphans[:10]:
    log('  orphelin (extrait) : ' + o)

log('GENERATION DESKTOP TERMINEE en %.0f min' % ((time.time() - t_all) / 60.0))
