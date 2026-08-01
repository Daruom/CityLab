# J3c « MAQUETTE DU SOL » — le materiau qui PEINT la dalle.
#
# Entree : les masques cuits par Tools/j3c_sols_masks.py (SourceData/Sols/).
# Sortie, dans une session editeur RHI REELLE :
#   - T_GroundMask_<cx>_<cy>  : le masque de la cellule, importe SANS compression
#     ni sRGB (les canaux sont de la DONNEE : classe + trois champs de distance) ;
#   - M_CityGroundMasked      : le master. Il echantillonne le masque et melange
#     quatre revetements Megascans en UV MONDE metriques, avec les memes
#     multiplicateurs d'harmonisation v4b que les M_Surf_* (importes de
#     import_surfaces.harmonise_tints — aucune valeur reinventee ici) ;
#   - MI_CityGround_<cx>_<cy> : une instance par cellule (son masque, son origine) ;
#   - M_Surf_marking          : la peinture blanche des tirets axiaux, sous le meme
#     nommage que les autres revetements pour entrer dans FSurfaceLibrary cote C++.
#
# LA FRONTIERE EST NETTE PARCE QU'ELLE EST UN CHAMP DE DISTANCE, pas un bord de
# texel : le noeud Custom convertit le SDF en metres puis en poids sur une largeur
# de transition egale au FOOTPRINT du pixel a l'ecran (ddx/ddy). Au ras du sol la
# bordure est franche, a 300 m elle est antialiasee — sans mipmap floue ni
# escalier. EdgeMinM est le plancher : si les derivees ne disent rien, la
# transition retombe sur une valeur fixe et ca marche quand meme.
#
# SOLVERT (2026-07-30) — 5e COUCHE : L'HERBE (canal R du masque, SDF cuit par
# j3c_sols_masks depuis OCS GE CS2.*). Les films verts SM_Surface_* disparaissent.
#   - revetement : ground_grass_tb3nce2k (scan 2 x 2 m — l'echelle est imposee par
#     physical_size du pack : la recette ne PERMET PAS de se tromper de tiling) ;
#   - bord ORGANIQUE : la distance d'herbe est deplacee par 2 octaves d'un bruit
#     tuilable (T_SolNoise512, cuit par work/SOLVERT/gen_noise512.py) — ondulation
#     ~3 m d'amplitude +-27 cm + meandre ~16 m d'amplitude +-60 cm : la corde de
#     9 m de la donnee disparait sous un bord vivant ;
#   - bande d'USURE : dried_grass_pjwfo0 melange sur ~80 cm autour de la frontiere
#     (le raccord herbe/pave se lit comme une transition, pas comme une decoupe) ;
#   - MACRO-VARIATION de teinte a vraie grande periode (~30 m de motif dominant,
#     amplitude 0,80..1,25) : casse la repetition la ou l'oeil la voit (5-30 m).
#   - ordre du melange : slab -> HERBE -> grav -> priv -> road (l'herbe passe sur
#     la dalle, la voirie passe sur l'herbe : le peint gagne toujours).
#
# Pieges payes, respectes ici :
#   - session -nullrhi = materiaux SANS shaders -> ce script tourne en RHI reel ;
#   - set_editor_property ne salit pas le package -> recompile + save FORCE ;
#   - flag used_with_nanite obligatoire (la dalle desktop est Nanite).
import json
import os

import unreal

SURFACES_ROOT = '/Game/City/Surfaces'
GROUND_ROOT = '/Game/City/Ground'
MASTER_PATH = GROUND_ROOT + '/M_CityGroundMasked'
MARKING_SLUG = 'marking'

# Classe de sol -> pack Megascans. Les quatre packs sont deja importes par
# Tools/import_surfaces.py (textures sous /Game/City/Surfaces/<slug>/).
#   dalle    : le fond de ville v4b, inchange — trottoirs, places, parvis, cours.
#   chaussee : l'asphalte nu. Les scans a ligne axiale PEINTE (fine_road_*) ne
#              servent plus a rien : la ligne axiale est devenue de la geometrie,
#              posee la ou la donnee dit qu'il y en a une, et pas ailleurs.
#   privee   : la voirie tombee DANS une parcelle (cour, allee de residence,
#              parking prive). Elle doit se LIRE autrement que la rue publique,
#              sinon la distinction cadastrale ne sert a rien — d'ou le pave.
#   gravier  : chemins et allees de parc (natures etroites BD TOPO).
CLASSES = [
    ('slab', 'dirty_sidewalk_tiles_ugxjcdpn'),
    ('road', 'asphalt_road_tiggcjdo'),
    ('priv', 'herringbone_brick_pavement_ue3gbepkw'),
    ('grav', 'gravel_on_soil_okosdmp0'),
]
# SOLVERT : la 5e couche et sa bande d'usure (importees par import_surfaces comme
# les autres packs ; herbes NON harmonisees — teinte x1).
# CORRECTION mesuree (2026-07-30) : ground_grass_tb3nce2k, retenu par le rapport
# pour sa taille (2 x 2 m), est en realite un scan d'herbe SECHE beige (RGB moyen
# 159/142/110, R > G). Le 2 x 2 m VERT de la bibliotheque est uncut_grass_oilpt20
# (84/96/42) — verifie sur planche comparative work/SOLVERT/_compare_grass.png.
GRASS_SLUG = 'uncut_grass_oilpt20'
WEAR_SLUG = 'dried_grass_pjwfo0'
MAPS = ('BaseColor', 'Normal', 'Roughness')

# Doit valoir SDF_RANGE_M de j3c_sols_masks.py — la seule constante partagee entre
# le cuiseur et le shader, ecrite en toutes lettres des deux cotes.
SDF_RANGE_M = 2.0
CELL_SIZE_M = 500.0
# Transition minimale de la frontiere, en metres. 4 cm : au ras du sol on voit une
# arete franche, jamais un escalier de texels.
EDGE_MIN_M = 0.04

# SOLVERT — bord d'herbe organique et macro-variation. Les periodes sont celles de
# la TUILE du bruit (le motif dominant du bruit fait ~1/3 de tuile, cf. gen_noise512).
NOISE_NAME = 'T_SolNoise512'
NOISE1_M = 9.0        # octave fine : ondulation ~3 m ...
# --- V4 : LE MEANDRE EST COUPE (amplitudes a zero) --------------------------
# Le bruit de bord date des bords ORGANIQUES (SOLVERT, 30/07) : il servait a
# cacher la corde de 9 m des polygones OCS GE. Depuis, deux choses ont change :
# la v3 REGULARISE le contour a la regle (Douglas-Peucker 1,25 m) et surtout elle
# POSE UNE PIERRE dessus. Or le bruit deplacait la frontiere PEINTE de +-87 cm
# (27 + 60) autour de sa propre pierre : c'est la cause premiere du grief
# utilisateur « peinture et pierre desalignees », et ca ne pouvait pas se corriger
# en rapprochant les deux — le bruit est plus large que la pierre elle-meme.
# Partout ailleurs le contour meurt sur un objet DROIT (facade, bordure de
# chaussee) ou sur une berge : une frontiere droite y est la bonne reponse.
# Les trois echantillons de bruit restent cables (N3 sert toujours a la
# macro-teinte) et les amplitudes sont deux constantes : remettre 0,55 / 1,20
# restitue le bord organique en une ligne.
NOISE1_AMP = 0.0      # (etait 0,55 = +-27 cm)
NOISE2_M = 48.0       # meandre ~16 m ...
NOISE2_AMP = 0.0      # (etait 1,20 = +-60 cm)
MACRO_M = 90.0        # macro-teinte : motif dominant ~30 m
MACRO_MIN = 0.80
MACRO_MAX = 1.25
WEAR_HALF_M = 0.8     # demi-largeur de la bande d'usure dried_grass

E = unreal.EditorAssetLibrary
AT = unreal.AssetToolsHelpers.get_asset_tools()
MEL = unreal.MaterialEditingLibrary


def log(msg):
    unreal.log_warning('MASK: ' + msg)


def masks_dir():
    proj = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
    return os.path.normpath(os.path.join(proj, 'SourceData', 'Sols'))


def tools_dir():
    proj = unreal.Paths.convert_relative_path_to_full(unreal.Paths.project_dir())
    return os.path.normpath(os.path.join(proj, 'Tools'))


def load_surfaces_lib():
    """import_surfaces.py charge EN BIBLIOTHEQUE : on ne veut que ses fonctions
    (harmonise_tints, physical_size, ground_root), surtout pas son mode script qui
    fermerait l'editeur."""
    path = os.path.join(tools_dir(), 'import_surfaces.py')
    with open(path, encoding='utf-8') as f:
        src = f.read()
    ns = {'__name__': 'import_surfaces_lib', '__file__': path}
    exec(compile(src, path, 'exec'), ns)  # noqa: S102
    return ns


def import_mask_texture(png_path, name):
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', png_path)
    task.set_editor_property('destination_path', GROUND_ROOT)
    task.set_editor_property('destination_name', name)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', False)
    AT.import_asset_tasks([task])
    tex = E.load_asset('%s/%s' % (GROUND_ROOT, name))
    if tex is None:
        return None
    # DE LA DONNEE, PAS UNE IMAGE : sans sRGB off et sans compression, le canal de
    # classe et les trois champs de distance seraient reencodes en gamma puis
    # ecrases par des blocs DXT — la frontiere de chaussee se mettrait a onduler.
    tex.set_editor_property('srgb', False)
    tex.set_editor_property('compression_settings',
                            unreal.TextureCompressionSettings.TC_VECTOR_DISPLACEMENTMAP)
    tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_CLAMP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_CLAMP)
    E.save_asset('%s/%s' % (GROUND_ROOT, name), only_if_is_dirty=False)
    return tex


def import_noise_texture(png_path):
    """T_SolNoise512 : bruit tuilable LINEAIRE (sRGB off, wrap). Une seule texture
    512x512 pour les 2 octaves de bord ET la macro-teinte (3 echantillons a 3
    echelles differentes)."""
    task = unreal.AssetImportTask()
    task.set_editor_property('filename', png_path)
    task.set_editor_property('destination_path', GROUND_ROOT)
    task.set_editor_property('destination_name', NOISE_NAME)
    task.set_editor_property('automated', True)
    task.set_editor_property('replace_existing', True)
    task.set_editor_property('save', False)
    AT.import_asset_tasks([task])
    tex = E.load_asset('%s/%s' % (GROUND_ROOT, NOISE_NAME))
    if tex is None:
        return None
    tex.set_editor_property('srgb', False)
    tex.set_editor_property('compression_settings',
                            unreal.TextureCompressionSettings.TC_DEFAULT)
    tex.set_editor_property('lod_group', unreal.TextureGroup.TEXTUREGROUP_WORLD)
    tex.set_editor_property('address_x', unreal.TextureAddress.TA_WRAP)
    tex.set_editor_property('address_y', unreal.TextureAddress.TA_WRAP)
    try:
        tex.set_editor_property('virtual_texture_streaming', False)
    except Exception:
        pass
    E.save_asset('%s/%s' % (GROUND_ROOT, NOISE_NAME), only_if_is_dirty=False)
    return tex


def sample(mat, tex, uvs, x, y, sampler, shared=True):
    s = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureSample, x, y)
    s.set_editor_property('texture', tex)
    s.set_editor_property('sampler_type', sampler)
    if shared:
        # Sampler PARTAGE : douze textures de revetement + le masque depasseraient
        # les 16 emplacements d'un materiau. Les revetements se contentent du
        # sampler global « Wrap ».
        try:
            s.set_editor_property('sampler_source',
                                  unreal.SamplerSourceMode.SSM_WRAP_WORLD_GROUP_SETTINGS)
        except Exception as e:  # noqa: BLE001
            log('  (sampler partage refuse : %s)' % e)
    MEL.connect_material_expressions(uvs, '', s, 'UVs')
    return s


def build_master(textures, sizes, tints):
    """Le master. UV0 du mesh = METRES MONDE (le generateur les ecrit ainsi) :
    chaque revetement divise par sa taille physique, le masque par 500 m apres
    soustraction de l'origine de la cellule."""
    mat = E.load_asset(MASTER_PATH)
    if mat is None:
        mat = AT.create_asset('M_CityGroundMasked', GROUND_ROOT, unreal.Material,
                              unreal.MaterialFactoryNew())
    if mat is None:
        raise RuntimeError('creation de %s impossible' % MASTER_PATH)
    MEL.delete_all_material_expressions(mat)
    mat.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property('used_with_nanite', True)

    uv0 = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate,
                                         -2400, 0)

    # --- UV du masque : (UV0 metres - origine de la cellule) / 500 m
    origin = MEL.create_material_expression(mat, unreal.MaterialExpressionVectorParameter,
                                            -2400, -400)
    origin.set_editor_property('parameter_name', 'CellOriginM')
    origin.set_editor_property('default_value', unreal.LinearColor(0.0, 0.0, 0.0, 0.0))
    org_xy = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask,
                                            -2150, -400)
    org_xy.set_editor_property('r', True)
    org_xy.set_editor_property('g', True)
    org_xy.set_editor_property('b', False)
    org_xy.set_editor_property('a', False)
    MEL.connect_material_expressions(origin, '', org_xy, '')
    sub = MEL.create_material_expression(mat, unreal.MaterialExpressionSubtract, -1950, -200)
    MEL.connect_material_expressions(uv0, '', sub, 'A')
    MEL.connect_material_expressions(org_xy, '', sub, 'B')
    cell = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter,
                                          -1950, -60)
    cell.set_editor_property('parameter_name', 'CellSizeM')
    cell.set_editor_property('default_value', CELL_SIZE_M)
    mask_uv = MEL.create_material_expression(mat, unreal.MaterialExpressionDivide, -1750, -200)
    MEL.connect_material_expressions(sub, '', mask_uv, 'A')
    MEL.connect_material_expressions(cell, '', mask_uv, 'B')

    mask = MEL.create_material_expression(
        mat, unreal.MaterialExpressionTextureSampleParameter2D, -1550, -300)
    mask.set_editor_property('parameter_name', 'GroundMask')
    mask.set_editor_property('texture', textures['mask'])
    mask.set_editor_property('sampler_type', unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)
    MEL.connect_material_expressions(mask_uv, '', mask, 'UVs')

    # --- SOLVERT : 3 echantillons du bruit tuilable (2 octaves de bord + macro),
    #     en UV monde metriques comme les revetements.
    noise_nodes = {}
    for key, period, ny in (('N1', NOISE1_M, 300), ('N2', NOISE2_M, 500), ('N3', MACRO_M, 700)):
        ntc = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate,
                                             -1850, ny)
        ntc.set_editor_property('u_tiling', 1.0 / period)
        ntc.set_editor_property('v_tiling', 1.0 / period)
        noise_nodes[key] = sample(mat, textures['noise'], ntc, -1650, ny,
                                  unreal.MaterialSamplerType.SAMPLERTYPE_LINEAR_COLOR)

    # --- Poids des classes, depuis les CHAMPS DE DISTANCE (canaux R/G/B/A).
    #     R = herbe (SOLVERT), G = chaussee, B = voirie privee, A = gravier.
    #     La distance d'HERBE est deplacee par 2 octaves de bruit AVANT le calcul du
    #     poids : le bord vit, et fw (footprint ecran) antialiase aussi le bruit.
    edge = MEL.create_material_expression(mat, unreal.MaterialExpressionScalarParameter,
                                          -1550, 100)
    edge.set_editor_property('parameter_name', 'EdgeMinM')
    edge.set_editor_property('default_value', EDGE_MIN_M)
    weights = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -1250, -200)
    weights.set_editor_property('description', 'ClassWeightsFromSDF')
    weights.set_editor_property('output_type',
                                unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    weights.set_editor_property('code', (
        '// R/G/B/A = distances signees : herbe, chaussee, voirie privee, gravier.\n'
        'float4 sd = (float4(M.r, M.g, M.b, M.a) - 0.5) * %.1f;\n'
        '// bord d herbe ORGANIQUE : 2 octaves de bruit deplacent la frontiere.\n'
        'float dg = sd.x + (N1 - 0.5) * %.2f + (N2 - 0.5) * %.2f;\n'
        'float4 w = 0;\n'
        '{ float d = dg;   float fw = max(EdgeMinM, (abs(ddx(d)) + abs(ddy(d))) * 0.75);\n'
        '  w.x = saturate(d / fw + 0.5); }\n'
        '{ float d = sd.y; float fw = max(EdgeMinM, (abs(ddx(d)) + abs(ddy(d))) * 0.75);\n'
        '  w.y = saturate(d / fw + 0.5); }\n'
        '{ float d = sd.z; float fw = max(EdgeMinM, (abs(ddx(d)) + abs(ddy(d))) * 0.75);\n'
        '  w.z = saturate(d / fw + 0.5); }\n'
        '{ float d = sd.w; float fw = max(EdgeMinM, (abs(ddx(d)) + abs(ddy(d))) * 0.75);\n'
        '  w.w = saturate(d / fw + 0.5); }\n'
        'return w;' % (2.0 * SDF_RANGE_M, NOISE1_AMP, NOISE2_AMP)))
    ins = []
    for nom in ('M', 'EdgeMinM', 'N1', 'N2'):
        ci = unreal.CustomInput()
        ci.set_editor_property('input_name', nom)
        ins.append(ci)
    weights.set_editor_property('inputs', ins)
    MEL.connect_material_expressions(mask, 'RGBA', weights, 'M')
    MEL.connect_material_expressions(edge, '', weights, 'EdgeMinM')
    MEL.connect_material_expressions(noise_nodes['N1'], 'R', weights, 'N1')
    MEL.connect_material_expressions(noise_nodes['N2'], 'R', weights, 'N2')
    # w.x = herbe, w.y = chaussee, w.z = privee, w.w = gravier
    wg = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1050, -400)
    wx = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1050, -320)
    wy = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1050, -240)
    wz = MEL.create_material_expression(mat, unreal.MaterialExpressionComponentMask, -1050, -160)
    for m, (r, g, b, a) in ((wg, (True, False, False, False)), (wx, (False, True, False, False)),
                            (wy, (False, False, True, False)), (wz, (False, False, False, True))):
        m.set_editor_property('r', r)
        m.set_editor_property('g', g)
        m.set_editor_property('b', b)
        m.set_editor_property('a', a)
        MEL.connect_material_expressions(weights, '', m, '')

    # --- SOLVERT : bande d'usure a la frontiere d'herbe (meme distance bruitee).
    wear_t = MEL.create_material_expression(mat, unreal.MaterialExpressionCustom, -1250, 550)
    wear_t.set_editor_property('description', 'GrassWearBand')
    wear_t.set_editor_property('output_type', unreal.CustomMaterialOutputType.CMOT_FLOAT1)
    wear_t.set_editor_property('code', (
        'float dg = (M.r - 0.5) * %.1f + (N1 - 0.5) * %.2f + (N2 - 0.5) * %.2f;\n'
        'return saturate(1.0 - abs(dg) / %.2f);'
        % (2.0 * SDF_RANGE_M, NOISE1_AMP, NOISE2_AMP, WEAR_HALF_M)))
    ins_w = []
    for nom in ('M', 'N1', 'N2'):
        ci = unreal.CustomInput()
        ci.set_editor_property('input_name', nom)
        ins_w.append(ci)
    wear_t.set_editor_property('inputs', ins_w)
    MEL.connect_material_expressions(mask, 'RGBA', wear_t, 'M')
    MEL.connect_material_expressions(noise_nodes['N1'], 'R', wear_t, 'N1')
    MEL.connect_material_expressions(noise_nodes['N2'], 'R', wear_t, 'N2')

    # --- SOLVERT : macro-variation de teinte (0,80..1,25) a vraie grande periode.
    macro_lo = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -1250, 800)
    macro_lo.set_editor_property('r', MACRO_MIN)
    macro_hi = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -1250, 880)
    macro_hi.set_editor_property('r', MACRO_MAX)
    macro_mul = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate,
                                               -1050, 820)
    MEL.connect_material_expressions(macro_lo, '', macro_mul, 'A')
    MEL.connect_material_expressions(macro_hi, '', macro_mul, 'B')
    MEL.connect_material_expressions(noise_nodes['N3'], 'R', macro_mul, 'Alpha')

    # --- Un jeu d'echantillons par classe, en UV metriques a l'echelle du scan.
    outs = {}
    y = -900
    for key, slug in CLASSES:
        sx, sy = sizes[key]
        tc = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate,
                                            -900, y)
        tc.set_editor_property('u_tiling', 1.0 / sx)
        tc.set_editor_property('v_tiling', 1.0 / sy)
        node = {}
        for i, kind in enumerate(MAPS):
            tex = textures[key].get(kind)
            if tex is None:
                continue
            sampler = (unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if kind == 'Normal'
                       else unreal.MaterialSamplerType.SAMPLERTYPE_MASKS if kind == 'Roughness'
                       else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
            s = sample(mat, tex, tc, -650, y + i * 200, sampler)
            if kind == 'BaseColor':
                k = tints.get(slug, (1.0, 1.0, 1.0))
                if k != (1.0, 1.0, 1.0):
                    kc = MEL.create_material_expression(
                        mat, unreal.MaterialExpressionConstant3Vector, -650, y - 120)
                    kc.set_editor_property('constant', unreal.LinearColor(k[0], k[1], k[2], 1.0))
                    mul = MEL.create_material_expression(
                        mat, unreal.MaterialExpressionMultiply, -420, y)
                    MEL.connect_material_expressions(s, '', mul, 'A')
                    MEL.connect_material_expressions(kc, '', mul, 'B')
                    node[kind] = (mul, '')
                else:
                    node[kind] = (s, '')
            elif kind == 'Roughness':
                node[kind] = (s, 'R')
            else:
                node[kind] = (s, '')
        outs[key] = node
        y += 900

    # --- SOLVERT : la couche HERBE = scan 2 m module par la macro-teinte, fondu
    #     vers dried_grass dans la bande d'usure (meme geometrie que le poids).
    def grass_layer():
        sxg, syg = sizes['grass']
        tcg = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate,
                                             -900, y + 0)
        tcg.set_editor_property('u_tiling', 1.0 / sxg)
        tcg.set_editor_property('v_tiling', 1.0 / syg)
        sxw, syw = sizes['wear']
        tcw = MEL.create_material_expression(mat, unreal.MaterialExpressionTextureCoordinate,
                                             -900, y + 700)
        tcw.set_editor_property('u_tiling', 1.0 / sxw)
        tcw.set_editor_property('v_tiling', 1.0 / syw)
        node = {}
        for i, kind in enumerate(MAPS):
            gt = textures['grass'].get(kind)
            if gt is None:
                continue
            sampler = (unreal.MaterialSamplerType.SAMPLERTYPE_NORMAL if kind == 'Normal'
                       else unreal.MaterialSamplerType.SAMPLERTYPE_MASKS if kind == 'Roughness'
                       else unreal.MaterialSamplerType.SAMPLERTYPE_COLOR)
            sg = sample(mat, gt, tcg, -650, y + i * 200, sampler)
            pin_g = 'R' if kind == 'Roughness' else ''
            cur, pin = sg, pin_g
            if kind == 'BaseColor':
                mulm = MEL.create_material_expression(
                    mat, unreal.MaterialExpressionMultiply, -420, y + i * 200)
                MEL.connect_material_expressions(sg, '', mulm, 'A')
                MEL.connect_material_expressions(macro_mul, '', mulm, 'B')
                cur, pin = mulm, ''
            wt = textures['wear'].get(kind)
            if wt is not None:
                sw = sample(mat, wt, tcw, -650, y + 700 + i * 200, sampler)
                lpw = MEL.create_material_expression(
                    mat, unreal.MaterialExpressionLinearInterpolate, -250, y + i * 200)
                MEL.connect_material_expressions(cur, pin, lpw, 'A')
                MEL.connect_material_expressions(sw, 'R' if kind == 'Roughness' else '',
                                                 lpw, 'B')
                MEL.connect_material_expressions(wear_t, '', lpw, 'Alpha')
                cur, pin = lpw, ''
            node[kind] = (cur, pin)
        return node

    outs['grass'] = grass_layer()

    # --- Melange : dalle, puis HERBE, puis gravier, puis privee, puis chaussee
    #     (priorite du masque : le PEINT gagne toujours sur l'herbe).
    def blend(kind, prop, y0):
        base = outs['slab'].get(kind)
        if base is None:
            return False
        cur, pin = base
        for i, (key, w) in enumerate((('grass', wg), ('grav', wz), ('priv', wy),
                                      ('road', wx))):
            other = outs[key].get(kind)
            if other is None:
                continue
            lp = MEL.create_material_expression(mat, unreal.MaterialExpressionLinearInterpolate,
                                                -150 + i * 200, y0)
            MEL.connect_material_expressions(cur, pin, lp, 'A')
            MEL.connect_material_expressions(other[0], other[1], lp, 'B')
            MEL.connect_material_expressions(w, '', lp, 'Alpha')
            cur, pin = lp, ''
        MEL.connect_material_property(cur, pin, prop)
        return True

    wired = []
    if blend('BaseColor', unreal.MaterialProperty.MP_BASE_COLOR, -400):
        wired.append('BaseColor')
    if blend('Normal', unreal.MaterialProperty.MP_NORMAL, 100):
        wired.append('Normal')
    if blend('Roughness', unreal.MaterialProperty.MP_ROUGHNESS, 600):
        wired.append('Roughness')

    MEL.recompile_material(mat)
    saved = E.save_asset(MASTER_PATH, only_if_is_dirty=False)
    nanite = E.load_asset(MASTER_PATH).get_editor_property('used_with_nanite')
    log('master %s : %s cables, saved=%s nanite=%s'
        % (MASTER_PATH, '+'.join(wired), saved, nanite))
    return mat, wired


def build_marking(tints):
    """La peinture des tirets axiaux. Pas de scan : un blanc de marquage routier
    (~0,62 en lineaire, pas 1,0 — une bande peinte n'est pas une source de
    lumiere) sous le nommage M_Surf_<slug> pour que FSurfaceLibrary la trouve."""
    folder = '%s/%s' % (SURFACES_ROOT, MARKING_SLUG)
    if not E.does_directory_exist(folder):
        E.make_directory(folder)
    path = '%s/M_Surf_%s' % (folder, MARKING_SLUG)
    mat = E.load_asset(path)
    if mat is None:
        mat = AT.create_asset('M_Surf_%s' % MARKING_SLUG, folder, unreal.Material,
                              unreal.MaterialFactoryNew())
    MEL.delete_all_material_expressions(mat)
    mat.set_editor_property('material_domain', unreal.MaterialDomain.MD_SURFACE)
    mat.set_editor_property('shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT)
    mat.set_editor_property('used_with_nanite', True)
    c = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -400, 0)
    c.set_editor_property('constant', unreal.LinearColor(0.62, 0.61, 0.58, 1.0))
    MEL.connect_material_property(c, '', unreal.MaterialProperty.MP_BASE_COLOR)
    r = MEL.create_material_expression(mat, unreal.MaterialExpressionConstant, -400, 200)
    r.set_editor_property('r', 0.55)
    MEL.connect_material_property(r, '', unreal.MaterialProperty.MP_ROUGHNESS)
    MEL.recompile_material(mat)
    saved = E.save_asset(path, only_if_is_dirty=False)
    log('marquage %s : saved=%s' % (path, saved))
    return path


def build_instance(master, cx, cy, tex, origin_m):
    name = 'MI_CityGround_%d_%d' % (cx, cy)
    path = '%s/%s' % (GROUND_ROOT, name)
    mic = E.load_asset(path)
    if mic is None:
        mic = AT.create_asset(name, GROUND_ROOT, unreal.MaterialInstanceConstant,
                              unreal.MaterialInstanceConstantFactoryNew())
    if mic is None:
        raise RuntimeError('creation de %s impossible' % path)
    MEL.set_material_instance_parent(mic, master)
    MEL.set_material_instance_texture_parameter_value(mic, 'GroundMask', tex)
    MEL.set_material_instance_vector_parameter_value(
        mic, 'CellOriginM', unreal.LinearColor(origin_m[0], origin_m[1], 0.0, 0.0))
    MEL.set_material_instance_scalar_parameter_value(mic, 'CellSizeM', CELL_SIZE_M)
    MEL.set_material_instance_scalar_parameter_value(mic, 'EdgeMinM', EDGE_MIN_M)
    MEL.update_material_instance(mic)
    E.save_asset(path, only_if_is_dirty=False)
    return path


def run():
    src = load_surfaces_lib()
    ground = src['ground_root']()
    tints = src['harmonise_tints'](ground)
    for _key, slug in CLASSES:
        k = tints.get(slug, (1.0, 1.0, 1.0))
        log('  teinte %-40s x%.4f' % (slug, k[0]))

    if not E.does_directory_exist(GROUND_ROOT):
        E.make_directory(GROUND_ROOT)

    d = masks_dir()
    index_path = os.path.join(d, 'index.json')
    if not os.path.exists(index_path):
        raise RuntimeError('MASK: %s absent — lancer Tools/j3c_sols_masks.py d abord'
                           % index_path)
    with open(index_path, encoding='utf-8') as f:
        index = json.load(f)

    # --- textures des masques
    masks = {}
    for entry in index['cells']:
        cx, cy = entry['cell']
        png = os.path.join(d, 'mask_%d_%d.png' % (cx, cy))
        if not os.path.exists(png):
            log('ANOMALIE : %s absent' % png)
            continue
        tex = import_mask_texture(png, 'T_GroundMask_%d_%d' % (cx, cy))
        if tex is None:
            log('ANOMALIE : import de %s echoue' % png)
            continue
        masks[(cx, cy)] = tex
        log('masque %+d,%+d importe : %d x %d, srgb=%s'
            % (cx, cy, tex.blueprint_get_size_x(), tex.blueprint_get_size_y(),
               tex.get_editor_property('srgb')))
    if not masks:
        raise RuntimeError('MASK: aucun masque importe')

    # --- textures de revetement (deja importees par import_surfaces.py)
    textures = {'mask': list(masks.values())[0]}
    sizes = {}
    for key, slug in CLASSES:
        folder = '%s/%s' % (SURFACES_ROOT, slug)
        got = {}
        for kind in MAPS:
            p = '%s/T_%s_%s' % (folder, slug, kind)
            t = E.load_asset(p)
            if t is None:
                log('ANOMALIE : %s absent (lancer import_surfaces.py)' % p)
            else:
                got[kind] = t
        textures[key] = got
        pack_dir = os.path.join(ground, slug)
        sizes[key] = src['physical_size'](pack_dir) if os.path.isdir(pack_dir) else (2.0, 2.0)
        log('classe %-6s -> %-40s scan %gx%g m, cartes %s'
            % (key, slug, sizes[key][0], sizes[key][1], '+'.join(sorted(got))))

    # --- SOLVERT : herbe (5e couche) + bande d'usure + bruit tuilable.
    for key, slug in (('grass', GRASS_SLUG), ('wear', WEAR_SLUG)):
        folder = '%s/%s' % (SURFACES_ROOT, slug)
        got = {}
        for kind in MAPS:
            p = '%s/T_%s_%s' % (folder, slug, kind)
            t = E.load_asset(p)
            if t is None:
                log('ANOMALIE : %s absent (lancer import_surfaces.py)' % p)
            else:
                got[kind] = t
        textures[key] = got
        pack_dir = os.path.join(ground, slug)
        sizes[key] = src['physical_size'](pack_dir) if os.path.isdir(pack_dir) else (2.0, 2.0)
        log('couche %-6s -> %-40s scan %gx%g m, cartes %s'
            % (key, slug, sizes[key][0], sizes[key][1], '+'.join(sorted(got))))
    if not textures['grass']:
        raise RuntimeError('MASK: textures %s absentes — la couche herbe ne peut pas '
                           'se construire (import_surfaces d abord)' % GRASS_SLUG)
    noise_png = os.path.join(d, 'noise512.png')
    if not os.path.exists(noise_png):
        raise RuntimeError('MASK: %s absent — lancer work/SOLVERT/gen_noise512.py' % noise_png)
    textures['noise'] = import_noise_texture(noise_png)
    if textures['noise'] is None:
        raise RuntimeError('MASK: import du bruit %s echoue' % noise_png)

    master, wired = build_master(textures, sizes, tints)
    build_marking(tints)

    made = []
    for entry in index['cells']:
        cx, cy = entry['cell']
        if (cx, cy) not in masks:
            continue
        origin = entry.get('origin') or [cx * CELL_SIZE_M, cy * CELL_SIZE_M]
        made.append(build_instance(master, cx, cy, masks[(cx, cy)], origin))
    unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True, True)
    log('TERMINE : %d masques, master (%s), %d instances de cellule'
        % (len(masks), '+'.join(wired), len(made)))
    return {'masks': len(masks), 'wired': wired, 'instances': len(made)}


if __name__ == '__main__':
    run()
