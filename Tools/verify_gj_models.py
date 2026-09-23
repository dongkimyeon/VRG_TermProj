"""Read-only saved-level verification. Run through UE PythonScript commandlet."""
import unreal, json
from pathlib import Path
world=unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Test')
wp=unreal.WorldPartitionBlueprintLibrary
guids=[d.get_editor_property('guid') for d in wp.get_actor_descs()]
wp.load_actors(guids)
actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
find=lambda name:[a for a in actors if a.get_class().get_name()==name]
racks=find('RackThermalActor'); rooms=find('DataCenterRoomActor')
assert len(racks)==16 and rooms
nodes=0
for rack in racks:
    assert rack.get_editor_property('use_gj_models')
    rack.refresh_visuals()
    meshes=[c.get_editor_property('static_mesh') for c in rack.get_components_by_class(unreal.StaticMeshComponent)]
    names=[m.get_name() for m in meshes if m]
    assert names.count('SM_GB200_RackFrame')==1
    assert names.count('SM_GB200_ComputeNode')==18
    assert names.count('SM_QuantumIB_Switch_Photo')==2
    assert names.count('SM_GJ_NVSwitchTray')==9
    assert names.count('SM_GJ_PowerShelf')==8
    assert rack.get_editor_property('params').get_editor_property('num_sleds')==37
    assert len(rack.get_editor_property('input').get_editor_property('component_heat_w'))==613
    assert all(names.count(f'SM_GJ_Harness_{i:02}')==1 for i in range(18))
    assert all(names.count(f'SM_GJ_SwitchHarness_{i:02}')==1 for i in range(9))
    nodes+=names.count('SM_GB200_ComputeNode')
    blanks=rack.get_components_by_class(unreal.InstancedStaticMeshComponent)
    assert len(blanks)==1 and blanks[0].get_instance_count()==10,'1U blanks'
curves=0
for room in rooms:
    assert room.get_editor_property('use_gj_connections')
    room.refresh_gj_connections()
    assert room.get_num_routed_racks()==16
    curves+=len(room.get_components_by_class(unreal.SplineMeshComponent))
    assert room.is_patch_complete(),room.get_patch_summary()
assert curves>0
report={'status':'PASS','spine_cabinets':len(find('SpineCabinetActor')),'patch':[r.get_patch_summary() for r in rooms],'racks':len(racks),'compute_nodes':nodes,'management_switches':2*len(racks),'nvlink_trays':9*len(racks),'power_shelves':8*len(racks),'spline_spans':curves,'saved_level':'/Game/Maps/L_Test'}
(Path(unreal.Paths.project_saved_dir())/'GJ_Reload.json').write_text(json.dumps(report,indent=2),encoding='utf8')
unreal.log('GJ_RELOAD_PASS '+json.dumps(report))
