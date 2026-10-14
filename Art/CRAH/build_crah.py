"""Reference-inspired CRAH cabinets. Metres, +X supply, floor-centre origin.

Run: <Blender>/blender.exe -b --python Art/CRAH/build_crah.py
STULZ photographs inform appearance only. No trademarks, image textures or CAD.
Nominal envelopes are the project's existing simulation boxes, not product specs.
"""
import bpy
import bmesh
import math
import json
from pathlib import Path
from mathutils import Vector, Matrix

OUT = Path(__file__).resolve().parent
PALETTE = {
    'MC_CRAH_Charcoal': ((.038, .041, .045), .45, .36),
    'MC_CRAH_Panel': ((.061, .065, .070), .40, .40),
    'MC_CRAH_Recess': ((.007, .009, .011), .15, .48),
    'MC_CRAH_Grille': ((.095, .105, .115), .65, .34),
    'MC_CRAH_Hardware': ((.24, .26, .28), .80, .28),
    'MC_CRAH_Screen': ((.72, .75, .77), .05, .28),
    'MC_CRAH_Ink': ((.10, .12, .13), .20, .42),
}
SPECS = {'InRow': (1.068, .6, 2.236), 'Perimeter': (.9, 2., 2.)}


def material(name, rgb, metal, rough):
    m = bpy.data.materials.new(name)
    m.diffuse_color = (*rgb, 1)
    m.use_nodes = True
    n = next(n for n in m.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
    n.inputs['Base Color'].default_value = (*rgb, 1)
    n.inputs['Metallic'].default_value = metal
    n.inputs['Roughness'].default_value = rough
    if name.endswith('Screen'):
        n.inputs['Emission Color'].default_value = (*rgb, 1)
        n.inputs['Emission Strength'].default_value = .2
    return m


def finish(name, bm, mat, bevel=0, collection=None):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    me.materials.append(MATS['MC_CRAH_' + mat])
    ob = bpy.data.objects.new(name, me)
    (collection or MODEL).objects.link(ob)
    if bevel:
        bpy.context.view_layer.objects.active = ob
        ob.select_set(True)
        mod = ob.modifiers.new('Folded metal edges', 'BEVEL')
        mod.width = bevel
        mod.segments = 2
        bpy.ops.object.modifier_apply(modifier=mod.name)
        ob.select_set(False)
    return ob


def box(name, c, d, mat='Panel', bevel=.0015):
    bm = bmesh.new()
    verts = bmesh.ops.create_cube(bm, size=1)['verts']
    bmesh.ops.scale(bm, verts=verts, vec=d)
    bmesh.ops.translate(bm, verts=verts, vec=c)
    return finish(name, bm, mat, bevel)


def cylinder(name, c, radius, depth, mat='Hardware', segments=24, axis='X'):
    bm = bmesh.new()
    verts = bmesh.ops.create_cone(bm, cap_ends=True, segments=segments,
        radius1=radius, radius2=radius, depth=depth)['verts']
    if axis == 'X':
        bmesh.ops.rotate(bm, verts=verts, cent=(0, 0, 0), matrix=Matrix.Rotation(math.pi / 2, 3, 'Y'))
    bmesh.ops.translate(bm, verts=verts, vec=c)
    return finish(name, bm, mat)


def ring(name, x, y, z, outer, inner, depth, mat='Grille', segments=40):
    vs, fs = [], []
    n = segments
    for xx, rr in [(x-depth/2, outer), (x+depth/2, outer), (x-depth/2, inner), (x+depth/2, inner)]:
        vs += [(xx, y + rr*math.cos(i*math.tau/n), z + rr*math.sin(i*math.tau/n)) for i in range(n)]
    for i in range(n):
        j = (i+1) % n
        fs += [(i,j,n+j,n+i), (2*n+j,2*n+i,3*n+i,3*n+j),
               (j,i,2*n+i,2*n+j), (n+i,n+j,3*n+j,3*n+i)]
    me = bpy.data.meshes.new(name)
    me.from_pydata(vs, [], fs)
    me.materials.append(MATS['MC_CRAH_'+mat])
    me.update()
    ob = bpy.data.objects.new(name, me)
    MODEL.objects.link(ob)
    return ob


def fan(name, x, y, z, radius):
    # Shallow visible rotor only; no internal coil, pipe or control equipment.
    cylinder(name+'_shadow', (x-.024,y,z), radius, .008, 'Recess', 40)
    ring(name+'_shroud', x, y, z, radius, radius*.89, .018)
    cylinder(name+'_hub', (x+.006,y,z), radius*.23, .026, 'Hardware', 32)
    cylinder(name+'_hub_cap', (x+.020,y,z), radius*.15, .006, 'Charcoal', 24)
    for blade in range(7):
        a = blade*math.tau/7
        polygon = [(radius*.20,-.12), (radius*.46,-.05), (radius*.84,.16),
                   (radius*.86,.43), (radius*.55,.35), (radius*.23,.25)]
        vs = []
        for xx in (x-.010, x-.005):
            vs += [(xx, y+r*math.cos(a+t), z+r*math.sin(a+t)) for r,t in polygon]
        count = len(polygon)
        fs = [tuple(range(count-1,-1,-1)), tuple(range(count,count*2))]
        fs += [(i,(i+1)%count,(i+1)%count+count,i+count) for i in range(count)]
        me=bpy.data.meshes.new(name+'_blade')
        me.from_pydata(vs,[],fs)
        me.materials.append(MATS['MC_CRAH_Hardware'])
        me.update()
        ob=bpy.data.objects.new(name+'_blade',me)
        MODEL.objects.link(ob)
    for r in (.35,.56,.76):
        ring(name+'_wire_guard',x+.025,y,z,radius*r,radius*r-.0022,.003,'Charcoal',32)
    for a in (0,math.pi/2):
        bar=box(name+'_guard_spoke',(0,0,0),(.003,radius*1.77,.004),'Charcoal',0)
        bar.rotation_euler.x=a
        bar.location=(x+.026,y,z)


def front_grid(prefix, x, width, bottom, top, pitch, vertical_pitch):
    # Actual open geometry. Grille shadows and fans remain visible through gaps.
    count=int((top-bottom)/pitch)
    for i in range(count+1):
        box(prefix+'_horizontal',(x,0,bottom+i*(top-bottom)/count),(.003,width,.0025),'Grille',0)
    count=int(width/vertical_pitch)
    for i in range(count+1):
        box(prefix+'_vertical',(x+.001,-width/2+i*width/count,(top+bottom)/2),(.002,.002,top-bottom),'Grille',0)


def hmi(x, y, z, width=.16, height=.12):
    box('HMI_bezel',(x-.005,y,z),(.009,width,height),'Recess',.002)
    box('HMI_monochrome_screen',(x+.0005,y,z),(.002,width*.78,height*.70),'Screen',.001)
    box('HMI_static_header',(x+.0017,y,z+height*.20),(.0006,width*.64,.004),'Ink',0)
    for dy in (-width*.2,width*.13):
        box('HMI_static_symbol',(x+.0017,y+dy,z-.006),(.0006,width*.18,height*.16),'Ink',0)


def build_inrow():
    d,w,h=SPECS['InRow']
    box('Cabinet_shadow_body',(-.005,0,h/2),(.96,.554,h-.14),'Recess',.003)
    for y in (-w/2+.008,w/2-.008):
        box('Flat_side_panel',(0,y,h/2),(d-.018,.016,h-.035),'Charcoal',.002)
    for x in (-d/2+.012,d/2-.012):
        for y in (-w/2+.017,w/2-.017):
            box('Full_height_corner_post',(x,y,h/2),(.024,.034,h),'Charcoal',.002)
    box('Top_cap',(0,0,h-.012),(d,w,.024),'Panel',.002)
    box('Base_plinth',(0,0,.045),(d,w,.09),'Charcoal',.003)
    for x in (-d/2+.010,d/2-.010):
        box('Bottom_service_strip',(x,0,.13),(.018,w-.064,.074),'Panel',.002)
    front=d/2
    box('Front_HMI_header',(front-.020,0,2.075),(.020,.532,.258),'Panel',.002)
    hmi(front-.006,-.07,2.08,.17,.12)
    cylinder('Front_lock',(front-.005,.193,2.075),.008,.008,'Hardware',20)
    box('Recessed_door_handle',(front-.009,.239,1.14),(.012,.018,.18),'Recess',.002)
    box('Door_handle_insert',(front-.003,.239,1.14),(.004,.010,.14),'Hardware',.001)
    for z in (.49,1.05,1.61):
        fan('Front_supply_fan',front-.045,0,z,.211)
    front_grid('Front_supply_grille',front-.006,.458,.209,1.920,.026,.030)
    for z in (.195,1.934):
        box('Front_grille_frame',(front-.011,0,z),(.018,.526,.024),'Charcoal',.001)
    # Rear intake: finer horizontal louvres and a central stile, visibly different from supply.
    rear=-d/2
    for z in (.19,2.055):
        box('Rear_grille_frame',(rear+.012,0,z),(.024,.532,.032),'Panel',.001)
    front_grid('Rear_intake_grille',rear+.007,.508,.218,2.023,.020,.064)
    box('Rear_centre_stile',(rear+.004,0,1.12),(.008,.024,1.84),'Charcoal',.001)
    box('Rear_service_header',(rear+.012,0,2.15),(.024,.532,.125),'Panel',.002)
    for x in (front-.005,rear+.005):
        for y in (-.245,.245):
            for z in (.13,1.97,2.16):
                cylinder('Captive_fastener',(x,y,z),.0035,.003,'Hardware',12)


def build_perimeter():
    d,w,h=SPECS['Perimeter']
    box('Cabinet_shadow_body',(-.025,0,1.07),(.81,1.95,1.79),'Recess',.003)
    box('Base_plinth',(0,0,.045),(d,w,.09),'Charcoal',.003)
    for y in (-.99,.99):
        box('Flat_side_panel',(0,y,1.03),(d,.02,1.94),'Charcoal',.002)
    box('Flat_rear_panel',(-.44,0,1.03),(.02,1.96,1.94),'Panel',.002)
    for y in (-.974,.974):
        box('Front_corner_post',(.435,y,1.04),(.03,.035,1.92),'Charcoal',.002)
    for y in (-.636,0,.636):
        box('Upper_service_door',(.43,y,1.5),(.022,.622,.94),'Panel',.003)
        box('Lower_fan_module_shadow',(.376,y,.52),(.004,.61,.86),'Recess',.001)
        fan('Lower_front_supply_fan',.403,y,.52,.282)
        box('Door_handle_recess',(.446,y+.245,1.48),(.004,.021,.16),'Recess',.001)
        box('Door_handle',(.448,y+.245,1.48),(.003,.011,.12),'Hardware',.001)
        cylinder('Door_lock',(.447,y+.245,1.64),.007,.004,'Hardware',16)
    hmi(.444,-.62,1.79,.18,.13)
    for z in (.105,1.0):
        box('Fan_bay_horizontal_frame',(.436,0,z),(.028,1.94,.032),'Charcoal',.002)
    for y in (-.948,-.318,.318,.948):
        box('Fan_bay_vertical_frame',(.435,y,.545),(.03,.025,.87),'Charcoal',.001)
    # Protective mesh over all three front fan bays; no floor discharge.
    front_grid('Lower_supply_guard',.446,1.874,.135,.96,.042,.042)
    # Top intake covers the available upper face, kept inside the exact h=2 envelope.
    for x in (-.435,.435):
        box('Top_intake_edge',(x,0,1.986),(.03,2.,.028),'Panel',.002)
    for y in (-.983,.983):
        box('Top_intake_side',(0,y,1.986),(.84,.034,.028),'Panel',.002)
    box('Top_intake_shadow',(0,0,1.955),(.824,1.932,.008),'Recess',0)
    for i in range(47):
        box('Top_intake_crossbar',(0,-.92+i*.04,1.992),(.818,.007,.012),'Grille',0)
    for i in range(13):
        box('Top_intake_longitudinal',(-.39+i*.065,0,1.988),(.005,1.86,.012),'Grille',0)
    for y in (-.92,-.30,.30,.92):
        for z in (.11,.985,1.96):
            cylinder('Front_frame_fastener',(.448,y,z),.0035,.003,'Hardware',12)


def export_model(kind):
    sc=bpy.context.scene
    dg=bpy.context.evaluated_depsgraph_get()
    baked=[]
    for ob in list(MODEL.objects):
        me=bpy.data.meshes.new_from_object(ob.evaluated_get(dg),depsgraph=dg)
        cp=bpy.data.objects.new('Export_'+ob.name,me)
        sc.collection.objects.link(cp)
        cp.matrix_world=ob.matrix_world.copy()
        baked.append(cp)
    bpy.ops.object.select_all(action='DESELECT')
    for ob in baked: ob.select_set(True)
    bpy.context.view_layer.objects.active=baked[0]
    bpy.ops.object.join()
    mesh=bpy.context.object
    mesh.name='SM_CRAH_'+kind
    sc.cursor.location=(0,0,0)
    bpy.ops.object.origin_set(type='ORIGIN_CURSOR')
    bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(island_margin=.008)
    bpy.ops.object.mode_set(mode='OBJECT')
    mod=mesh.modifiers.new('Triangles','TRIANGULATE')
    bpy.ops.object.modifier_apply(modifier=mod.name)
    coords=[v.co for v in mesh.data.vertices]
    low=[min(v[i] for v in coords) for i in range(3)]
    high=[max(v[i] for v in coords) for i in range(3)]
    dims=[high[i]-low[i] for i in range(3)]
    nominal=SPECS[kind]
    expected_low=[-nominal[0]/2,-nominal[1]/2,0]
    expected_high=[nominal[0]/2,nominal[1]/2,nominal[2]]
    error=max(abs(a-b) for a,b in zip(low+high,expected_low+expected_high))
    report={'nominal_dimensions_m':nominal,'bounds_min_m':low,'bounds_max_m':high,
            'dimensions_m':dims,'max_bound_error_m':error,'vertices':len(coords),
            'triangles':len(mesh.data.polygons),'materials':[m.name for m in mesh.data.materials],
            'uv_channels':len(mesh.data.uv_layers),'origin_m':list(mesh.location),
            'front_axis':'+X','intake':'-X' if kind=='InRow' else '+Z',
            'supply':'+X full face' if kind=='InRow' else '+X lower half',
            'authoring_parts':len(MODEL.objects)}
    assert all(math.isfinite(v) for co in coords for v in co)
    assert error<1e-6,report
    assert len(mesh.data.polygons)<=15000,report
    assert len(mesh.data.uv_layers)>0
    bpy.ops.export_scene.gltf(filepath=str(OUT/(mesh.name+'.glb')),use_selection=True,export_extras=True)
    mesh.data.transform(Matrix.Scale(100,4))
    sc.unit_settings.scale_length=.01
    bpy.ops.export_scene.fbx(filepath=str(OUT/(mesh.name+'.fbx')),use_selection=True,object_types={'MESH'},
        axis_forward='-Y',axis_up='Z',apply_unit_scale=True,apply_scale_options='FBX_SCALE_NONE',
        bake_space_transform=False,mesh_smooth_type='FACE',bake_anim=False)
    mesh.data.transform(Matrix.Scale(.01,4))
    sc.unit_settings.scale_length=1
    bpy.data.objects.remove(mesh,do_unlink=True)
    return report


def camera(name,loc,target,scale):
    data=bpy.data.cameras.new(name)
    data.type='ORTHO'; data.ortho_scale=scale
    ob=bpy.data.objects.new(name,data); STUDIO.objects.link(ob); ob.location=loc
    ob.rotation_euler=(Vector(target)-ob.location).to_track_quat('-Z','Y').to_euler()
    return ob


def build():
    global MATS,MODEL,STUDIO
    OUT.mkdir(parents=True,exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.context.preferences.filepaths.save_version=0
    sc=bpy.context.scene; sc.name='CRAH_Reference'; sc.unit_settings.system='METRIC'
    MATS={k:material(k,*v) for k,v in PALETTE.items()}
    models={}; reports={}
    for kind,build_fn in [('InRow',build_inrow),('Perimeter',build_perimeter)]:
        MODEL=bpy.data.collections.new('CRAH_'+kind+'_Editable_Parts')
        sc.collection.children.link(MODEL); models[kind]=MODEL
        build_fn(); reports[kind]=export_model(kind)
        MODEL.hide_render=True
    (OUT/'materials.json').write_text(json.dumps(PALETTE,indent=2),encoding='utf8')
    (OUT/'verification.json').write_text(json.dumps({'models':reports,'units':'metres',
        'geometry_bound_tolerance_m':1e-6,'triangle_limit_per_mesh':15000,
        'reference_scope':'Appearance only; project dimensions, no manufacturer CAD',
        'physics_tested':False},indent=2),encoding='utf8')
    STUDIO=bpy.data.collections.new('Studio'); sc.collection.children.link(STUDIO)
    floor=box('Studio_floor',(0,0,-.035),(200,200,.05),'Panel',0)
    MODEL.objects.unlink(floor); STUDIO.objects.link(floor)
    for name,loc,power,size in [('Key',(4,-4,6),1100,4),('Fill',(2,4,4),850,3),('Rim',(-3,1,5),1200,3)]:
        data=bpy.data.lights.new(name,'AREA'); data.energy=power; data.shape='DISK'; data.size=size
        ob=bpy.data.objects.new(name,data); STUDIO.objects.link(ob); ob.location=loc
        ob.rotation_euler=(Vector((0,0,1))-ob.location).to_track_quat('-Z','Y').to_euler()
    sc.world=bpy.data.worlds.new('CRAH_studio_world'); sc.world.use_nodes=True
    sc.world.node_tree.nodes['Background'].inputs[0].default_value=(.16,.18,.22,1)
    sc.world.node_tree.nodes['Background'].inputs[1].default_value=.4
    cameras={
        'InRow':camera('CAM_InRow_Front',(6,-3.7,3.2),(0,0,1.08),2.95),
        'InRowRear':camera('CAM_InRow_Rear',(-6,3.7,3.4),(0,0,1.08),2.95),
        'Perimeter':camera('CAM_Perimeter_Front',(5,-6,4.2),(0,0,.94),3.40),
        'PerimeterTop':camera('CAM_Perimeter_Top',(4,5,6),(0,0,.9),3.40),
    }
    sc.render.engine='CYCLES'; sc.cycles.samples=40; sc.cycles.use_denoising=True
    sc.render.resolution_x=1300; sc.render.resolution_y=1300; sc.render.resolution_percentage=100
    sc.render.image_settings.file_format='PNG'; sc.camera=cameras['InRow']
    sc['front_axis']='+X'; sc['origin']='floor centre'; sc['project_nominal_dimensions_m']=json.dumps(SPECS)
    sc['limitations']='Closed exteriors only; no internal equipment, no plumbing ports or manufacturer ratings'
    for kind,col in models.items():
        col.hide_render=kind!='InRow'
        col.hide_viewport=kind!='InRow'
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/'CRAH_Reference.blend'),compress=True)
    for kind,cam_name,filename in [('InRow','InRow','crah_inrow_preview.png'),
            ('InRow','InRowRear','crah_inrow_rear.png'),
            ('Perimeter','Perimeter','crah_perimeter_preview.png'),
            ('Perimeter','PerimeterTop','crah_perimeter_top.png')]:
        for key,col in models.items(): col.hide_render=key!=kind; col.hide_viewport=key!=kind
        sc.camera=cameras[cam_name]; sc.render.filepath=str(OUT/filename)
        bpy.ops.render.render(write_still=True)
    print('CRAH_MODEL_PASS',json.dumps(reports))


if __name__=='__main__':
    build()
