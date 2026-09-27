"""Read-only L_Test check: CDU replacement and configurable port invariants."""
import unreal,json
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir()).resolve()
assert unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Test')
wp=unreal.WorldPartitionBlueprintLibrary
wp.load_actors([d.get_editor_property('guid') for d in wp.get_actor_descs()])
actors=unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
cdus=[a for a in actors if a.get_class().get_name()=='CoolingDistributionUnitActor']; assert cdus
report=[]
def vec(v): return [v.x,v.y,v.z]
def transform(a):
    r=a.get_actor_rotation()
    return {'location_cm':vec(a.get_actor_location()),'rotation_deg':[r.pitch,r.yaw,r.roll],'scale':vec(a.get_actor_scale3d())}
def values(a,props):
    return {p:vec(a.get_editor_property(p)) if p=='size_m' else a.get_editor_property(p) for p in props}
for a in cdus:
    props=['size_m','connection_spacing_m','connection_height_m','pump_speed_ratio','reference_pressure_pa','facility_supply_temp_k']
    before=values(a,props)
    tr=transform(a); ports=[vec(a.get_supply_port_world_m()),vec(a.get_return_port_world_m())]
    original=a.get_editor_property('use_reference_model')
    a.set_editor_property('use_reference_model',False); a.refresh_visuals()
    components=a.get_components_by_class(unreal.StaticMeshComponent)
    assert not any(c.static_mesh and c.static_mesh.get_name()=='SM_CDU_Reference' for c in components)
    a.set_editor_property('use_reference_model',True); a.refresh_visuals()
    components=a.get_components_by_class(unreal.StaticMeshComponent)
    bodies=[c for c in components if c.static_mesh and c.static_mesh.get_name()=='SM_CDU_Reference']
    assert len(bodies)==1 and len(components)==3,[(c.get_name(),str(c.static_mesh)) for c in components]
    assert bodies[0].get_num_materials()==6
    assert before==values(a,props)
    assert tr==transform(a)
    assert ports==[vec(a.get_supply_port_world_m()),vec(a.get_return_port_world_m())]
    # Non-default connection settings remain independent of the cabinet mesh.
    for height,spacing in [(.30,.36),(.25,.30)]:
        a.set_editor_property('connection_height_m',height); a.set_editor_property('connection_spacing_m',spacing); a.refresh_visuals()
        local=unreal.Vector(a.get_editor_property('size_m').x*.5,-spacing*.5,height)
        expected=a.get_actor_location()/100+a.get_actor_forward_vector()*local.x+a.get_actor_right_vector()*local.y+a.get_actor_up_vector()*local.z
        error=expected-a.get_supply_port_world_m()
        assert max(abs(error.x),abs(error.y),abs(error.z))<1e-6
    a.set_editor_property('connection_height_m',float(before['connection_height_m']))
    a.set_editor_property('connection_spacing_m',float(before['connection_spacing_m']))
    a.set_editor_property('use_reference_model',original); a.refresh_visuals()
    report.append({'actor':a.get_actor_label(),'transform':tr,'ports_world_m':ports,'mesh':'SM_CDU_Reference','body_components':3,'materials':6,'inputs_preserved':True})
result={'status':'PASS','cdus':report,'level_saved':False,'checks':['replacement mesh','fallback','unchanged transforms and inputs','adjustable port coordinates']}
(ROOT/'Art/CDU/ue_verification.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf8')
unreal.log('CDU_VERIFY_PASS '+str(len(cdus)))
