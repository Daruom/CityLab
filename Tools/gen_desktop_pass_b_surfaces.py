# J2 desktop — PASSE B (processus separe, reprise apres kill memoire de la passe
# unique) : ImportCitySurfaces SEUL + sauvegarde. La passe A (ImportCityStreamed)
# est deja sauvee sur disque ; NE PAS la rejouer. Lecon : ~101 Go de working set
# accumules dans un seul processus (builders + MeshDescriptions + Nanite de ~2 400
# meshes jamais liberes) -> kill silencieux Windows. Un processus par passe.
# NB : Surfaces et Markers ne sauvent PAS eux-memes (seul Streamed appelle
# SaveDirtyPackages) — la sauvegarde explicite ici est OBLIGATOIRE.
# Execution : UnrealEditor-Cmd CityLab.uproject -run=pythonscript -script=<ce fichier>
#             -nullrhi -unattended -stdout -FullStdOutLogOutput
import ctypes
import os
import re
import time
import unreal

PROJ = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
SD = os.path.join(PROJ, 'SourceData')
ASSETS = '/Game/City/Toulouse10'
WALL = '/Game/Dev/M_BldgWall.M_BldgWall'


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
    unreal.log_warning('GEN10B: %s [RAM %.1f Go]' % (msg, ws_gb()))


def counts(struct):
    return {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)', struct.export_text())}


t_all = time.time()
profile = unreal.CityGenProfile()
if not profile.import_text('(bDesktop=True)') or 'bDesktop=True' not in profile.export_text():
    raise RuntimeError('GEN10B: profil desktop non pose')
log('demarrage')

t = time.time()
unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Toulouse10')
log('map chargee en %.0f s' % (time.time() - t))

t = time.time()
s2 = counts(unreal.CityImportTools.get_default_object().call_method('ImportCitySurfaces', args=(
    os.path.join(SD, 'toulouse10_surfaces.json'), ASSETS, WALL, 500.0,
    unreal.Vector(0, 0, 0), profile)))
log('ImportCitySurfaces %.0f s : %s' % (time.time() - t, s2))
if s2.get('Meshes', 0) == 0:
    raise RuntimeError('GEN10B: ImportCitySurfaces a echoue (0 mesh) : %s' % s2)

t = time.time()
unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
log('sauvegarde en %.0f s' % (time.time() - t))
log('PASSE B TERMINEE en %.0f min' % ((time.time() - t_all) / 60.0))
