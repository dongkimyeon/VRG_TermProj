"""UE Python commandlet: create the W6 water display materials (no level mutation).

Creates (idempotent, rebuilt on every run):
  /Game/Water/Materials/M_VrgWater       translucent water (floor surface + droplet/jet/film instances)
  /Game/Water/Materials/M_VrgWetSurface  dark glossy wet-surface overlay

Run after building the editor:
  & '<UE>/Engine/Binaries/Win64/UnrealEditor-Cmd.exe' '<repo>/VRG_TermProj.uproject' -run=pythonscript '-script=<repo>/Tools/install_water_materials.py' '-EnablePlugins=PythonScriptPlugin,EditorScriptingUtilities' -unattended -NullRHI

Vertex colour contract of M_VrgWater (floor UProceduralMeshComponent):
  R = depth / 10 mm (0..1), G,B = 0.5 + 0.5 * flow velocity x,y / (1 m/s).
  Instanced engine meshes (droplets, jet tubes, wall films) have white/absent vertex colour -> deep tint, constant ripple dir.
Colours, opacities and ripple amplitudes are display choices (not physical values).
Report: Saved/Water_Materials_Install.json, log line WATER_MATERIALS_PASS.
"""
import unreal, json
from pathlib import Path

ROOT = Path(unreal.Paths.project_dir()).resolve()
DEST = '/Game/Water/Materials'
at = unreal.AssetToolsHelpers.get_asset_tools()
ml = unreal.MaterialEditingLibrary
eal = unreal.EditorAssetLibrary
MP = unreal.MaterialProperty


def get_or_create(name):
    path = DEST + '/' + name
    if eal.does_asset_exist(path):
        mat = unreal.load_asset(path)
    else:
        mat = at.create_asset(name, DEST, unreal.Material, unreal.MaterialFactoryNew())
    assert mat, path
    ml.delete_all_material_expressions(mat)
    return mat, path


def try_set(mat, prop, value, info, key):
    """Set an editor property that may not exist (or be renamed) in 5.8; log instead of failing."""
    try:
        mat.set_editor_property(prop, value)
        info[key] = str(value)
        return True
    except Exception as e:  # noqa: BLE001
        unreal.log_warning('install_water_materials: %s.%s not set (%s)' % (mat.get_name(), prop, e))
        info[key] = 'not set: ' + str(e)
        return False


def enum_value(enum_name, *candidates):
    enum = getattr(unreal, enum_name, None)
    if enum is None:
        return None, None
    for c in candidates:
        v = getattr(enum, c, None)
        if v is not None:
            return v, enum_name + '.' + c
    return None, None


def setup_translucent(mat, info):
    # Blend mode must succeed.
    mat.set_editor_property('blend_mode', unreal.BlendMode.BLEND_TRANSLUCENT)
    info['blend_mode'] = 'BLEND_TRANSLUCENT'
    try_set(mat, 'shading_model', unreal.MaterialShadingModel.MSM_DEFAULT_LIT, info, 'shading_model')
    tlm, tlm_name = enum_value('TranslucencyLightingMode', 'TLM_SURFACE_PER_PIXEL_LIGHTING', 'TLM_SURFACE')
    if tlm is not None:
        if try_set(mat, 'translucency_lighting_mode', tlm, info, 'translucency_lighting_mode'):
            info['translucency_lighting_mode'] = tlm_name
    else:
        unreal.log_warning('install_water_materials: TranslucencyLightingMode enum not found')
        info['translucency_lighting_mode'] = 'not set: enum missing'
    try_set(mat, 'two_sided', True, info, 'two_sided')
    try_set(mat, 'used_with_instanced_static_meshes', True, info, 'used_with_instanced_static_meshes')


def const(mat, value, x, y):
    n = ml.create_material_expression(mat, unreal.MaterialExpressionConstant, x, y)
    n.set_editor_property('r', value)
    return n


def custom(mat, x, y, code, inputs, out_type, desc):
    n = ml.create_material_expression(mat, unreal.MaterialExpressionCustom, x, y)
    n.set_editor_property('code', code)
    n.set_editor_property('description', desc)
    n.set_editor_property('output_type', out_type)
    pins = []
    for name in inputs:
        ci = unreal.CustomInput()
        ci.set_editor_property('input_name', name)
        pins.append(ci)
    n.set_editor_property('inputs', pins)
    return n


def link(src, src_out, dst, dst_in):
    ok = ml.connect_material_expressions(src, src_out, dst, dst_in)
    assert ok, 'connect failed: %s.%s -> %s.%s' % (src.get_name(), src_out, dst.get_name(), dst_in)


def to_prop(src, src_out, prop):
    ok = ml.connect_material_property(src, src_out, prop)
    assert ok, 'connect property failed: %s' % prop


F1 = unreal.CustomMaterialOutputType.CMOT_FLOAT1
F3 = unreal.CustomMaterialOutputType.CMOT_FLOAT3

report = {'assets': {}}

# ---------------------------------------------------------------- M_VrgWater
water, water_path = get_or_create('M_VrgWater')
winfo = {}
setup_translucent(water, winfo)

vc = ml.create_material_expression(water, unreal.MaterialExpressionVertexColor, -1100, 0)
tint = ml.create_material_expression(water, unreal.MaterialExpressionVectorParameter, -1100, -200)
tint.set_editor_property('parameter_name', 'Tint')
tint.set_editor_property('default_value', unreal.LinearColor(1, 1, 1, 1))
opacity_scale = ml.create_material_expression(water, unreal.MaterialExpressionScalarParameter, -1100, 300)
opacity_scale.set_editor_property('parameter_name', 'OpacityScale')
opacity_scale.set_editor_property('default_value', 1.0)
fresnel = ml.create_material_expression(water, unreal.MaterialExpressionFresnel, -1100, 450)
fresnel.set_editor_property('exponent', 5.0)
fresnel.set_editor_property('base_reflect_fraction', 0.04)
wpos = ml.create_material_expression(water, unreal.MaterialExpressionWorldPosition, -1100, 650)
time = ml.create_material_expression(water, unreal.MaterialExpressionTime, -1100, 800)

# Base colour: shallow -> deep tint by depth factor (VertexColor.R), times Tint.
base = custom(water, -600, -100,
              'float3 shallow = float3(0.55, 0.75, 0.80);\n'
              'float3 deep = float3(0.05, 0.25, 0.35);\n'
              'return lerp(shallow, deep, saturate(Depth)) * Tint;',
              ['Depth', 'Tint'], F3, 'Water base colour')
link(vc, 'R', base, 'Depth')
link(tint, '', base, 'Tint')
to_prop(base, '', MP.MP_BASE_COLOR)

# Opacity: face-on 0.25 + 0.45 * depth, grazing -> 0.9 via Fresnel, times OpacityScale, clamped 0..1.
opac = custom(water, -600, 300,
              'float face = 0.25 + 0.45 * saturate(Depth);\n'
              'return saturate(lerp(face, 0.9, saturate(Fres)) * Scale);',
              ['Depth', 'Fres', 'Scale'], F1, 'Water opacity')
link(vc, 'R', opac, 'Depth')
link(fresnel, '', opac, 'Fres')
link(opacity_scale, '', opac, 'Scale')
to_prop(opac, '', MP.MP_OPACITY)

# Normal: small procedural ripple travelling along the flow direction (G,B) plus a cross term.
ripple = custom(water, -600, 650,
                'float2 d = (Flow.gb - 0.5) * 2.0;\n'
                'float s2 = dot(d, d);\n'
                'float2 dir = d * rsqrt(max(s2, 1e-4));\n'
                'float speed = saturate(sqrt(s2));\n'
                'float2 p = WP.xy;\n'
                'float ph = dot(p * 0.05, dir) - T * 4.0;\n'
                'float xr = sin(p.x * 0.07 + p.y * 0.05 + T * 1.3);\n'
                'float a = 0.08 * speed;\n'
                'float b = 0.04;\n'
                'float3 n = float3(a * sin(ph) * dir.x + b * cos(p.y * 0.06 + T * 1.1 + xr),\n'
                '                  a * sin(ph) * dir.y + b * sin(p.x * 0.06 - T * 0.9 + xr),\n'
                '                  1.0);\n'
                'return normalize(n);',
                ['Flow', 'WP', 'T'], F3, 'Water flow ripple normal')
# Flow gets the RGB vertex colour (float3); the code uses .gb = flow x,y.
link(vc, '', ripple, 'Flow')
link(wpos, '', ripple, 'WP')
link(time, '', ripple, 'T')
to_prop(ripple, '', MP.MP_NORMAL)

to_prop(const(water, 0.04, -300, 900), '', MP.MP_ROUGHNESS)
to_prop(const(water, 0.5, -300, 1000), '', MP.MP_SPECULAR)
to_prop(const(water, 0.0, -300, 1100), '', MP.MP_METALLIC)

# Refraction: IOR 1.33 into the Refraction input; method guarded (property/enum names vary across 5.x).
ior = const(water, 1.33, -300, 1200)
try:
    to_prop(ior, '', MP.MP_REFRACTION)
    winfo['refraction_input'] = 1.33
except Exception as e:  # noqa: BLE001
    unreal.log_warning('install_water_materials: refraction input not connected (%s)' % e)
    winfo['refraction_input'] = 'not set: ' + str(e)
rm, rm_name = enum_value('RefractionMode', 'RM_INDEX_OF_REFRACTION')
if rm is not None:
    if try_set(water, 'refraction_method', rm, winfo, 'refraction_method'):
        winfo['refraction_method'] = rm_name
else:
    unreal.log_warning('install_water_materials: RefractionMode.RM_INDEX_OF_REFRACTION not found; engine default refraction method kept')
    winfo['refraction_method'] = 'not set: enum missing (engine default)'

ml.recompile_material(water)
assert eal.save_loaded_asset(water), water_path
report['assets'][water_path] = winfo

# ---------------------------------------------------------- M_VrgWetSurface
wet, wet_path = get_or_create('M_VrgWetSurface')
sinfo = {}
setup_translucent(wet, sinfo)
col = ml.create_material_expression(wet, unreal.MaterialExpressionConstant3Vector, -400, 0)
col.set_editor_property('constant', unreal.LinearColor(0.02, 0.025, 0.03, 1))
to_prop(col, '', MP.MP_BASE_COLOR)
op = ml.create_material_expression(wet, unreal.MaterialExpressionScalarParameter, -400, 150)
op.set_editor_property('parameter_name', 'Opacity')
op.set_editor_property('default_value', 0.35)
to_prop(op, '', MP.MP_OPACITY)
to_prop(const(wet, 0.08, -400, 300), '', MP.MP_ROUGHNESS)
to_prop(const(wet, 0.6, -400, 400), '', MP.MP_SPECULAR)
to_prop(const(wet, 0.0, -400, 500), '', MP.MP_METALLIC)
ml.recompile_material(wet)
assert eal.save_loaded_asset(wet), wet_path
report['assets'][wet_path] = sinfo

# ------------------------------------------------------------------ verify
for path in (water_path, wet_path):
    m = unreal.load_asset(path)
    assert m and isinstance(m, unreal.Material), path
    assert m.get_editor_property('blend_mode') == unreal.BlendMode.BLEND_TRANSLUCENT, path

out = ROOT / 'Saved' / 'Water_Materials_Install.json'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(report, indent=2, ensure_ascii=False), encoding='utf-8')
unreal.log('WATER_MATERIALS_PASS ' + json.dumps(report, ensure_ascii=False))
