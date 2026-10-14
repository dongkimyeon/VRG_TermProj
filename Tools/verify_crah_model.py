"""Asset bounds/material checks, plus transient actor checks in the real project.

Use --assets-only in an isolated asset-import project. It does NOT verify the
VRG actors, L_Test or physics. No levels are saved in either mode.
"""
import argparse
import json
from pathlib import Path
import unreal

parser = argparse.ArgumentParser()
parser.add_argument('--assets-only', action='store_true')
args, _ = parser.parse_known_args()
ROOT = Path(__file__).resolve().parents[1]
ART = ROOT / 'Art/CRAH'
source = json.loads((ART / 'verification.json').read_text(encoding='utf8'))


def vec(value):
    return [value.x, value.y, value.z]


report = {'engine_version': unreal.SystemLibrary.get_engine_version(),
          'project': unreal.Paths.project_dir(), 'scope': 'assets_only' if args.assets_only else 'assets_and_transient_actors',
          'level_saved': False, 'physics_tested': False, 'assets': [], 'actors': []}

for kind in ('InRow', 'Perimeter'):
    name = 'SM_CRAH_' + kind
    mesh = unreal.load_asset('/Game/Connections/' + name)
    assert mesh, name
    bb = mesh.get_bounding_box()
    measured = vec(bb.min) + vec(bb.max)
    authored = source['models'][kind]
    expected = [v * 100 for v in authored['bounds_min_m'] + authored['bounds_max_m']]
    error = max(abs(a - b) for a, b in zip(measured, expected))
    # Geometric import tolerance only (cm); this is not a physical-model residual.
    tolerance_cm = .001
    assert error <= tolerance_cm, (name, measured, expected, error)
    assert abs(bb.min.z) <= tolerance_cm
    slots = [str(slot.material_slot_name) for slot in mesh.static_materials]
    assert set(slots) == set(authored['materials']), slots
    assert len(slots) == 7
    for index, slot in enumerate(slots):
        assert mesh.get_material(index).get_path_name().startswith('/Game/Connections/Materials/MC_CRAH_')
    # This subsystem is not automatically instantiated in Python commandlets.
    subsystem = unreal.get_editor_subsystem(unreal.StaticMeshEditorSubsystem)
    if subsystem is None:
        subsystem = unreal.new_object(unreal.StaticMeshEditorSubsystem)
    triangles = mesh.get_num_triangles(0)
    vertices = mesh.get_num_vertices(0)
    uv_channels = subsystem.get_num_uv_channels(mesh, 0)
    # FBX import removes degenerate triangles; record both counts rather than
    # assuming the authored topology is preserved byte-for-byte by the importer.
    assert 0 < triangles <= authored['triangles'] <= 15000, triangles
    assert uv_channels >= 1
    assert subsystem.get_simple_collision_count(mesh) == 0
    report['assets'].append({'name': name, 'bounds_min_cm': vec(bb.min), 'bounds_max_cm': vec(bb.max),
        'max_import_bound_error_cm': error, 'tolerance_cm': tolerance_cm, 'material_slots': slots,
        'triangles': triangles, 'authored_triangles': authored['triangles'],
        'import_removed_triangles': authored['triangles'] - triangles,
        'vertices': vertices, 'uv_channels': uv_channels, 'simple_collision_count': 0})

if not args.assets_only:
    cls = unreal.load_class(None, '/Script/VRG_TermProj.CrahUnitActor')
    assert cls, 'Run --assets-only outside the built VRG project'
    subsystem = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    for kind, enum_value in [('InRow', unreal.CrahUnitKind.IN_ROW), ('Perimeter', unreal.CrahUnitKind.PERIMETER)]:
        actor = subsystem.spawn_actor_from_class(cls, unreal.Vector(0, 0, 0), transient=True)
        assert actor
        try:
            size = unreal.Vector(*source['models'][kind]['nominal_dimensions_m'])
            actor.set_editor_property('kind', enum_value)
            actor.set_editor_property('size_m', size)
            actor.set_editor_property('volume_flow_m3s', 1.234)
            initial = [vec(actor.get_editor_property('size_m')), str(actor.get_editor_property('kind')),
                       actor.get_editor_property('volume_flow_m3s')]
            for enabled in (True, False, True):
                actor.set_editor_property('use_reference_model', enabled)
                actor.refresh_visuals()
                body = next(c for c in actor.get_components_by_class(unreal.StaticMeshComponent) if c.get_name() == 'Body')
                assert body.static_mesh.get_name() == ('SM_CRAH_' + kind if enabled else 'Cube')
                assert body.get_collision_enabled() == unreal.CollisionEnabled.NO_COLLISION
                assert initial == [vec(actor.get_editor_property('size_m')), str(actor.get_editor_property('kind')),
                                   actor.get_editor_property('volume_flow_m3s')]
                expected_scale = [1., 1., 1.] if enabled else vec(size)
                assert max(abs(a-b) for a,b in zip(vec(body.get_editor_property('relative_scale3d')), expected_scale)) < 1e-9
                expected_offset = [0., 0., 0.] if enabled else [0., 0., size.z*50.]
                assert vec(body.get_editor_property('relative_location')) == expected_offset
            arrow = actor.get_components_by_class(unreal.ArrowComponent)[0]
            assert vec(arrow.get_editor_property('relative_location')) == [size.x*50., 0., size.z*(25. if kind == 'Perimeter' else 50.)]
            report['actors'].append({'kind': kind, 'option_fallback': True, 'input_values_unchanged': True,
                                     'supply_arrow_unchanged': True, 'no_collision': True})
        finally:
            subsystem.destroy_actor(actor)

report['status'] = 'PASS'
filename = 'ue_assets_verification.json' if args.assets_only else 'ue_verification.json'
(ART / filename).write_text(json.dumps(report, ensure_ascii=False, indent=2), encoding='utf8')
unreal.log('CRAH_VERIFY_PASS ' + json.dumps(report, ensure_ascii=False))
