# Verrou 2 / passe complete : collision batiments dediee — genere les SM_Bldg_*_Col
# (prismes fermes par batiment, pose Lot A) et les cable en ComplexCollisionMesh des
# SM_Bldg_*_Wall de L_Toulouse10, sauvegarde au fil de l'eau (outil C++
# CityImportTools.GenerateBuildingCollisionCell/All, appel par call_method sur le CDO
# — les outils AICallable n'ont pas de glue Python).
#
# Execution headless (PAS besoin de charger la map : l'outil ne touche que des assets) :
#   UnrealEditor-Cmd CityLab.uproject -run=pythonscript -script=<ce fichier avec SLASHES>
#     -nullrhi -unattended -stdout -FullStdOutLogOutput
# BLDGCOL_CELL="x,y" dans l'environnement : une seule cellule (verrou).
# Sans variable : passe COMPLETE sur toutes les cellules a batiments.
import os
import re
import time
import unreal

PROJ = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
JSON = os.path.join(PROJ, 'SourceData', 'toulouse10.json')
ASSETS = '/Game/City/Toulouse10'
CELL_M = 500.0


def log(msg):
    unreal.log_warning('BLDGCOL: ' + msg)


def counts(struct):
    # UPROPERTY() nus : get_editor_property refuse (« protected ») -> export_text.
    # Les champs a valeur par defaut (0/False) n'y figurent PAS.
    txt = struct.export_text()
    out = {k: int(v) for k, v in re.findall(r'(\w+)=(-?\d+)\b', txt)}
    for k, v in re.findall(r'(\w+)=(True|False)', txt):
        out[k] = (v == 'True')
    return out


t0 = time.time()
profile = unreal.CityGenProfile()
if not profile.import_text('(bDesktop=True)'):
    raise RuntimeError('BLDGCOL: import_text a echoue sur CityGenProfile')
cdo = unreal.CityImportTools.get_default_object()

cell = os.environ.get('BLDGCOL_CELL', '').strip()
if cell:
    cx, cy = (int(v) for v in cell.split(','))
    log('cellule unique (%d, %d)' % (cx, cy))
    s = counts(cdo.call_method('GenerateBuildingCollisionCell',
                               args=(JSON, ASSETS, CELL_M, cx, cy, profile)))
    log('cellule (%d, %d) en %.0f s : %s' % (cx, cy, time.time() - t0, s))
    if s.get('Buildings', 0) == 0 or not s.get('bWallWired', False) or not s.get('bSaved', False):
        raise RuntimeError('BLDGCOL: cellule (%d, %d) incomplete : %s' % (cx, cy, s))
else:
    log('passe complete (toutes cellules)')
    s = counts(cdo.call_method('GenerateBuildingCollisionAll',
                               args=(JSON, ASSETS, CELL_M, profile)))
    log('passe complete en %.0f min : %s' % ((time.time() - t0) / 60.0, s))
    if s.get('Cells', 0) == 0 or s.get('MissingWalls', 1) != 0:
        raise RuntimeError('BLDGCOL: passe incomplete : %s' % s)

log('TERMINE')
