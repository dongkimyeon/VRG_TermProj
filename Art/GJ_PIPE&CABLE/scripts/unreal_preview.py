"""Run ONLY in the included ConnectionsPreview project. Imports and replaces its preview map.
Existing VRG project and original model files are never modified.
"""
import unreal, json, math, sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
if Path(unreal.Paths.get_project_file_path()).stem != 'ConnectionsPreview':
    raise RuntimeError('Run this preview builder only in ConnectionsPreview.uproject')
manifest=json.loads((ROOT/'reports/connection_manifest.json').read_text(encoding='utf8'))
dest='/Game/Connections'; tools=unreal.AssetToolsHelpers.get_asset_tools(); lib=unreal.MaterialEditingLibrary
materials={}
for key,(rgb,metal,rough) in manifest['palette'].items():
    name='MC_'+key; folder=dest+'/Materials'
    m=unreal.load_asset(folder+'/'+name) if unreal.EditorAssetLibrary.does_asset_exist(folder+'/'+name) else tools.create_asset(name,folder,unreal.Material,unreal.MaterialFactoryNew())
    lib.delete_all_material_expressions(m)
    node=lib.create_material_expression(m,unreal.MaterialExpressionConstant3Vector,-300,0); node.constant=unreal.LinearColor(*rgb,1)
    lib.connect_material_property(node,'',unreal.MaterialProperty.MP_BASE_COLOR)
    for value,prop,y in [(metal,unreal.MaterialProperty.MP_METALLIC,120),(rough,unreal.MaterialProperty.MP_ROUGHNESS,240)]:
        n=lib.create_material_expression(m,unreal.MaterialExpressionConstant,-300,y); n.r=value; lib.connect_material_property(n,'',prop)
    m.set_editor_property('used_with_spline_meshes',True)
    lib.recompile_material(m); unreal.EditorAssetLibrary.save_loaded_asset(m); materials[name]=m
meshes={}; checks={}
for name,info in manifest['assets'].items():
    task=unreal.AssetImportTask(); task.filename=str(ROOT/'exports'/f'{name}.fbx'); task.destination_path=dest
    task.destination_name=name; task.automated=True; task.replace_existing=True; task.replace_existing_settings=True; task.save=True
    opt=unreal.FbxImportUI(); opt.import_mesh=True; opt.import_as_skeletal=False; opt.import_materials=False; opt.import_textures=False
    opt.mesh_type_to_import=unreal.FBXImportType.FBXIT_STATIC_MESH; opt.automated_import_should_detect_type=False
    d=opt.static_mesh_import_data; d.combine_meshes=False; d.auto_generate_collision=False; d.import_uniform_scale=1
    d.convert_scene=True; d.convert_scene_unit=True; d.force_front_x_axis=False; d.generate_lightmap_u_vs=True
    task.options=opt; task.factory=unreal.FbxFactory(); tools.import_asset_tasks([task])
    sm=unreal.load_asset(dest+'/'+name); assert isinstance(sm,unreal.StaticMesh),name
    for i,slot in enumerate(sm.static_materials): sm.set_material(i,materials[str(slot.material_slot_name)])
    unreal.EditorAssetLibrary.save_loaded_asset(sm); meshes[name]=sm
    bounds=sm.get_bounds(); size=bounds.box_extent*2
    expected=[(info['bounds_m'][1][i]-info['bounds_m'][0][i])*100 for i in range(3)]
    err=max(abs(a-b) for a,b in zip([size.x,size.y,size.z],expected)); assert err<.01,(name,err)
    checks[name]={'bounds_error_cm':err}
# Original model imports only in this disposable preview project.
base=ROOT.parent/'GB200_NVL72_Models_20260922'
sys.path.insert(0,str(base/'scripts')); import import_into_unreal
original=import_into_unreal.import_kit('/Game/ExistingGB200')
sub=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
world=sub.get_editor_world(); level=unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
world=unreal.EditorLoadingAndSavingUtils.new_blank_map(False)
actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
def vec(p): return unreal.Vector(p[0]*100,-p[1]*100,p[2]*100)
def spawnmesh(sm,p,label):
    a=actors.spawn_actor_from_class(unreal.StaticMeshActor,vec(p)); a.set_actor_label(label)
    a.static_mesh_component.set_static_mesh(sm); return a
racks=[]
for idx,y in enumerate([-.43,.43]):
    rack=spawnmesh(original['SM_GB200_RackFrame'],(0,y,0),f'Rack_{idx+1}'); racks.append(rack)
    for n in range(18): spawnmesh(original['SM_GB200_ComputeNode'],(.055,y,.392+n*.04445),f'Rack{idx+1}_Node{n+1:02}')
    spawnmesh(original['SM_QuantumIB_Switch_Photo'],(.134,y,1.91),f'Rack{idx+1}_Switch')
cls=unreal.load_class(None,'/Script/GB200Connections.GB200PatchCable'); assert cls
for port,info in manifest['endpoints'].items():
    role=info['role']; p=info['position_blender_m']; d=info['outward_blender']
    name='SM_CDU_Adapter' if port.startswith('CDU.') else ('SM_Plug_Compute' if '.Node' in port else 'SM_Plug_Switch') if role=='Data' else 'SM_QD_'+role
    a=spawnmesh(meshes[name],p,'Plug_'+port)
    a.set_actor_rotation(unreal.MathLibrary.make_rot_from_x(unreal.Vector(d[0],-d[1],d[2])),False)
    if '.Manifold.' in port:
        spawnmesh(meshes['SM_Manifold_Tee'],(p[0]-.059,p[1],p[2]),'Tee_'+port)
proxy=spawnmesh(unreal.load_asset('/Engine/BasicShapes/Cube'),(-1.85,-1.2,.95),'CDU_Project_Dimensions_Proxy')
proxy.set_actor_scale3d(unreal.Vector(.9,.8,1.9)); proxy.static_mesh_component.set_material(0,materials['MC_Dark'])
# Preserve authored Bezier geometry exactly via UE custom Hermite tangents.
for route in manifest['routes']:
    a=actors.spawn_actor_from_class(cls,unreal.Vector()); a.set_actor_label(route['name']); a.set_editor_property('auto_path',False)
    diameter=route['diameter_m']; body='Data' if diameter<.006 else 'Hose' if diameter<.02 else 'Riser' if diameter<.08 else 'Header'
    a.set_editor_property('body_mesh',meshes['SM_Connection_'+body]); a.set_editor_property('body_material',materials['MC_'+route['role']])
    a.set_editor_property('start_port_id',route['start']); a.set_editor_property('end_port_id',route['end'])
    path=a.get_editor_property('path'); path.clear_spline_points(False)
    for b in route['bezier']: path.add_spline_point(vec(b['p']),unreal.SplineCoordinateSpace.LOCAL,False)
    for i,b in enumerate(route['bezier']):
        p=vec(b['p']); arrive=(p-vec(b['left']))*3; leave=(vec(b['right'])-p)*3
        path.set_spline_point_type(i,unreal.SplinePointType.CURVE_CUSTOM_TANGENT,False)
        path.set_tangents_at_spline_point(i,arrive,leave,unreal.SplineCoordinateSpace.LOCAL,False)
    path.update_spline(); a.rebuild_connection()
    assert a.get_visual_segment_count()>0,route['name']
# Explicit patch pairing demo: move either endpoint actor -> automatic refresh in editor tick.
start=actors.spawn_actor_from_class(unreal.TargetPoint,unreal.Vector(135,-65,115)); start.set_actor_label('MOVE_ME_Start_Port')
end=actors.spawn_actor_from_class(unreal.TargetPoint,unreal.Vector(135,65,115)); end.set_actor_label('MOVE_ME_End_Port')
auto=actors.spawn_actor_from_class(cls,unreal.Vector()); auto.set_actor_label('AUTO_PATCH_Move_Endpoint_Actors')
for k,v in [('start_anchor',start),('end_anchor',end),('start_offset_cm',unreal.Vector()),('end_offset_cm',unreal.Vector()),('start_outward',unreal.Vector(1,0,0)),('end_outward',unreal.Vector(1,0,0)),('body_mesh',meshes['SM_Connection_Data']),('start_plug_mesh',meshes['SM_Plug_Switch']),('end_plug_mesh',meshes['SM_Plug_Switch'])]: auto.set_editor_property(k,v)
auto.rebuild_connection(); spline=auto.get_editor_property('path'); before=spline.get_spline_length()
end.set_actor_location(unreal.Vector(135,130,135),False,False); auto.rebuild_connection(); after=spline.get_spline_length()
assert after>before+30,(before,after)
last=spline.get_location_at_spline_point(4,unreal.SplineCoordinateSpace.WORLD)
assert (last-unreal.Vector(139.4,130,135)).length()<.01,str(last)
checks['auto_patch']={'initial_length_cm':before,'extended_length_cm':after,'segments':auto.get_visual_segment_count(),'endpoint_error_cm':(last-unreal.Vector(139.4,130,135)).length(),'note':'Explicit rebuild after move tested; editor tick/live viewport not measured.'}
end.set_actor_location(unreal.Vector(135,5,115),False,False); auto.rebuild_connection()
short=spline.get_spline_length(); assert short<before,(short,before)
auto.set_editor_property('sag_cm',30); auto.rebuild_connection()
mid=spline.get_location_at_spline_point(2,unreal.SplineCoordinateSpace.WORLD)
assert abs(mid.z-85)<.01,str(mid)
checks['auto_patch'].update(shortened_length_cm=short,sag_point_z_cm=mid.z)
end.set_actor_location(unreal.Vector(135,65,115),False,False); auto.set_editor_property('sag_cm',12); auto.rebuild_connection()
# Cube platform and lights make the local map inspectable with the editor camera.
floor=spawnmesh(unreal.load_asset('/Engine/BasicShapes/Cube'),(-.3,0,-.08),'Preview_floor'); floor.set_actor_scale3d(unreal.Vector(4,3,.1))
sun=actors.spawn_actor_from_class(unreal.DirectionalLight,unreal.Vector(0,0,400)); sun.set_actor_rotation(unreal.Rotator(-45,-35,0),False)
actors.spawn_actor_from_class(unreal.SkyLight,unreal.Vector(0,0,350))
assert unreal.EditorLoadingAndSavingUtils.save_map(world,'/Game/L_ConnectionsPreview')
level.load_level('/Game/L_ConnectionsPreview')
restored=[a for a in actors.get_all_level_actors() if a.get_class()==cls]
assert len(restored)==len(manifest['routes'])+1,len(restored)
assert all(a.get_visual_segment_count()>0 for a in restored),'Transient spline meshes were not rebuilt on reload'
checks['map_reload']={'restored_connections':len(restored),'all_have_mesh_segments':True}
(ROOT/'reports/unreal_validation.json').write_text(json.dumps({'status':'PASS','meshes':checks,'authored_spline_routes':len(manifest['routes']),'limits':'NullRHI import/construction checks; no VR performance, physical cable dynamics, collision routing or hydraulic binding.'},indent=2),encoding='utf8')
unreal.log('CONNECTION_PREVIEW_VALIDATION_PASS')



