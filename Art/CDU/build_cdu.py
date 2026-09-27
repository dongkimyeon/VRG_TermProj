"""Reference CDU: Blender 5.2, metres, +X front, origin at floor centre.

Run Blender --background --python this_file.py. Source dimensions match the
existing CoolingDistributionUnitActor: depth .9, width .8, height 1.9 m.
The reference establishes appearance, not manufacturer dimensions or ratings.
"""
import bpy, bmesh, math, json
from pathlib import Path
from mathutils import Vector, Matrix

OUT = Path(__file__).resolve().parent
PALETTE = {
    'MC_CDU_Charcoal': ((.038,.041,.045),.45,.36),
    'MC_CDU_Panel': ((.061,.065,.070),.4,.40),
    'MC_CDU_Recess': ((.007,.009,.011),.15,.48),
    'MC_CDU_Hardware': ((.22,.25,.28),.8,.28),
    'MC_CDU_Screen': ((.76,.87,.89),.05,.28),
    'MC_CDU_Ink': ((.09,.16,.18),.2,.42),
}

def material(name, rgb, metal, rough):
    m=bpy.data.materials.new(name); m.diffuse_color=(*rgb,1)
    n=next(n for n in m.node_tree.nodes if n.type=='BSDF_PRINCIPLED')
    n.inputs['Base Color'].default_value=(*rgb,1)
    n.inputs['Metallic'].default_value=metal; n.inputs['Roughness'].default_value=rough
    if name.endswith('Screen'):
        n.inputs['Emission Color'].default_value=(*rgb,1)
        n.inputs['Emission Strength'].default_value=.3
    return m

def finish(name,bm,mat,bevel=0,collection=None):
    me=bpy.data.meshes.new(name); bm.to_mesh(me); bm.free(); me.materials.append(MATS[mat])
    ob=bpy.data.objects.new(name,me); (collection or MODEL).objects.link(ob)
    bpy.context.view_layer.objects.active=ob; ob.select_set(True)
    if bevel:
        mod=ob.modifiers.new('Folded edges','BEVEL'); mod.width=bevel; mod.segments=3
        bpy.ops.object.modifier_apply(modifier=mod.name)
    ob.select_set(False)
    return ob

def box(name,c,d,mat='MC_CDU_Panel',bevel=.002):
    bm=bmesh.new(); r=bmesh.ops.create_cube(bm,size=1)
    bmesh.ops.scale(bm,verts=r['verts'],vec=d); bmesh.ops.translate(bm,verts=r['verts'],vec=c)
    return finish(name,bm,mat,bevel)

def cylinder(name,c,r,depth,mat='MC_CDU_Hardware',segments=48):
    bm=bmesh.new(); v=bmesh.ops.create_cone(bm,cap_ends=True,segments=segments,radius1=r,radius2=r,depth=depth)['verts']
    bmesh.ops.rotate(bm,verts=v,cent=(0,0,0),matrix=Matrix.Rotation(math.pi/2,3,'Y'))
    bmesh.ops.translate(bm,verts=v,vec=c)
    return finish(name,bm,mat)

def ring(name,x,y,z,r,inner,depth,mat='MC_CDU_Recess'):
    vs=[]; fs=[]; n=48
    for xx,rr in [(x-depth/2,r),(x+depth/2,r),(x-depth/2,inner),(x+depth/2,inner)]:
        vs += [(xx,y+rr*math.cos(i*math.tau/n),z+rr*math.sin(i*math.tau/n)) for i in range(n)]
    for i in range(n):
        j=(i+1)%n
        fs.extend([(i,j,n+j,n+i),(2*n+j,2*n+i,3*n+i,3*n+j),(j,i,2*n+i,2*n+j),(n+i,n+j,3*n+j,3*n+i)])
    me=bpy.data.meshes.new(name); me.from_pydata(vs,[],fs); me.materials.append(MATS[mat]); me.update()
    ob=bpy.data.objects.new(name,me); MODEL.objects.link(ob); return ob

def camera(name,loc,target,scale):
    data=bpy.data.cameras.new(name); data.type='ORTHO'; data.ortho_scale=scale
    ob=bpy.data.objects.new(name,data); STUDIO.objects.link(ob); ob.location=loc
    ob.rotation_euler=(Vector(target)-ob.location).to_track_quat('-Z','Y').to_euler(); return ob

def build():
    global MATS,MODEL,STUDIO
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc=bpy.context.scene; sc.name='CDU_Reference'; sc.unit_settings.system='METRIC'
    MODEL=bpy.data.collections.new('CDU_Editable_Parts'); sc.collection.children.link(MODEL)
    STUDIO=bpy.data.collections.new('Studio'); sc.collection.children.link(STUDIO)
    MATS={k:material(k,*v) for k,v in PALETTE.items()}
    # Cabinet envelope and recessed base. Front is +X.
    box('Internal_shadow_body',(-.019,0,1.02),(.842,.768,1.704),'MC_CDU_Recess',.006)
    box('Plinth',(-.03,0,.081),(.80,.704,.12),'MC_CDU_Charcoal',.008)
    for x in (-.32,.32):
        for y in (-.30,.30): box('Levelling_foot',(x,y,.016),(.07,.07,.032),'MC_CDU_Recess',.005)
    for y in (-.391,.391):
        box('Side_panel',(0,y,1.02),(.882,.018,1.714),'MC_CDU_Charcoal',.004)
        for x in (-.375,.375):
            for z in (.27,1.75):
                ob=cylinder('Side_captive_screw',(0,0,0),.0035,.002)
                ob.rotation_euler.z=math.pi/2; ob.location=(x,y+math.copysign(.010,y),z)
    for yy in (-.193,.193):
        box('Rear_service_panel',(-.442,yy,1.027),(.016,.380,1.698),bevel=.003)
    # Two top access covers and a dark gasket seam.
    for x in (-.221,.221):
        box('Top_access_cover',(x,0,1.887),(.438,.797,.026),'MC_CDU_Panel',.004)
    for y in (-.382,.382):
        box('Front_corner_post',(.425,y,1.022),(.035,.032,1.716),'MC_CDU_Charcoal',.003)
    for y in (-.188,.188):
        box('Front_upper_door',(.425,y,1.45),(.030,.369,.828),bevel=.004)
        box('Front_lower_door',(.425,y,.470),(.030,.369,.305),bevel=.003)
        box('Recessed_door_pull',(.442,y,1.195),(.008,.319,.024),'MC_CDU_Recess',.002)
    # Three true circular recesses in the service fascia (not painted circles).
    panel=box('Three_vent_service_fascia',(.425,0,.830),(.030,.746,.393),bevel=.003)
    for i,y in enumerate((-.235,0,.235)):
        cutter=cylinder('Temporary_vent_cutter',(.425,y,.820),.076,.13,segments=64)
        bpy.context.view_layer.objects.active=panel
        mod=panel.modifiers.new('Circular opening','BOOLEAN'); mod.operation='DIFFERENCE'; mod.object=cutter
        bpy.ops.object.modifier_apply(modifier=mod.name); bpy.data.objects.remove(cutter,do_unlink=True)
        cylinder('Vent_dark_cavity',(.407,y,.820),.075,.002,'MC_CDU_Recess',64)
        ring('Vent_rolled_rim',.442,y,.820,.079,.073,.008)
        for j in range(-5,6):
            dz=j*.011; length=2*math.sqrt(.070**2-dz**2)
            box('Vent_protective_grille',(.444,y,.820+dz),(.002,length,.0015),'MC_CDU_Charcoal',0)
    # HMI, small buttons, latch and fasteners. Screen markings are static art.
    box('HMI_bezel',(.444,-.086,1.365),(.009,.175,.123),'MC_CDU_Recess',.003)
    box('HMI_glass',(.450,-.086,1.365),(.003,.147,.095),'MC_CDU_Screen',.001)
    box('HMI_header',(.452,-.086,1.397),(.001,.124,.006),'MC_CDU_Ink',0)
    for y in (-.119,-.073):
        box('HMI_static_indicator',(.452,y,1.358),(.001,.030,.023),'MC_CDU_Ink',0)
    for z in (1.32,1.39): box('Control_button',(.444,-.238,z),(.008,.036,.012),'MC_CDU_Recess',.002)
    cylinder('Door_lock',(.445,.028,1.093),.006,.006,'MC_CDU_Hardware',24)
    for y in (-.356,.356):
        for z in (.36,.63,1.045,1.82): cylinder('Front_captive_screw',(.442,y,z),.0034,.003,'MC_CDU_Hardware',16)
    box('Lower_service_slot',(.441,.258,.365),(.005,.105,.025),'MC_CDU_Recess',.001)
    box('Front_kick_recess',(.366,0,.221),(.015,.737,.17),'MC_CDU_Recess',.002)
    for y in (-.31,.31): box('Base_edge_rail',(.387,y,.167),(.08,.03,.03),'MC_CDU_Charcoal',.003)
    # Runtime ports remain independently positioned by ConnectionHeight/Spacing.
    for y in (-.15,.15): ring('Port_mounting_seat',.381,y,.25,.063,.054,.01,'MC_CDU_Hardware')
    # Preview ports are separate and excluded from the runtime cabinet FBX.
    ports=bpy.data.collections.new('Preview_ports'); sc.collection.children.link(ports)
    for y,key,rgb in [(-.15,'Supply',(.08,.30,.47)),(.15,'Return',(.43,.12,.10))]:
        name='MC_CDU_'+key; MATS[name]=material(name,rgb,.5,.3)
        ob=cylinder('Preview_'+key,(.42,y,.25),.055,.06,name)
        MODEL.objects.unlink(ob); ports.objects.link(ob)
    sc['asset_dimensions_m']=[.9,.8,1.9]; sc['front_axis']='+X'; sc['origin']='floor centre'
    sc['reference']='User-supplied GB200 rack image, isolated left CDU, 2026-09-27'
    sc['limitations']='Appearance reference, estimated dimensions; no internal equipment or manufacturer CAD'
    # Joined export is an evaluated copy; editable authoring parts remain intact.
    dg=bpy.context.evaluated_depsgraph_get(); baked=[]
    for ob in list(MODEL.objects):
        me=bpy.data.meshes.new_from_object(ob.evaluated_get(dg),depsgraph=dg)
        cp=bpy.data.objects.new('Export_'+ob.name,me); sc.collection.objects.link(cp); cp.matrix_world=ob.matrix_world.copy(); baked.append(cp)
    bpy.ops.object.select_all(action='DESELECT')
    for ob in baked: ob.select_set(True)
    bpy.context.view_layer.objects.active=baked[0]; bpy.ops.object.join(); mesh=baked[0]; mesh.name='SM_CDU_Reference'
    bpy.context.scene.cursor.location=(0,0,0); bpy.ops.object.origin_set(type='ORIGIN_CURSOR')
    bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    bpy.ops.object.mode_set(mode='EDIT'); bpy.ops.mesh.select_all(action='SELECT'); bpy.ops.uv.smart_project(island_margin=.008); bpy.ops.object.mode_set(mode='OBJECT')
    mod=mesh.modifiers.new('Triangles','TRIANGULATE'); bpy.ops.object.modifier_apply(modifier=mod.name)
    report={'triangles':len(mesh.data.polygons),'vertices':len(mesh.data.vertices),'materials':[m.name for m in mesh.data.materials],
            'dimensions_m':list(mesh.dimensions),'origin_m':list(mesh.location),'front_axis':'+X','port_centres_m':[[.45,-.15,.25],[.45,.15,.25]]}
    assert all(math.isfinite(v) for p in mesh.data.vertices for v in p.co)
    assert max(abs(a-b) for a,b in zip(mesh.dimensions,(.9,.8,1.9)))<.02
    bpy.ops.export_scene.gltf(filepath=str(OUT/'CDU_Reference.glb'),use_selection=True,export_extras=True)
    # Bake centimetres as the existing GJ export pipeline does.
    mesh.data.transform(Matrix.Scale(100,4)); sc.unit_settings.scale_length=.01
    bpy.ops.export_scene.fbx(filepath=str(OUT/'SM_CDU_Reference.fbx'),use_selection=True,object_types={'MESH'},
        axis_forward='-Y',axis_up='Z',apply_unit_scale=True,apply_scale_options='FBX_SCALE_NONE',
        bake_space_transform=False,mesh_smooth_type='FACE',bake_anim=False)
    mesh.data.transform(Matrix.Scale(.01,4)); sc.unit_settings.scale_length=1
    bpy.data.objects.remove(mesh,do_unlink=True)
    (OUT/'materials.json').write_text(json.dumps(PALETTE,indent=2),encoding='utf8')
    (OUT/'verification.json').write_text(json.dumps(report,indent=2),encoding='utf8')
    # Neutral photographic studio.
    ground=box('Studio_floor',(0,0,-.03),(200,200,.05),'MC_CDU_Panel',0)
    MODEL.objects.unlink(ground); STUDIO.objects.link(ground)
    for name,loc,power,size in [('Key',(3,-4,5),700,4),('Fill',(2,3,3),500,3),('Rim',(-3,1,4),900,3)]:
        data=bpy.data.lights.new(name,'AREA'); data.energy=power; data.shape='DISK'; data.size=size
        ob=bpy.data.objects.new(name,data); STUDIO.objects.link(ob); ob.location=loc
        ob.rotation_euler=(Vector((0,0,1))-ob.location).to_track_quat('-Z','Y').to_euler()
    sc.world=bpy.data.worlds.new('CDU_studio_world'); sc.world.use_nodes=True
    sc.world.node_tree.nodes['Background'].inputs[0].default_value=(.16,.18,.22,1)
    sc.world.node_tree.nodes['Background'].inputs[1].default_value=.4
    hero=camera('CAM_CDU_Hero',(3.5,-4.5,3.0),(0,0,.93),2.65)
    front=camera('CAM_CDU_Front',(5,0,1.20),(0,0,.95),2.35)
    sc.render.engine='CYCLES'; sc.cycles.samples=40; sc.cycles.use_denoising=True
    sc.render.resolution_x=1100; sc.render.resolution_y=1200; sc.render.resolution_percentage=100
    sc.render.image_settings.file_format='PNG'; sc.camera=hero
    for area in bpy.context.screen.areas:
        if area.type=='VIEW_3D': area.spaces.active.region_3d.view_perspective='CAMERA'
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'CDU_Reference.blend'),compress=True)
    for cam,file in [(hero,'cdu_preview.png'),(front,'cdu_front.png')]:
        sc.camera=cam; sc.render.filepath=str(OUT/file); bpy.ops.render.render(write_still=True)
    print('CDU_MODEL_PASS',json.dumps(report))

if __name__=='__main__': build()
