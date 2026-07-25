# J2e — PATCH NANITE SUR LES VITRES : NaniteSettings.bEnabled=true + resave sur
# chaque SM_Bldg_*_Glass de /Game/City/Toulouse10 (456 attendus). Retours
# utilisateur du 25/07 : fenetres qui « flottent » — murs Nanite mais vitres
# non-Nanite, transitions LOD decouplees. Le verre genere est OPAQUE : Nanite ok.
# Le generateur (CityImportTools.cpp, profil desktop) pose desormais le flag a la
# generation ; ce script met a niveau les assets DEJA generes (J2c), a executer
# dans CityLab (usine) ET dans Survol (jeu) — memes chemins /Game des deux cotes.
# Execution : UnrealEditor-Cmd <proj>.uproject -run=pythonscript -script=<ce fichier>
#             -nullrhi -unattended -stdout -FullStdOutLogOutput
import re
import time
import unreal

ASSETS = '/Game/City/Toulouse10'
GLASS = re.compile(r'^SM_Bldg_-?\d+_-?\d+_Glass$')


def log(msg):
    unreal.log_warning('GLASSNANITE: ' + msg)


t_all = time.time()
ar = unreal.AssetRegistryHelpers.get_asset_registry()
targets = sorted(set(str(ad.package_name)
                     for ad in ar.get_assets_by_path(ASSETS, recursive=True)
                     if GLASS.match(str(ad.asset_name))))
log('%d vitres SM_Bldg_*_Glass trouvees sous %s' % (len(targets), ASSETS))

smes = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
patched = 0
already = 0
errors = 0
for i, pkg in enumerate(targets):
    mesh = unreal.EditorAssetLibrary.load_asset(pkg)
    if not isinstance(mesh, unreal.StaticMesh):
        unreal.log_error('GLASSNANITE: pas un StaticMesh : %s' % pkg)
        errors += 1
        continue
    ns = mesh.get_editor_property('nanite_settings')
    if ns.get_editor_property('enabled'):
        already += 1
        continue
    ns.set_editor_property('enabled', True)
    if smes is not None and hasattr(smes, 'set_nanite_settings'):
        # Voie canonique : pose le struct ET reconstruit le mesh (apply_changes).
        smes.set_nanite_settings(mesh, ns, True)
    else:
        # Repli : set_editor_property declenche PostEditChange (rebuild au save).
        mesh.set_editor_property('nanite_settings', ns)
    if unreal.EditorAssetLibrary.save_asset(pkg, only_if_is_dirty=False):
        patched += 1
    else:
        unreal.log_error('GLASSNANITE: echec de sauvegarde : %s' % pkg)
        errors += 1
    if (i + 1) % 50 == 0:
        log('progression %d/%d (patchees %d, deja actives %d, erreurs %d, %.0f s)'
            % (i + 1, len(targets), patched, already, errors, time.time() - t_all))

log('TERMINE en %.0f s — %d patchees, %d deja actives, %d erreurs, %d cibles'
    % (time.time() - t_all, patched, already, errors, len(targets)))
if errors > 0 or (patched + already) != len(targets):
    unreal.log_error('GLASSNANITE: RESULTAT INCOMPLET — a examiner avant commit')
