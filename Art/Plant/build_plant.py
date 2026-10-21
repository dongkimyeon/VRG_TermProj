"""Plant exteriors, Blender 5.2. +X front, metres, floor-centre origin.
Nominal envelopes are project diagram sizes, NOT catalogue dimensions.
Sources (appearance only) and deviations: README.md / modelling request.
"""
import bpy, bmesh, math, json, sys
from pathlib import Path
from mathutils import Vector, Matrix
OUT=Path(__file__).resolve().parent
PALETTE={
 'MC_Plant_Body':((.68,.72,.73),.45,.32),
 'MC_Plant_Frame':((.045,.060,.071),.65,.34),
 'MC_Plant_Coil':((.24,.29,.29),.75,.40),
 'MC_Plant_Fan':((.012,.018,.022),.30,.38),
 'MC_Plant_Pipe':((.43,.49,.53),.85,.25),
 'MC_Plant_Panel':((.76,.79,.79),.35,.36),
 'MC_Plant_Screen':((.035,.22,.29),.10,.24),
 'MC_Plant_Accent':((.045,.21,.33),.45,.31),
}
SPECS={'DryCooler':((2.4,1.2,1.3),8000),'Chiller':((2.4,1.2,1.8),15000),
 'Pump':((.6,.6,.6),5000),'PlateHx':((.8,.5,1.6),8000),
 'Valve2Way':((.3,.2,.4),3000),'CheckValve':((.3,.2,.2),2000),
 'LeakMarker':((.35,.35,.05),1000)}
PORTS={'DryCooler':[[(-.85,-.6,.65),(0,-1,0)],[(.85,-.6,.65),(0,-1,0)]],
 'Chiller':[[(-.75,-.6,.55),(0,-1,0)],[(.6,-.6,.55),(0,-1,0)],
            [(-.75,.6,.55),(0,1,0)],[(.6,.6,.55),(0,1,0)]],
 'Pump':[[ (.3,0,.3),(1,0,0)],[(.13,0,.6),(0,0,1)]],
 'PlateHx':[[ (.4,y,z),(1,0,0)] for y in (-.12,.12) for z in (.35,1.3)]}

def obj(name,verts,faces,mat):
    me=bpy.data.meshes.new(name); me.from_pydata(verts,[],faces); me.update()
    bm=bmesh.new(); bm.from_mesh(me); bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces)); bm.to_mesh(me); bm.free()
    me.materials.append(MATS['MC_Plant_'+mat]); ob=bpy.data.objects.new(name,me); MODEL.objects.link(ob); return ob

def box(name,c,d,mat='Body',bevel=0):
    bm=bmesh.new(); v=bmesh.ops.create_cube(bm,size=1)['verts']
    bmesh.ops.scale(bm,verts=v,vec=d); bmesh.ops.translate(bm,verts=v,vec=c)
    me=bpy.data.meshes.new(name); bm.to_mesh(me); bm.free(); me.materials.append(MATS['MC_Plant_'+mat])
    ob=bpy.data.objects.new(name,me); MODEL.objects.link(ob)
    if bevel:
        bpy.context.view_layer.objects.active=ob; ob.select_set(True)
        m=ob.modifiers.new('Folded edges','BEVEL'); m.width=bevel; m.segments=1
        bpy.ops.object.modifier_apply(modifier=m.name); ob.select_set(False)
    return ob

def basis(axis):
    n=Vector(axis).normalized(); u=n.cross(Vector((0,0,1)) if abs(n.z)<.9 else Vector((0,1,0))).normalized()
    return n,u,n.cross(u)

def tube(name,a,b,r,mat='Pipe',n=24,inner=0):
    a,b=Vector(a),Vector(b); axis,u,v=basis(b-a); verts=[]; faces=[]
    for radius in ((r,inner) if inner else (r,)):
        for p in (a,b): verts.extend(tuple(p+radius*(math.cos(i*math.tau/n)*u+math.sin(i*math.tau/n)*v)) for i in range(n))
    for i in range(n):
        j=(i+1)%n; faces.append((i,j,n+j,n+i))
        if inner: faces += [(2*n+j,2*n+i,3*n+i,3*n+j),(j,i,2*n+i,2*n+j),(n+i,n+j,3*n+j,3*n+i)]
    if not inner: faces += [tuple(reversed(range(n))),tuple(range(n,2*n))]
    ob=obj(name,verts,faces,mat)
    for p in ob.data.polygons:
        if len(p.vertices)==4: p.use_smooth=True
    return ob

def cyl(name,c,r,depth,mat='Pipe',axis=(1,0,0),n=24,inner=0):
    c=Vector(c); d=Vector(axis)*depth/2; return tube(name,c-d,c+d,r,mat,n,inner)

def bolt(name,c,axis,r=.008): return cyl(name,c,r,r*.9,'Pipe',axis,6)

def flange(name,end,axis,r=.08,shaft=.065,bolts=6):
    end=Vector(end); axis,u,v=basis(axis)
    tube(name+'_neck',end-axis*shaft,end-axis*.017,r*.60,'Pipe',24,inner=r*.43)
    tube(name+'_flange',end-axis*.020,end,r,'Pipe',32,inner=r*.43)
    # Bolt heads on the interior-facing side, within the envelope.
    for i in range(bolts): bolt(name+'_bolt',end-axis*.024+(u*math.cos(i*math.tau/bolts)+v*math.sin(i*math.tau/bolts))*r*.77,axis,r*.085)

def skid(d,w):
    for y in (-w/2+.045,w/2-.045): box('Base_rail',(0,y,.045),(d,.09,.09),'Frame',.004)
    for x in (-d/2+.055,d/2-.055): box('Base_crossmember',(x,0,.035),(.11,w,.07),'Frame',.003)

def hmi(x,y,z):
    box('Controller_bezel',(x-.006,y,z),(.012,.23,.17),'Frame',.003)
    box('Controller_glass',(x+.001,y,z),(.002,.19,.12),'Screen',.001)
    for j in range(3): box('Static_screen_bars',(x+.0025,y-.05+j*.05,z),(.001,.024,.042),'Panel')

def drycooler():
    skid(2.4,1.2)
    for x in (-1.05,1.05):
        for y in (-.50,.50): box('Galvanised_leg',(x,y,.245),(.08,.08,.40),'Pipe',.003)
    box('Coil_shadow',(0,0,.78),(2.18,1.04,.72),'Fan')
    for sy in (-1,1):
        for k in range(72): box('Condenser_fin',(-1.045+k*2.09/71,sy*.534,.78),(.006,.018,.68),'Coil')
        for z in (.50,.77,1.05): box('Coil_protective_rail',(0,sy*.554,z),(2.17,.012,.012),'Frame')
        box('Side_accent_band',(0,sy*.575,1.155),(2.22,.025,.035),'Accent')
    for x in (-1.16,1.16):
        box('Folded_end_panel',(x,0,.775),(.08,1.14,.81),'Body',.004)
    box('Fan_deck',(0,0,1.19),(2.4,1.2,.05),'Body',.004)
    for x in (-.60,.60):
        cyl('Fan_dark_well',(x,0,1.219),.475,.012,'Fan',(0,0,1),48)
        cyl('Fan_shroud',(x,0,1.25),.489,.07,'Body',(0,0,1),48,.451)
        cyl('Fan_hub',(x,0,1.252),.105,.05,'Pipe',(0,0,1),24)
        for k in range(7):
            a=k*math.tau/7; poly=[(.10,-.10),(.23,-.05),(.425,.10),(.43,.36),(.28,.29),(.11,.24)]
            vs=[(x+r*math.cos(a+t),r*math.sin(a+t),zz) for zz in (1.237,1.245) for r,t in poly]
            obj('Axial_fan_blade',vs,[(5,4,3,2,1,0),(6,7,8,9,10,11)]+[(i,(i+1)%6,(i+1)%6+6,i+6) for i in range(6)],'Pipe')
        for rr in (.18,.30,.415): cyl('Circular_fan_guard',(x,0,1.294),rr,.006,'Frame',(0,0,1),40,rr-.004)
        for angle in (0,math.pi/3,2*math.pi/3):
            bar=box('Guard_spoke',(0,0,0),(.88,.006,.006),'Frame'); bar.location=(x,0,1.297); bar.rotation_euler.z=angle
    for i,(p,n) in enumerate(PORTS['DryCooler']): flange('Water_header_'+str(i),p,n,.073,.10)

def chiller():
    skid(2.4,1.2)
    for y in (-.29,.29):
        for x in (-.75,.62): box('Shell_saddle',(x,y,.205),(.19,.41,.25),'Frame',.012)
        cyl('Horizontal_heat_exchanger',(-.12,y,.55),.255,1.99,'Accent',n=48)
        for x in (-1.13,.88):
            cyl('Bolted_water_box_cover',(x,y,.55),.271,.045,'Pipe',n=48)
            for k in range(12): bolt('Water_box_bolt',(x+(.026 if x>0 else -.026),y+.231*math.cos(k*math.tau/12),.55+.231*math.sin(k*math.tau/12)),(1,0,0),.008)
        for x in (-.71,.53): cyl('Shell_band',(x,y,.55),.262,.035,'Pipe',n=40,inner=.254)
    box('Compressor_cradle',(-.18,0,.99),(1.25,.65,.18),'Frame',.012)
    cyl('Compressor_housing',(-.24,0,1.20),.265,1.10,'Accent',n=40)
    for x in (-.73,.28): cyl('Compressor_end',(x,0,1.20),.277,.06,'Pipe',n=32)
    box('Compressor_terminal_box',(-.18,0,1.475),(.48,.40,.15),'Frame',.008)
    cyl('Oil_separator',(.58,.27,1.07),.105,.60,'Pipe',(0,0,1),32)
    box('Control_cabinet',(1.035,0,.97),(.29,1.12,1.66),'Panel',.008)
    box('Controller_top_accent',(1.185,0,1.69),(.018,1.04,.055),'Accent',.003)
    for y in (-.257,.257): box('Control_door',(1.185,y,1.01),(.018,.506,1.30),'Body',.003)
    hmi(1.197,-.22,1.30)
    for y in (-.04,.04): box('Cabinet_handle',(1.195,y,.96),(.009,.015,.17),'Frame',.002)
    for y in (-.3,.3):
        for z in (.43,.47,.51): box('Control_vent',(1.197,y,z),(.003,.36,.012),'Frame')
    for i,(p,n) in enumerate(PORTS['Chiller']): flange('Water_connection_'+str(i),p,n,.070,.145)
    # Compressor assembly reaches the nominal top, using a vented protective cap.
    box('Electrical_enclosure_cap',(-.15,0,1.585),(.62,.51,.12),'Body',.009)

def pump():
    skid(.6,.6)
    box('Motor_foot',(-.15,0,.12),(.26,.25,.12),'Frame',.005)
    cyl('Electric_motor',(-.15,0,.29),.126,.29,'Accent',n=32)
    for k in range(16):
        a=k*math.tau/16; fin=box('Motor_cooling_fin',(0,0,0),(.23,.008,.025),'Accent')
        fin.location=(-.155,.132*math.cos(a),.29+.132*math.sin(a)); fin.rotation_euler.x=a-math.pi/2
    cyl('Motor_end_shroud',(-.28,0,.29),.144,.04,'Frame',n=32)
    for rr in (.055,.094,.126): cyl('Motor_rear_guard',(-.299,0,.29),rr,.002,'Pipe',n=24,inner=rr-.003)
    box('Motor_junction_box',(-.15,0,.455),(.13,.145,.07),'Frame',.004)
    cyl('Coupling_guard',(.035,0,.30),.075,.11,'Body',n=24)
    # Asymmetric volute shell: rising-radius profile around the shaft.
    n=40; vs=[]
    for x in (.075,.205):
        for k in range(n):
            a=k*math.tau/n; r=.13+.042*k/(n-1); vs.append((x,r*math.cos(a),.30+r*math.sin(a)))
    obj('Cast_volute',vs,[tuple(reversed(range(n))),tuple(range(n,2*n))]+[(i,(i+1)%n,(i+1)%n+n,i+n) for i in range(n)],'Accent')
    box('Pump_foot',(.14,0,.115),(.16,.22,.13),'Frame',.003)
    cyl('Volute_front_cover',(.211,0,.3),.142,.020,'Accent',n=32)
    for k in range(8): bolt('Casing_bolt',(.226,.116*math.cos(k*math.tau/8),.3+.116*math.sin(k*math.tau/8)),(1,0,0),.006)
    flange('Suction',(.3,0,.3),(1,0,0),.071,.075)
    flange('Discharge',(.13,0,.6),(0,0,1),.065,.145)

def platehx():
    skid(.8,.5)
    for y in (-.22,.22): box('Pressure_frame',(0,y,.83),(.73,.06,1.50),'Accent',.008)
    for k in range(30): box('Stainless_corrugated_plate',(0,-.178+k*.356/29,.84),(.63,.007,1.25),'Pipe',.002)
    for x in (-.33,.33):
        for z in (.29,.67,1.11,1.44):
            tube('Tie_bolt',(x,-.247,z),(x,.247,z),.011,'Pipe',12)
            for y in (-.236,.236): bolt('Tie_nut',(x,y,z),(0,1,0),.020)
    for z in (.15,1.57): tube('Guide_bar',(0,-.25,z),(0,.25,z),.030,'Pipe',20)
    for y in (-.22,.22): box('Top_frame_tab',(0,y,1.585),(.13,.06,.030),'Accent',.002)
    for i,(p,n) in enumerate(PORTS['PlateHx']): flange('Four_corner_nozzle_'+str(i),p,n,.046,.115)

def valve(check=False):
    cyl('Valve_barrel',(0,0,.10),.071,.235,'Pipe',n=24)
    for side in (-1,1): flange('Line_flange',(side*.15,0,.10),(side,0,0),.10,.045,4)
    if check:
        cyl('Check_cover',(0,0,.17),.065,.06,'Accent',(0,0,1),24)
    else:
        cyl('Valve_bonnet',(0,0,.217),.039,.14,'Pipe',(0,0,1),20)
        box('Electric_actuator',(0,0,.34),(.18,.15,.12),'Accent',.008)
        box('Actuator_cover',(0,0,.397),(.15,.12,.006),'Frame',.002)
        cyl('Stem_collar',(0,0,.272),.051,.02,'Frame',(0,0,1),20)

def leak():
    n=40; vs=[]
    for z in (0,.05):
        for k in range(n):
            a=k*math.tau/n; r=.161+.014*math.cos(4*a); vs.append((r*math.cos(a),r*math.sin(a),z))
    obj('Irregular_puddle',vs,[tuple(reversed(range(n))),tuple(range(n,2*n))]+[(i,(i+1)%n,(i+1)%n+n,i+n) for i in range(n)],'Accent')

def export(kind):
    sc=bpy.context.scene; dg=bpy.context.evaluated_depsgraph_get(); parts=[]
    for ob in MODEL.objects:
        me=bpy.data.meshes.new_from_object(ob.evaluated_get(dg),depsgraph=dg)
        cp=bpy.data.objects.new('Bake_'+ob.name,me); sc.collection.objects.link(cp); cp.matrix_world=ob.matrix_world.copy(); parts.append(cp)
    bpy.ops.object.select_all(action='DESELECT')
    for cp in parts: cp.select_set(True)
    bpy.context.view_layer.objects.active=parts[0]; bpy.ops.object.join(); ob=bpy.context.object; ob.name='SM_Plant_'+kind
    sc.cursor.location=(0,0,0); bpy.ops.object.origin_set(type='ORIGIN_CURSOR'); bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(island_margin=.008); bpy.ops.object.mode_set(mode='OBJECT')
    bm=bmesh.new(); bm.from_mesh(ob.data); bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces)); bm.to_mesh(ob.data); bm.free()
    m=ob.modifiers.new('Triangles','TRIANGULATE'); bpy.ops.object.modifier_apply(modifier=m.name)
    bm=bmesh.new(); bm.from_mesh(ob.data)
    bmesh.ops.dissolve_degenerate(bm,dist=1e-7,edges=list(bm.edges))
    empty=[f for f in bm.faces if f.calc_area()<1e-12]
    if empty: bmesh.ops.delete(bm,geom=empty,context='FACES_ONLY')
    bmesh.ops.triangulate(bm,faces=list(bm.faces)); bmesh.ops.recalc_face_normals(bm,faces=list(bm.faces))
    bm.to_mesh(ob.data); bm.free(); ob.data.update()
    nominal,limit=SPECS[kind]; cs=[v.co for v in ob.data.vertices]
    low=[min(v[i] for v in cs) for i in range(3)]; high=[max(v[i] for v in cs) for i in range(3)]
    expected=[-nominal[0]/2,-nominal[1]/2,0,nominal[0]/2,nominal[1]/2,nominal[2]]
    err=max(abs(a-b) for a,b in zip(low+high,expected))
    result={'nominal_dimensions_m':nominal,'bounds_min_m':low,'bounds_max_m':high,'max_bound_error_m':err,
      'vertices':len(cs),'triangles':len(ob.data.polygons),'triangle_limit':limit,'materials':[m.name for m in ob.data.materials],
      'uv_channels':len(ob.data.uv_layers),'origin_m':list(ob.location),'front_axis':'+X','ports':PORTS.get(kind,[])}
    assert err<1e-6,result
    assert len(ob.data.polygons)<=limit,result
    assert len(ob.data.materials)<=8 and len(ob.data.uv_layers)==1,result
    assert all(math.isfinite(c) for co in cs for c in co)
    assert all(p.area>1e-12 for p in ob.data.polygons),'degenerate polygon'
    bpy.ops.export_scene.gltf(filepath=str(OUT/(ob.name+'.glb')),use_selection=True)
    ob.data.transform(Matrix.Scale(100,4)); sc.unit_settings.scale_length=.01
    bpy.ops.export_scene.fbx(filepath=str(OUT/(ob.name+'.fbx')),use_selection=True,object_types={'MESH'},axis_forward='-Y',axis_up='Z',apply_unit_scale=True,apply_scale_options='FBX_SCALE_NONE',bake_space_transform=False,mesh_smooth_type='FACE',bake_anim=False)
    bpy.data.objects.remove(ob,do_unlink=True); sc.unit_settings.scale_length=1
    print('PLANT_MODEL_PASS',kind,json.dumps(result),flush=True); return result

def build():
    global MATS,MODEL
    bpy.ops.wm.read_factory_settings(use_empty=True); sc=bpy.context.scene; sc.name='Plant_Reference'; sc.unit_settings.system='METRIC'
    MATS={}
    for name,(rgb,metal,rough) in PALETTE.items():
        m=bpy.data.materials.new(name); m.diffuse_color=(*rgb,1); m.use_nodes=True
        n=next(n for n in m.node_tree.nodes if n.type=='BSDF_PRINCIPLED'); n.inputs['Base Color'].default_value=(*rgb,1)
        n.inputs['Metallic'].default_value=metal; n.inputs['Roughness'].default_value=rough; MATS[name]=m
    models={}; reports={}
    for kind,fn in [('DryCooler',drycooler),('Chiller',chiller),('Pump',pump),('PlateHx',platehx),('Valve2Way',valve),('CheckValve',lambda:valve(True)),('LeakMarker',leak)]:
        MODEL=bpy.data.collections.new('Plant_'+kind); sc.collection.children.link(MODEL); models[kind]=MODEL
        fn(); reports[kind]=export(kind); MODEL.hide_render=True; MODEL.hide_viewport=True
    (OUT/'materials.json').write_text(json.dumps(PALETTE,indent=2),encoding='utf8')
    (OUT/'verification.json').write_text(json.dumps({'models':reports,'units':'m','geometry_bound_tolerance_m':1e-6,'no_textures':True,'no_collision':True},indent=2),encoding='utf8')
    studio=bpy.data.collections.new('Studio'); sc.collection.children.link(studio); MODEL=studio
    box('Studio_floor',(0,0,-.035),(200,200,.05),'Panel')
    for name,loc,power,size in [('Key',(3,-4,6),950,4),('Fill',(-2,3,4),700,3),('Rim',(-4,-2,5),1000,3)]:
        d=bpy.data.lights.new(name,'AREA'); d.energy=power; d.size=size
        ob=bpy.data.objects.new(name,d); studio.objects.link(ob); ob.location=loc
        ob.rotation_euler=(Vector((0,0,.6))-ob.location).to_track_quat('-Z','Y').to_euler()
    sc.world=bpy.data.worlds.new('Plant_studio'); sc.world.use_nodes=True
    sc.world.node_tree.nodes['Background'].inputs[0].default_value=(.14,.17,.20,1); sc.world.node_tree.nodes['Background'].inputs[1].default_value=.4
    camdata=bpy.data.cameras.new('Camera'); camdata.type='ORTHO'; cam=bpy.data.objects.new('Camera',camdata); studio.objects.link(cam); sc.camera=cam
    sc.render.engine='CYCLES'; sc.cycles.samples=24; sc.cycles.use_denoising=True; sc.render.resolution_x=1200; sc.render.resolution_y=1000
    sc.render.image_settings.file_format='PNG'; sc['asset_scope']='External plant equipment; project nominal sizes; no physics changes'
    models['DryCooler'].hide_render=False; models['DryCooler'].hide_viewport=False
    cam.location=(4,-5,4); cam.rotation_euler=(Vector((0,0,.65))-cam.location).to_track_quat('-Z','Y').to_euler(); camdata.ortho_scale=3.5
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'Plant_Reference.blend'),compress=True)
    for kind in SPECS:
        selected=next((a.split('=',1)[1].split(',') for a in sys.argv if a.startswith('--renders=')),None)
        if selected and kind not in selected: continue
        for k,co in models.items(): co.hide_render=k!=kind; co.hide_viewport=k!=kind
        d,w,h=SPECS[kind][0]; target=Vector((0,0,h*.48)); cam.location=(d*1.8,-max(d,w)*2,h*1.6)
        cam.rotation_euler=(target-cam.location).to_track_quat('-Z','Y').to_euler(); camdata.ortho_scale=max(d,w,h)*(1.95 if kind=='Pump' else 1.55)
        sc.render.filepath=str(OUT/('plant_'+kind.lower()+'_preview.png')); bpy.ops.render.render(write_still=True)
    print('PLANT_BUILD_COMPLETE',flush=True)

if __name__=='__main__': build()
