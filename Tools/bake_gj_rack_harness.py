"""Blender batch: bake fixed in-rack harnesses, preserving separate sled visibility.
Also re-exports the compute node (rear data IO) to runtime_exports/ExistingGB200.
Curves and editable source stay in Art/GJ_PIPE&CABLE; inter-rack routes remain UE splines.
"""
import bpy,bmesh,json,math
from pathlib import Path
from mathutils import Matrix,Vector
import sys
ROOT=Path(__file__).resolve().parents[1]
ART=ROOT/'Art/GJ_PIPE&CABLE'
sys.path.insert(0,str(ART/'scripts'))
import rear_data_layout as L
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
# One harness per liquid-cooled tray: compute trays carry QD hoses + the data patch, NVLink switch trays QD hoses.
jobs=[(f'R01.Node{n+1:02}.',f'SM_GJ_Harness_{n:02}') for n in range(len(L.COMPUTE_Z))]
jobs+=[(f'R01.NVSwitch{k+1:02}.',f'SM_GJ_SwitchHarness_{k:02}') for k in range(len(L.SWITCHTRAY_Z))]
for prefix,name in jobs:
    parts=[]
    for r in m['routes']:
        if not r['start'].startswith(prefix): continue
        src=bpy.data.objects[r['name']]
        parts.append(evalcopy(src,Matrix.Translation(offset)@src.matrix_world))
        for end in ['start','end']:
            e=m['endpoints'][r[end]]; p=Vector(e['position_blender_m'])+offset; d=e['outward_blender']; role=e['role']
            asset=('SM_Plug_Compute' if end=='start' else 'SM_Plug_Switch') if role=='Data' else 'SM_QD_'+role
            parts+=fitting(asset,p,d)
    stats[name]=save(name,parts)
# Fixed 50 mm manifolds: no baked row header, so room routing can use real CDU locations.
parts=[]
for sign,color in [(-1,'Supply'),(1,'Return')]:
    y=sign*.245
    cu=bpy.data.curves.new('Manifold','CURVE'); cu.dimensions='3D'; cu.bevel_depth=.025; cu.bevel_resolution=3; cu.use_fill_caps=True
    sp=cu.splines.new('POLY'); sp.points.add(1)
    tees=[z+.024 for _,z in L.liquid_trays()]
    sp.points[0].co=(-.65,y,min(tees)-.076,1); sp.points[1].co=(-.65,y,max(tees)+.10,1)
    cu.materials.append(bpy.data.materials['MC_'+color]); ob=bpy.data.objects.new('Manifold',cu); scene.collection.objects.link(ob)
    bpy.context.window.scene=scene; bpy.context.view_layer.update(); parts.append(evalcopy(ob,Matrix.Identity(4)))
    for z in tees:parts+=fitting('SM_Manifold_Tee',(-.65,y,z))
stats['SM_GJ_Manifolds']=save('SM_GJ_Manifolds',parts)
# 1U blank filler plate (19-inch, 1.5 mm steel, two captive screws). Origin: rail face, bottom centre.
# Instanced by the rack / spine cabinet into every empty U; scaled in Z when a gap is not a whole U.
def blank_part(name,mesh_fn,mat):
    bm=bmesh.new(); mesh_fn(bm); me=bpy.data.meshes.new(name); bm.to_mesh(me); bm.free()
    me.materials.append(bpy.data.materials[mat]); ob=bpy.data.objects.new(name,me); export.collection.objects.link(ob); return ob
def plate(bm):
    r=bmesh.ops.create_cube(bm,size=1); bmesh.ops.scale(bm,vec=(.0015,.4826,.0439),verts=r['verts'])
    bmesh.ops.translate(bm,vec=(.00075,0,.0225),verts=r['verts'])
def screws(bm):
    for y in (-.2335,.2335):
        r=bmesh.ops.create_cone(bm,cap_ends=True,segments=16,radius1=.0032,radius2=.0032,depth=.002)
        bmesh.ops.rotate(bm,verts=r['verts'],cent=(0,0,0),matrix=Matrix.Rotation(math.pi/2,3,'Y'))
        bmesh.ops.translate(bm,vec=(.0025,y,.0225),verts=r['verts'])
stats['SM_GJ_Blank1U']=save('SM_GJ_Blank1U',[blank_part('Plate',plate,'MC_Dark'),blank_part('Screws',screws,'MC_Steel')])
# Rack devices: compute node (data IO on the rear cross-member), NVLink switch tray, power shelf.
# Keep authored UVs; only bake modifiers and units. Materials are the ExistingGB200 set.
(OUT/'ExistingGB200').mkdir(exist_ok=True)
for asset,mesh_name in [('ASSET_ComputeNode','SM_GB200_ComputeNode'),('ASSET_NVSwitchTray','SM_GJ_NVSwitchTray'),('ASSET_PowerShelf','SM_GJ_PowerShelf')]:
    bpy.context.window.scene=scene
    node=bpy.data.collections[asset]; scene.collection.children.link(node); bpy.context.view_layer.update()
    parts=[evalcopy(o,o.matrix_world.copy()) for o in node.objects if o.type=='MESH']
    scene.collection.children.unlink(node)
    bpy.context.window.scene=export; bpy.ops.object.select_all(action='DESELECT')
    for o in parts:o.select_set(True)
    bpy.context.view_layer.objects.active=parts[0]; bpy.ops.object.join(); o=parts[0]; o.name=mesh_name
    if not o.data.uv_layers:
        o.data.uv_layers.new(name='UV0'); bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.uv.smart_project(island_margin=.002); bpy.ops.object.mode_set(mode='OBJECT')
    mod=o.modifiers.new('Triangles','TRIANGULATE'); bpy.ops.object.modifier_apply(modifier=mod.name)
    stats[o.name]=len(o.data.polygons); o.data.transform(Matrix.Scale(100,4)); export.unit_settings.scale_length=.01
    bpy.ops.export_scene.fbx(filepath=str(OUT/f'ExistingGB200/{mesh_name}.fbx'),use_selection=True,object_types={'MESH'},axis_forward='-Y',axis_up='Z',apply_unit_scale=True,apply_scale_options='FBX_SCALE_NONE',bake_space_transform=False,mesh_smooth_type='FACE',bake_anim=False)
    bpy.data.objects.remove(o,do_unlink=True)
(OUT/'manifest.json').write_text(json.dumps({'triangles':stats,'units':'baked centimetres; source metres','origin':'rack floor centre','source':'../GB200_Connections.blend'},indent=2),encoding='utf8')
print('RACK_HARNESS_BAKE_PASS',len(stats))
