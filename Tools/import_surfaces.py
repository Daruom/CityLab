# J3c point 2 « builder sols » — importe les packs de revetements Megascans et
# fabrique un materiau par pack.
#
# Pour chaque pack <slug> de TemplateRessources/Content/Ground :
#   - importe BaseColor / Normal / Roughness (2K JPG) sous /Game/City/Surfaces/<slug>/
#     (Normal = compression NormalMap + sRGB off, Roughness = Masks + sRGB off) ;
#   - cree M_Surf_<slug> : DefaultLit, BaseColor/Normal/Roughness cables, UV =
#     TexCoord(0) mis a l'echelle par 1 / taille physique du scan (lue au JSON du
#     pack, ex. « 2x2 » = 2 m). Le generateur C++ ecrit des UV0 EN METRES : la
#     division par la taille physique redonne la bonne echelle reelle.
#   - pose le flag d'usage Nanite (les meshes de sol desktop sont Nanite : sans le
#     flag, -game rend le fallback decime en Default Material).
#
# Piege paye (25/07, fix_nanite_usage.py) : set_editor_property ne marque PAS le
# package dirty -> save_asset(only_if_is_dirty=True par defaut) retourne True SANS
# rien reecrire. Ici : recompile + save force + relecture de verification.
#
# Piege paye (J2c) : une session -nullrhi cree des materiaux SANS shaders. Ce script
# doit tourner dans une session RHI REELLE :
#   UnrealEditor.exe <CityLab.uproject> -ExecCmds="py <chemin/en/slashes>" -stdout
# (chemin SANS espace : « Unreal Projects » casse le parsing de -ExecCmds — copier
# le script dans un dossier sans espace pour l'executer).
import json
import os
import time

import unreal

SURFACES_ROOT = '/Game/City/Surfaces'
# Packs retenus en v1. sidewalk_trim est importe pour plus tard mais n'a pas de
# materiau (il est a opacite : il lui faut un materiau masque, hors perimetre v1).
PACKS = [
    'asphalt_road_tiggcjdo',
    'fine_road_vgdlejpew',
    'fine_road_viciaalew',
    'marked_rough_road_vh1lbhqs',
    'cobblestone_thjldijbw',
    'herringbone_brick_pavement_ue3gbepkw',
    'dirty_sidewalk_tiles_ugxjcdpn',
    'grass_cut_pjxmz0',
    'uncut_grass_oilpt20',
    'wild_grass_sfknaeoa',
    'gravel_on_soil_okosdmp0',
    'pedestrian_crossing_lines_veggecd',
]
MAPS = ('BaseColor', 'Normal', 'Roughness')
QUIT_AFTER_S = 90.0
# v2 — HARMONISATION. Verdict utilisateur sur le proto v1 : « les revetements se
# rencontrent SANS HARMONIE ». Les scans viennent de lieux et d'eclairages
# differents : cote a cote, chaque frontiere saute aux yeux. Chaque materiau MINERAL
# recoit donc une constante de teinte qui le rapproche PARTIELLEMENT d'une clarte
# commune, calculee comme la moyenne des luminances des scans — donnee lue au JSON
# du pack, aucune valeur inventee. Force partielle : les scans gardent leur identite.
# Les herbes ne sont PAS harmonisees (les tirer vers le beige les grisaillerait).
# Bornes ASYMETRIQUES : les scans les plus sombres avaient besoin d'un fort coup de
# clair. On autorise a ECLAIRCIR beaucoup (x2,4) et a assombrir modestement (x0,7).
#
# v3 — LA CIBLE EST RECALCULEE SUR LA PALETTE REDUITE. Deux scans etaient TOXIQUES
# pour l'harmonie et sont sortis de la palette C++ (SurfaceClassForRoad) :
# cobblestone (rend bleu-nuit) et marked_rough_road (0,0152 de luminance, reste noir
# meme colle a sa borne x2,40). Les laisser dans la moyenne tirait la cible vers le
# bas (0,1030 en v2) et sous-eclaircissait toute la voirie.
#
# v4 — LA DALLE ENTRE DANS LA PALETTE, LE PAVE EN SORT. Verdict DA v3 : « grand
# puzzle » — le coupable n'etait pas la palette (deja a 3 classes) mais LE FOND : la
# dalle urbaine etait restee a la teinte unie de J2. Elle prend donc la matiere
# minerale la plus NEUTRE (dirty_sidewalk_tiles), et comme les voies pietonnes ne
# produisent plus aucun ruban, herringbone n'est plus reference nulle part.
# Cible v4 = moyenne des luminances des 5 scans actifs = 0,1166.
HARMONISE_STRENGTH = 0.55
# Bornes du tirage PARTIEL vers la cible commune — inchangees depuis la v2 : c'est
# ce plancher qui tient le gravier a x0,70 exactement (sa chaleur distingue les
# parcs, elle ne doit pas bouger d'un cheveu entre deux versions).
HARMONISE_MIN, HARMONISE_MAX = 0.70, 2.40
# Bornes ELARGIES reservees a la cible chaussee de la v4b, qui vise plus bas que la
# cible commune et pourrait donc demander a assombrir plus fort.
ROAD_MIN, ROAD_MAX = 0.50, 2.60
GRASS_PACKS = ('grass_cut_pjxmz0', 'uncut_grass_oilpt20', 'wild_grass_sfknaeoa')
# v5 « voirie » — packs laisses INTACTS (multiplicateur 1,0). Le passage pieton porte
# un marquage PEINT : l'eclaircir ou l'assombrir pour le rapprocher de la clarte
# commune reviendrait a repeindre les bandes. Il se lit par son motif, pas par sa
# clarte moyenne, et il est pose SUR la chaussee (donc deja en contraste voulu).
NEUTRAL_PACKS = ('pedestrian_crossing_lines_veggecd',)
# v4b — LE SOL DE REFERENCE ET CE QUI DOIT S'EN DETACHER.
SLAB_PACK = 'dirty_sidewalk_tiles_ugxjcdpn'
ROAD_PACKS = ('asphalt_road_tiggcjdo', 'fine_road_vgdlejpew', 'fine_road_viciaalew')
# Une chaussee est plus SOMBRE que le trottoir qui la borde : 0,72 de la clarte de
# la dalle, soit un ratio dalle/chaussee de ~1,39. C'est ce contraste qui rend la
# rue lisible depuis le ciel (la v4, a 3,5 % d'ecart, ne la montrait plus).
ROAD_DARKER = 0.72
# v5 « voirie » — LA BORDURE. Pack DERIVE : pas de scan a elle, elle reutilise les
# textures du pack de dalle sous un slug propre (donc son propre M_Surf_curb, son
# propre slot de materiau et sa propre teinte). x0,92 sur la clarte harmonisee de la
# dalle : assez pour que l'arete du trottoir se lise a contre-jour, pas assez pour
# dessiner un liseré noir vu du ciel.
CURB_SLUG = 'curb'
CURB_SOURCE_PACK = 'dirty_sidewalk_tiles_ugxjcdpn'
CURB_DARKER = 0.92
# Scans MINERAUX effectivement references par le builder sols v4 : la dalle porteuse
# + les seuls rubans qui restent (chaussees auto, allees de gravier). Seuls ceux-la
# definissent la cible de clarte ; les packs hors palette restent importes (assets
# conserves) et recoivent la meme formule, mais ne pesent plus sur la cible.
ACTIVE_MINERAL = (
    'dirty_sidewalk_tiles_ugxjcdpn',   # LA DALLE, fond de toute la ville
    'asphalt_road_tiggcjdo',
    'fine_road_vgdlejpew',
    'fine_road_viciaalew',
    'gravel_on_soil_okosdmp0',
)

E = unreal.EditorAssetLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary


def log(msg):
    unreal.log_warning('SURF: ' + msg)


def ground_root():
    """<...>/TemplateRessources/Content/Ground, resolu depuis le projet (pas __file__ :
    le script est execute depuis une copie dans un dossier sans espace)."""
    proj = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
    root = os.path.normpath(os.path.join(
        proj, '..', 'TemplateRessources', 'Content', 'Ground'))
    return root


def physical_size(pack_dir):
    """Taille physique du scan en metres, lue au JSON du pack (« 2x2 », « 4x0.5 »)."""
    for name in os.listdir(pack_dir):
        if not name.endswith('.json'):
            continue
        with open(os.path.join(pack_dir, name), encoding='utf-8') as f:
            meta = json.load(f)
        for m in meta.get('maps', []):
            ps = m.get('physicalSize')
            if ps and 'x' in ps:
                a, b = ps.split('x')[:2]
                return float(a), float(b)
    return 2.0, 2.0


def srgb_to_linear(byte_value):
    c = byte_value / 255.0
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def base_average_linear(pack_dir):
    """Couleur moyenne LINEAIRE de la BaseColor du scan (champ averageColor du JSON
    du pack). None si le champ manque : le pack n'est alors pas harmonise."""
    for name in os.listdir(pack_dir):
        if not name.endswith('.json'):
            continue
        with open(os.path.join(pack_dir, name), encoding='utf-8') as f:
            meta = json.load(f)
        for m in meta.get('maps', []):
            if m.get('type') != 'basecolor':
                continue
            hexa = (m.get('averageColor') or '').lstrip('#')
            if len(hexa) == 6:
                return tuple(srgb_to_linear(int(hexa[i:i + 2], 16)) for i in (0, 2, 4))
    return None


def luminance(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def harmonise_tints(root):
    """Multiplicateur par pack : un SCALAIRE applique aux trois canaux,
    1 + force * (luminance cible / luminance du scan - 1), borne.

    Harmonisation en LUMINANCE SEULE, pas canal par canal. Mesure faite sur le
    proto v2 : une cible RGB (moyenne des couleurs moyennes) est tiree vers le
    chaud par les paves et le gravier — l'asphalte, quasi neutre, y gagnait +24 %
    de rouge pour -2 % de bleu, et la rue virait au sable. Le vrai defaut
    d'harmonie est un ecart de CLARTE (facteur 17 entre le plus sombre et le plus
    clair des scans), pas de teinte : chacun garde donc sa couleur propre.

    v3 : la cible ne compte que les scans ACTIFS (ACTIVE_MINERAL). Les packs sortis
    de la palette restent importes et teintes, mais n'influencent plus personne.

    v4b : DEUX CIBLES, PLUS UNE SEULE. La v4 avait tout tire vers une clarte commune
    — resultat mesure sur le zenith : asphalte harmonise 0,1025 contre dalle 0,0990,
    soit 3,5 % d'ecart, et la chaussee ne se lisait plus (la ville lisait comme une
    esplanade continue). L'harmonie ne doit pas effacer la LECTURE. Donc :
      - la DALLE garde exactement son multiplicateur de la v4 (tire a force 0,55 vers
        la moyenne des actifs) : c'est elle, le sol de reference ;
      - les CHAUSSEES visent DELIBEREMENT 0,72 x la clarte harmonisee de la dalle
        (28 % plus sombre, ratio dalle/chaussee ~1,39) et l'atteignent PLEINEMENT —
        une cible voulue ne se tire pas a force partielle, sinon on retombe dans la
        bouillie qu'on vient de corriger ;
      - le GRAVIER est inchange (x0,70) : sa chaleur est ce qui distingue les parcs."""
    avgs = {}
    for slug in PACKS:
        pack_dir = os.path.join(root, slug)
        if os.path.isdir(pack_dir):
            avg = base_average_linear(pack_dir)
            if avg:
                avgs[slug] = avg
    active = [v for k, v in avgs.items() if k in ACTIVE_MINERAL]
    if not active:
        return {}
    target = sum(luminance(v) for v in active) / len(active)

    def partial(lum):
        """Tirage PARTIEL vers la cible commune (regle historique v2/v3/v4)."""
        return min(HARMONISE_MAX, max(HARMONISE_MIN,
                   1.0 + HARMONISE_STRENGTH * (target / max(lum, 1e-4) - 1.0)))

    # Clarte de reference = celle de la dalle une fois harmonisee. Tout se lit par
    # rapport a elle.
    slab_lum = luminance(avgs[SLAB_PACK])
    slab_k = partial(slab_lum)
    slab_out = slab_lum * slab_k
    road_target = ROAD_DARKER * slab_out

    out = {}
    # v5 : la BORDURE est la dalle assombrie. Un materiau derive (M_Surf_curb, meme
    # textures, meme echelle physique) plutot qu'une teinte de sommet : les M_Surf_*
    # ne lisent PAS la VertexColor (BaseColor = scan x constante d'harmonisation).
    # x0,92 = juste ce qu'il faut pour que l'arete se lise sans devenir un trait noir.
    out[CURB_SLUG] = (slab_k * CURB_DARKER,) * 3
    for slug, avg in avgs.items():
        if slug in GRASS_PACKS or slug in NEUTRAL_PACKS:
            out[slug] = (1.0, 1.0, 1.0)
            continue
        lum = luminance(avg)
        if slug in ROAD_PACKS:
            # Cible VOULUE, atteinte en plein (pas de force partielle).
            k, why = min(ROAD_MAX, max(ROAD_MIN, road_target / max(lum, 1e-4))), 'chaussee'
        elif slug == SLAB_PACK:
            k, why = slab_k, 'DALLE (reference)'
        else:
            k, why = partial(lum), '' if slug in ACTIVE_MINERAL else 'hors palette'
        out[slug] = (k, k, k)
        log('  %-40s lum %.4f -> x%.4f = %.4f%s'
            % (slug, lum, k, lum * k, ('  (%s)' % why) if why else ''))
    log('dalle harmonisee %.4f | cible chaussees %.4f (x%.2f) | ratio dalle/chaussee %.2f'
        % (slab_out, road_target, ROAD_DARKER, slab_out / max(road_target, 1e-4)))
    log('  %-40s lum %.4f -> x%.4f = %.4f  (BORDURE, dalle x%.2f)'
        % (CURB_SLUG, slab_lum, out[CURB_SLUG][0], slab_lum * out[CURB_SLUG][0], CURB_DARKER))
    log('cible commune %.4f sur %d scans ACTIFS (sert encore a la dalle et au gravier)'
        % (target, len(active)))
    return out


def find_map(pack_dir, kind):
    for name in sorted(os.listdir(pack_dir)):
        if name.lower().endswith('_2k_%s.jpg' % kind.lower()):
            return os.path.join(pack_dir, name)
    return None


def import_texture(src, folder, asset_name):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', src)
    task.set_editor_property('destination_path', folder)
    task.set_editor_property('destination_name', asset_name)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', False)
    AT.import_asset_tasks([task])
    return E.load_asset('%s/%s' % (folder, asset_name))


def configure_texture(tex, kind):
    """Reglages d'echantillonnage. Normal et Roughness sont des donnees LINEAIRES :
    les laisser en sRGB delave le relief et fausse la rugosite."""
    if kind == 'Normal':
        tex.set_editor_property('srgb', False)
        tex.set_editor_property(
            'compression_settings', unreal.TextureCompressionSettings.TC_NORMALMAP)
        tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD_NORMAL_MAP)
    elif kind == 'Roughness':
        tex.set_editor_property('srgb', False)
        tex.set_editor_property(
            'compression_settings', unreal.TextureCompressionSettings.TC_MASKS)
        tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD)
    else:
        tex.set_editor_property('srgb', True)
        tex.set_editor_property(
            'compression_settings', unreal.TextureCompressionSettings.TC_DEFAULT)
        tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_WRAP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_WRAP)


def build_material(slug, folder, textures, size_x, size_y, tint):
    path = '%s/M_Surf_%s' % (folder, slug)
    mat = E.load_asset(path)
    if mat is None:
        mat = AT.create_asset('M_Surf_%s' % slug, folder, unreal.Material,
                              unreal.MaterialFactoryNew())
    if mat is None:
        raise RuntimeError('creation de %s impossible' % path)
    # Regeneration en place : on repart d'un graphe vide (idempotence).
    MEL.delete_all_material_expressions(mat)
    # Domaine et modele d'ombrage : ce sont deja les defauts d'un materiau neuf —
    # on les pose quand meme (regeneration en place), sans faire echouer le pack si
    # l'API bouge.
    for prop, value in (('material_domain', unreal.MaterialDomain.MD_SURFACE),
                        ('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)):
        try:
            mat.set_editor_property(prop, value)
        except Exception as e:  # noqa: BLE001
            log('  (%s non pose : %s)' % (prop, e))
    # Flag d'usage Nanite : les cellules de sol desktop sont Nanite.
    mat.set_editor_property('used_with_nanite', True)

    # UV0 en METRES / taille physique du scan = UV en tuiles du scan.
    tc = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate, -700, 0)
    tc.set_editor_property('u_tiling', 1.0 / size_x)
    tc.set_editor_property('v_tiling', 1.0 / size_y)

    plan = [
        ('BaseColor', unreal.MaterialSamplerType.SAMPLERTYPE_COLOR,
         unreal.MaterialProperty.MP_BASE_COLOR, '', -250),
        ('Normal', unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL,
         unreal.MaterialProperty.MP_NORMAL, '', 50),
        ('Roughness', unreal.MaterialSamplerType.SAMPLERTYPE_MASKS,
         unreal.MaterialProperty.MP_ROUGHNESS, 'R', 350),
    ]
    wired = []
    for kind, sampler, prop, out_pin, y in plan:
        tex = textures.get(kind)
        if tex is None:
            continue
        s = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSample, -400, y)
        s.set_editor_property('texture', tex)
        s.set_editor_property('sampler_type', sampler)
        MEL.connect_material_expressions(tc, '', s, 'UVs')
        if kind == 'BaseColor' and tint and tint != (1.0, 1.0, 1.0):
            # v2 : constante d'harmonisation entre le scan et la sortie BaseColor.
            k = MEL.create_material_expression(
                mat, unreal.MaterialExpressionConstant3Vector, -400, -420)
            k.set_editor_property('constant', unreal.LinearColor(tint[0], tint[1], tint[2], 1.0))
            mul = MEL.create_material_expression(
                mat, unreal.MaterialExpressionMultiply, -180, -300)
            MEL.connect_material_expressions(s, '', mul, 'A')
            MEL.connect_material_expressions(k, '', mul, 'B')
            MEL.connect_material_property(mul, '', prop)
        else:
            MEL.connect_material_property(s, out_pin, prop)
        wired.append(kind)
    MEL.recompile_material(mat)
    saved = E.save_asset(path, only_if_is_dirty=False)
    nanite = E.load_asset(path).get_editor_property('used_with_nanite')
    return wired, saved, nanite


def run():
    root = ground_root()
    log('packs lus dans %s' % root)
    tints = harmonise_tints(root)
    n_tex, n_mat, problems = 0, 0, []
    # v5 : textures du pack de dalle memorisees au passage, pour en deriver la bordure.
    curb_src = {'textures': None, 'size': (2.0, 2.0)}
    for slug in PACKS:
        pack_dir = os.path.join(root, slug)
        if not os.path.isdir(pack_dir):
            problems.append('%s : dossier absent' % slug)
            continue
        folder = '%s/%s' % (SURFACES_ROOT, slug)
        if not E.does_directory_exist(folder):
            E.make_directory(folder)
        size_x, size_y = physical_size(pack_dir)
        textures = {}
        for kind in MAPS:
            src = find_map(pack_dir, kind)
            if src is None:
                problems.append('%s : carte %s absente' % (slug, kind))
                continue
            name = 'T_%s_%s' % (slug, kind)
            tex = import_texture(src, folder, name)
            if tex is None:
                problems.append('%s : import %s echoue' % (slug, kind))
                continue
            configure_texture(tex, kind)
            E.save_asset('%s/%s' % (folder, name), only_if_is_dirty=False)
            textures[kind] = tex
            n_tex += 1
        if 'BaseColor' not in textures:
            problems.append('%s : pas de BaseColor, materiau non cree' % slug)
            continue
        tint = tints.get(slug, (1.0, 1.0, 1.0))
        wired, saved, nanite = build_material(slug, folder, textures, size_x, size_y, tint)
        n_mat += 1
        log('%s : scan %gx%g m, cartes %s, harmonisation %.2f/%.2f/%.2f, saved=%s nanite=%s'
            % (slug, size_x, size_y, '+'.join(wired), tint[0], tint[1], tint[2], saved, nanite))
        if slug == CURB_SOURCE_PACK:
            curb_src['textures'] = dict(textures)
            curb_src['size'] = (size_x, size_y)

    # --- v5 : PACK DERIVE « curb ». Aucune texture importee de plus : le materiau
    # reference celles de la dalle, sous son propre dossier et son propre slug (donc
    # son propre slot de materiau cote mesh). Meme echelle physique : le motif de la
    # bordure est en phase avec celui du trottoir qu'elle borde.
    if curb_src['textures']:
        folder = '%s/%s' % (SURFACES_ROOT, CURB_SLUG)
        if not E.does_directory_exist(folder):
            E.make_directory(folder)
        tint = tints.get(CURB_SLUG, (1.0, 1.0, 1.0))
        wired, saved, nanite = build_material(
            CURB_SLUG, folder, curb_src['textures'],
            curb_src['size'][0], curb_src['size'][1], tint)
        n_mat += 1
        log('%s (derive de %s) : scan %gx%g m, cartes %s, teinte %.3f, saved=%s nanite=%s'
            % (CURB_SLUG, CURB_SOURCE_PACK, curb_src['size'][0], curb_src['size'][1],
               '+'.join(wired), tint[0], saved, nanite))
    else:
        problems.append('%s absent : materiau de bordure NON cree' % CURB_SOURCE_PACK)

    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    log('TERMINE : %d textures, %d materiaux M_Surf_*' % (n_tex, n_mat))
    for p in problems:
        log('ANOMALIE ' + p)


def run_then_quit():
    """Mode SCRIPT : importe, teinte, puis ferme l'editeur — en laissant aux shaders
    des nouveaux materiaux le temps de compiler (un QUIT_EDITOR immediat les laisse
    en attente)."""
    run()
    state = {'t0': time.time(), 'handle': None, 'busy': False}

    def _tick(_dt):
        if state['busy']:
            return
        state['busy'] = True
        try:
            if time.time() - state['t0'] < QUIT_AFTER_S:
                return
            if state['handle'] is not None:
                unreal.unregister_slate_post_tick_callback(state['handle'])
                state['handle'] = None
            log('fermeture de l editeur')
            world = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem).get_editor_world()
            unreal.SystemLibrary.execute_console_command(world, 'QUIT_EDITOR')
        finally:
            state['busy'] = False

    state['handle'] = unreal.register_slate_post_tick_callback(_tick)
    log('fermeture armee dans %.0f s' % QUIT_AFTER_S)


# Execute en SCRIPT (bat dedie, `py import_surfaces.py`) : tout s'enchaine, editeur
# ferme a la fin. Execute en BIBLIOTHEQUE (exec avec un __name__ different — cf. la
# SESSION UNIQUE de la v4 qui enchaine materiaux + proto + captures) : rien ne se
# declenche tout seul et surtout l'editeur ne se ferme pas au milieu ; l'appelant
# appelle run() lui-meme. Defaut = mode script, pour qu'un interpreteur qui ne
# definirait pas __name__ garde l'ancien comportement.
if globals().get('__name__', '__main__') == '__main__':
    run_then_quit()
