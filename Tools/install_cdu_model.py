"""UE Python commandlet: import reference CDU materials/mesh, no level mutation.
Run after building the editor. Existing CDU actors pick up the new mesh by path.
"""
import unreal, json
from pathlib import Path

ROOT=Path(unreal.Paths.project_dir()).resolve()
ART=ROOT/'Art/CDU'
DEST='/Game/Connections'
tools=unreal.AssetToolsHelpers.get_asset_tools()
ml=unreal.MaterialEditingLibrary
palette=json.loads((ART/'materials.json').read_text())
for name,(rgb,metal,rough) in palette.items():
    path=DEST+'/Materials/'+name
    mat=unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else tools.create_asset(name,DEST+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
    ml.delete_all_material_expressions(mat)
    c=ml.create_material_expression(mat,unreal.MaterialExpressionConstant3Vector,-280,0)
    c.set_editor_property('constant',unreal.LinearColor(*rgb,1))
    ml.connect_material_property(c,'',unreal.MaterialProperty.MP_BASE_COLOR)
    for val,prop,y in [(metal,unreal.MaterialProperty.MP_METALLIC,140),(rough,unreal.MaterialProperty.MP_ROUGHNESS,240)]:
        n=ml.create_material_expression(mat,unreal.MaterialExpressionConstant,-280,y)
        n.set_editor_property('r',val); ml.connect_material_property(n,'',prop)
    ml.recompile_material(mat); assert unreal.EditorAssetLibrary.save_loaded_asset(mat)
task=unreal.AssetImportTask(); task.filename=str(ART/'SM_CDU_Reference.fbx')
task.destination_path=DEST; task.destination_name='SM_CDU_Reference'
task.automated=True; task.replace_existing=True; task.replace_existing_settings=True; task.save=True
opt=unreal.FbxImportUI(); opt.import_mesh=True; opt.import_materials=False; opt.import_textures=False
opt.import_as_skeletal=False; opt.mesh_type_to_import=unreal.FBXImportType.FBXIT_STATIC_MESH
opt.automated_import_should_detect_type=False
d=opt.static_mesh_import_data; d.combine_meshes=True; d.auto_generate_collision=False
d.convert_scene=True; d.convert_scene_unit=True; d.force_front_x_axis=False
d.import_uniform_scale=1; d.generate_lightmap_u_vs=True
task.options=opt; task.factory=unreal.FbxFactory(); tools.import_asset_tasks([task])
mesh=unreal.load_asset(DEST+'/SM_CDU_Reference'); assert mesh
for i,slot in enumerate(mesh.static_materials):
    mat=unreal.load_asset(DEST+'/Materials/'+str(slot.material_slot_name)); assert mat
    mesh.set_material(i,mat)
assert unreal.EditorAssetLibrary.save_loaded_asset(mesh)
bb=mesh.get_bounding_box()
extent=[bb.max.x-bb.min.x,bb.max.y-bb.min.y,bb.max.z-bb.min.z]
assert max(abs(a-b) for a,b in zip(extent,[90.25,80.4,190]))<.1,extent
assert abs(bb.min.z)<.01,bb
assert len(mesh.static_materials)==6
unreal.log('CDU_IMPORT_PASS '+str(extent))
