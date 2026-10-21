"""Read-only L_Test SIE capture of the actual automatic plant yard.
Run in a dedicated editor through -ExecCmds=py; closes that editor when done.
"""
import unreal,json,traceback,time
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir()).resolve(); ART=ROOT/'Art/Plant'
unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Test')
wp=unreal.WorldPartitionBlueprintLibrary; wp.load_actors([d.get_editor_property('guid') for d in wp.get_actor_descs()])
ues=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
state={'wait':10.,'stage':'begin','shots':[],'report':{},'attempts':0}
state['last_tick']=time.monotonic()
world=None; positions={}; sim=None
def command(cmd): unreal.SystemLibrary.execute_console_command(world,cmd)
def instances():
    global positions
    positions={}
    for a in unreal.GameplayStatics.get_all_actors_of_class(world,unreal.Actor):
        if a.get_class().get_name() not in ('FacilityPlantDisplayActor','PlantEquipmentActor'):continue
        for c in a.get_components_by_class(unreal.InstancedStaticMeshComponent):
            mesh=c.get_editor_property('static_mesh')
            if not mesh or not mesh.get_name().startswith('SM_Plant_'):continue
            for i in range(c.get_instance_count()):
                p=c.get_instance_transform(i,world_space=True).translation
                positions.setdefault(mesh.get_name(),[]).append(p)
    state['report']['models_drawn']={k:len(v) for k,v in positions.items()}
def aim(kind):
    if kind=='yard':
        ps=[p for k,vs in positions.items() if k in ('SM_Plant_DryCooler','SM_Plant_Chiller') for p in vs]
        low=unreal.Vector(min(p.x for p in ps),min(p.y for p in ps),0); high=unreal.Vector(max(p.x for p in ps),max(p.y for p in ps),0)
        p=(low+high)/2; span=max(high.x-low.x,high.y-low.y,1000)
        eye=p+unreal.Vector(span*.85,-span*.75,span*.85); target=p+unreal.Vector(0,0,80)
    else:
        name={'drycooler':'SM_Plant_DryCooler','chiller':'SM_Plant_Chiller','pump':'SM_Plant_Pump','platehx':'SM_Plant_PlateHx'}[kind]
        vs=positions.get(name,[])
        if not vs:raise RuntimeError('No actual yard instance: '+name)
        p=vs[len(vs)//2]
        if kind=='pump': eye=p+unreal.Vector(-150,190,125);target=p+unreal.Vector(0,0,30)
        elif kind=='platehx':eye=p+unreal.Vector(240,-260,200);target=p+unreal.Vector(0,0,80)
        else:eye=p+unreal.Vector(430,-550,330);target=p+unreal.Vector(0,0,70)
    ues.set_level_viewport_camera_info(eye,unreal.MathLibrary.find_look_at_rotation(eye,target))
def save_report(): (ART/'ue_screen_verification.json').write_text(json.dumps(state['report'],indent=2),encoding='utf8')
def tick(dt):
    global world,sim
    now=time.monotonic();dt=now-state['last_tick'];state['last_tick']=now
    state['wait']-=dt
    if state['wait']>0:return
    try:
        step=state['stage']
        if step=='begin':
            unreal.EditorLevelLibrary.editor_play_simulate();state['stage']='collect';state['wait']=15;return
        if step=='collect':
            world=unreal.EditorLevelLibrary.get_game_world()
            if not world:raise RuntimeError('SIE game world missing')
            sims=unreal.GameplayStatics.get_all_actors_of_class(world,unreal.DataCenterSimulationActor);assert sims
            sim=sims[0];sim.enter_layout_mode();instances()
            if not positions.get('SM_Plant_DryCooler'):
                state['attempts']+=1
                if state['attempts']<5:state['wait']=5;return
                raise RuntimeError('Automatic plant yard not drawn')
            state['report']['level']='/Game/Maps/L_Test';state['report']['context']='actual SIE layout yard; no level saved'
            state['shots']=['yard','drycooler','chiller','pump']
            if positions.get('SM_Plant_PlateHx'):state['shots'].append('platehx')
            command('viewmode lit');command('ShowFlag.Selection 0');state['stage']='aim';state['wait']=1;return
        if step=='aim':
            if not state['shots']:state['stage']='live';state['wait']=1;return
            state['current']=state['shots'].pop(0);aim(state['current']);state['stage']='shot';state['wait']=9;return
        if step=='shot':
            file=ART/('ue_plant_'+state['current']+'.png')
            state['task']=unreal.AutomationLibrary.take_high_res_screenshot(1500,1100,file.as_posix())
            state['pending_file']=file;state['capture_deadline']=time.monotonic()+180;state['stage']='capture_wait';state['wait']=2;return
        if step=='capture_wait':
            file=state['pending_file']
            if not file.exists():
                if time.monotonic()>state['capture_deadline']:raise RuntimeError('Screenshot was not written: '+str(file))
                state['wait']=2;return
            state['report'].setdefault('screenshots',[]).append(file.name)
            state['stage']='aim';state['wait']=2;save_report();return
        if step=='live':
            sim.set_editor_property('facility_plant',True);sim.enter_simulation_mode();state['stage']='live_aim';state['wait']=5;return
        if step=='live_aim':
            sim.pause_sim();instances();aim('drycooler');state['current']='live';state['stage']='live_shot';state['wait']=7;return
        if step=='live_shot':
            state['task']=unreal.AutomationLibrary.take_high_res_screenshot(1500,1100,(ART/'ue_plant_live.png').as_posix())
            state['stage']='live_wait';state['wait']=3;state['capture_deadline']=time.monotonic()+180;return
        if step=='live_wait':
            if not (ART/'ue_plant_live.png').exists():
                if time.monotonic()>state['capture_deadline']:raise RuntimeError('Live screenshot missing')
                state['wait']=2;return
            state['report']['simulation_mode_capture']=True;state['stage']='quit';state['wait']=2;save_report();return
        if step=='quit':
            unreal.unregister_slate_post_tick_callback(handle);unreal.EditorLevelLibrary.editor_end_play();unreal.SystemLibrary.quit_editor()
    except Exception:
        state['report']['error']=traceback.format_exc();save_report();unreal.log_error(state['report']['error'])
        state['stage']='quit';state['wait']=2
handle=unreal.register_slate_post_tick_callback(tick)
