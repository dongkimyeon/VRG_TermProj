"""Put GB200_Connections.blend on the full GB200 NVL72 device layout (in place, idempotent).
18 compute trays, 9 NVLink switch trays (liquid, same rear QD pair as a compute tray), 8 power shelves,
2 management switches, at the U positions of rear_data_layout.LAYOUT_U, which mirrors the thermal model
preset (FRackThermalModel::MakeGb200Nvl72Params). Coolant hoses, manifold tees and risers follow every
liquid-cooled tray. Run migrate_rear_data.py afterwards for the data harness.
Run: blender -b --python scripts/migrate_nvl72_layout.py
"""
import bpy, bmesh, json, math, sys
from pathlib import Path
from mathutils import Vector, Matrix
ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
from reference_route import route
import rear_data_layout as L

bpy.ops.wm.open_mainfile(filepath=str(ROOT / 'GB200_Connections.blend'))
sc = bpy.data.scenes['01_Connected_Racks']; bpy.context.window.scene = sc
coll = bpy.data.collections
context, cold, hot, hardware = coll['01_Existing_GB200_Models'], coll['02_Supply'], coll['03_Return'], coll['05_Connectors_Supports']
manifest_path = ROOT / 'reports/connection_manifest.json'
m = json.loads(manifest_path.read_text(encoding='utf8'))
RACKS = [('R01', -.43), ('R02', .43)]

# ---------- 1. New device assets (tray-local frame of the compute node: front face +X at 0.42) ----------
def mat(name): return bpy.data.materials[name]
def cube(co, name, mn, mx, material):
    bm = bmesh.new(); r = bmesh.ops.create_cube(bm, size=1)
    bmesh.ops.scale(bm, vec=[mx[i] - mn[i] for i in range(3)], verts=r['verts'])
    bmesh.ops.translate(bm, vec=[(mx[i] + mn[i]) / 2 for i in range(3)], verts=r['verts'])
    me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free(); me.materials.append(mat(material))
    ob = bpy.data.objects.new(name, me); co.objects.link(ob); return ob
def cyl_x(co, name, centre, radius, length, material, seg=20):
    bm = bmesh.new(); r = bmesh.ops.create_cone(bm, cap_ends=True, segments=seg, radius1=radius, radius2=radius, depth=length)
    bmesh.ops.rotate(bm, verts=r['verts'], cent=(0, 0, 0), matrix=Matrix.Rotation(math.pi / 2, 3, 'Y'))
    bmesh.ops.translate(bm, vec=centre, verts=r['verts'])
    me = bpy.data.meshes.new(name); bm.to_mesh(me); bm.free(); me.materials.append(mat(material))
    ob = bpy.data.objects.new(name, me); co.objects.link(ob); return ob
def chassis(co):
    cube(co, 'Base', (-.42, -.219, 0), (.42, .219, .002), 'M_Aluminium')
    cube(co, 'Side wall', (-.42, -.219, .001), (.42, -.217, .041), 'M_Aluminium')
    cube(co, 'Side wall', (-.42, .217, .001), (.42, .219, .041), 'M_Aluminium')
    cube(co, 'Top cover', (-.42, -.219, .0405), (.42, .219, .0435), 'M_Anodized')
    cube(co, 'Rear cross-member', (-.42, -.219, .001), (-.414, .219, .041), 'M_Aluminium')
    for s in (-1, 1): cube(co, 'Handle', (.4225, s * .235 - .004, .018), (.4555, s * .235 + .004, .03), 'M_Aluminium')

def make_asset(name, build):
    if name in coll:
        return coll[name]
    co = coll.new(name); build(co); return co

def switch_tray(co):
    chassis(co)
    cube(co, 'Front bezel', (.419, -.217, .003), (.421, .217, .0415), 'M_Graphite')
    for i in range(24): cube(co, 'Vent slot', (.421, -.19 + i * .0125, .008), (.4214, -.19 + i * .0125 + .006, .036), 'M_Rubber')
    for y in (.132, .142): cube(co, 'Status LED', (.421, y, .03), (.4222, y + .004, .033), 'M_StatusGreen')
    for y in (.16, .185): cube(co, 'Mgmt port', (.412, y, .012), (.4216, y + .016, .026), 'M_Aluminium')
    for s in (-1, 1): cyl_x(co, 'Rear QD', (-.41, s * L.QD_DY, .024), .005, .013, 'M_Champagne')
    # NVLink spine cartridges plug in here (copper backplane, not modelled as cables).
    for y in (-.13, -.05, .05, .13): cube(co, 'NVLink spine connector', (-.428, y - .03, .006), (-.42, y + .03, .038), 'M_Graphite')

def power_shelf(co):
    chassis(co)
    w = .0715
    for i in range(6):
        y0 = -.2145 + i * w; yc = y0 + w / 2
        cube(co, 'PSU face', (.405, y0 + .001, .002), (.421, y0 + w - .001, .0415), 'M_Anodized')
        cyl_x(co, 'PSU fan ring', (.4215, yc, .022), .016, .002, 'M_Aluminium')
        cyl_x(co, 'PSU fan hub', (.4222, yc, .022), .011, .002, 'M_Graphite')
        cube(co, 'PSU handle', (.421, yc - .02, .034), (.4295, yc + .02, .038), 'M_Aluminium')
        cube(co, 'PSU LED', (.421, yc + .026, .005), (.4222, yc + .03, .008), 'M_StatusGreen')
    cube(co, 'Busbar clip', (-.435, -.03, .01), (-.414, .03, .034), 'M_Copper')

make_asset('ASSET_NVSwitchTray', switch_tray)
make_asset('ASSET_PowerShelf', power_shelf)

# ---------- 2. Devices at the preset U positions ----------
DEVICE_COLLS = ('ASSET_ComputeNode', 'ASSET_QuantumSwitch', 'ASSET_NVSwitchTray', 'ASSET_PowerShelf')
for ob in list(context.objects):
    if ob.instance_type == 'COLLECTION' and ob.instance_collection and ob.instance_collection.name in DEVICE_COLLS:
        bpy.data.objects.remove(ob, do_unlink=True)
def place(asset, p, label, yaw=0.0):
    ob = bpy.data.objects.new(label, None); context.objects.link(ob)
    ob.instance_type = 'COLLECTION'; ob.instance_collection = coll[asset]; ob.location = p
    ob.rotation_euler = (0, 0, math.radians(yaw)); return ob
for rack, yc in RACKS:
    for n, z in enumerate(L.COMPUTE_Z): place('ASSET_ComputeNode', (L.NODE_X, yc, z), f'{rack}_Node{n + 1:02}')
    for k, z in enumerate(L.SWITCHTRAY_Z): place('ASSET_NVSwitchTray', (L.NODE_X, yc, z), f'{rack}_NVSwitch{k + 1:02}')
    for k, z in enumerate(L.SHELF_Z): place('ASSET_PowerShelf', (L.NODE_X, yc, z), f'{rack}_PowerShelf{k + 1:02}')
    # Both management switches mounted reversed (ports to the rear); the top one is the harness target.
    place('ASSET_QuantumSwitch', (L.SWITCH_POS[0], yc, L.MGMT_Z[0]), f'{rack}_Mgmt01', L.SWITCH_YAW_DEG)
    place('ASSET_QuantumSwitch', (L.SWITCH_POS[0], yc, L.SWITCH_POS[1]), f'{rack}_Switch', L.SWITCH_YAW_DEG)

# ---------- 3. Coolant: QD hoses, manifold tees, risers for every liquid-cooled tray ----------
for ob in list(bpy.data.objects):
    inst = ob.instance_collection.name if ob.instance_type == 'COLLECTION' and ob.instance_collection else ''
    placed = inst in ('SM_QD_Supply', 'SM_QD_Return', 'SM_Manifold_Tee') and hardware in ob.users_collection
    curve = ob.type == 'CURVE' and (ob.name.endswith('_Hose') or ob.name.endswith('_Riser'))
    if placed or curve:
        data = ob.data; bpy.data.objects.remove(ob, do_unlink=True)
        if data is not None and data.users == 0: bpy.data.curves.remove(data)
m['routes'] = [r for r in m['routes'] if not (r['name'].endswith('_Hose') or r['name'].endswith('_Riser'))]
m['endpoints'] = {k: v for k, v in m['endpoints'].items() if not (v['role'] in ('Supply', 'Return') and ('.Node' in k or '.NVSwitch' in k or '.Manifold.' in k))}

def endpoint(name, p, d, role): m['endpoints'][name] = {'position_blender_m': list(p), 'outward_blender': list(d), 'role': role}
def fitting(asset, p, d=(1, 0, 0)):
    ob = bpy.data.objects.new(asset, None); hardware.objects.link(ob)
    ob.instance_type = 'COLLECTION'; ob.instance_collection = coll[asset]; ob.location = p
    ob.rotation_euler = Vector(d).to_track_quat('X', 'Z').to_euler()
def record(name, ob, role, diam, start='', end=''):
    sp = ob.data.splines[0]
    m['routes'].append({'name': name, 'role': role, 'diameter_m': diam, 'start': start, 'end': end,
        'bezier': [{'p': list(b.co), 'left': list(b.handle_left), 'right': list(b.handle_right)} for b in sp.bezier_points]})
def smooth(name, points, diam, material, co, start, end):
    cu = bpy.data.curves.new(name, 'CURVE'); cu.dimensions = '3D'; cu.resolution_u = 16
    cu.bevel_depth = diam / 2; cu.bevel_resolution = 3; cu.use_fill_caps = True
    sp = cu.splines.new('BEZIER'); sp.bezier_points.add(len(points) - 1)
    for b, p in zip(sp.bezier_points, points): b.co = p; b.handle_left_type = 'AUTO'; b.handle_right_type = 'AUTO'
    ob = bpy.data.objects.new(name, cu); co.objects.link(ob); cu.materials.append(mat('MC_' + material))
    ob['endpoint_start'] = start; ob['endpoint_end'] = end; ob['role'] = material
    bpy.context.view_layer.update(); record(name, ob, material, diam, start, end)

trays = L.liquid_trays()
tee_z = [z + .024 for _, z in trays]
top, bottom = max(tee_z) + .10, min(tee_z) - .076
for rack, yc in RACKS:
    for idx, (tray, z) in enumerate(trays):
        for sign, color, co in [(-1, 'Supply', cold), (1, 'Return', hot)]:
            yp = yc + sign * L.QD_DY; ym = yc + sign * L.MANIFOLD_DY
            e1 = f'{rack}.{tray}.{color}'; e2 = f'{rack}.Manifold.{color}.{idx + 1:02}'
            a = (L.QD_X, yp, z + .024); b = (L.MANIFOLD_QD_X, ym, z + .024)
            endpoint(e1, a, (-1, 0, 0), color); endpoint(e2, b, (1, 0, 0), color)
            fitting('SM_QD_' + color, a, (-1, 0, 0)); fitting('SM_Manifold_Tee', (L.MANIFOLD_X, ym, z + .024)); fitting('SM_QD_' + color, b)
            # Service loop in the free rear space, outside the body and side panels.
            smooth(e1 + '_Hose', [(-.4165, yp, z + .024), (-.46, yp, z + .024), (-.49, yp + sign * .023, z - .010),
                                  (-.515, ym, z + .024), (-.5395, ym, z + .024)], .013, 'Rubber', co, e1, e2)
    for sign, color, co in [(-1, 'Supply', cold), (1, 'Return', hot)]:
        ym = yc + sign * L.MANIFOLD_DY; hx = -.82
        pts = [(L.MANIFOLD_X, ym, .34), (L.MANIFOLD_X, ym, top)] if sign < 0 else [(hx, ym, .25), (L.MANIFOLD_X, ym, .25), (L.MANIFOLD_X, ym, top)]
        name = f'{rack}_{color}_Riser'
        ob = route(name, pts, .025, mat('MC_' + color), co, .075, color)
        record(name, ob, color, .05)
# Manifold end caps and upper bands move with the taller manifold.
for ob in bpy.data.objects:
    if ob.name.startswith('Manifold cap'): ob.location.z = top + .007
    if (ob.name.startswith('Manifold band') or ob.name.startswith('Rear manifold bracket')) and ob.location.z > .8: ob.location.z = top - .12

m['rack_layout'] = {'u0_m': L.Z0, 'unit_m': L.UNIT, 'layout_u': L.LAYOUT_U,
    'kinds': {'0': 'compute tray', '1': 'NVLink switch tray', '2': 'power shelf', '3': 'management switch', '-1': 'blank'},
    'source': 'Mirrors FRackThermalModel::MakeGb200Nvl72Params (thermal model decides where heat is released).'}
manifest_path.write_text(json.dumps(m, indent=2), encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT / 'GB200_Connections.blend'))
print('NVL72_LAYOUT_MIGRATION_PASS', len(trays), 'liquid trays per rack, manifold', round(bottom, 3), '-', round(top, 3))
