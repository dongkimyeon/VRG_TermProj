"""Import only new CRAH assets; never load/save the project level.

The art source is relative to this script; the destination is the running UE
project. This also permits asset-only import in a separate older-engine project.
"""
import unreal
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / 'Art/CRAH'
DEST = '/Game/Connections'
asset_tools = unreal.AssetToolsHelpers.get_asset_tools()
ml = unreal.MaterialEditingLibrary
palette = json.loads((ART / 'materials.json').read_text(encoding='utf8'))

for name, (rgb, metal, rough) in palette.items():
    path = DEST + '/Materials/' + name
    mat = unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else asset_tools.create_asset(
        name, DEST + '/Materials', unreal.Material, unreal.MaterialFactoryNew())
    assert mat, path
    ml.delete_all_material_expressions(mat)
    c = ml.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -280, 0)
    c.set_editor_property('constant', unreal.LinearColor(*rgb, 1))
    ml.connect_material_property(c, '', unreal.MaterialProperty.MP_BASE_COLOR)
    for val, prop, y in [(metal, unreal.MaterialProperty.MP_METALLIC, 140),
                         (rough, unreal.MaterialProperty.MP_ROUGHNESS, 240)]:
        n = ml.create_material_expression(mat, unreal.MaterialExpressionConstant, -280, y)
        n.set_editor_property('r', val)
        ml.connect_material_property(n, '', prop)
    if name.endswith('Screen'):
        n = ml.create_material_expression(mat, unreal.MaterialExpressionConstant3Vector, -280, 340)
        n.set_editor_property('constant', unreal.LinearColor(*(v * .2 for v in rgb), 1))
        ml.connect_material_property(n, '', unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    ml.recompile_material(mat)
    assert unreal.EditorAssetLibrary.save_loaded_asset(mat)

for kind in ('InRow', 'Perimeter'):
    name = 'SM_CRAH_' + kind
    task = unreal.AssetImportTask()
    task.filename = str(ART / (name + '.fbx'))
    task.destination_path = DEST
    task.destination_name = name
    task.automated = True
    task.replace_existing = True
    task.replace_existing_settings = True
    task.save = True
    opt = unreal.FbxImportUI()
    opt.import_mesh = True
    opt.import_materials = False
    opt.import_textures = False
    opt.import_as_skeletal = False
    opt.mesh_type_to_import = unreal.FBXImportType.FBXIT_STATIC_MESH
    opt.automated_import_should_detect_type = False
    data = opt.static_mesh_import_data
    data.combine_meshes = True
    data.auto_generate_collision = False
    data.convert_scene = True
    data.convert_scene_unit = True
    data.force_front_x_axis = False
    data.import_uniform_scale = 1
    data.generate_lightmap_u_vs = True
    task.options = opt
    task.factory = unreal.FbxFactory()
    asset_tools.import_asset_tasks([task])
    mesh = unreal.load_asset(DEST + '/' + name)
    assert mesh, name
    for index, slot in enumerate(mesh.static_materials):
        mat = unreal.load_asset(DEST + '/Materials/' + str(slot.material_slot_name))
        assert mat, str(slot.material_slot_name)
        mesh.set_material(index, mat)
    assert unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    unreal.log('CRAH_IMPORT_PASS ' + name)

unreal.log('CRAH_IMPORT_ENGINE ' + unreal.SystemLibrary.get_engine_version())
