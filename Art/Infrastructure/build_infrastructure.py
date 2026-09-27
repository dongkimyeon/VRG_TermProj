"""Reference-inspired, metre-scale Blender authoring assets; no simulation changes.

Run in Blender's Scripting workspace or via --background --python this_file.py.
MCP can call build_base(), build_racks(), build_routes(), finish() separately.
Only creates a new named scene; existing user scenes are retained.
"""
import bpy
import math
import json
from pathlib import Path
from mathutils import Vector

OUT = Path(__file__).resolve().parent
SCENE = "VRG_Infrastructure"
PITCH = 0.66
RACKS = 4
ROWS = (("A", -1.8, -1), ("B", 1.8, 1))
MATS = {}


def scene():
    return bpy.data.scenes[SCENE]


def coll(name):
    return bpy.data.collections["VRG_" + name]


def mat(name, rgb, metallic=0.0, roughness=0.4, emission=0.0):
    m = bpy.data.materials.new("VRG_" + name)
    m.diffuse_color = (*rgb, 1)
    n = next(n for n in m.node_tree.nodes if n.type == "BSDF_PRINCIPLED")
    n.inputs["Base Color"].default_value = (*rgb, 1)
    n.inputs["Metallic"].default_value = metallic
    n.inputs["Roughness"].default_value = roughness
    if emission:
        n.inputs["Emission Color"].default_value = (*rgb, 1)
        n.inputs["Emission Strength"].default_value = emission
    return m


def materials():
    return {k: bpy.data.materials["VRG_" + k] for k in
            ("Frame", "Panel", "Steel", "Brass", "Cold", "Hot", "Cyan", "Lime",
             "Orange", "Rubber", "White", "Floor", "Dark", "LED")}


class Batch:
    """Compact disconnected detail mesh, grouped by component/material."""
    def __init__(self, name, material, collection, transform=None):
        self.name, self.material, self.collection = name, material, collection
        self.v, self.f, self.smooth = [], [], []
        self.transform = transform or (lambda p: p)

    def geometry(self, verts, faces, smooth=False):
        base = len(self.v)
        self.v.extend(tuple(self.transform(Vector(v))) for v in verts)
        self.f.extend(tuple(base+i for i in f) for f in faces)
        self.smooth.extend([smooth] * len(faces))

    def box(self, c, d):
        x,y,z = c
        a,b,h = (v/2 for v in d)
        self.geometry([(x+sx*a,y+sy*b,z+sz*h) for sx,sy,sz in
                       [(-1,-1,-1),(1,-1,-1),(1,1,-1),(-1,1,-1),
                        (-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]],
                      [(0,3,2,1),(4,5,6,7),(0,1,5,4),(1,2,6,5),(2,3,7,6),(3,0,4,7)])

    def cylinder(self, a, b, r, sides=16, inner=0):
        a,b = Vector(a),Vector(b)
        direction = (b-a).normalized()
        u = direction.cross(Vector((0,0,1)) if abs(direction.z)<0.9 else Vector((0,1,0))).normalized()
        w = direction.cross(u).normalized()
        verts=[]
        radii=(r,inner) if inner else (r,)
        for radius in radii:
            for p in (a,b):
                verts.extend(p+radius*(math.cos(i*math.tau/sides)*u+math.sin(i*math.tau/sides)*w) for i in range(sides))
        faces=[]
        for i in range(sides):
            j=(i+1)%sides
            faces.append((i,j,sides+j,sides+i))
            if inner:
                faces.extend([(2*sides+j,2*sides+i,3*sides+i,3*sides+j),
                              (j,i,2*sides+i,2*sides+j),
                              (sides+i,sides+j,3*sides+j,3*sides+i)])
        if not inner:
            faces.extend([tuple(reversed(range(sides))),tuple(range(sides,2*sides))])
        self.geometry(verts, faces, True)

    def finish(self, bevel=0):
        mesh=bpy.data.meshes.new(self.name)
        mesh.from_pydata(self.v, [], self.f)
        mesh.update()
        ob=bpy.data.objects.new(self.name,mesh)
        self.collection.objects.link(ob)
        mesh.materials.append(self.material)
        for p,s in zip(mesh.polygons,self.smooth): p.use_smooth=s
        if bevel:
            mod=ob.modifiers.new("Edge highlights", "BEVEL")
            mod.width=bevel
            mod.segments=2
            mod.limit_method="ANGLE"
        ob["units"]="m"
        return ob


def route(name, points, radius, material, collection, bend=0.10, role="", start="", end=""):
    """Editable Bezier, exact straight spans and tangent circular 90-degree fillets."""
    ps=[Vector(p) for p in points]
    knots=[]
    def add(p):
        knots.append([p.copy(),p.copy(),p.copy()])
    add(ps[0])
    for i in range(1,len(ps)-1):
        a,b,c=ps[i-1:i+2]
        incoming=(b-a).normalized(); outgoing=(c-b).normalized()
        angle=math.acos(max(-1,min(1,incoming.dot(outgoing))))
        if angle < 1e-5:
            add(b); continue
        cut=min(bend, (b-a).length*0.40,(c-b).length*0.40)
        p=b-incoming*cut; q=b+outgoing*cut
        arc_radius=cut/math.tan(angle/2)
        handle=4/3*math.tan(angle/4)*arc_radius
        add(p); knots[-1][2]=p+incoming*handle
        add(q); knots[-1][1]=q-outgoing*handle
    add(ps[-1])
    for i in range(len(knots)-1):
        a,b=knots[i],knots[i+1]
        if (a[2]-a[0]).length<1e-8 and (b[1]-b[0]).length<1e-8:
            delta=(b[0]-a[0])/3
            a[2]=a[0]+delta; b[1]=b[0]-delta
    cu=bpy.data.curves.new(name,"CURVE")
    cu.dimensions="3D"; cu.resolution_u=8; cu.render_resolution_u=12
    cu.bevel_depth=radius; cu.bevel_resolution=3; cu.fill_mode="FULL"; cu.use_fill_caps=True
    sp=cu.splines.new("BEZIER"); sp.bezier_points.add(len(knots)-1)
    for bp,(p,left,right) in zip(sp.bezier_points,knots):
        bp.co=p; bp.handle_left_type="FREE"; bp.handle_right_type="FREE"
        bp.handle_left=left; bp.handle_right=right
    ob=bpy.data.objects.new(name,cu); collection.objects.link(ob); cu.materials.append(material)
    ob["role"]=role; ob["diameter_m"]=2*radius
    ob["endpoint_start"]=start; ob["endpoint_end"]=end
    ob["route_points_m"]=json.dumps([list(p) for p in points])
    ob["simulation_binding"]="Authoring asset; no solver binding"
    return ob


def label(name,text,location,size,material,collection,rotation=(math.pi/2,0,0)):
    data=bpy.data.curves.new(name,"FONT"); data.body=text; data.size=size
    data.extrude=0.0003
    ob=bpy.data.objects.new(name,data); collection.objects.link(ob)
    ob.location=location; ob.rotation_euler=rotation; data.materials.append(material)
    return ob


def build_base():
    if SCENE in bpy.data.scenes:
        raise RuntimeError("VRG_Infrastructure already exists. Keep the existing scene; do not overwrite blindly.")
    sc=bpy.data.scenes.new(SCENE)
    bpy.context.window.scene=sc
    sc.unit_settings.system="METRIC"; sc.unit_settings.scale_length=1.0
    for name in ("01_RackContext","02_ColdSupply","03_WarmReturn","04_DataCables",
                 "05_PowerCables","06_TraysSupports","07_Fittings","08_Presentation"):
        co=bpy.data.collections.new("VRG_"+name); sc.collection.children.link(co)
    palette={"Frame":((.025,.035,.048),.65,.3),"Panel":((.065,.079,.095),.5,.38),
             "Steel":((.48,.55,.62),.8,.26),"Brass":((.48,.32,.11),.7,.27),
             "Cold":((.012,.29,.55),.4,.26),"Hot":((.60,.055,.027),.4,.29),
             "Cyan":((.06,.64,.79),.05,.34),"Lime":((.37,.8,.02),.05,.37),
             "Orange":((1,.22,.015),.05,.35),"Rubber":((.014,.02,.028),.05,.46),
             "White":((.77,.85,.92),.25,.32),"Floor":((.11,.15,.20),.25,.55),
             "Dark":((.006,.012,.024),.3,.36),"LED":((.15,.9,.28),0,.3)}
    for key,(rgb,m,r) in palette.items(): mat(key,rgb,m,r,2 if key=="LED" else 0)
    ms=materials(); co=coll("08_Presentation")
    b=Batch("STAGE_Raised_Platform",ms["Dark"],co)
    b.box((.45,0,-.115),(6.4,7.2,.20)); b.finish(.04)
    b=Batch("STAGE_Floor_Tiles",ms["Floor"],co)
    for x in range(10):
        for y in range(12): b.box((-2.4+x*.60,-3.3+y*.60,-.018),(.588,.588,.03))
    b.finish(.003)
    b=Batch("STAGE_Aisle_Marking",ms["White"],co)
    for yy in (-1.14,1.14): b.box((.99,yy,.002),(2.8,.018,.003))
    b.finish()
    sc["asset_scope"]="Reference-inspired GB200 context / editable connection geometry / no physics integration"
    sc["source_references"]="Image/190547 Patch Manager; 190658 trays; 190725 manifold; 190738 cables; 191036 floor piping"
    sc["rack_dimensions_m"]=[.6,1.2,2.0]
    sc["rack_count"]=8
    print("Created scene and materials; original scene preserved.")


def build_racks():
    ms=materials(); co=coll("01_RackContext")
    for row,cy,sign in ROWS:
        for r in range(RACKS):
            cx=r*PITCH; rid=f"{row}{r+1:02d}"
            # Proper 180-degree rotation for near row, not a mirrored mesh.
            def tr(p, cx=cx, cy=cy, sign=sign): return Vector((cx+sign*p[0],cy+sign*p[1],p[2]))
            frame=Batch(rid+"_Rack_Frame",ms["Frame"],co,tr)
            steel=Batch(rid+"_Mounting_Rails",ms["Steel"],co,tr)
            panels=Batch(rid+"_18_Compute_Trays",ms["Panel"],co,tr)
            trim=Batch(rid+"_Tray_Handles",ms["Brass"],co,tr)
            ports=Batch(rid+"_Port_Recesses",ms["Dark"],co,tr)
            leds=Batch(rid+"_Status_LEDs",ms["LED"],co,tr)
            for x in (-.28,.28):
                for y in (-.58,.58):
                    frame.box((x,y,1.04),(.04,.04,1.92))
                    steel.cylinder((x,y,0),(x,y,.12),.012)
                    steel.cylinder((x,y,0),(x,y,.025),.035)
            for z in (.10,1.97):
                frame.box((0,0,z),(.6,1.2,.06))
            for x in (-.296,.296):
                frame.box((x,-.04,1.06),(.008,1.03,1.78))
            for x in (-.256,.256):
                steel.box((x,.595,1.04),(.018,.015,1.77))
                for j in range(38): ports.box((x,.605,.18+j*.044),(.010,.004,.013))
            for j in range(18):
                z=.23+j*.081
                panels.box((0,-.01,z),(.49,1.06,.071))
                for face in (-1,1):
                    yy=face*.552
                    trim.box((0,yy,z-.026),(.43,.013,.006))
                    for xx in (-.223,.223): trim.box((xx,yy,z),(.012,.021,.06))
                    for k in range(12): ports.box((-.18+k*.029,yy+face*.008,z+.008),(.016,.006,.012))
                leds.box((.196,.566,z+.008),(.007,.008,.006))
            for z in (1.78,1.86):
                panels.box((0,.43,z),(.50,.27,.062))
                for j in range(20):
                    ports.box((-.218+j*.0228,.572,z),(.015,.008,.017))
            for bb,bev in ((frame,.005),(steel,0),(panels,.002),(trim,.001),(ports,0),(leds,0)):
                ob=bb.finish(bev); ob["rack_id"]=rid
            # ID on each outward-facing rack top fascia.
            rot=(math.pi/2,0,math.pi if sign>0 else 0)
            label(rid+"_ID",rid+" / COMPUTE",tr(Vector((.22,.623,1.944))),.033,ms["White"],co,rot)
    print("Built 8 rack contexts, 18 compute trays each.")


def build_routes():
    ms=materials(); fits=coll("07_Fittings"); supports=coll("06_TraysSupports")
    for row,cy,sign in ROWS:
        # A distinct CDU for each row, matching the independent-row project layout.
        cdu=Batch(row+"_CDU_Cabinet",ms["Frame"],coll("01_RackContext"))
        cdu.box((-1.3,cy,.85),(.70,1.0,1.6)); cdu.finish(.025)
        details=Batch(row+"_CDU_Details",ms["Steel"],fits)
        details.box((-1.3,cy+sign*.506,1.12),(.38,.015,.20))
        for zz in range(9): details.box((-1.3,cy+sign*.517,.45+zz*.035),(.43,.015,.008))
        details.finish(.002)
        screen=Batch(row+"_CDU_Display",ms["Dark"],fits)
        screen.box((-1.3,cy+sign*.519,1.13),(.31,.008,.125)); screen.finish()
        label(row+"_CDU_Label","CDU / "+row,(-1.52,cy+sign*.526,1.45),.07,ms["White"],fits,
              (math.pi/2,0,math.pi if sign>0 else 0))
        hardware=Batch(row+"_Pipe_Couplings",ms["Steel"],fits)
        brackets=Batch(row+"_Pipe_Supports",ms["Panel"],supports)
        for idx,(role,color,colname) in enumerate((("SUPPLY","Cold","02_ColdSupply"),("RETURN","Hot","03_WarmReturn"))):
            yy=cy+sign*(.78+idx*.16)
            c=coll(colname)
            route(row+"_"+role+"_Header",[(-1.3+idx*.20,cy+sign*.50,.25),(-1.3+idx*.20,yy,.25),(2.53,yy,.25)],.055,ms[color],c,.16,role,row+"_CDU_"+role,row+"_HEADER_END")
            hardware.cylinder((2.51,yy,.25),(2.555,yy,.25),.063)
            for xx in (-.76,-.1,.7,1.5,2.3):
                hardware.cylinder((xx-.02,yy,.25),(xx+.02,yy,.25),.066,24,.055)
                brackets.box((xx,yy,.09),(.035,.045,.15))
                brackets.box((xx,yy,.012),(.13,.13,.02))
                for dx in (-.045,.045):
                    hardware.cylinder((xx+dx,yy,.021),(xx+dx,yy,.03),.009,6)
            for r in range(RACKS):
                cx=r*PITCH; rid=f"{row}{r+1:02d}"
                mx=cx+sign*(-.24 if idx==0 else .24)
                my=cy+sign*.68
                route(rid+"_"+role+"_Riser",[(mx,yy,.25),(mx,yy,.39),(mx,my,.39),(mx,my,1.91)],.025,ms[color],c,.07,role,row+"_HEADER_"+str(r),rid+"_MANIFOLD_"+role)
                # Visible tee collar and isolating valve above the header.
                hardware.cylinder((mx,yy,.235),(mx,yy,.30),.038,20,.025)
                hardware.cylinder((mx,yy,.31),(mx,yy,.37),.041,20)
                hardware.cylinder((mx,yy,.34),(mx,yy+sign*.061,.34),.009,12)
                vb=Batch(rid+"_"+role+"_Valve_Handle",ms[color],fits)
                vb.box((mx+.045,yy+sign*.067,.34),(.115,.014,.015)); vb.finish(.004)
                hardware.cylinder((mx,my,1.89),(mx,my,1.935),.030,20)
                for zz in (.62,1.2,1.8):
                    hardware.cylinder((mx,my,zz-.018),(mx,my,zz+.018),.031,20,.025)
                    brackets.box((mx,cy+sign*.632,zz),(.052,.07,.026))
                for j in range(18):
                    z=.23+j*.081
                    portx=cx+sign*(-.15 if idx==0 else .15)
                    pz=z+(.012 if idx==0 else -.012)
                    pts=[(mx,my,z),(mx,cy+sign*.78,z),(portx,cy+sign*.78,pz),
                         (portx,cy+sign*.57,pz)]
                    hose=route(rid+f"_{role}_Hose_{j+1:02d}",pts,.0065,ms[color],c,.045,role,rid+"_MANIFOLD_"+role,rid+f"_TRAY_{j+1:02d}_"+role)
                    hose["rack_id"]=rid
                    hardware.cylinder((mx,my,z),(mx,cy+sign*.717,z),.012,12)
                    hardware.cylinder((portx,cy+sign*.562,pz),(portx,cy+sign*.61,pz),.012,12)
                    hardware.cylinder((portx,cy+sign*.59,pz),(portx,cy+sign*.603,pz),.014,12)
        hardware.finish(); brackets.finish(.002)
        # Overhead cable ladders at the existing project's 2.85 m height.
        ty=cy+sign*.88; z=2.85
        tray=Batch(row+"_Overhead_Ladder_Tray",ms["Steel"],supports)
        for edge in (-.20,.20): tray.box((.74,ty+edge,z),(3.92,.022,.10))
        for i in range(33): tray.box((-1.2+i*.12,ty,z-.042),(.022,.42,.014))
        for xx in (-1.05,.10,1.3,2.45):
            tray.box((xx,ty,z-.11),(.05,.53,.035))
            for dy in (-.245,.245):
                tray.cylinder((xx,ty+dy,z-.11),(xx,ty+dy,3.23),.005,8)
                tray.box((xx,ty+dy,3.23),(.08,.08,.018))
        tray.finish(.002)
        power_tray=Batch(row+"_Power_Tray",ms["Panel"],supports)
        py=ty+sign*.36
        for edge in (-.06,.06): power_tray.box((.74,py+edge,z),(3.92,.012,.05))
        for i in range(25): power_tray.box((-1.18+i*.16,py,z-.02),(.015,.13,.008))
        power_tray.finish(.002)
        for lane in range(4):
            y=py-.038+lane*.025
            route(row+f"_TRUNK_Power_{lane+1:02d}",[(-1.18,y,2.88),(2.65,y,2.88)],.0055,ms["Rubber"],coll("05_PowerCables"),.1,"POWER",row+"_POWER_IN",row+"_POWER_BUS")
        for r in range(RACKS):
            cx=r*PITCH; rid=f"{row}{r+1:02d}"
            connectors=Batch(rid+"_Cable_Plugs",ms["Rubber"],fits)
            clamps=Batch(rid+"_Cable_Combs",ms["Panel"],supports)
            for k in range(8):
                dx=-.20+k*.018
                yy=cy+sign*(.73+k*.014)
                route(rid+f"_DATA_Drop_{k+1:02d}",[(cx+dx,ty,2.88),(cx+dx,yy,2.88),
                      (cx+dx,yy,1.76),(cx+.06+k*.020,yy,1.76),(cx+.06+k*.020,cy+sign*.58,1.78)],.005,ms["Cyan"],coll("04_DataCables"),.09,"DATA",row+"_TRUNK",rid+"_PATCH_TOP")
            for j in range(18):
                zz=.23+j*.081
                # Orange/green patch wiring kept clear of water quick connectors.
                for k,(color,dx) in enumerate((("Lime",-.065),("Orange",.065))):
                    bundle_x=.287+(j%3)*.006
                    side=cx+sign*(bundle_x if k else -bundle_x)
                    portx=cx+sign*dx
                    yy=cy+sign*(.865+k*.022+(j//3)*.006)
                    pts=[(side,cy+sign*.61,1.87-k*.08),(side,yy,1.87-k*.08),
                         (side,yy,zz+.03),(portx,yy,zz+.03),(portx,cy+sign*.58,zz+.03)]
                    route(rid+f"_PATCH_{color}_{j+1:02d}",pts,.0024,ms[color],coll("04_DataCables"),.025,"DATA",rid+"_PATCH_"+color,rid+f"_TRAY_{j+1:02d}_DATA_{k}")
                    connectors.box((portx,cy+sign*.582,zz+.03),(.012,.034,.009))
                if j%3==0:
                    for side in (-1,1): clamps.box((cx+side*.30,cy+sign*.87,zz),(.035,.075,.012))
            for k in range(4):
                xx=cx+.25+k*.013
                pts=[(xx,py-.038+k*.025,2.88),(xx,cy+sign*.95,2.88),(xx,cy+sign*.95,.38+k*.035),
                     (cx+.20,cy+sign*.95,.38+k*.035),(cx+.20,cy+sign*.58,.38+k*.035)]
                route(rid+f"_POWER_{k+1:02d}",pts,.0055,ms["Rubber"],coll("05_PowerCables"),.055,"POWER",row+"_POWER_BUS",rid+"_PDU_"+str(k))
                connectors.box((cx+.20,cy+sign*.59,.38+k*.035),(.024,.045,.023))
            connectors.finish(.001); clamps.finish(.002)
    build_crossover()
    # Use the current CDU asset on regeneration as well as the saved scene.
    import runpy
    runpy.run_path(str(OUT.parent / "CDU" / "replace_infrastructure_cdu.py"), run_name="cdu_helpers")["replace"]()
    print("Created separate cold / warm / data / power routes, fittings and supports.")


def build_crossover():
    ms=materials(); ty=2.68; z=2.85
    co=coll("04_DataCables"); supports=coll("06_TraysSupports")
    for lane in range(12):
        offset=-.165+lane*.030
        route(f"TRUNK_Data_{lane+1:02d}",[(-1.18,-ty+offset,z+.025),(3.04-offset,-ty+offset,z+.025),
              (3.04-offset,ty-offset,z+.025),(-1.18,ty-offset,z+.025)],.008,ms["Cyan"],co,.34-offset,"DATA","A_TRUNK_IN","B_TRUNK_IN")
    for side in (-1,1):
        route("TRAY_Crossover_Rail_"+str(side),[(2.68,-ty+side*.20,z),(3.04-side*.20,-ty+side*.20,z),
              (3.04-side*.20,ty-side*.20,z),(2.68,ty-side*.20,z)],.011,ms["Steel"],supports,.34-side*.20,"TRAY")
    bars=Batch("TRAY_Crossover_Rungs",ms["Steel"],supports)
    for i in range(39): bars.box((3.04,-2.28+i*.12,z-.042),(.42,.022,.014))
    for cy in (-1.8,0,1.8):
        bars.box((3.04,cy,z-.11),(.53,.05,.035))
        for xx in (2.795,3.285):
            bars.cylinder((xx,cy,z-.11),(xx,cy,3.23),.005,8)
            bars.box((xx,cy,3.23),(.08,.08,.018))
    bars.finish(.002)


def camera(name, position, target, scale):
    data=bpy.data.cameras.new(name); data.type="ORTHO"; data.ortho_scale=scale
    ob=bpy.data.objects.new(name,data); coll("08_Presentation").objects.link(ob)
    ob.location=position; ob.rotation_euler=(Vector(target)-ob.location).to_track_quat('-Z','Y').to_euler()
    data.lens=48; data.clip_end=200
    return ob


def finish():
    sc=scene(); ms=materials(); co=coll("08_Presentation")
    sc.camera=camera("CAM_Overview",(9,-12,10),(.5,0,1.05),10.8)
    camera("CAM_Rack_Detail",(3.0,-5.2,2.6),(1.05,-2.5,1.2),2.45)
    camera("CAM_Pipe_Detail",(4.0,-5.8,2.3),(.9,-2.55,.48),3.8)
    for name,loc,power,size in [("KEY",(1,-4,8),2400,7),("FILL",(-4,1,5),1800,5),("RIM",(2,5,6),2700,5)]:
        data=bpy.data.lights.new("LIGHT_"+name,"AREA"); data.energy=power; data.shape="DISK"; data.size=size
        ob=bpy.data.objects.new("LIGHT_"+name,data); co.objects.link(ob); ob.location=loc
        ob.rotation_euler=(Vector((.6,0,.8))-ob.location).to_track_quat('-Z','Y').to_euler()
    world=bpy.data.worlds.new("VRG_Studio"); world.use_nodes=True
    bg=next(n for n in world.node_tree.nodes if n.type=="BACKGROUND")
    bg.inputs[0].default_value=(.10,.14,.20,1); bg.inputs[1].default_value=.45; sc.world=world
    try: sc.render.engine="CYCLES"
    except TypeError: pass
    if sc.render.engine=="CYCLES":
        sc.cycles.samples=32; sc.cycles.use_denoising=True
    sc.render.resolution_x=1600; sc.render.resolution_y=1200; sc.render.resolution_percentage=100
    sc.render.image_settings.file_format="PNG"
    sc.render.filepath=str(OUT/"overview.png")
    # Model labels are purpose labels, not simulated temperatures.
    label("STAGE_Title","VRG / CONNECTION SYSTEMS",(-2.25,-3.39,.012),.15,ms["White"],co,(0,0,0))
    label("STAGE_Subtitle","08 RACKS   /   EDITABLE ROUTES   /   METRE SCALE",(-2.23,-3.58,.013),.060,ms["Steel"],co,(0,0,0))
    for i,(color,txt) in enumerate((("Cold","COLD SUPPLY"),("Hot","WARM RETURN"),("Cyan","DATA"),("Rubber","POWER"))):
        x=-2.18+i*1.4
        b=Batch("LEGEND_"+color,ms[color],co); b.box((x,3.30,.01),(.15,.045,.012)); b.finish(.008)
        label("LEGEND_Text_"+color,txt,(x+.13,3.28,.015),.065,ms["White"],co,(0,0,0))
    for window in bpy.context.window_manager.windows:
        if window.scene==sc:
            for area in window.screen.areas:
                if area.type=="VIEW_3D":
                    area.spaces.active.region_3d.view_perspective="CAMERA"
                    area.spaces.active.region_3d.view_camera_zoom=22
                    area.spaces.active.overlay.show_overlays=False
                    area.spaces.active.shading.type="MATERIAL"
    validate()
    bpy.ops.wm.save_as_mainfile(filepath=str(OUT/"VRG_Connections.blend"))
    print("Saved",OUT/"VRG_Connections.blend")


def validate():
    sc=scene(); routes=[o for o in sc.objects if o.type=="CURVE"]
    report={"scene":sc.name,"units":"m","rack_count":sc["rack_count"],"object_count":len(sc.objects),
            "curve_count":len(routes),"counts_by_role":{},"errors":[],"routes":[]}
    for o in routes:
        role=o.get("role",""); report["counts_by_role"][role]=report["counts_by_role"].get(role,0)+1
        if not all(math.isfinite(v) for p in o.data.splines[0].bezier_points for v in p.co): report["errors"].append(o.name+": nonfinite")
        if o.data.bevel_depth<=0: report["errors"].append(o.name+": invalid radius")
        report["routes"].append({"name":o.name,"role":role,"diameter_m":o["diameter_m"],
            "start":o["endpoint_start"],"end":o["endpoint_end"],"points_m":json.loads(o["route_points_m"]),
            "bezier_knots_m":[{"co":list(p.co),"left":list(p.handle_left),"right":list(p.handle_right)}
                              for p in o.data.splines[0].bezier_points]})
    assert not report["errors"],report["errors"]
    (OUT/"routes_manifest.json").write_text(json.dumps(report,ensure_ascii=False,indent=2),encoding="utf-8")
    print(json.dumps({k:v for k,v in report.items() if k!="routes"}))
    return report


if __name__=="__main__":
    build_base(); build_racks(); build_routes(); finish()
