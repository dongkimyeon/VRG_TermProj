"""Local authoring kit. Reference: VRG Art/Infrastructure (5ec749b).
No project files changed. Metres, +X front. Run in a NEW background Blender.
"""
import bpy, math, json, sys, ast
from pathlib import Path
from mathutils import Vector, Matrix
ROOT=Path(__file__).resolve().parents[1]
BASE=ROOT.parent/'GB200_NVL72_Models_20260922'
REPO=Path('C:/GitGit/VRG_TermProj')
for folder in ['exports','previews','reports']: (ROOT/folder).mkdir(exist_ok=True)
bpy.ops.wm.read_factory_settings(use_empty=True)
sc=bpy.context.scene; sc.name='01_Connected_Racks'; sc.unit_settings.system='METRIC'
def collection(name):
    c=bpy.data.collections.new(name); sc.collection.children.link(c); return c
context=collection('01_Existing_GB200_Models')
cold=collection('02_Supply'); hot=collection('03_Return'); data=collection('04_Data_Patches')
hardware=collection('05_Connectors_Supports'); stage=collection('06_Presentation')
palette={'Supply':((.018,.29,.55),.4,.3),'Return':((.62,.045,.025),.4,.3),
 'Cyan':((.025,.55,.7),.05,.39),'Lime':((.35,.72,.015),.05,.4),
 'Rubber':((.014,.019,.024),.05,.55),'Steel':((.46,.53,.59),.83,.3),
 'Brass':((.43,.31,.12),.8,.3),'Dark':((.025,.035,.046),.6,.35),
 'White':((.75,.82,.9),.1,.4),'Floor':((.105,.135,.18),.15,.58)}
mats={}
for name,(rgb,metal,rough) in palette.items():
    m=bpy.data.materials.new('MC_'+name); m.diffuse_color=(*rgb,1); m.use_nodes=True
    p=m.node_tree.nodes.get('Principled BSDF'); p.inputs['Base Color'].default_value=(*rgb,1)
    p.inputs['Metallic'].default_value=metal; p.inputs['Roughness'].default_value=rough; mats[name]=m
def box(name,pos,size,mat,co=hardware,bevel=.001):
    bpy.ops.mesh.primitive_cube_add(size=1,location=pos); ob=bpy.context.object; ob.name=name
    for c in list(ob.users_collection): c.objects.unlink(ob)
    co.objects.link(ob); ob.scale=size; bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    ob.data.materials.append(mats[mat])
    if bevel:
        mod=ob.modifiers.new('Edge finish','BEVEL'); mod.width=bevel; mod.segments=2
        mod=ob.modifiers.new('Normals','WEIGHTED_NORMAL'); mod.keep_sharp=True
    return ob
def cylinder(name,pos,r,length,mat,co=hardware,axis='X',n=24):
    bpy.ops.mesh.primitive_cylinder_add(vertices=n,radius=r,depth=length,location=pos)
    ob=bpy.context.object; ob.name=name
    for c in list(ob.users_collection): c.objects.unlink(ob)
    co.objects.link(ob); ob.data.materials.append(mats[mat])
    if axis=='X': ob.rotation_euler[1]=math.pi/2
    if axis=='Y': ob.rotation_euler[0]=math.pi/2
    for p in ob.data.polygons: p.use_smooth=len(p.vertices)==4
    return ob
# Standalone copy of the reference path function, preserving provenance.
sys.path.insert(0,str(ROOT/"scripts"))
from reference_route import route
import rear_data_layout as L
routes=[]; endpoints={}; assets={}
def endpoint(name,p,d,role):
    endpoints[name]={'position_blender_m':list(p),'outward_blender':list(d),'role':role}
def path(name,points,diam,mat,co,start='',end='',bend=.07):
    ob=route(name,points,diam/2,mats[mat],co,bend,mat,start,end)
    routes.append({'name':name,'role':mat,'diameter_m':diam,'start':start,'end':end,
      'bezier': [{'p':list(b.co),'left':list(b.handle_left),'right':list(b.handle_right)} for b in ob.data.splines[0].bezier_points]})
    return ob
def smooth(name,points,diam,mat,co,start='',end=''):
    cu=bpy.data.curves.new(name,'CURVE'); cu.dimensions='3D'; cu.resolution_u=16
    cu.bevel_depth=diam/2; cu.bevel_resolution=3; cu.use_fill_caps=True
    sp=cu.splines.new('BEZIER'); sp.bezier_points.add(len(points)-1)
    for b,p in zip(sp.bezier_points,points): b.co=p; b.handle_left_type='AUTO'; b.handle_right_type='AUTO'
    ob=bpy.data.objects.new(name,cu); co.objects.link(ob); cu.materials.append(mats[mat])
    ob['endpoint_start']=start; ob['endpoint_end']=end; ob['role']=mat
    bpy.context.view_layer.update()
    routes.append({'name':name,'role':mat,'diameter_m':diam,'start':start,'end':end,
      'bezier':[{'p':list(b.co),'left':list(b.handle_left),'right':list(b.handle_right)} for b in sp.bezier_points]})
    return ob
def asset(name,fn,note):
    co=bpy.data.collections.new(name); sc.collection.children.link(co); fn(co)
    assets[name]={'collection':co,'note':note}; return co
def body(co,name,r,material):
    # Dense longitudinal rings: one metre, 50 segments, no end caps at spline joins.
    vs=[]; fs=[]; sides=16; rings=51
    for i in range(rings):
        for j in range(sides):
            a=2*math.pi*j/sides; vs.append((i/(rings-1),r*math.cos(a),r*math.sin(a)))
    for i in range(rings-1):
        for j in range(sides): fs.append((i*sides+j,i*sides+(j+1)%sides,(i+1)*sides+(j+1)%sides,(i+1)*sides+j))
    me=bpy.data.meshes.new(name); me.from_pydata(vs,[],fs); me.update()
    ob=bpy.data.objects.new(name,me); co.objects.link(ob); me.materials.append(mats[material])
    uv=me.uv_layers.new(name='UV0')
    for p in me.polygons:
        p.use_smooth=True; i=p.index//sides; j=p.index%sides
        for li,val in zip(p.loop_indices,[(i/50,j/16),(i/50,(j+1)/16),((i+1)/50,(j+1)/16),((i+1)/50,j/16)]): uv.data[li].uv=val
    return ob
for name,r,material in [('Data',.0024,'Cyan'),('Hose',.0065,'Rubber'),('Power',.0055,'Rubber'),('Riser',.025,'Supply'),('Header',.055,'Supply')]:
    asset('SM_Connection_'+name,lambda co,n=name,r=r,m=material:body(co,n,r,m),'X=0..1 m; spline forward X; 50 spans; open ends; diameter '+str(2*r)+' m')
def qd(co,color):
    for x,r,length,mat in [(.008,.005,.016,'Steel'),(.023,.0085,.014,'Steel'),(.034,.0088,.005,color),(.044,.0065,.015,'Rubber')]: cylinder('QD_'+mat,(x,0,0),r,length,mat,co)
    for i in range(5): cylinder('Grip',(.018+i*.002,.0,.0),.0088,.0008,'Dark',co)
for color in ['Supply','Return']: asset('SM_QD_'+color,lambda co,c=color:qd(co,c),'Nose at origin, hose tail X=.0515 m. Generic art coupling matching existing 10 mm node stub.')
def plug(co,width):
    box('Metal insert',(.006,0,0),(.012,width,.009),'Steel',co,.0003)
    box('Shell',(.021,0,0),(.018,width+.001,.0095),'Dark',co,.0007)
    box('Release tab',(.021,0,.0057),(.013,width*.45,.0012),'Lime',co,.0002)
    cylinder('Boot',(.037,0,0),.0037,.014,'Rubber',co)
    for i in range(4): cylinder('Boot rib',(.031+i*.003,0,0),.004,.001,'Rubber',co)
asset('SM_Plug_Compute',lambda c:plug(c,.022),'Generic 22x9 mm insertion head for 24x13 mm model aperture; not certified connector specification.')
asset('SM_Plug_Switch',lambda c:plug(c,.017),'Generic 17x9 mm insertion head for 19x12 mm model aperture. Tail X=.044 m.')
def adapter(co):
    cylinder('CDU sleeve',(.016,0,0),.060,.032,'Steel',co)
    cylinder('Header socket',(.047,0,0),.055,.032,'Supply',co)
    for x in [.003,.028]: cylinder('Retaining ring',(x,0,0),.064,.006,'Steel',co)
asset('SM_CDU_Adapter',adapter,'Fits project visual CDU 120 mm port to 110 mm header; generic estimated coupling, tail .063 m.')
def tee(co):
    cylinder('Run',(0,0,0),.025,.032,'Steel',co,axis='Z')
    cylinder('Branch',(.019,0,0),.011,.055,'Steel',co)
    cylinder('Tip',(.05,0,0),.005,.018,'Steel',co)
asset('SM_Manifold_Tee',tee,'50 mm vertical manifold with generic 10 mm branch stub +X; branch tip X=.059 m.')
def valve(co):
    cylinder('Valve body',(.04,0,0),.031,.08,'Brass',co)
    for x in [.003,.077]: cylinder('Union',(x,0,0),.034,.016,'Steel',co)
    cylinder('Stem',(.04,0,.035),.007,.036,'Steel',co,axis='Z')
    box('Lever',(.072,0,.055),(.09,.016,.008),'Dark',co,.002)
asset('SM_IsolationValve',valve,'50 mm nominal art branch; rigid, never spline-deform valve.')
def instance_asset(name,p,d=(1,0,0),label=None):
    ob=bpy.data.objects.new(label or name,None); hardware.objects.link(ob)
    ob.instance_type='COLLECTION'; ob.instance_collection=assets[name]['collection']; ob.location=p
    ob.rotation_euler=Vector(d).to_track_quat('X','Z').to_euler(); return ob
# Export individual assets while still at the origin; evaluated modifiers, cm baked for UE reimport.
export=bpy.data.scenes.new('Temporary_Export'); export.unit_settings.system='METRIC'; stats={}
for name,entry in assets.items():
    bpy.context.window.scene=sc; bpy.context.view_layer.update(); deps=bpy.context.evaluated_depsgraph_get(); copies=[]
    for src in entry['collection'].objects:
        me=bpy.data.meshes.new_from_object(src.evaluated_get(deps),depsgraph=deps)
        ob=bpy.data.objects.new(name+'_part',me); export.collection.objects.link(ob); ob.matrix_world=src.matrix_world.copy(); copies.append(ob)
    bpy.context.window.scene=export; bpy.ops.object.select_all(action='DESELECT')
    for ob in copies: ob.select_set(True)
    bpy.context.view_layer.objects.active=copies[0]; bpy.ops.object.join(); ob=copies[0]; ob.name=name
    export.cursor.location=(0,0,0); bpy.ops.object.origin_set(type='ORIGIN_CURSOR'); bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    if not name.startswith('SM_Connection_'):
        while ob.data.uv_layers: ob.data.uv_layers.remove(ob.data.uv_layers[0])
        ob.data.uv_layers.new(name='UV0'); bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
        bpy.ops.uv.smart_project(island_margin=.005); bpy.ops.object.mode_set(mode='OBJECT')
    mod=ob.modifiers.new('Triangles','TRIANGULATE'); bpy.ops.object.modifier_apply(modifier=mod.name)
    stats[name]={'bounds_m':[[min(v.co[a] for v in ob.data.vertices) for a in range(3)],[max(v.co[a] for v in ob.data.vertices) for a in range(3)]],
      'triangles':len(ob.data.polygons),'note':entry['note'],'materials':[m.name for m in ob.data.materials]}
    bpy.ops.export_scene.gltf(filepath=str(ROOT/'exports'/f'{name}.glb'),use_selection=True,export_format='GLB')
    ob.data.transform(Matrix.Scale(100,4)); export.unit_settings.scale_length=.01
    bpy.ops.export_scene.fbx(filepath=str(ROOT/'exports'/f'{name}.fbx'),use_selection=True,object_types={'MESH'},axis_forward='-Y',axis_up='Z',apply_unit_scale=True,apply_scale_options='FBX_SCALE_NONE',bake_space_transform=False,mesh_smooth_type='FACE',bake_anim=False)
    bpy.data.objects.remove(ob,do_unlink=True)
bpy.context.window.scene=sc; bpy.data.scenes.remove(export)
# Asset collections are instanced, not rendered directly at their authoring origin.
for entry in assets.values(): sc.collection.children.unlink(entry['collection'])
with bpy.data.libraries.load(str(BASE/'GB200_Modular_Kit.blend'),link=False) as (src,dst):
    dst.collections=[n for n in src.collections if n in ['ASSET_RackFrame','ASSET_ComputeNode','ASSET_QuantumSwitch']]
original={co.name:co for co in dst.collections}
for co in original.values():
    for ob in co.objects:
        if ob.parent is None: ob.location=(0,0,0)
L.move_node_io_to_rear(original['ASSET_ComputeNode'])  # data IO on the rear, with the coolant
def existing(co,p,label,yaw=0):
    ob=bpy.data.objects.new(label,None); context.objects.link(ob); ob.instance_type='COLLECTION'; ob.instance_collection=original[co]; ob.location=p
    ob.rotation_euler=(0,0,math.radians(yaw))
for rack,yc in [('R01',-.43),('R02',.43)]:
    existing('ASSET_RackFrame',(0,yc,0),rack)
    existing('ASSET_QuantumSwitch',(L.SWITCH_POS[0],yc,L.SWITCH_POS[1]),rack+'_Switch',L.SWITCH_YAW_DEG)
    for n in range(18):
        z=.392+n*.04445; existing('ASSET_ComputeNode',(.055,yc,z),rack+f'_Node{n+1:02}')
        for sign,color,co in [(-1,'Supply',cold),(1,'Return',hot)]:
            yp=yc+sign*.197; ym=yc+sign*.245
            e1=f'{rack}.Node{n+1:02}.{color}'; e2=f'{rack}.Manifold.{color}.{n+1:02}'
            a=(-.365,yp,z+.024); b=(-.591,ym,z+.024)
            endpoint(e1,a,(-1,0,0),color); endpoint(e2,b,(1,0,0),color)
            instance_asset('SM_QD_'+color,a,(-1,0,0)); instance_asset('SM_Manifold_Tee',(-.65,ym,z+.024))
            instance_asset('SM_QD_'+color,b)
            # Service loop runs in free rear space, outside body and side panels.
            smooth(e1+'_Hose',[(-.4165,yp,z+.024),(-.46,yp,z+.024),(-.49,yp+sign*.023,z-.010),(-.515,ym,z+.024),(-.5395,ym,z+.024)],.013,'Rubber',co,e1,e2)
        # Data on the rear: vertical ribbon in the centre channel between the two manifolds.
        a=L.node_data_port(yc,n); b=L.switch_lower_port(yc,n+1)
        e1=f'{rack}.Node{n+1:02}.Data01'; e2=f'{rack}.Switch.Lower{n+1:02}'
        endpoint(e1,a,L.OUT,'Data'); endpoint(e2,b,L.OUT,'Data')
        instance_asset('SM_Plug_Compute',a,L.OUT); instance_asset('SM_Plug_Switch',b,L.OUT)
        path(e1+'_Patch',L.patch_points(yc,n),.0048,'Lime',data,e1,e2,.04)
    for sign,color,co in [(-1,'Supply',cold),(1,'Return',hot)]:
        ym=yc+sign*.245; hx=-1.0 if sign<0 else -.82
        riser_points=[(-.65,ym,.34),(-.65,ym,1.34)] if sign<0 else [(hx,ym,.25),(-.65,ym,.25),(-.65,ym,1.34)]
        path(rack+'_'+color+'_Riser',riser_points,.05,color,co,bend=.075)
        if sign<0:
            path(rack+'_Supply_Feed',[(hx,ym,.25),(hx,ym,.45),(-.65,ym,.45)],.05,color,co,bend=.075)
        # End cap / shutoff hardware. Attached visual pipe topology only.
        cylinder('Manifold cap',(-.65,ym,1.347),.029,.014,'Steel',axis='Z')
        instance_asset('SM_IsolationValve',(hx+.10,ym,.45) if sign<0 else (-.65,ym,.27),(1,0,0) if sign<0 else (0,0,1))
        for zz in [.36,1.22]:
            box('Rear manifold bracket',(-.58,ym,zz),(.16,.02,.018),'Steel')
            cylinder('Manifold band',(-.65,ym,zz),.028,.018,'Steel',axis='Z')
# Row headers & current project CDU proxy: actor local ports (+.45, +/-.15, .25).
# Blender Y mirrored from UE for the explicitly named supply / return ports.
cdu=(-1.85,-1.2,0)
box('CDU project size proxy',(-1.85,-1.2,.95),(.9,.8,1.9),'Dark',context,.01)
box('CDU trim',(-1.394,-1.2,1.56),(.012,.64,.114),'Brass',context)
for color,co,yy,hx in [('Supply',cold,-1.05,-1.0),('Return',hot,-1.35,-.82)]:
    p=(-1.40,yy,.25); endpoint('CDU.'+color,p,(1,0,0),color)
    cylinder('CDU '+color,p,.060,.025,color)
    instance_asset('SM_CDU_Adapter',p)
    path('CDU_'+color+'_Header',[(-1.337,yy,.25),(hx,yy,.25),(hx,1.0,.25)],.11,color,co,'CDU.'+color,'Row.'+color,.16)
    cylinder('Header end cap',(hx,1.0,.25),.059,.025,'Steel',axis='Y')
    for yy2 in [-.8,0,.8]:
        cylinder('Header coupling',(hx,yy2,.25),.060,.026,'Steel',axis='Y')
        box('Pipe foot',(hx,yy2,.06),(.16,.12,.025),'Dark'); box('Pipe saddle',(hx,yy2,.13),(.04,.03,.13),'Steel')
# Four independent switch-to-switch links from the rear upper ports, in the hot aisle behind the racks.
for j in range(4):
    a=L.switch_upper_port(-.43,j+11); b=L.switch_upper_port(.43,j+11)
    ea=f'R01.Switch.Upper{j+11:02}'; eb=f'R02.Switch.Upper{j+11:02}'
    endpoint(ea,a,L.OUT,'Data'); endpoint(eb,b,L.OUT,'Data')
    instance_asset('SM_Plug_Switch',a,L.OUT); instance_asset('SM_Plug_Switch',b,L.OUT)
    path('InterRack_'+str(j+1),L.inter_rack_points(a[1],b[1],j),.0048,'Cyan',data,ea,eb,.10)
# Neutral support tray, based on teammate's ladder-style carrier. Rear, above the data links.
for xx in [-.58,-.85]: box('Tray side',(xx,0,2.43),(.012,1.7,.034),'Steel')
for j in range(13): box('Tray rung',(-.715,-.78+j*.13,2.423),(.27,.016,.014),'Steel')
for yc in [-.43,.43]:
    for xx in [-.58,-.85]: box('Tray stand',(xx,yc,2.327),(.014,.02,.19),'Dark')
box('Platform',(-.42,-.2,-.06),(3.35,3.05,.10),'Floor',stage,.02)
def label(text,p,size=.08):
    cu=bpy.data.curves.new(text,'FONT'); cu.body=text; cu.size=size; cu.align_x='CENTER'; cu.materials.append(mats['White'])
    ob=bpy.data.objects.new(text,cu); stage.objects.link(ob); ob.location=p
    return ob
label('GB200  /  MODULAR CONNECTIONS',(.35,-1.44,.006),.075)
def camera(name,p,target,scale):
    d=bpy.data.cameras.new(name); o=bpy.data.objects.new(name,d); stage.objects.link(o); o.location=p
    o.rotation_euler=(Vector(target)-o.location).to_track_quat('-Z','Y').to_euler(); d.type='ORTHO'; d.ortho_scale=scale; return o
cams=[(camera('CAM_Front',(5,-4,3.8),(-.40,-.20,1.14),4.6),'01_front_connections.png'),
 (camera('CAM_Rear',(-5,3,3.0),(-.7,-.2,1.08),4.25),'02_rear_cooling.png'),
 (camera('CAM_Ports',(-2.0,.05,1.28),(-.50,-.43,.95),.95),'03_hose_connectors.png'),
 (camera('CAM_Patch',(-2,-.2,2.0),(-.55,-.43,1.8),.8),'04_switch_patches.png')]
sc.render.engine='CYCLES'; sc.cycles.samples=32; sc.cycles.use_denoising=True
sc.render.resolution_x=1600; sc.render.resolution_y=1200; sc.render.resolution_percentage=100
sc.world=bpy.data.worlds.new('Studio'); sc.world.use_nodes=True
sc.world.node_tree.nodes['Background'].inputs[0].default_value=(.16,.20,.26,1); sc.world.node_tree.nodes['Background'].inputs[1].default_value=.45
for name,p,power,size in [('Key',(3,-4,5),1500,4),('Fill',(-3,1,4),1800,3),('Rim',(1,4,3),1300,3)]:
    d=bpy.data.lights.new(name,'AREA'); d.energy=power; d.shape='DISK'; d.size=size
    o=bpy.data.objects.new(name,d); stage.objects.link(o); o.location=p; o.rotation_euler=(Vector((-.3,0,1))-o.location).to_track_quat('-Z','Y').to_euler()
sc.camera=cams[0][0]
# Second scene: modular originals, each separately selectable at useful display scale.
gallery=bpy.data.scenes.new('02_Modular_Parts'); gallery.unit_settings.system='METRIC'
for i,(name,entry) in enumerate(assets.items()):
    ob=bpy.data.objects.new(name,None); gallery.collection.objects.link(ob); ob.instance_type='COLLECTION'; ob.instance_collection=entry['collection']; ob.location=(0,(i%4)*.35,(i//4)*.25)
    ob['authoring_note']=entry['note']
sc['README']='Editable curves + independent rigid fittings. Sources in scripts/. UE spline actor sample in UnrealSample/. Visual routes do not change the physics graph.'
for screen in bpy.data.screens:
    for area in screen.areas:
        if area.type=='VIEW_3D': area.spaces.active.region_3d.view_perspective='CAMERA'; area.spaces.active.clip_end=100
(ROOT/'reports/connection_manifest.json').write_text(json.dumps({'units':'Blender m; UE cm with Y mirrored', 'source_commit':'5ec749b','palette':palette,'assets':stats,'endpoints':endpoints,'routes':routes,'cdu_local_ue_m':{'Supply':[.45,-.15,.25],'Return':[.45,.15,.25]},'limits':'Visual sample; estimated fittings; no hydraulic or electrical solver binding'},indent=2),encoding='utf8')
bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'GB200_Connections.blend'))
if '--no-render' not in sys.argv:
    for cam,filename in cams:
        sc.camera=cam; sc.render.filepath=str(ROOT/'previews'/filename); bpy.ops.render.render(write_still=True)
print('CONNECTION_KIT_COMPLETE',len(assets),len(routes),len(endpoints))

