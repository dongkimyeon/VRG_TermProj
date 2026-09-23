"""Move the data side of GB200_Connections.blend to the rear (-X), in place.
Needed because build_connections.py reads GB200_Modular_Kit.blend, which is not in the repository.
The rack is NOT rotated: coolant manifolds / QD hoses and the +X intake face stay where they are.
Run: blender -b --python scripts/migrate_rear_data.py [-- --no-render]
Idempotent: a second run rebuilds the same data routes and leaves the node / switch alone.
"""
import bpy, json, math, sys
from pathlib import Path
from mathutils import Vector
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from reference_route import route
import rear_data_layout as L

bpy.ops.wm.open_mainfile(filepath=str(ROOT / 'GB200_Connections.blend'))
sc = bpy.data.scenes['01_Connected_Racks']; bpy.context.window.scene = sc
mats = bpy.data.materials; data = bpy.data.collections['04_Data_Patches']; hardware = bpy.data.collections['05_Connectors_Supports']
manifest_path = ROOT / 'reports/connection_manifest.json'
m = json.loads(manifest_path.read_text(encoding='utf8'))

# 1. Node IO cages onto the rear cross-member (shared collection -> every node instance).
moved = L.move_node_io_to_rear(bpy.data.collections['ASSET_ComputeNode'])

# 2. Switch: same rack slot height, yawed 180 deg so its ports face the rear.
for rack, yc in [('R01', -.43), ('R02', .43)]:
    sw = bpy.data.objects[rack + '_Switch']
    sw.location = (L.SWITCH_POS[0], yc, L.SWITCH_POS[1]); sw.rotation_euler = (0, 0, math.radians(L.SWITCH_YAW_DEG))

# 3. Remove the old front data routes and plugs; keep coolant untouched.
for ob in list(bpy.data.objects):
    inst = ob.instance_collection.name if ob.instance_type == 'COLLECTION' and ob.instance_collection else ''
    placed_plug = inst in ('SM_Plug_Compute', 'SM_Plug_Switch') and hardware in ob.users_collection  # not the parts gallery
    data_curve = ob.type == 'CURVE' and (ob.name.endswith('_Patch') or ob.name.startswith('InterRack_'))
    if data_curve or placed_plug:
        cu = ob.data; bpy.data.objects.remove(ob, do_unlink=True)
        if cu is not None and cu.users == 0: bpy.data.curves.remove(cu)
m['routes'] = [r for r in m['routes'] if r['role'] not in ('Lime', 'Cyan')]
m['endpoints'] = {k: v for k, v in m['endpoints'].items() if v['role'] != 'Data'}

def endpoint(name, p): m['endpoints'][name] = {'position_blender_m': list(p), 'outward_blender': list(L.OUT), 'role': 'Data'}
def plug(asset, p):
    ob = bpy.data.objects.new(asset, None); hardware.objects.link(ob)
    ob.instance_type = 'COLLECTION'; ob.instance_collection = bpy.data.collections[asset]; ob.location = p
    ob.rotation_euler = Vector(L.OUT).to_track_quat('X', 'Z').to_euler()
def path(name, points, mat, start, end, bend):
    ob = route(name, points, .0024, mats['MC_' + mat], data, bend, mat, start, end)
    m['routes'].append({'name': name, 'role': mat, 'diameter_m': .0048, 'start': start, 'end': end,
        'bezier': [{'p': list(b.co), 'left': list(b.handle_left), 'right': list(b.handle_right)} for b in ob.data.splines[0].bezier_points]})

# 4. Node -> switch patches on the rear, vertical ribbon in the centre channel between the manifolds.
for rack, yc in [('R01', -.43), ('R02', .43)]:
    for n in range(18):
        e1 = f'{rack}.Node{n+1:02}.Data01'; e2 = f'{rack}.Switch.Lower{n+1:02}'
        a = L.node_data_port(yc, n); b = L.switch_lower_port(yc, n + 1)
        endpoint(e1, a); endpoint(e2, b); plug('SM_Plug_Compute', a); plug('SM_Plug_Switch', b)
        path(e1 + '_Patch', L.patch_points(yc, n), 'Lime', e1, e2, .04)
# 5. Rack-to-rack links leave the rear upper ports into the hot aisle, never over the roof.
for j in range(4):
    col = j + 11; ea = f'R01.Switch.Upper{col:02}'; eb = f'R02.Switch.Upper{col:02}'
    a = L.switch_upper_port(-.43, col); b = L.switch_upper_port(.43, col)
    endpoint(ea, a); endpoint(eb, b); plug('SM_Plug_Switch', a); plug('SM_Plug_Switch', b)
    path(f'InterRack_{j+1}', L.inter_rack_points(a[1], b[1], j), 'Cyan', ea, eb, .10)
# 6. The support tray follows the cables to the rear.
for ob in bpy.data.objects:
    if ob.name.startswith(('Tray side', 'Tray rung', 'Tray stand')) and ob.location.x > 0: ob.location.x = -ob.location.x

cam = bpy.data.objects['CAM_Patch']; cam.location = (-2, -.2, 2.0)  # rear switch close-up; clear of the CDU proxy box
cam.rotation_euler = (Vector((-.55, -.43, 1.8)) - cam.location).to_track_quat('-Z', 'Y').to_euler()
m['data_side'] = 'rear (-X): node Data01 and switch ports face -X; switch yawed 180 deg at x=-0.134'
manifest_path.write_text(json.dumps(m, indent=2), encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / 'GB200_Connections.blend'))
if '--no-render' not in sys.argv:
    for cam, filename in [('CAM_Front', '01_front_connections.png'), ('CAM_Rear', '02_rear_cooling.png'),
                          ('CAM_Ports', '03_hose_connectors.png'), ('CAM_Patch', '04_switch_patches.png')]:
        sc.camera = bpy.data.objects[cam]; sc.render.filepath = str(ROOT / 'previews' / filename); bpy.ops.render.render(write_still=True)
print('REAR_DATA_MIGRATION_PASS', 'node_io_moved' if moved else 'node_io_already_rear',
      sum(1 for r in m['routes'] if r['role'] in ('Lime', 'Cyan')), 'data routes')
