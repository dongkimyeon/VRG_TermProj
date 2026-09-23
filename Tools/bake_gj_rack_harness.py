"""Blender batch: bake fixed in-rack harnesses, preserving separate sled visibility.
Curves and editable source stay in Art/GJ_PIPE&CABLE; inter-rack routes remain UE splines.
"""
import bpy,json,math
from pathlib import Path
from mathutils import Matrix,Vector
ROOT=Path(__file__).resolve().parents[1]
ART=ROOT/'Art/GJ_PIPE&CABLE'
OUT=ART/'runtime_exports'; OUT.mkdir(exist_ok=True)
bpy.ops.wm.open_mainfile(filepath=str(ART/'GB200_Connections.blend'))
scene=bpy.data.scenes['01_Connected_Racks']; bpy.context.window.scene=scene
m=json.loads((ART/'reports/connection_manifest.json').read_text(encoding='utf8'))
export=bpy.data.scenes.new('Runtime_Bake'); export.unit_settings.system='METRIC'
def evalcopy(src,transform):
    bpy.context.window.scene=scene; deps=bpy.context.evaluated_depsgraph_get()
    me=bpy.data.meshes.new_from_object(src.evaluated_get(deps),depsgraph=deps)
    ob=bpy.data.objects.new(src.name+'_bake',me); export.collection.objects.link(ob); ob.matrix_world=transform; return ob
def fitting(asset,p,d=(1,0,0)):
    q=Vector(d).to_track_quat('X','Z'); tr=Matrix.Translation(Vector(p))@q.to_matrix().to_4x4()
    return [evalcopy(o,tr@o.matrix_world) for o in bpy.data.collections[asset].objects]
def save(name,parts):
    bpy.context.window.scene=export; bpy.ops.object.select_all(action='DESELECT')
    for o in parts:o.select_set(True)
    bpy.context.view_layer.objects.active=parts[0]; bpy.ops.object.join(); o=parts[0]; o.name=name
    export.cursor.location=(0,0,0); bpy.ops.object.origin_set(type='ORIGIN_CURSOR'); bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    mod=o.modifiers.new('Triangles','TRIANGULATE'); bpy.ops.object.modifier_apply(modifier=mod.name)
    while o.data.uv_layers:o.data.uv_layers.remove(o.data.uv_layers[0])
    o.data.uv_layers.new(name='UV0'); bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(island_margin=.002); bpy.ops.object.mode_set(mode='OBJECT')
    count=len(o.data.polygons); o.data.transform(Matrix.Scale(100,4)); export.unit_settings.scale_length=.01
    bpy.ops.export_scene.fbx(filepath=str(OUT/(name+'.fbx')),use_selection=True,object_types={'MESH'},axis_forward='-Y',axis_up='Z',apply_unit_scale=True,apply_scale_options='FBX_SCALE_NONE',bake_space_transform=False,mesh_smooth_type='FACE',bake_anim=False)
    bpy.data.objects.remove(o,do_unlink=True); return count
stats={}; offset=Vector((0,.43,0))
for n in range(18):
    prefix=f'R01.Node{n+1:02}.'; parts=[]
    for r in m['routes']:
        if not r['start'].startswith(prefix): continue
        src=bpy.data.objects[r['name']]
        parts.append(evalcopy(src,Matrix.Translation(offset)@src.matrix_world))
        for end in ['start','end']:
            e=m['endpoints'][r[end]]; p=Vector(e['position_blender_m'])+offset; d=e['outward_blender']; role=e['role']
            asset=('SM_Plug_Compute' if end=='start' else 'SM_Plug_Switch') if role=='Data' else 'SM_QD_'+role
            parts+=fitting(asset,p,d)
    name=f'SM_GJ_Harness_{n:02}'; stats[name]=save(name,parts)
# Fixed 50 mm manifolds: no baked row header, so room routing can use real CDU locations.
parts=[]
for sign,color in [(-1,'Supply'),(1,'Return')]:
    y=sign*.245
    cu=bpy.data.curves.new('Manifold','CURVE'); cu.dimensions='3D'; cu.bevel_depth=.025; cu.bevel_resolution=3; cu.use_fill_caps=True
    sp=cu.splines.new('POLY'); sp.points.add(1)
    sp.points[0].co=(-.65,y,.34,1); sp.points[1].co=(-.65,y,1.34,1)
    cu.materials.append(bpy.data.materials['MC_'+color]); ob=bpy.data.objects.new('Manifold',cu); scene.collection.objects.link(ob)
    bpy.context.window.scene=scene; bpy.context.view_layer.update(); parts.append(evalcopy(ob,Matrix.Identity(4)))
    for n in range(18):parts+=fitting('SM_Manifold_Tee',(-.65,y,.416+n*.04445))
stats['SM_GJ_Manifolds']=save('SM_GJ_Manifolds',parts)
(OUT/'manifest.json').write_text(json.dumps({'triangles':stats,'units':'baked centimetres; source metres','origin':'rack floor centre','source':'../GB200_Connections.blend'},indent=2),encoding='utf8')
print('RACK_HARNESS_BAKE_PASS',len(stats))
