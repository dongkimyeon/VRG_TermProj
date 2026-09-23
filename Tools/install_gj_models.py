"""UE Python commandlet: install GJ runtime art into the EXISTING L_Test actors.
Run after bake_gj_rack_harness.py and an editor build. Does not replace thermal actors.
"""
import unreal, json, shutil
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir()).resolve()
ART=ROOT/'Art/GJ_PIPE&CABLE'
# Keep original package names so material references remain valid after migration.
for folder in ['ExistingGB200','Connections']:
    for src in (ART/'UnrealSample/Content'/folder).rglob('*.uasset'):
        dst=ROOT/'Content'/folder/src.relative_to(ART/'UnrealSample/Content'/folder)
        if not dst.exists(): dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
tools=unreal.AssetToolsHelpers.get_asset_tools(); ml=unreal.MaterialEditingLibrary
def import_mesh(fbx,dest):
    task=unreal.AssetImportTask(); task.filename=str(fbx); task.destination_path=dest
    task.destination_name=fbx.stem; task.automated=True; task.replace_existing=True; task.replace_existing_settings=True; task.save=True
    opt=unreal.FbxImportUI(); opt.import_mesh=True; opt.import_materials=False; opt.import_textures=False
    opt.import_as_skeletal=False; opt.mesh_type_to_import=unreal.FBXImportType.FBXIT_STATIC_MESH; opt.automated_import_should_detect_type=False
    d=opt.static_mesh_import_data; d.combine_meshes=False; d.auto_generate_collision=False
    d.convert_scene=True; d.convert_scene_unit=True; d.force_front_x_axis=False; d.import_uniform_scale=1; d.generate_lightmap_u_vs=True
    task.options=opt; task.factory=unreal.FbxFactory(); tools.import_asset_tasks([task])
    mesh=unreal.load_asset(dest+'/'+fbx.stem); assert mesh,fbx.stem
    for i,slot in enumerate(mesh.static_materials):
        mat=unreal.load_asset(dest+'/Materials/'+str(slot.material_slot_name)); assert mat,str(slot.material_slot_name)
        mesh.set_material(i,mat)
    unreal.EditorAssetLibrary.save_loaded_asset(mesh); return mesh
for fbx in sorted((ART/'runtime_exports').glob('*.fbx')): import_mesh(fbx,'/Game/Connections')
# Compute node with rear data IO. Outer bounds may move only by the 2 mm rear cage lip (-42.0 -> -42.2 cm).
node_bounds={}
for fbx in sorted((ART/'runtime_exports/ExistingGB200').glob('*.fbx')):
    asset_path='/Game/ExistingGB200/'+fbx.stem
    old=unreal.load_asset(asset_path) if unreal.EditorAssetLibrary.does_asset_exist(asset_path) else None
    before=old.get_bounding_box() if old else None
    after=import_mesh(fbx,'/Game/ExistingGB200').get_bounding_box()
    node_bounds[fbx.stem]={'before_cm':str(before),'after_cm':str(after)}
    if before: assert all(abs(a-b)<.25 for a,b in [(before.min.x,after.min.x),(before.min.y,after.min.y),(before.min.z,after.min.z),(before.max.x,after.max.x),(before.max.y,after.max.y),(before.max.z,after.max.z)]),node_bounds[fbx.stem]
path='/Game/Connections/Materials/MC_FlowBody'
mat=unreal.load_asset(path) if unreal.EditorAssetLibrary.does_asset_exist(path) else tools.create_asset('MC_FlowBody','/Game/Connections/Materials',unreal.Material,unreal.MaterialFactoryNew())
ml.delete_all_material_expressions(mat)
color=ml.create_material_expression(mat,unreal.MaterialExpressionVectorParameter,-300,0); color.set_editor_property('parameter_name','Color'); color.set_editor_property('default_value',unreal.LinearColor(.2,.55,.9,1))
ml.connect_material_property(color,'',unreal.MaterialProperty.MP_BASE_COLOR)
for value,prop,y in [(.35,unreal.MaterialProperty.MP_METALLIC,120),(.3,unreal.MaterialProperty.MP_ROUGHNESS,240)]:
    n=ml.create_material_expression(mat,unreal.MaterialExpressionConstant,-300,y); n.set_editor_property('r',value); ml.connect_material_property(n,'',prop)
mat.set_editor_property('used_with_spline_meshes',True); ml.recompile_material(mat); unreal.EditorAssetLibrary.save_loaded_asset(mat)
world=unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Test'); assert world
wp=unreal.WorldPartitionBlueprintLibrary; guids=[d.get_editor_property('guid') for d in wp.get_actor_descs()]
wp.load_actors(guids); wp.pin_actors(guids)
sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem); actors=sub.get_all_level_actors()
find=lambda name:[a for a in actors if a.get_class().get_name()==name]
racks=find('RackThermalActor'); rooms=find('DataCenterRoomActor'); cdus=find('CoolingDistributionUnitActor')
assert racks and rooms and cdus,'Expected existing rack / room / CDU actors'
rows=[]
for r in racks:
    r.modify()  # Mark the World Partition external package dirty before native preset mutations.
    old_transform=str(r.get_actor_transform())
    r.apply_gb200_nvl72_preset()
    r.set_editor_property('use_gj_models',True); r.set_editor_property('rack_size_m',unreal.Vector(1.068,.6,2.236)); r.refresh_visuals()
    assert r.get_editor_property('params').get_editor_property('num_sleds')==37
    assert len(r.get_editor_property('input').get_editor_property('component_heat_w'))==613
    assert old_transform==str(r.get_actor_transform()),'Rack moved'
    counts={}
    for c in r.get_components_by_class(unreal.StaticMeshComponent):
        sm=c.get_editor_property('static_mesh')
        if sm:counts[sm.get_name()]=counts.get(sm.get_name(),0)+1
    assert counts.get('SM_GB200_RackFrame')==1 and counts.get('SM_GB200_ComputeNode')==18 and counts.get('SM_QuantumIB_Switch_Photo')==2,counts
    assert counts.get('SM_GJ_NVSwitchTray')==9 and counts.get('SM_GJ_PowerShelf')==8,counts
    assert all(counts.get(f'SM_GJ_Harness_{i:02}')==1 for i in range(18)),counts
    assert all(counts.get(f'SM_GJ_SwitchHarness_{i:02}')==1 for i in range(9)),counts
    rows.append({'actor':r.get_actor_label(),'position_cm':str(r.get_actor_location()),'rotation':str(r.get_actor_rotation()),'mesh_counts':counts})
for r in rooms:
    r.modify()
    r.set_editor_property('use_gj_connections',True); r.set_editor_property('rack_size_m',unreal.Vector(1.068,.6,2.236))
    r.spawn_spine_cabinets()  # one row-end patch/spine cabinet per row that has none (idempotent)
    r.refresh_gj_connections()
    assert r.is_patch_complete(),r.get_patch_summary()
    assert r.get_num_routed_racks()==len(racks)
    assert r.get_components_by_class(unreal.SplineMeshComponent)
for air in find('AirFieldDebugActor'):
    air.modify(); air.fit_to_racks()
for sim in find('DataCenterSimulationActor'):sim.rebuild_coupling()
assert unreal.EditorLoadingAndSavingUtils.save_dirty_packages(True,True)
report={'status':'PASS','racks':rows,'patch':[r.get_patch_summary() for r in rooms],'spine_cabinets':[str(a.get_actor_location()) for a in unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors() if a.get_class().get_name()=='SpineCabinetActor'],'reimported_rack_meshes':node_bounds,'room_count':len(rooms),'cdu_count':len(cdus),'thermal_preset':'GB200 NVL72: 37 devices / 613 nodes','actor_transforms_preserved':True,'level':'/Game/Maps/L_Test'}
(ROOT/'Saved/GJ_Install.json').write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding='utf8')
unreal.log('GJ_INSTALL_PASS '+str(len(racks))+' racks')
