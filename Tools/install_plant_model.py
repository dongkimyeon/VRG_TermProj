"""Import the plant exteriors and shared PBR materials; never save a level."""
import unreal, json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]; ART=ROOT/'Art/Plant'; DEST='/Game/Connections'
at=unreal.AssetToolsHelpers.get_asset_tools(); ml=unreal.MaterialEditingLibrary
for name,(rgb,metal,rough) in json.loads((ART/'materials.json').read_text()).items():
    path=DEST+'/Materials/'+name
    mat=unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else at.create_asset(name,DEST+'/Materials',unreal.Material,unreal.MaterialFactoryNew())
    ml.delete_all_material_expressions(mat)
    if name=='MC_Plant_Accent':
        c=ml.create_material_expression(mat,unreal.MaterialExpressionVectorParameter,-280,0)
        c.set_editor_property('parameter_name','Color'); c.set_editor_property('default_value',unreal.LinearColor(*rgb,1))
    else:
        c=ml.create_material_expression(mat,unreal.MaterialExpressionConstant3Vector,-280,0); c.set_editor_property('constant',unreal.LinearColor(*rgb,1))
    ml.connect_material_property(c,'',unreal.MaterialProperty.MP_BASE_COLOR)
    for value,prop,y in [(metal,unreal.MaterialProperty.MP_METALLIC,120),(rough,unreal.MaterialProperty.MP_ROUGHNESS,220)]:
        n=ml.create_material_expression(mat,unreal.MaterialExpressionConstant,-280,y); n.set_editor_property('r',value); ml.connect_material_property(n,'',prop)
    mat.set_editor_property('used_with_instanced_static_meshes',True)
    ml.recompile_material(mat); unreal.EditorAssetLibrary.save_loaded_asset(mat)
report={}
expected=json.loads((ART/'verification.json').read_text())['models']
for kind,spec in expected.items():
    name='SM_Plant_'+kind; task=unreal.AssetImportTask(); task.filename=str(ART/(name+'.fbx'))
    task.destination_path=DEST; task.destination_name=name; task.automated=True; task.replace_existing=True; task.replace_existing_settings=True; task.save=False
    opt=unreal.FbxImportUI(); opt.import_mesh=True; opt.import_materials=False; opt.import_textures=False
    opt.import_as_skeletal=False; opt.mesh_type_to_import=unreal.FBXImportType.FBXIT_STATIC_MESH; opt.automated_import_should_detect_type=False
    d=opt.static_mesh_import_data; d.combine_meshes=True; d.auto_generate_collision=False; d.convert_scene=True; d.convert_scene_unit=True; d.force_front_x_axis=False; d.import_uniform_scale=1; d.generate_lightmap_u_vs=False
    task.options=opt; task.factory=unreal.FbxFactory(); at.import_asset_tasks([task])
    mesh=unreal.load_asset(DEST+'/'+name); assert mesh,name
    slots=list(mesh.static_materials)
    for slot in slots:
        mat=unreal.load_asset(DEST+'/Materials/'+str(slot.material_slot_name)); assert mat
        slot.set_editor_property('material_interface',mat)
    mesh.set_editor_property('static_materials',slots)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh)
    bb=mesh.get_bounding_box(); low=[bb.min.x/100,bb.min.y/100,bb.min.z/100]; high=[bb.max.x/100,bb.max.y/100,bb.max.z/100]
    error=max(abs(a-b) for a,b in zip(low+high,spec['bounds_min_m']+spec['bounds_max_m']))
    assert error<1e-6,(name,error)
    assert len(mesh.static_materials)==len(spec['materials'])
    report[kind]={'bounds_min_m':low,'bounds_max_m':high,'max_import_error_m':error,'materials':[str(s.material_slot_name) for s in mesh.static_materials]}
(ART/'ue_import_verification.json').write_text(json.dumps({'status':'PASS','models':report},indent=2),encoding='utf8')
unreal.log('PLANT_IMPORT_PASS '+str(len(report)))
