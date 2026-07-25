# J2 — GENERATION DESKTOP de L_Toulouse10 (profil FCityGenProfile::Desktop) + habillage.
# Regenere la ville 10x10 km EN PLACE dans la map existante via les 3 outils
# CityImportTools, avec les MEMES parametres que les generations mobiles historiques
# (logs du 24/07 : Wall=/Game/Dev/M_BldgWall, Glass=/Game/Dev/M_BldgGlass,
# assets /Game/City/Toulouse10, blocs /Game/Maps/T10Blocks, cellules 500/1000/2000 m,
# origine (0,0,0), ordre Streamed -> Surfaces -> Markers) + Profile bDesktop=True
# (Resolved() applique le prereglage complet : sol 64x64 drape MNT, collision 16x16,
# routes 15 m + ponts, fenetres en creux, split Wall/Glass, Nanite, PBR Lumen).
#
# Habillage desktop (idempotent, labels City*) : soleil Movable atmosphere pitch -35,
# SkyLight real-time capture, SkyAtmosphere (le ciel — PAS de dome custom), brume
# ExponentialHeightFog 0.005, PlayerStart 'CityPlayerStart' (-7000, 0, 500).
#
# Les outils C++ n'ont PAS de glue Blueprint/Python (UFUNCTION meta AICallable) :
# on passe par call_method sur le CDO (conversion reflexion pure, structs incluses).
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
    # Les proprietes des structs outils sont des UPROPERTY() nus : get_editor_property
    # est REFUSE (« protected »). export_text passe par ImportText/ExportText UScriptStruct,
    # sans controle de visibilite — les champs a valeur PAR DEFAUT (0) n'y figurent pas.
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', struct.export_text())}


t_all = time.time()

# --- 0. Smoke test call_method + struct profile (echec rapide plutot que 10 min) ---
# set_editor_property est refuse sur les UPROPERTY() nus (« protected ») : on passe
# par import_text (ImportText reflexion, aucun controle de flags d'edition).
profile = unreal.CityGenProfile()
if not profile.import_text('(bDesktop=True)'):
    raise RuntimeError('GEN10: import_text a echoue sur CityGenProfile')
if 'bDesktop=True' not in profile.export_text():
    raise RuntimeError('GEN10: bDesktop non pose sur CityGenProfile (export: %s)'
                       % profile.export_text())
unreal.BuildingTools.get_default_object().call_method('ExecConsoleCommand', args=('stat None',))
log('smoke test call_method + CityGenProfile(bDesktop) OK')

# --- 1. Map ---
t = time.time()
unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)
log('map chargee en %.0f s' % (time.time() - t))

cdo = unreal.CityImportTools.get_default_object()

# --- 2. Ville streamee (batiments detail/proxy, sol drape, routes, arbres) ---
t = time.time()
s = counts(cdo.call_method('ImportCityStreamed', args=(
    os.path.join(SD, 'toulouse10.json'), os.path.join(SD, 'toulouse10_surfaces.json'),
    ASSETS, BLOCKS, WALL, GLASS, 500.0, 1000.0, 2000.0, unreal.Vector(0, 0, 0), profile)))
log('ImportCityStreamed %.0f s : %s' % (time.time() - t, s))
if s.get('StreamingBlocks', 0) == 0 or s.get('Buildings', 0) == 0:
    raise RuntimeError('GEN10: ImportCityStreamed a echoue (comptes a zero) : %s' % s)

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

# --- 7. Manifeste des packages REFERENCES par la map + blocs (pour la copie Survol :
#         exclut les assets orphelins SM_City_* / anciens SM_Bldg_ mobiles) ---
ar = unreal.AssetRegistryHelpers.get_asset_registry()
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

# Bilan disque vs references (les orphelins ne partiront pas vers Survol).
on_disk = set(p.split('.')[0] for p in unreal.EditorAssetLibrary.list_assets(ASSETS))
orphans = sorted(on_disk - seen)
log('assets %s : %d sur disque, %d references, %d orphelins' % (
    ASSETS, len(on_disk), len(on_disk) - len(orphans), len(orphans)))
for o in orphans[:10]:
    log('  orphelin (extrait) : ' + o)

log('GENERATION DESKTOP TERMINEE en %.0f min' % ((time.time() - t_all) / 60.0))
