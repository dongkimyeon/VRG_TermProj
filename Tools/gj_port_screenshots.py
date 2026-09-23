"""Editor (not commandlet) Python: front / rear screenshots of one GJ rack in L_Test.
Run with a real RHI:
  UnrealEditor.exe VRG_TermProj.uproject -ExecutePythonScript=Tools/gj_port_screenshots.py
Writes Saved/Screenshots/GJ_Front.png, GJ_Rear.png and GJ_Patch.png, then quits. Read-only: nothing is saved.
"""
import unreal
from pathlib import Path
OUT = Path(unreal.Paths.project_saved_dir()).resolve() / 'Screenshots'
unreal.EditorLoadingAndSavingUtils.load_map('/Game/Maps/L_Test')
wp = unreal.WorldPartitionBlueprintLibrary
wp.load_actors([d.get_editor_property('guid') for d in wp.get_actor_descs()])
actors = unreal.get_editor_subsystem(unreal.EditorActorSubsystem).get_all_level_actors()
racks = [a for a in actors if a.get_class().get_name() == 'RackThermalActor']
for room in [a for a in actors if a.get_class().get_name() == 'DataCenterRoomActor']: room.refresh_gj_connections()
# Middle rack of the row whose racks share the first rack's facing: neighbours on both sides show the links.
fwd0 = racks[0].get_actor_forward_vector()
row = [r for r in racks if r.get_actor_forward_vector().dot(fwd0) > .9]
right = unreal.Vector(-fwd0.y, fwd0.x, 0)
row.sort(key=lambda r: r.get_actor_location().dot(right))
rack = row[len(row) // 2]; loc = rack.get_actor_location(); fwd = rack.get_actor_forward_vector()
target = loc + unreal.Vector(0, 0, 130)
centre = sum([r.get_actor_location() for r in racks], unreal.Vector()) / len(racks)
def view(side):
    # Front: cold aisle (1U blanks). Rear: hot aisle, kept short so the facing row does not block it.
    # Patch: overhead view of the tray loop, rack drops and row-end spine cabinets.
    if side == 'Patch':
        eye = centre + fwd * -900 + right * -700 + unreal.Vector(0, 0, 750)
        return eye, unreal.MathLibrary.find_look_at_rotation(eye, centre + unreal.Vector(0, 0, 120))
    eye = loc + fwd * (260 if side == 'Front' else -170) + right * 110 + unreal.Vector(0, 0, 175)
    return eye, unreal.MathLibrary.find_look_at_rotation(eye, target)
ues = unreal.get_editor_subsystem(unreal.UnrealEditorSubsystem)
# Slate ticks many times per frame while the map streams, so pace the steps in real seconds.
steps = []
for side in ['Front', 'Rear', 'Patch']:
    steps += [('cam', side, 6.0), ('shot', side, 4.0)]
steps.append(('quit', None, 0.0))
state = {'wait': 8.0}
def tick(dt):
    state['wait'] -= dt
    if state['wait'] > 0 or not steps: return
    kind, side, wait = steps.pop(0); state['wait'] = wait
    if kind == 'cam':
        eye, rot = view(side); ues.set_level_viewport_camera_info(eye, rot)
    elif kind == 'shot':
        unreal.SystemLibrary.execute_console_command(None, f'HighResShot 1600x900 filename={(OUT / ("GJ_" + side)).as_posix()}')
        unreal.log(f'GJ_SHOT {side} rack={rack.get_actor_label()}')
    elif kind == 'quit':
        unreal.unregister_slate_post_tick_callback(handle); unreal.SystemLibrary.quit_editor()
handle = unreal.register_slate_post_tick_callback(tick)
unreal.log(f'GJ_SHOT_SETUP rack={rack.get_actor_label()} racks={len(racks)} row={len(row)}')
