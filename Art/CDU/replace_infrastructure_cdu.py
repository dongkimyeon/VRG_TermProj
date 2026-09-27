"""Replace the two legacy CDU bodies while preserving every editable route.
Run against Art/Infrastructure/VRG_Connections.blend; also called by its builder.
"""
import bpy, math, json
from pathlib import Path
from mathutils import Matrix

ROOT=Path(__file__).resolve().parents[2]

def replace():
    sc=bpy.data.scenes['VRG_Infrastructure']; bpy.context.window.scene=sc
    def curves():
        return {o.name:[tuple(b.co) for s in o.data.splines for b in s.bezier_points] for o in sc.objects if o.type=='CURVE'}
    before=curves()
    with bpy.data.libraries.load(str(ROOT/'Art/CDU/CDU_Reference.blend'),link=False) as (src,dst):
        dst.collections=['CDU_Editable_Parts']
    original=dst.collections[0]
    target=bpy.data.collections['VRG_01_RackContext']
    for row,cy,sign in [('A',-1.8,-1),('B',1.8,1)]:
        for ob in list(sc.objects):
            if ob.name.startswith(row+'_CDU_'): bpy.data.objects.remove(ob,do_unlink=True)
        # Preserve the established .7 x 1.0 x 1.6 m envelope and bottom height.
        matrix=Matrix.Translation((-1.3,cy,.05)) @ Matrix.Rotation(sign*math.pi/2,4,'Z') @ Matrix.Diagonal((1/.9,.7/.8,1.6/1.9,1))
        parts=[]
        for ob in original.objects:
            if ob.name.startswith('Port_mounting_seat'): continue
            cp=ob.copy(); cp.data=ob.data.copy(); target.objects.link(cp)
            cp.matrix_world=matrix @ ob.matrix_world; parts.append(cp)
        bpy.ops.object.select_all(action='DESELECT')
        for ob in parts: ob.select_set(True)
        bpy.context.view_layer.objects.active=parts[0]; bpy.ops.object.join()
        parts[0].name=row+'_CDU_Cabinet'; parts[0]['asset_source']='Art/CDU/CDU_Reference.blend'
    for ob in list(original.objects): bpy.data.objects.remove(ob,do_unlink=True)
    bpy.data.collections.remove(original)
    assert before==curves(),'Editable routes changed'
    sc['cdu_reference_revision']='2026-09-27 user image'
    print('CDU_SCENE_REPLACED',len(before),'unchanged curves')

if __name__=='__main__':
    replace()
    import runpy
    ns=runpy.run_path(str(ROOT/'Art/Infrastructure/build_infrastructure.py'),run_name='asset_helpers')
    ns['validate']()
    bpy.ops.wm.save_as_mainfile(filepath=str(ROOT/'Art/Infrastructure/VRG_Connections.blend'),compress=True)
