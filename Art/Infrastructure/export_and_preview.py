"""Run against VRG_Connections.blend with Blender --background --python.

Exports evaluated meshes while leaving the editable source .blend unchanged.
Reimports the FBX in a temporary scene and checks scale and geometry bounds.
"""
import bpy
import json
from pathlib import Path
from mathutils import Vector

OUT=Path(__file__).resolve().parent
source=bpy.data.scenes['VRG_Infrastructure']
bpy.context.window.scene=source

# Preview cameras use the source curves at their authoring resolution.
for camera,filename in [('CAM_Overview','overview.png'),('CAM_Rack_Detail','rack_detail.png'),('CAM_Pipe_Detail','pipe_detail.png')]:
    source.camera=bpy.data.objects[camera]
    source.render.filepath=str(OUT/filename)
    bpy.ops.render.render(write_still=True)
source.camera=bpy.data.objects['CAM_Overview']

# Reduced tessellation is for the exchange files only.
original=[]
for ob in source.objects:
    if ob.type=='CURVE':
        original.append((ob.data,ob.data.resolution_u,ob.data.bevel_resolution))
        ob.data.resolution_u=4
        ob.data.bevel_resolution=2 if ob.get('role') in ('SUPPLY','RETURN') else 1
bpy.context.view_layer.update()
dg=bpy.context.evaluated_depsgraph_get()
export=bpy.data.scenes.new('VRG_Exchange_Temporary')
export.unit_settings.system='METRIC'; export.unit_settings.scale_length=1
rows=[]
for ob in source.objects:
    if ob.type not in ('MESH','CURVE','FONT'): continue
    ev=ob.evaluated_get(dg)
    mesh=bpy.data.meshes.new_from_object(ev,preserve_all_data_layers=True,depsgraph=dg)
    if not mesh.vertices:
        bpy.data.meshes.remove(mesh); continue
    copied=bpy.data.objects.new(ob.name+'_Mesh',mesh)
    copied.matrix_world=ob.matrix_world.copy()
    for key in ('role','diameter_m','endpoint_start','endpoint_end','rack_id'):
        if key in ob: copied[key]=ob[key]
    export.collection.objects.link(copied)
    group=next((c.name for c in ob.users_collection if c.name.startswith('VRG_')),'')
    include=group not in ('VRG_08_Presentation','VRG_01_RackContext') or '_CDU_Cabinet' in ob.name
    rows.append((copied,include))
for cu,res,bev in original: cu.resolution_u=res; cu.bevel_resolution=bev
bpy.context.window.scene=export
bpy.context.view_layer.update()

def bounds(objects):
    pts=[o.matrix_world@Vector(v) for o in objects for v in o.bound_box]
    return [[min(p[i] for p in pts) for i in range(3)], [max(p[i] for p in pts) for i in range(3)]]

selected=[o for o,include in rows if include]
for o,include in rows: o.select_set(include)
expected=bounds(selected)
bpy.ops.export_scene.fbx(filepath=str(OUT/'VRG_Infrastructure.fbx'),use_selection=True,
    object_types={'MESH'},apply_unit_scale=True,apply_scale_options='FBX_SCALE_UNITS',
    axis_forward='-Y',axis_up='Z',use_mesh_modifiers=True,mesh_smooth_type='FACE',
    use_custom_props=True,bake_anim=False)

# Use the operator default GLB format after checking the output at the file level.
for ob,_ in rows: ob.select_set(True)
bpy.ops.export_scene.gltf(filepath=str(OUT/'VRG_Connections.glb'),use_selection=True,
    export_extras=True,export_yup=True,export_apply=True)
assert (OUT/'VRG_Connections.glb').read_bytes()[:4]==b'glTF'

triangles=0
for ob,_ in rows:
    ob.data.calc_loop_triangles(); triangles+=len(ob.data.loop_triangles)

check=bpy.data.scenes.new('VRG_FBX_Roundtrip')
check.unit_settings.system='METRIC'; check.unit_settings.scale_length=1
bpy.context.window.scene=check
bpy.ops.import_scene.fbx(filepath=str(OUT/'VRG_Infrastructure.fbx'))
bpy.context.view_layer.update()
imported=[o for o in check.objects if o.type=='MESH']
actual=bounds(imported)
error=max(abs(actual[i][j]-expected[i][j]) for i in range(2) for j in range(3))
assert len(imported)==len(selected),(len(imported),len(selected))
assert error<1e-4,('FBX scale / bounds mismatch in metres',error)
report={'source_scene':source.name,'fbx_objects':len(selected),'fbx_reimported_objects':len(imported),
        'bounds_expected_m':expected,'bounds_reimported_m':actual,'bounds_max_error_m':error,
        'glb_mesh_objects':len(rows),'glb_triangles':triangles,
        'source_curve_count':len(original),'unit_scale_m':1,
        'files':{name:(OUT/name).stat().st_size for name in ('VRG_Infrastructure.fbx','VRG_Connections.glb')},
        'limitations':['No Unreal import or runtime performance test','No solver or Unreal SplineComponent binding',
                      'Solid visualization surfaces, not CFD fluid volumes','No full scene collision clearance test',
                      'PBR solid colors, no texture UV authoring; add LOD/collision/lightmap UVs as needed']}
(OUT/'verification.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
print('VRG_EXPORT_VERIFIED',json.dumps(report))
