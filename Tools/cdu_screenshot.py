"""Open L_Test read-only, photograph a replacement CDU, quit this editor."""
import unreal
from pathlib import Path
ROOT=Path(unreal.Paths.project_dir()).resolve()
unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Test')
wp=unreal.WorldPartitionBlueprintLibrary
wp.load_actors([d.get_editor_property('guid') for d in wp.get_actor_descs()])
sub=unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
actors=sub.get_all_level_actors()
cdus=[a for a in actors if a.get_class().get_name()=='CoolingDistributionUnitActor']
assert cdus
for a in cdus: a.refresh_visuals()
for a in actors:
    if a.get_class().get_name()=='DataCenterRoomActor': a.refresh_gj_connections()
a=sorted(cdus,key=lambda a:a.get_actor_label())[0]
loc=a.get_actor_location(); fwd=a.get_actor_forward_vector(); right=a.get_actor_right_vector()
eye=loc+fwd*290-right*180+unreal.Vector(0,0,210)
target=loc+unreal.Vector(0,0,100)
ues=unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
ues.set_level_viewport_camera_info(eye,unreal.MathLibrary.find_look_at_rotation(eye,target))
sub.clear_actor_selection_set()
steps=[('camera',20),('shot',10),('quit',0)]
state={'wait':20}
def tick(dt):
    state['wait']-=dt
    if state['wait']>0:return
    kind,wait=steps.pop(0); state['wait']=wait
    if kind=='camera':
        ues.set_level_viewport_camera_info(eye,unreal.MathLibrary.find_look_at_rotation(eye,target))
        unreal.SystemLibrary.execute_console_command(None,'viewmode lit')
    elif kind=='shot':
        unreal.SystemLibrary.execute_console_command(None,'HighResShot 1400x1100 filename='+str(ROOT/'Art/CDU/cdu_unreal.png').replace('\\','/'))
        unreal.log('CDU_SCREENSHOT '+a.get_actor_label())
    else:
        unreal.unregister_slate_post_tick_callback(handle); unreal.SystemLibrary.quit_editor()
handle=unreal.register_slate_post_tick_callback(tick)
