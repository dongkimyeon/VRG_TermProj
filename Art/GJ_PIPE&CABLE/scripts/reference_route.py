# Adapted from Art/Infrastructure/build_infrastructure.py at 5ec749b.
import bpy,math,json
from mathutils import Vector

def route(name, points, radius, material, collection, bend=0.1, role='', start='', end=''):
    """Editable Bezier, exact straight spans and tangent circular 90-degree fillets."""
    ps = [Vector(p) for p in points]
    knots = []

    def add(p):
        knots.append([p.copy(), p.copy(), p.copy()])
    add(ps[0])
    for i in range(1, len(ps) - 1):
        a, b, c = ps[i - 1:i + 2]
        incoming = (b - a).normalized()
        outgoing = (c - b).normalized()
        angle = math.acos(max(-1, min(1, incoming.dot(outgoing))))
        if angle < 1e-05:
            add(b)
            continue
        cut = min(bend, (b - a).length * 0.4, (c - b).length * 0.4)
        p = b - incoming * cut
        q = b + outgoing * cut
        arc_radius = cut / math.tan(angle / 2)
        handle = 4 / 3 * math.tan(angle / 4) * arc_radius
        add(p)
        knots[-1][2] = p + incoming * handle
        add(q)
        knots[-1][1] = q - outgoing * handle
    add(ps[-1])
    for i in range(len(knots) - 1):
        a, b = (knots[i], knots[i + 1])
        if (a[2] - a[0]).length < 1e-08 and (b[1] - b[0]).length < 1e-08:
            delta = (b[0] - a[0]) / 3
            a[2] = a[0] + delta
            b[1] = b[0] - delta
    cu = bpy.data.curves.new(name, 'CURVE')
    cu.dimensions = '3D'
    cu.resolution_u = 8
    cu.render_resolution_u = 12
    cu.bevel_depth = radius
    cu.bevel_resolution = 3
    cu.fill_mode = 'FULL'
    cu.use_fill_caps = True
    sp = cu.splines.new('BEZIER')
    sp.bezier_points.add(len(knots) - 1)
    for bp, (p, left, right) in zip(sp.bezier_points, knots):
        bp.co = p
        bp.handle_left_type = 'FREE'
        bp.handle_right_type = 'FREE'
        bp.handle_left = left
        bp.handle_right = right
    ob = bpy.data.objects.new(name, cu)
    collection.objects.link(ob)
    cu.materials.append(material)
    ob['role'] = role
    ob['diameter_m'] = 2 * radius
    ob['endpoint_start'] = start
    ob['endpoint_end'] = end
    ob['route_points_m'] = json.dumps([list(p) for p in points])
    ob['simulation_binding'] = 'Authoring asset; no solver binding'
    return ob
