"""Plain Python: verify connection_manifest.json keeps every data item on the rear (-X).
Checks (Blender metres, scene coordinates, +X = intake face):
  1. every Data endpoint has x < 0 and outward -X,
  2. no data cable centreline sample reaches x >= 0 (nothing on the front half),
  3. data centrelines keep clear of coolant hoses / manifolds (centreline distance minus both radii),
  4. data centrelines stay out of the switch and node bodies.
Visual clearance only; not a cable physics or bend-radius test.
"""
import json, math, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
m = json.loads((ROOT / 'Art/GJ_PIPE&CABLE/reports/connection_manifest.json').read_text(encoding='utf8'))

def bez(r, steps=24):
    b = r['bezier']; out = []
    for k in range(len(b) - 1):
        p0, p1, p2, p3 = b[k]['p'], b[k]['right'], b[k + 1]['left'], b[k + 1]['p']
        for i in range(steps + 1):
            t = i / steps; u = 1 - t
            out.append([u**3*p0[a] + 3*u*u*t*p1[a] + 3*u*t*t*p2[a] + t**3*p3[a] for a in range(3)])
    return out

data_ep = {k: v for k, v in m['endpoints'].items() if v['role'] == 'Data'}
bad_ep = [k for k, v in data_ep.items() if v['position_blender_m'][0] >= 0 or v['outward_blender'] != [-1.0, 0.0, 0.0]]
data = [r for r in m['routes'] if r['role'] in ('Lime', 'Cyan')]
cool = [r for r in m['routes'] if r['role'] in ('Rubber', 'Supply', 'Return')]
samples = {r['name']: bez(r) for r in data}
front = [n for n, s in samples.items() if max(p[0] for p in s) >= 0]
cool_s = [(r, bez(r, 12)) for r in cool]
worst = (1e9, '', '')
for r in data:
    for c, cs in cool_s:
        gap = min(math.dist(p, q) for p in samples[r['name']][::2] for q in cs) - (r['diameter_m'] + c['diameter_m']) / 2
        if gap < worst[0]: worst = (gap, r['name'], c['name'])
# Solid bodies per rack (rack-local x, y half-width, z range), from the shared NVL72 layout: every mounted
# device (trays and shelves), and both management switches after their 180 deg yaw.
sys.path.insert(0, str(ROOT / 'Art/GJ_PIPE&CABLE/scripts'))
import rear_data_layout as L
boxes = [((.055 - .42, .055 + .4575), .2415, (z, z + .0435)) for z in L.COMPUTE_Z + L.SWITCHTRAY_Z + L.SHELF_Z]
boxes += [((L.SWITCH_POS[0] - .342, L.SWITCH_POS[0] + .34), .241, (z, z + .0437)) for z in L.MGMT_Z]
inside = []
for n, s in samples.items():
    for p in s:
        yc = -.43 if p[1] < 0 else .43
        for (x0, x1), hw, (z0, z1) in boxes:
            if x0 + .003 < p[0] < x1 - .003 and abs(p[1] - yc) < hw - .003 and z0 + .003 < p[2] < z1 - .003: inside.append(n)
result = {'data_endpoints': len(data_ep), 'endpoints_not_rear': bad_ep,
          'data_endpoint_max_x_m': max(v['position_blender_m'][0] for v in data_ep.values()),
          'data_routes': len(data), 'routes_reaching_front': front,
          'data_route_max_x_m': max(max(p[0] for p in s) for s in samples.values()),
          'min_surface_gap_to_coolant_m': round(worst[0], 5), 'closest_pair': worst[1:],
          'routes_inside_switch_or_node': sorted(set(inside))}
ok = not bad_ep and not front and worst[0] > 0 and not inside and len(data_ep) == 80
result['status'] = 'PASS' if ok else 'FAIL'
print(json.dumps(result, indent=2))
sys.exit(0 if ok else 1)
