"""GB200 NVL72 rack layout shared by build_connections.py, migrate_*.py and the harness bake.
Blender metres, rack-local except yc (rack centre Y). +X is the intake face / sled pull direction.
Data lives on the rear (-X) with the coolant. The rack itself is never rotated.
"""
NODE_X = .055                       # tray pivot in the rack (compute and NVLink switch trays)
UNIT = .04445                       # 1U
Z0 = .392 - 7 * UNIT                # U0 bottom; U7 was the first compute tray before the NVL72 layout

# GB200 NVL72 U map, bottom-up. MUST match LayoutU in Source/.../RackThermalModel.cpp (the thermal model
# decides where heat is released; the art must sit in the same place). 0 compute tray, 1 NVLink switch tray,
# 2 power shelf, 3 management switch, -1 blank.
LAYOUT_U = [-1, -1] + [2] * 4 + [0] * 8 + [1] * 9 + [0] * 10 + [2] * 4 + [3] * 2
def slots_of(kind): return [Z0 + u * UNIT for u, t in enumerate(LAYOUT_U) if t == kind]
COMPUTE_Z = slots_of(0)             # 18, bottom-up = compute ordinal / harness index
SWITCHTRAY_Z = slots_of(1)          # 9
SHELF_Z = slots_of(2)               # 8
MGMT_Z = slots_of(3)                # 2; the top one takes the node data harness and the uplinks
SWITCH_POS = (-.134, MGMT_Z[-1])    # switch pivot (x, z); yawed 180 deg so its port panel faces -X
SWITCH_YAW_DEG = 180.0
NODE_IO_Y = [-.1155 + k * .033 for k in range(8)]   # rear IO cages, clear of the QD couplers at |y|=.197
NODE_IO_Z = .023
NODE_PORT_X = NODE_X - .42          # rear cross-member face
SWITCH_PORT_X = SWITCH_POS[0] - .342  # cage outer face after the 180 deg yaw
LOWER_PORT_Z, UPPER_PORT_Z = SWITCH_POS[1] + .012, SWITCH_POS[1] + .031
PLUG_TAIL = .044                    # SM_Plug_Compute / SM_Plug_Switch tail length
OUT = (-1.0, 0.0, 0.0)

def node_z(n): return COMPUTE_Z[n]

def switch_port_y(column):
    """1-based cage column. Unrotated cage y is -.197 + (column-1)*.022; the yaw mirrors it."""
    return .197 - (column - 1) * .022

def node_data_port(yc, n): return (NODE_PORT_X, yc + NODE_IO_Y[0], node_z(n) + NODE_IO_Z)
def switch_lower_port(yc, column): return (SWITCH_PORT_X, yc + switch_port_y(column), LOWER_PORT_Z)
def switch_upper_port(yc, column): return (SWITCH_PORT_X, yc + switch_port_y(column), UPPER_PORT_Z)

def trunk_x(n):
    # Lower nodes sit further out so a higher node's lead stops before crossing their risers.
    return -.545 - (17 - n) * .005

def patch_points(yc, n):
    """Node n (0-based) Data01 -> switch Lower(n+1). Vertical ribbon in the rear centre channel; the fan-out
    to the switch columns runs 0.1 m below the ports, above the top compute tray."""
    a = node_data_port(yc, n); b = switch_lower_port(yc, n + 1); x = trunk_x(n); zl = LOWER_PORT_Z - .10
    return [(a[0] - PLUG_TAIL, a[1], a[2]), (x, a[1], a[2]), (x, a[1], zl),
            (x, b[1], zl), (x, b[1], b[2]), (b[0] - PLUG_TAIL, b[1], b[2])]

def inter_rack_points(ya, yb, j, z=None):
    z = UPPER_PORT_Z if z is None else z
    x = -(.71 + j * .012); top = 2.48 + j * .012; t = SWITCH_PORT_X - PLUG_TAIL
    return [(t, ya, z), (x, ya, z), (x, ya, top), (x, yb, top), (x, yb, z), (t, yb, z)]

# Coolant: every liquid-cooled tray (compute and NVLink switch) has the same rear QD pair as the compute
# node and one manifold tee pair at its height.
QD_X, MANIFOLD_QD_X, MANIFOLD_X = -.365, -.591, -.65
QD_DY, MANIFOLD_DY = .197, .245
def liquid_trays():
    """(name, z) bottom-up for every liquid-cooled tray; manifold tee index = position in this list + 1."""
    out = [(f'Node{n + 1:02}', z) for n, z in enumerate(COMPUTE_Z)] + [(f'NVSwitch{k + 1:02}', z) for k, z in enumerate(SWITCHTRAY_Z)]
    return sorted(out, key=lambda t: t[1])

def move_node_io_to_rear(co):
    """Edit the shared ASSET_ComputeNode collection in place: yaw each IO cage 180 deg onto the rear
    cross-member and cut matching apertures through it. Idempotent (skips if already on the rear)."""
    import bpy, math
    from mathutils import Matrix, Vector
    obj = {o.name.split('.')[0] if o.name.startswith('ComputeIO_') else o.name: o for o in co.objects}
    def centre(o):
        ws = [o.matrix_world @ Vector(c) for c in o.bound_box]; return sum(ws, Vector()) / 8
    if centre(obj['ComputeIO_00_Cage']).x < 0: return False
    for k in range(8):
        parts = [obj[f'ComputeIO_{k:02}_{p}'] for p in ('Cage', 'Recess', 'Contacts')]
        c = centre(parts[0]); target = Vector((-c.x, NODE_IO_Y[k], c.z))
        m = Matrix.Translation(target) @ Matrix.Rotation(math.pi, 4, 'Z') @ Matrix.Translation(-c)
        for o in parts: o.matrix_world = m @ o.matrix_world
    member = obj['RearCrossMember']
    import bmesh
    bm = bmesh.new()
    for y in NODE_IO_Y:  # recess-sized apertures; the cage lip frames the cut edge
        r = bmesh.ops.create_cube(bm, size=1)
        bmesh.ops.scale(bm, vec=(.03, .024, .013), verts=r['verts'])
        bmesh.ops.translate(bm, vec=(-.415, y, .023), verts=r['verts'])
    cutter_me = bpy.data.meshes.new('IO_Aperture_Cutter'); bm.to_mesh(cutter_me); bm.free()
    cutter = bpy.data.objects.new('IO_Aperture_Cutter', cutter_me)
    scene = bpy.context.scene; scene.collection.objects.link(cutter)
    linked = co.name not in scene.collection.children
    if linked: scene.collection.children.link(co)  # modifiers only evaluate for objects in the view layer
    # Bake bevel + boolean into the member mesh so the apertures ship in the FBX.
    mod = member.modifiers.new('IO apertures', 'BOOLEAN'); mod.operation = 'DIFFERENCE'; mod.object = cutter; mod.solver = 'EXACT'
    bpy.context.view_layer.update(); deps = bpy.context.evaluated_depsgraph_get()
    baked = bpy.data.meshes.new_from_object(member.evaluated_get(deps), depsgraph=deps)
    old = member.data; member.modifiers.clear(); member.data = baked; baked.name = old.name + '_Apertures'
    if linked: scene.collection.children.unlink(co)
    bpy.data.objects.remove(cutter, do_unlink=True); bpy.data.meshes.remove(cutter_me)
    return True
