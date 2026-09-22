import bpy,json,math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
manifest=json.loads((ROOT/'reports/connection_manifest.json').read_text(encoding='utf8')); result={}
for name,info in manifest['assets'].items():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.fbx(filepath=str(ROOT/'exports'/f'{name}.fbx'))
    ob=bpy.data.objects[name]; vs=[ob.matrix_world@v.co for v in ob.data.vertices]
    bounds=[[min(v[i] for v in vs) for i in range(3)],[max(v[i] for v in vs) for i in range(3)]]
    error=max(abs(bounds[j][i]-info['bounds_m'][j][i]) for j in range(2) for i in range(3))
    assert error<1e-5,(name,bounds)
    assert ob.data.uv_layers and all(math.isfinite(x) for v in vs for x in v)
    assert all(p.area>1e-14 for p in ob.data.polygons),name
    result[name]={'max_bounds_error_m':error,'triangles':len(ob.data.polygons),'uv_channels':len(ob.data.uv_layers)}
(ROOT/'reports/fbx_validation.json').write_text(json.dumps(result,indent=2),encoding='utf8')
print('FBX_VALIDATION_PASS',len(result))
