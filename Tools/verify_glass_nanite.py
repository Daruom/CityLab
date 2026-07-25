# J2e — VERIFICATION par echantillon du patch Nanite vitres : lit le flag
# NaniteSettings.enabled sur N vitres SM_Bldg_*_Glass et le compte global.
# Execution : UnrealEditor-Cmd <proj>.uproject -run=pythonscript -script=<ce fichier>
#             -nullrhi -unattended -stdout -FullStdOutLogOutput
import random
import re
import unreal

ASSETS = '/Game/City/Toulouse10'
GLASS = re.compile(r'^SM_Bldg_-?\d+_-?\d+_Glass$')

ar = unreal.AssetRegistryHelpers.get_asset_registry()
targets = sorted(set(str(ad.package_name)
                     for ad in ar.get_assets_by_path(ASSETS, recursive=True)
                     if GLASS.match(str(ad.asset_name))))
random.seed(42)
sample = random.sample(targets, min(8, len(targets)))
ok = 0
for pkg in sample:
    mesh = unreal.EditorAssetLibrary.load_asset(pkg)
    enabled = (isinstance(mesh, unreal.StaticMesh)
               and mesh.get_editor_property('nanite_settings').get_editor_property('enabled'))
    unreal.log_warning('GLASSVERIF: %s -> Nanite=%s' % (pkg, enabled))
    if enabled:
        ok += 1
unreal.log_warning('GLASSVERIF: BILAN %d/%d echantillons Nanite=True (sur %d vitres)'
                   % (ok, len(sample), len(targets)))
