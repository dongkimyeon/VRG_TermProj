"""Verify exported curve endpoints actually meet the rigid connector tails (metres)."""
import json,math
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1]
m=json.loads((ROOT/'reports/connection_manifest.json').read_text(encoding='utf8'))
errors=[]
for r in m['routes']:
    for side,knot in [('start',r['bezier'][0]),('end',r['bezier'][-1])]:
        name=r[side]
        if name not in m['endpoints']: continue
        e=m['endpoints'][name]; length=.063 if name.startswith('CDU.') else .044 if e['role']=='Data' else .0515
        expected=[p+length*d for p,d in zip(e['position_blender_m'],e['outward_blender'])]
        error=math.dist(expected,knot['p']); assert error<1e-6,(r['name'],side,error)
        errors.append(error)
result={'status':'PASS','connector_tail_joins':len(errors),'max_gap_m':max(errors),
        'scope':'Centreline endpoint-to-connector-tail alignment only. Not an obstacle or minimum bend-radius test.'}
(ROOT/'reports/port_validation.json').write_text(json.dumps(result,indent=2),encoding='utf8')
print(json.dumps(result))
