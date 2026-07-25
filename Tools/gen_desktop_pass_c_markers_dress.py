# J2 desktop — PASSE C (processus separe) : ImportCityMarkers + HABILLAGE DESKTOP
# + sauvegarde + manifeste STRICT des packages references (seed = map persistante
# seule : les blocs orphelins sur disque et leurs dependances n'y entrent pas).
# Prerequis : passes A (streamed, deja sauvee) et B (surfaces) terminees.
# Execution : UnrealEditor-Cmd CityLab.uproject -run=pythonscript -script=<ce fichier>
#             -nullrhi -unattended -stdout -FullStdOutLogOutput
import ctypes
import os
import time
import unreal

PROJ = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SD = os.path.join(PROJ, 'SourceData')
ASSETS = '/Game/City/Toulouse10'
WALL = '/Game/Dev/M_BldgWall.M_BldgWall'
MAP_PATH = '/Game/Maps/L_Toulouse10'
MANIFEST = os.path.join(PROJ, 'Saved', 'desktop_manifest_strict.txt')


def ws_gb():
    class PMC(ctypes.Structure):
        _fields_ = ([('cb', ctypes.c_ulong), ('PageFaultCount', ctypes.c_ulong)] +
                    [(n, ctypes.c_size_t) for n in (
                        'PeakWorkingSetSize', 'WorkingSetSize', 'QuotaPeakPagedPoolUsage',
                        'QuotaPagedPoolUsage', 'QuotaPeakNonPagedPoolUsage',
                        'QuotaNonPagedPoolUsage', 'PagefileUsage', 'PeakPagefileUsage')])
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    ctypes.windll.psapi.GetProcessMemoryInfo(
        ctypes.windll.kernel32.GetCurrentProcess(), ctypes.byref(pmc), pmc.cb)
    return pmc.WorkingSetSize / (1024.0 ** 3)


def log(msg):
    unreal.log_warning('GEN10C: %s [RAM %.1f Go]' % (msg, ws_gb()))


t_all = time.time()
profile = unreal.CityGenProfile()
if not profile.import_text('(bDesktop=True)') or 'bDesktop=True' not in profile.export_text():
    raise RuntimeError('GEN10C: profil desktop non pose')
log('demarrage')

t = time.time()
unreal.EditorLoadingAndSavingUtils.load_map(MAP_PATH)
log('map chargee en %.0f s' % (time.time() - t))

# --- Marqueurs (drapes MNT) ---
t = time.time()
placed = unreal.CityImportTools.get_default_object().call_method('ImportCityMarkers', args=(
    os.path.join(SD, 'toulouse10_markers.json'), ASSETS, WALL,
    unreal.Vector(0, 0, 0), profile))
log('ImportCityMarkers %.0f s : %d marqueurs' % (time.time() - t, placed))
if placed == 0:
    raise RuntimeError('GEN10C: ImportCityMarkers a echoue (0 marqueur)')

# --- Habillage desktop (idempotent, labels City*) ---
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

# --- Sauvegarde ---
t = time.time()
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
log('sauvegarde en %.0f s' % (time.time() - t))

# --- Manifeste strict (BFS dependances depuis la map persistante SEULE) ---
ar = unreal.AssetRegistryHelpers.get_asset_registry()
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
blocks = [p for p in seen if p.startswith('/Game/Maps/T10Blocks/')]
city = [p for p in seen if p.startswith('/Game/City/Toulouse10/')]
other = sorted(seen - set(blocks) - set(city) - {MAP_PATH})
log('manifeste strict : %d packages (blocs=%d, city=%d) -> %s'
    % (len(seen), len(blocks), len(city), MANIFEST))
for o in other:
    log('  hors City/Blocs : ' + o)

on_disk = set(p.split('.')[0] for p in unreal.EditorAssetLibrary.list_assets(ASSETS))
orphans = sorted(on_disk - seen)
log('assets %s : %d sur disque, %d references, %d orphelins (non copies vers Survol)'
    % (ASSETS, len(on_disk), len(on_disk) - len(orphans), len(orphans)))

log('PASSE C TERMINEE en %.0f min' % ((time.time() - t_all) / 60.0))
