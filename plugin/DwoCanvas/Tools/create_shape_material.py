# DwoCanvas - builds /DwoCanvas/Materials/M_DwoCanvasShape (UI domain SDF rounded rectangle).
#
# UnrealEditor-Cmd.exe <Project>.uproject -run=pythonscript -script="<this file>"
#
# Parameters match FDwoCanvasShapeStyle::ApplyToMaterial and SDwoCanvasShape::OnPaint (Quad).

import unreal

PACKAGE_DIR = "/DwoCanvas/Materials"
NAME = "M_DwoCanvasShape"

HLSL = r"""
struct FDB
{
    float Sd(float2 q, float2 c, float4 R)
    {
        float r = (q.x > 0) ? ((q.y > 0) ? R.z : R.y) : ((q.y > 0) ? R.w : R.x);
        r = min(r, min(c.x, c.y));
        float2 d = abs(q) - c + r;
        return length(max(d, 0)) + min(max(d.x, d.y), 0) - r;
    }
    float4 Over(float4 src, float4 dst) { return src + dst * (1 - src.a); }
    float4 Pm(float4 c, float a) { return float4(c.rgb * a, a); }
    // Design tools interpolate gradients between sRGB-encoded stops
    float3 ToSrgb(float3 c) { c = max(c, 0); return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
    float3 ToLinear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
    float4 LerpSrgb(float4 a, float4 b, float t) { return float4(ToLinear(lerp(ToSrgb(a.rgb), ToSrgb(b.rgb), t)), lerp(a.a, b.a, t)); }
    // erf approximation (max error ~1e-4)
    float Erf(float x) { float x2 = x * x; float k = 0.147; return sign(x) * sqrt(1 - exp(-x2 * (1.2732395 + k * x2) / (1 + k * x2))); }
    // Coverage of a Gaussian-blurred edge at signed distance d (sigma = blur / 2, as in Figma / CSS box-shadow)
    float Shadow(float d, float blur, float aa) { float s = max(blur * 0.5, aa * 0.5); return 0.5 - 0.5 * Erf(d / (s * 1.4142136)); }
    // Layers are composited in sRGB like design tools (and like UMG blends widgets in gamma space)
    float4 S(float4 c) { return float4(ToSrgb(c.rgb), c.a); }
};
FDB F;

float pad = Quad.z;
float2 sz = max(Quad.xy - 2 * pad, float2(1, 1));
float2 p = UV * Quad.xy - pad;
float2 c = sz * 0.5;
float2 q = p - c;
float sd = F.Sd(q, c, Radii);
float aa = max(fwidth(sd), 0.0001);
float inside = saturate(0.5 - sd / aa);

// Fill
float4 fill = float4(0, 0, 0, 0);
if (FillType > 0.5)
{
    fill = Stop0;
    if (FillType > 1.5)
    {
        float2 s = GradientHandles.xy * sz;
        float2 e = GradientHandles.zw * sz;
        float2 w = GradientWidth.xy * sz;
        float t;
        if (FillType < 2.5)
        {
            float2 ax = e - s;
            t = dot(p - s, ax) / max(dot(ax, ax), 0.0001);
        }
        else
        {
            float2 a1 = e - s;
            float2 a2 = w - s;
            float det = a1.x * a2.y - a1.y * a2.x;
            det = abs(det) < 0.0001 ? 0.0001 : det;
            float2 v = p - s;
            float A = (v.x * a2.y - v.y * a2.x) / det;
            float B = (a1.x * v.y - a1.y * v.x) / det;
            if (FillType < 3.5) t = length(float2(A, B));
            else if (FillType < 4.5) t = frac(atan2(B, A) / 6.2831853 + 1.0);
            else t = abs(A) + abs(B);
        }
        t = saturate(t);
        fill = F.LerpSrgb(Stop0, Stop1, saturate((t - StopPositions.x) / max(StopPositions.y - StopPositions.x, 0.0001)));
        if (StopCount < 1.5) fill = Stop0;
        if (StopCount > 2.5 && t > StopPositions.y) fill = F.LerpSrgb(Stop1, Stop2, saturate((t - StopPositions.y) / max(StopPositions.z - StopPositions.y, 0.0001)));
        if (StopCount > 3.5 && t > StopPositions.z) fill = F.LerpSrgb(Stop2, Stop3, saturate((t - StopPositions.z) / max(StopPositions.w - StopPositions.z, 0.0001)));
    }
}

fill = F.S(fill);
float4 result = float4(0, 0, 0, 0);

// Drop shadow: only outside the shape (design tools hide it under the fill)
if (DropColor.a > 0)
{
    float sds = F.Sd(q - DropParams.xy, c, Radii) - DropParams.w;
    float sh = F.Shadow(sds, DropParams.z, aa);
    // Quad.w = 1: shadow also under the shape (shows through a translucent fill)
    result = F.Pm(F.S(DropColor), DropColor.a * sh * (Quad.w > 0.5 ? 1 : (1 - inside)));
}

result = F.Over(F.Pm(fill, fill.a * inside), result);

// Inner shadow
if (InnerColor.a > 0)
{
    float sdi = F.Sd(q - InnerParams.xy, c, Radii) + InnerParams.w;
    float ish = 1 - F.Shadow(sdi, InnerParams.z, aa);
    result = F.Over(F.Pm(F.S(InnerColor), InnerColor.a * ish * inside), result);
}

// Stroke (inside / center / outside, optional dash)
if (StrokeParams.x > 0 && StrokeColor.a > 0)
{
    float w = StrokeParams.x;
    float inner = StrokeParams.y < 0.5 ? -w : (StrokeParams.y < 1.5 ? -w * 0.5 : 0);
    float outer = inner + w;
    float band = saturate(0.5 - (sd - outer) / aa) * saturate(0.5 + (sd - inner) / aa);
    if (StrokeParams.z > 0)
    {
        float per;
        float2 nq = abs(q) / max(c, float2(0.0001, 0.0001));
        if (nq.y >= nq.x) per = (q.y < 0) ? (q.x + c.x) : (2 * c.x + 2 * c.y + (c.x - q.x));
        else per = (q.x > 0) ? (2 * c.x + (q.y + c.y)) : (4 * c.x + 2 * c.y + (c.y - q.y));
        float period = max(StrokeParams.z + StrokeParams.w, 0.0001);
        band *= (fmod(per, period) < StrokeParams.z) ? 1 : 0;
    }
    result = F.Over(F.Pm(F.S(StrokeColor), StrokeColor.a * band), result);
}

// Back to linear for Slate (it gamma-encodes the output again), then widget tint / opacity
float3 rgb = F.ToLinear(result.rgb / max(result.a, 0.0001)) * VC.rgb;
return float4(rgb, result.a * VCA);
"""

VECTORS = [
    ("Quad", unreal.LinearColor(100, 100, 1, 0)),
    ("Radii", unreal.LinearColor(0, 0, 0, 0)),
    ("Stop0", unreal.LinearColor(1, 1, 1, 1)),
    ("Stop1", unreal.LinearColor(1, 1, 1, 1)),
    ("Stop2", unreal.LinearColor(1, 1, 1, 1)),
    ("Stop3", unreal.LinearColor(1, 1, 1, 1)),
    ("StopPositions", unreal.LinearColor(0, 1, 1, 1)),
    ("GradientHandles", unreal.LinearColor(0, 0.5, 1, 0.5)),
    ("GradientWidth", unreal.LinearColor(0.5, 1, 0, 0)),
    ("StrokeColor", unreal.LinearColor(0, 0, 0, 1)),
    ("StrokeParams", unreal.LinearColor(0, 0, 0, 0)),
    ("DropColor", unreal.LinearColor(0, 0, 0, 0)),
    ("DropParams", unreal.LinearColor(0, 4, 8, 0)),
    ("InnerColor", unreal.LinearColor(0, 0, 0, 0)),
    ("InnerParams", unreal.LinearColor(0, 2, 4, 0)),
]
SCALARS = [("FillType", 1.0), ("StopCount", 1.0)]


def main():
    mel = unreal.MaterialEditingLibrary
    path = PACKAGE_DIR + "/" + NAME
    # load_object works before the asset registry has scanned plugin content (does_asset_exist may say no in a commandlet)
    existing = unreal.load_object(None, path + "." + NAME)
    if existing:
        # Update the shader code in place (keeps references; delete + create fails in unattended runs)
        material = existing
        # Expressions are not readable from Python: find the Custom node by its object path (outer = the material)
        customs = []
        for index in range(8):
            node = unreal.find_object(material, "MaterialExpressionCustom_%d" % index)
            if node:
                customs.append(node)
                break
        if customs:
            customs[0].set_editor_property("code", HLSL)
            mel.recompile_material(material)
            unreal.EditorAssetLibrary.save_loaded_asset(material, only_if_is_dirty=False)
            unreal.log("DwoCanvas: updated " + path)
            return
        unreal.EditorAssetLibrary.delete_asset(path)

    material = unreal.AssetToolsHelpers.get_asset_tools().create_asset(NAME, PACKAGE_DIR, unreal.Material, unreal.MaterialFactoryNew())
    material.set_editor_property("material_domain", unreal.MaterialDomain.MD_UI)
    material.set_editor_property("blend_mode", unreal.BlendMode.BLEND_TRANSLUCENT)

    custom = mel.create_material_expression(material, unreal.MaterialExpressionCustom, -300, 0)
    custom.set_editor_property("code", HLSL)
    custom.set_editor_property("output_type", unreal.CustomMaterialOutputType.CMOT_FLOAT4)
    custom.set_editor_property("description", "DwoCanvasShape SDF")

    input_names = ["UV", "VC", "VCA"] + [n for n, _ in VECTORS] + [n for n, _ in SCALARS]
    inputs = []
    for name in input_names:
        item = unreal.CustomInput()
        item.set_editor_property("input_name", name)
        inputs.append(item)
    custom.set_editor_property("inputs", inputs)

    y = -600
    uv = mel.create_material_expression(material, unreal.MaterialExpressionTextureCoordinate, -900, y)
    assert mel.connect_material_expressions(uv, "", custom, "UV")
    y += 80
    vc = mel.create_material_expression(material, unreal.MaterialExpressionVertexColor, -900, y)
    assert mel.connect_material_expressions(vc, "", custom, "VC")
    assert mel.connect_material_expressions(vc, "A", custom, "VCA")
    y += 120

    for name, default in VECTORS:
        param = mel.create_material_expression(material, unreal.MaterialExpressionVectorParameter, -900, y)
        param.set_editor_property("parameter_name", name)
        param.set_editor_property("default_value", default)
        ok = mel.connect_material_expressions(param, "RGBA", custom, name)
        if not ok:
            raise RuntimeError("could not connect vector parameter " + name)
        y += 120

    for name, default in SCALARS:
        param = mel.create_material_expression(material, unreal.MaterialExpressionScalarParameter, -900, y)
        param.set_editor_property("parameter_name", name)
        param.set_editor_property("default_value", default)
        assert mel.connect_material_expressions(param, "", custom, name)
        y += 80

    rgb = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, 0, -60)
    rgb.set_editor_property("r", True)
    rgb.set_editor_property("g", True)
    rgb.set_editor_property("b", True)
    rgb.set_editor_property("a", False)
    assert mel.connect_material_expressions(custom, "", rgb, "")

    alpha = mel.create_material_expression(material, unreal.MaterialExpressionComponentMask, 0, 60)
    alpha.set_editor_property("r", False)
    alpha.set_editor_property("g", False)
    alpha.set_editor_property("b", False)
    alpha.set_editor_property("a", True)
    assert mel.connect_material_expressions(custom, "", alpha, "")

    assert mel.connect_material_property(rgb, "", unreal.MaterialProperty.MP_EMISSIVE_COLOR)
    assert mel.connect_material_property(alpha, "", unreal.MaterialProperty.MP_OPACITY)

    mel.recompile_material(material)
    unreal.EditorAssetLibrary.save_loaded_asset(material)
    unreal.log("DwoCanvas: wrote " + path)


main()
