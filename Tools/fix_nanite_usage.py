"""Pose le flag d'usage Nanite sur les 4 materiaux PBR ville et FORCE la sauvegarde.

Symptome corrige : en -game, « Material ... missing usage flag Nanite! Default
Material will be used in game » -> les batiments Nanite rendent leur fallback
decime 0,1 % (toits fondus). Le generateur (CityImportTools.cpp,
GetOrCreatePBRMaterial) pose desormais le flag a la creation ; ce script met a
niveau les materiaux DEJA generes, a executer dans CityLab (usine) ET dans
Survol (jeu) — memes chemins /Game des deux cotes.

Piege paye (25/07) : set_editor_property ne marque PAS le package dirty ->
save_asset(only_if_is_dirty=True par defaut) retourne True SANS reecrire le
.uasset (timestamp inchange). Ici : recompile + save force + verification.

Execution : UnrealEditor-Cmd <proj>.uproject -run=pythonscript -script=<ce fichier>
            -unattended -stdout   (chemin -script= en SLASHES, jamais backslashes)
"""
import unreal

E = unreal.EditorAssetLibrary
for n in ('M_CityWall_PBR', 'M_CityGlass_PBR', 'M_CityGround_PBR', 'M_CityRoad_PBR'):
    p = '/Game/City/Toulouse10/' + n
    m = E.load_asset(p)
    if not m:
        unreal.log_warning('NFIX2 ABSENT ' + n)
        continue
    before = m.get_editor_property('used_with_nanite')
    m.set_editor_property('used_with_nanite', True)
    try:
        unreal.MaterialEditingLibrary.recompile_material(m)
        rec = 'ok'
    except Exception as e:
        rec = 'ERR ' + str(e)
    saved = E.save_asset(p, only_if_is_dirty=False)
    after = E.load_asset(p).get_editor_property('used_with_nanite')
    unreal.log_warning('NFIX2 {} avant={} apres={} recompile={} saved={}'.format(
        n, before, after, rec, saved))
