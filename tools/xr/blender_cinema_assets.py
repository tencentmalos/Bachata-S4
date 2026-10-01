# SPDX-License-Identifier: GPL-2.0-or-later
"""Build the XR cinema prop/room GLBs in headless Blender (5.x).

  blender -b --factory-startup --python tools/xr/blender_cinema_assets.py

Inputs: build/xr-cinema/textures (tools/xr/cinema_textures.py). Output:
assets/xr/cinema/models/*.glb, lit PBR (base colour / ORM / normal PNGs,
tangents), metres, glTF axes (Y up, -Z towards the screen). All geometry is
original and procedural. Coordinates below are written in glTF axes; P()
converts to Blender's Z-up frame, and the exporter converts back.
"""
import bpy
import bmesh
import json
import math
import struct
import sys
from pathlib import Path
from mathutils import Vector, Matrix
from mathutils.bvhtree import BVHTree

ROOT = Path(__file__).resolve().parents[2]
TEX = ROOT / 'build/xr-cinema/textures'
OUT = ROOT / 'assets/xr/cinema/models'
OUT.mkdir(parents=True, exist_ok=True)

# Shared layout (metres, glTF axes): a bigger screen to use the headset's
# field of view. Flat: 4.40 x 2.475 (16:9, ~200 in) 3.0 m away, centre 1.55 m
# (~72 x 45 deg at the seated eye 1.45 m).
SW, SH, SD, SCY = 4.40, 2.475, 3.0, 1.55
BACK = -3.12          # screen wall (inner face) of the two rooms
ROOM_W, ROOM_H = 7.0, 3.1




def P(x, y, z):
    return Vector((x, -z, y))


# ---------------------------------------------------------------- materials
_images = {}
MAT_PATCH = {}  # material name -> extra glTF fields applied after export


def image(name, colour):
    key = (name, colour)
    if key not in _images:
        img = bpy.data.images.load(str(TEX / name), check_existing=True)
        img.colorspace_settings.name = 'sRGB' if colour else 'Non-Color'
        _images[key] = img
    return _images[key]


def gltf_output_group():
    g = bpy.data.node_groups.get('glTF Material Output')
    if g:
        return g
    g = bpy.data.node_groups.new('glTF Material Output', 'ShaderNodeTree')
    g.interface.new_socket('Occlusion', in_out='INPUT', socket_type='NodeSocketFloat')
    g.interface.new_socket('Thickness', in_out='INPUT', socket_type='NodeSocketFloat')
    return g


MATS = {}


def mat(name, tex=None, colour=(.5, .5, .5), rough=.5, metal=0., emissive=None,
        strength=1., alpha=None, unlit=False, normal=1., clearcoat=None, mask=False, blend_tex=False):
    if name in MATS:
        return MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    bsdf = next(n for n in nt.nodes if n.type == 'BSDF_PRINCIPLED')
    bsdf.inputs['Base Color'].default_value = (*colour, 1)
    bsdf.inputs['Roughness'].default_value = rough
    bsdf.inputs['Metallic'].default_value = metal
    if tex:
        alb = nt.nodes.new('ShaderNodeTexImage')
        plain = tex.endswith(('sky', 'skyline', 'glow')) or tex.startswith('cover')
        alb.image = image(tex + ('.png' if plain else '_albedo.png'), True)
        nt.links.new(alb.outputs['Color'], bsdf.inputs['Base Color'])
        if mask or blend_tex:
            nt.links.new(alb.outputs['Alpha'], bsdf.inputs['Alpha'])
        if not unlit and not plain:
            orm = nt.nodes.new('ShaderNodeTexImage')
            orm.image = image(tex + '_orm.png', False)
            sep = nt.nodes.new('ShaderNodeSeparateColor')
            nt.links.new(orm.outputs['Color'], sep.inputs['Color'])
            nt.links.new(sep.outputs['Green'], bsdf.inputs['Roughness'])
            nt.links.new(sep.outputs['Blue'], bsdf.inputs['Metallic'])
            grp = nt.nodes.new('ShaderNodeGroup')
            grp.node_tree = gltf_output_group()
            nt.links.new(sep.outputs['Red'], grp.inputs['Occlusion'])
            nrm = nt.nodes.new('ShaderNodeTexImage')
            nrm.image = image(tex + '_normal.png', False)
            nm = nt.nodes.new('ShaderNodeNormalMap')
            nm.inputs['Strength'].default_value = normal
            nt.links.new(nrm.outputs['Color'], nm.inputs['Color'])
            nt.links.new(nm.outputs['Normal'], bsdf.inputs['Normal'])
    if emissive:
        bsdf.inputs['Emission Color'].default_value = (*emissive, 1)
        bsdf.inputs['Emission Strength'].default_value = strength
    if clearcoat is not None:
        bsdf.inputs['Coat Weight'].default_value = 1.
        bsdf.inputs['Coat Roughness'].default_value = clearcoat
    patch = {}
    if alpha is not None:
        bsdf.inputs['Alpha'].default_value = alpha
        m.surface_render_method = 'BLENDED'
        patch['alphaMode'] = 'BLEND'
    if blend_tex:
        m.surface_render_method = 'BLENDED'
        patch['alphaMode'] = 'BLEND'
    if mask:
        patch['alphaMode'] = 'MASK'
        patch['alphaCutoff'] = .5
    if unlit:
        patch['unlit'] = True
    if patch:
        MAT_PATCH[name] = patch
    MATS[name] = m
    return m


def materials():
    mat('oak_floor', 'oak_floor', normal=1.)
    mat('walnut', 'walnut', normal=.8)
    mat('ash', 'ash', normal=.8)
    mat('plaster', 'plaster', normal=.6)
    mat('fabric', 'fabric', normal=1.)
    mat('rug', 'rug', normal=1.)
    mat('plastic_matte', 'plastic_matte', normal=.4)
    mat('metal_brushed', 'metal_brushed', normal=.3)
    mat('teak_deck', 'teak_deck', normal=1.)
    mat('concrete', 'concrete', normal=.8)
    mat('black_powder', colour=(.018, .018, .02), rough=.55, metal=.6)
    mat('piano_black', colour=(.008, .008, .009), rough=.08, clearcoat=.03)
    mat('ps4_matte', 'plastic_matte', normal=.6)
    mat('ps4_gloss', colour=(.012, .012, .014), rough=.05, clearcoat=.02)
    mat('ds4_body', colour=(.020, .020, .022), rough=.55)
    mat('ds4_dark', colour=(.010, .010, .011), rough=.42)
    mat('ds4_rubber', colour=(.015, .015, .016), rough=.88)
    mat('ds4_button', colour=(.035, .036, .040), rough=.3)
    mat('touchpad', colour=(.020, .020, .022), rough=.7)
    mat('ds4_well', colour=(.20, .20, .21), rough=.35, metal=.35)
    mat('ds4_key', colour=(.016, .016, .018), rough=.22, clearcoat=.08)
    mat('light_blue', colour=(.05, .25, .6), rough=.3, emissive=(.10, .40, 1.), strength=2.5)
    mat('ps4_led', colour=(.05, .2, .6), rough=.3, emissive=(.12, .42, 1.), strength=3.)
    mat('ps4_glow', 'ps4_glow', unlit=True, blend_tex=True)
    mat('teak', 'teak_deck', normal=.8)
    mat('led_warm', colour=(1., .7, .4), rough=.5, emissive=(1., .62, .30), strength=12.)
    mat('bulb', colour=(1., .8, .5), rough=.2, emissive=(1., .55, .22), strength=4.)
    mat('downlight', colour=(1, 1, 1), rough=.3, emissive=(1., .86, .7), strength=10.)
    mat('screen_glass', colour=(.004, .004, .005), rough=.04)
    mat('tv_edge', colour=(.05, .05, .055), rough=.32, metal=.9)
    mat('speaker_grille', 'fabric', normal=1.4)
    mat('ceramic', colour=(.72, .69, .63), rough=.35)
    mat('ceramic_dark', colour=(.06, .06, .065), rough=.4)
    mat('book_a', colour=(.14, .17, .20), rough=.8)
    mat('book_b', colour=(.40, .33, .24), rough=.8)
    mat('book_c', colour=(.07, .09, .07), rough=.8)
    mat('leaf', colour=(.035, .085, .035), rough=.55)
    mat('grass', colour=(.09, .11, .05), rough=.7)
    mat('stem', colour=(.06, .05, .03), rough=.8)
    mat('soil', colour=(.03, .022, .016), rough=.95)
    mat('glass', colour=(.55, .65, .68), rough=.04, alpha=.16)
    mat('cable', colour=(.01, .01, .01), rough=.6)
    mat('sky', 'dusk_sky', unlit=True)
    for t in ('plaster_dark', 'oak_dark', 'sand', 'void_floor'):
        mat(t, t, normal=.8)
    mat('water', 'water', normal=1.)
    mat('sea_sky', 'sea_sky', unlit=True)
    mat('void_sky', 'void_sky', unlit=True)
    mat('rim_glow', colour=(.05, .2, .3), rough=.4, emissive=(.10, .45, .70), strength=4.)
    mat('ds5_white', colour=(.78, .79, .80), rough=.40)
    mat('ds5_black', colour=(.012, .012, .014), rough=.38)
    mat('ds5_button', colour=(.03, .03, .035), rough=.25, clearcoat=.05)
    mat('xbox_glow', colour=(.8, .8, .8), rough=.3, emissive=(1., 1., 1.), strength=2.)
    mat('xbox_a', colour=(.10, .55, .18), rough=.35, emissive=(.05, .30, .08), strength=.4)
    mat('xbox_b', colour=(.70, .08, .08), rough=.35, emissive=(.35, .03, .03), strength=.4)
    mat('xbox_x', colour=(.08, .28, .80), rough=.35, emissive=(.04, .14, .45), strength=.4)
    mat('xbox_y', colour=(.85, .65, .08), rough=.35, emissive=(.45, .33, .03), strength=.4)
    mat('logo_silver', colour=(.55, .55, .58), rough=.35, metal=.8)
    mat('logo_engraved', colour=(.010, .010, .011), rough=.25)
    for g, c in (('green', (.15, .85, .55)), ('red', (.95, .25, .25)), ('blue', (.35, .55, 1.)),
                 ('pink', (.95, .45, .75))):
        mat('glyph_' + g, colour=c, rough=.4, emissive=c, strength=.15)
    mat('case_blue', colour=(.015, .09, .32), rough=.32)
    mat('city_floor', colour=(.006, .007, .012), rough=1., unlit=True)
    mat('skyline', 'skyline', unlit=True, mask=True)
    mat('ceiling', 'plaster', normal=.4)
    mat('dock', colour=(.02, .02, .022), rough=.45)
    mat('felt', colour=(.03, .03, .032), rough=.95)
    mat('chrome_dark', colour=(.3, .3, .32), rough=.18, metal=1.)


# ---------------------------------------------------------------- geometry
def new_object(name, bm):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    ob = bpy.data.objects.new(name, me)
    bpy.context.scene.collection.objects.link(ob)
    return ob


def assign(ob, material):
    ob.data.materials.clear()
    ob.data.materials.append(MATS[material])


def apply_mods(ob):
    bpy.context.view_layer.objects.active = ob
    for o in bpy.context.selected_objects:
        o.select_set(False)
    ob.select_set(True)
    for mod in list(ob.modifiers):
        bpy.ops.object.modifier_apply(modifier=mod.name)


def bevel(ob, width, segments=3, angle=40):
    mod = ob.modifiers.new('bevel', 'BEVEL')
    mod.width = width
    mod.segments = segments
    mod.limit_method = 'ANGLE'
    mod.angle_limit = math.radians(angle)
    mod.harden_normals = True
    mod.miter_outer = 'MITER_ARC'
    for p in ob.data.polygons:
        p.use_smooth = True
    apply_mods(ob)


def smooth(ob, angle=35):
    for p in ob.data.polygons:
        p.use_smooth = True
    mod = ob.modifiers.new('wn', 'WEIGHTED_NORMAL')
    mod.keep_sharp = True
    ob.data.set_sharp_from_angle(angle=math.radians(angle))
    apply_mods(ob)


def box_uv(ob, tile, grain=0):
    """World-scale box projection in glTF axes; grain picks the axis (0 x,
    1 y, 2 z) laid along texture u, the wood grain direction."""
    me = ob.data
    if not me.uv_layers:
        me.uv_layers.new(name='UVMap')
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        n = poly.normal
        g = [abs(n.x), abs(n.z), abs(n.y)]  # glTF x, y, z magnitudes
        axis = g.index(max(g))
        others = [a for a in (0, 1, 2) if a != axis]
        if grain in others:
            u_axis = grain
            v_axis = others[0] if others[1] == grain else others[1]
        else:
            u_axis, v_axis = others
        for li in poly.loop_indices:
            co = ob.matrix_world @ me.vertices[me.loops[li].vertex_index].co
            gl = (co.x, co.z, -co.y)
            uv[li].uv = (gl[u_axis] / tile, gl[v_axis] / tile)


def cube(name, centre, size, material, bev=0., seg=3, tile=1., grain=0, shear=None):
    """Axis-aligned box in glTF coords. shear=(k): x' = x + k*y (unused) or
    a callable editing the bmesh verts in glTF space."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.)
    sx, sy, sz = size
    for v in bm.verts:
        x, y, z = v.co.x * sx, v.co.z * sy, -v.co.y * sz  # local glTF
        if shear:
            x, y, z = shear(x, y, z)
        v.co = P(centre[0] + x, centre[1] + y, centre[2] + z)
    ob = new_object(name, bm)
    assign(ob, material)
    if bev:
        bevel(ob, bev, seg)
    else:
        smooth(ob, 30)
    box_uv(ob, tile, grain)
    return ob


def cylinder(name, centre, radius, height, material, axis='y', segments=32, bev=0., tile=.5,
             radius_top=None, cap=True):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=cap, cap_tris=False, segments=segments,
                          radius1=radius, radius2=radius if radius_top is None else radius_top,
                          depth=height)
    rot = {'y': Matrix.Identity(4), 'x': Matrix.Rotation(math.pi / 2, 4, 'Y'),
           'z': Matrix.Rotation(math.pi / 2, 4, 'X')}[axis]
    bmesh.ops.transform(bm, matrix=rot, verts=bm.verts)
    bmesh.ops.translate(bm, vec=P(*centre), verts=bm.verts)
    ob = new_object(name, bm)
    assign(ob, material)
    if bev:
        bevel(ob, bev, 3, 30)
    else:
        smooth(ob, 40)
    box_uv(ob, tile)
    return ob


def sphere(name, centre, radius, material, scale=(1, 1, 1), seg=24):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=seg, v_segments=seg // 2, radius=radius)
    for v in bm.verts:
        x, y, z = v.co.x * scale[0], v.co.z * scale[1], -v.co.y * scale[2]
        v.co = P(centre[0] + x, centre[1] + y, centre[2] + z)
    ob = new_object(name, bm)
    assign(ob, material)
    for p in ob.data.polygons:
        p.use_smooth = True
    box_uv(ob, .3)
    return ob


def prism(name, outline, y0, y1, material, bev=0., seg=3, tile=1., grain=0):
    """Extrude a top-view outline [(x,z)...] (counter-clockwise seen from +y)
    from y0 to y1."""
    bm = bmesh.new()
    low = [bm.verts.new(P(x, y0, z)) for x, z in outline]
    high = [bm.verts.new(P(x, y1, z)) for x, z in outline]
    bm.faces.new(high)
    bm.faces.new(list(reversed(low)))
    n = len(outline)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new([low[i], low[j], high[j], high[i]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = new_object(name, bm)
    assign(ob, material)
    if bev:
        bevel(ob, bev, seg)
    else:
        smooth(ob)
    box_uv(ob, tile, grain)
    return ob


def rounded_rect(w, d, r, steps=6, cx=0., cz=0.):
    pts = []
    for (sx, sz, a0) in ((1, -1, -90), (1, 1, 0), (-1, 1, 90), (-1, -1, 180)):
        ox, oz = cx + sx * (w / 2 - r), cz + sz * (d / 2 - r)
        for k in range(steps + 1):
            a = math.radians(a0 + 90 * k / steps)
            pts.append((ox + r * math.cos(a), oz + r * math.sin(a)))
    return pts  # x right, z toward viewer; CCW seen from +y in glTF


def tube(name, points, radius, material, segments=10):
    """Polyline tube (cables, rails); points in glTF coords."""
    curve = bpy.data.curves.new(name, 'CURVE')
    curve.dimensions = '3D'
    curve.bevel_depth = radius
    curve.bevel_resolution = max(1, segments // 4)
    sp = curve.splines.new('POLY')
    sp.points.add(len(points) - 1)
    for p, c in zip(sp.points, points):
        p.co = (*P(*c), 1)
    ob = bpy.data.objects.new(name, curve)
    bpy.context.scene.collection.objects.link(ob)
    bpy.context.view_layer.objects.active = ob
    for o in bpy.context.selected_objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.ops.object.convert(target='MESH')
    ob = bpy.context.view_layer.objects.active
    assign(ob, material)
    for p in ob.data.polygons:
        p.use_smooth = True
    box_uv(ob, .3)
    return ob


# ---------------------------------------------------------------- export
def clear():
    for ob in list(bpy.data.objects):
        bpy.data.objects.remove(ob, do_unlink=True)
    for me in list(bpy.data.meshes):
        bpy.data.meshes.remove(me)
    for cu in list(bpy.data.curves):
        bpy.data.curves.remove(cu)


def patch_glb(path):
    raw = bytearray(path.read_bytes())
    n = struct.unpack_from('<I', raw, 12)[0]
    doc = json.loads(raw[20:20 + n])
    rest = raw[20 + n:]
    used = set(doc.get('extensionsUsed', []))
    for m in doc.get('materials', []):
        p = MAT_PATCH.get(m.get('name'))
        if not p:
            continue
        if p.get('unlit'):
            m.setdefault('extensions', {})['KHR_materials_unlit'] = {}
            used.add('KHR_materials_unlit')
        if 'alphaMode' in p:
            m['alphaMode'] = p['alphaMode']
            if 'alphaCutoff' in p:
                m['alphaCutoff'] = p['alphaCutoff']
            m['doubleSided'] = True
    if used:
        doc['extensionsUsed'] = sorted(used)
    doc['asset']['generator'] = 'shadPS4 cinema authoring (Blender)'
    j = json.dumps(doc, separators=(',', ':')).encode()
    j += b' ' * ((-len(j)) % 4)
    body = struct.pack('<I4s', len(j), b'JSON') + j + rest
    path.write_bytes(struct.pack('<III', 0x46546c67, 2, 12 + len(body)) + body)


EXPORTED = []


def export(name):
    for o in bpy.context.selected_objects:
        o.select_set(False)
    for ob in bpy.context.scene.objects:
        ob.select_set(True)
        if ob.type == 'MESH':
            tri = ob.modifiers.new('tri', 'TRIANGULATE')
            tri.min_vertices = 5
            tri.keep_custom_normals = True
    path = OUT / (name + '.glb')
    bpy.ops.export_scene.gltf(filepath=str(path), export_format='GLB', use_selection=True,
                              export_tangents=True, export_apply=True, export_yup=True,
                              export_image_format='AUTO', export_texcoords=True, export_normals=True,
                              export_materials='EXPORT', export_cameras=False, export_lights=False)
    patch_glb(path)
    EXPORTED.append(name)
    clear()


# ---------------------------------------------------------------- props
def coffee_table():
    """Walnut slab, 1.40 x 0.60, top at 0.52 m; black steel sled legs."""
    top_h, t = .52, .038
    prism('Table top', rounded_rect(1.40, .60, .045), top_h - t, top_h, 'walnut', bev=.008, tile=1.2)
    tube_w = .028
    for x in (-.58, .58):
        # sled: two uprights, top rail and floor runner
        for z in (-.23, .23):
            cube(f'Table leg {x:+.2f} {z:+.2f}', (x, (top_h - t) / 2, z), (tube_w, top_h - t, tube_w),
                 'black_powder', bev=.003, tile=.5)
        cube(f'Table runner {x:+.2f}', (x, tube_w / 2, 0), (tube_w, tube_w, .46 + tube_w), 'black_powder',
             bev=.003, tile=.5)
        cube(f'Table rail {x:+.2f}', (x, top_h - t - .012, 0), (.04, .024, .46 + tube_w), 'black_powder',
             bev=.003, tile=.5)
    cube('Table stretcher', (0, .14, 0), (1.16, .022, .022), 'black_powder', bev=.003)
    export('coffee-table')


def text_on_face(name, body, size, centre, rot_x, material, extrude=.00015):
    """Flat lettering laid on a face: rot_x (degrees, Blender X) turns the
    text plane from facing up to the face's normal."""
    cu = bpy.data.curves.new(name, 'FONT')
    cu.body = body
    cu.size = size
    cu.align_x = 'CENTER'
    cu.align_y = 'CENTER'
    cu.extrude = extrude
    ob = bpy.data.objects.new(name, cu)
    bpy.context.scene.collection.objects.link(ob)
    ob.rotation_euler = (math.radians(rot_x), 0, 0)
    ob.location = P(*centre)
    for o in bpy.context.selected_objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.convert(target='MESH')
    ob = bpy.context.view_layer.objects.active
    apply_transform(ob)
    assign(ob, material)
    box_uv(ob, .1)
    return ob


def ps4_pro():
    """PS4 Pro (CUH-7000 series), checked against the Wikimedia Commons
    product photo Sony-PlayStation4-Pro-Console-FL and the CUH-72xx
    side/front photos: ~295 x 55 x 327 mm, three stacked matte slabs of the
    same footprint separated by two recessed grooves; front/back faces lean
    forward at the top; disc slot and two USB ports in the lower groove at
    the front, a thin light strip along it; SONY at the left and PS4 at the
    right of the top slab's front face, the PlayStation logo on top (not
    modelled). Built lying flat (front +z), then stood on its side with the
    top face towards +x (the room centre)."""
    k = math.tan(math.radians(12))
    W, D = .295, .327
    slabs = ((0, .0145), (.0185, .0330), (.0370, .0550))   # bottom, middle, top
    grooves = ((.0145, .0185), (.0330, .0370))
    top = .055

    def slab(name, x0, x1, y0, y1, d, mat_, bev=.0018, zoff=0.):
        yc = (y0 + y1) / 2

        def sh(x, y, z):
            return x, y, z + (y + yc) * k
        return cube(name, ((x0 + x1) / 2, yc, zoff), (x1 - x0, y1 - y0, d), mat_, bev=bev, shear=sh)

    front = lambda y: D / 2 + y * k
    for i, (y0, y1) in enumerate(slabs):
        slab('PS4 Pro slab %d' % i, -W / 2, W / 2, y0, y1, D, 'ps4_matte')
    for i, (y0, y1) in enumerate(grooves):
        slab('PS4 Pro groove %d' % i, -W / 2 + .010, W / 2 - .010, y0 - .001, y1 + .001, D - .030, 'ds4_dark',
             bev=0, zoff=-.003)
    # Lower groove: light strip, disc slot (left), two USB ports (right).
    g0, g1 = grooves[0]
    yg = (g0 + g1) / 2
    zg = front(yg) - .015
    cube('PS4 Pro light strip', (-.02, g0 + .0007, zg + .0012), (W - .08, .0011, .0015), 'ps4_led')
    cube('PS4 Pro disc slot', (-.07, yg + .0006, zg + .0006), (.13, .0014, .002), 'ps4_gloss')
    for x in (.075, .100):
        cube('PS4 Pro USB', (x, yg, zg + .0006), (.0125, .0030, .002), 'chrome_dark')
    cube('PS4 Pro power', (-.125, yg, zg + .0006), (.006, .0016, .002), 'ds5_button')
    cube('PS4 Pro eject', (-.137, yg, zg + .0006), (.006, .0016, .002), 'ds5_button')
    yc = (slabs[2][0] + slabs[2][1]) / 2
    rot = 90 + math.degrees(math.atan(k))
    text_on_face('SONY', 'SONY', .0062, (-W / 2 + .028, yc, front(yc) + .0003), rot, 'logo_silver')
    text_on_face('PS4 mark', 'PS4', .0085, (W / 2 - .030, yc, front(yc) + .0003), rot, 'logo_engraved')
    # Glow card in front of the light strip (vertical, facing +z).
    bm = bmesh.new()
    gx0, gx1, gy0, gy1 = -W / 2 + .03, W / 2 - .07, g0 - .010, g0 + .012
    gz = front(g0) + .0025
    vs = [bm.verts.new(P(x, y, gz + (y - g0) * k)) for x, y in ((gx0, gy0), (gx1, gy0), (gx1, gy1), (gx0, gy1))]
    bm.faces.new(vs)
    card = new_object('PS4 Pro light glow', bm)
    assign(card, 'ps4_glow')
    me = card.data
    me.uv_layers.new(name='UVMap')
    for li, (u, v) in zip(range(4), ((0, 0), (0, 1), (1, 1), (1, 0))):
        me.uv_layers.active.data[li].uv = (u, v)
    stand_t = .008
    rot_m = Matrix.Translation(P(-top / 2, W / 2 + stand_t, 0)) @ Matrix.Rotation(math.pi / 2, 4, 'Y')
    for ob in list(bpy.context.scene.objects):
        ob.matrix_world = rot_m @ ob.matrix_world
        apply_transform(ob)
    cube('PS4 vertical stand', (0, stand_t / 2, 0), (.11, stand_t, .22), 'black_powder', bev=.003)
    cube('PS4 stand rim', (0, stand_t + .002, 0), (.062, .004, .21), 'ds4_dark', bev=.0015)
    export('ps4-pro')


def fused_body(name, centre, wings, grip, material, voxel=.0012):
    """Rounded centre box + two wings + two sphere-chain grips, voxel
    remeshed into one smooth shell resting on y=0."""
    parts = [cube(name + ' centre', centre[0], centre[1], material, bev=centre[2], seg=4)]
    for side in (-1, 1):
        (wx, wy, wz), size, bev_ = wings
        parts.append(cube(name + ' wing', (side * wx, wy, wz), size, material, bev=bev_, seg=4))
        g0, g1, r0, r1 = grip
        a0 = Vector((side * g0[0], g0[1], g0[2]))
        a1 = Vector((side * g1[0], g1[1], g1[2]))
        for i in range(21):
            t = i / 20
            parts.append(sphere(name + ' grip', tuple(a0 * (1 - t) + a1 * t), r0 * (1 - t) + r1 * t, material,
                                seg=20))
    for o in bpy.context.selected_objects:
        o.select_set(False)
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    body = bpy.context.view_layer.objects.active
    body.name = name
    rm = body.modifiers.new('remesh', 'REMESH')
    rm.mode = 'VOXEL'
    rm.voxel_size = voxel
    sm = body.modifiers.new('smooth', 'SMOOTH')
    sm.factor = .7
    sm.iterations = 10
    dec = body.modifiers.new('dec', 'DECIMATE')
    dec.ratio = .35
    apply_mods(body)
    for p in body.data.polygons:
        p.use_smooth = True
    assign(body, material)
    low = min((body.matrix_world @ v.co).z for v in body.data.vertices)
    body.location.z -= low
    bpy.context.view_layer.update()
    apply_transform(body)
    box_uv(body, .3)
    tree = BVHTree.FromObject(body, bpy.context.evaluated_depsgraph_get())

    def top(x, z):
        hit = tree.ray_cast(P(x, 1., z), Vector((0, 0, -1)))
        return hit[0].z if hit[0] else .03
    return body, tree, top


def overlay(name, tree, inside, bounds, material, offset=.0006, thick=.0008, res=.0015):
    """Conforming skin over the body where inside(x, z) holds: a grid
    projected down onto the shell, then given thickness."""
    (x0, x1), (z0, z1) = bounds
    nx, nz = int((x1 - x0) / res), int((z1 - z0) / res)
    bm = bmesh.new()
    grid = {}
    for j in range(nz + 1):
        for i in range(nx + 1):
            x, z = x0 + (x1 - x0) * i / nx, z0 + (z1 - z0) * j / nz
            if not inside(x, z):
                continue
            hit = tree.ray_cast(P(x, 1., z), Vector((0, 0, -1)))
            if hit[0] is None:
                continue
            grid[i, j] = bm.verts.new(hit[0] + hit[1] * offset)
    for (i, j) in list(grid):
        q = [(i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1)]
        if all(k in grid for k in q):
            bm.faces.new([grid[k] for k in q])
    ob = new_object(name, bm)
    me = ob.data
    if me.polygons and me.polygons[0].normal.z < 0:
        me.flip_normals()
    assign(ob, material)
    sol = ob.modifiers.new('sol', 'SOLIDIFY')
    sol.thickness = thick
    sol.offset = -1
    for p in me.polygons:
        p.use_smooth = True
    apply_mods(ob)
    box_uv(ob, .3)
    return ob


def overlay_shape(name, tree, centre, boundary, material, offset=.0006, thick=.0008, rings=10):
    """Conforming skin over a convex top-view region: a fan of rings from
    the centre to the boundary polyline (clean edges, unlike overlay's grid),
    projected down onto the shell and given thickness."""
    cx, cz = centre
    bm = bmesh.new()
    grid = []
    for k in range(rings + 1):
        t = k / rings
        row = []
        for bx, bz in (boundary if k else boundary[:1]):
            x, z = cx + (bx - cx) * t, cz + (bz - cz) * t
            hit = tree.ray_cast(P(x, 1., z), Vector((0, 0, -1)))
            row.append(bm.verts.new((hit[0] + hit[1] * offset) if hit[0] else P(x, .03, z)))
        grid.append(row)
    n = len(boundary)
    for i in range(n):
        bm.faces.new([grid[0][0], grid[1][i], grid[1][(i + 1) % n]])
    for k in range(1, rings):
        for i in range(n):
            j = (i + 1) % n
            bm.faces.new([grid[k][i], grid[k + 1][i], grid[k + 1][j], grid[k][j]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    ob = new_object(name, bm)
    me = ob.data
    if me.polygons and sum(p.normal.z for p in me.polygons) < 0:
        me.flip_normals()
    assign(ob, material)
    sol = ob.modifiers.new('sol', 'SOLIDIFY')
    sol.thickness = thick
    sol.offset = -1
    for p in me.polygons:
        p.use_smooth = True
    apply_mods(ob)
    box_uv(ob, .3)
    return ob


def circle_pts(cx, cz, r, n=48):
    return [(cx + r * math.cos(math.tau * i / n), cz + r * math.sin(math.tau * i / n)) for i in range(n)]


def in_rrect(cx, cz, w, d, r):
    def f(x, z):
        dx = max(abs(x - cx) - (w / 2 - r), 0)
        dz = max(abs(z - cz) - (d / 2 - r), 0)
        return dx * dx + dz * dz <= r * r
    return f


def stick(x, z, y, ring_mat, cap_mat):
    cylinder('Stick well', (x, y - .0015, z), .0128, .004, ring_mat, segments=32)
    cylinder('Stick shaft', (x, y + .006, z), .0052, .014, ring_mat, segments=16)
    cylinder('Stick cap', (x, y + .0138, z), .0098, .0045, cap_mat, segments=32, bev=.0018)


def symbol(kind, x, y, z, s, material):
    """Face-button glyph drawn with a thin tube on the cap (s = half size)."""
    if kind == 'triangle':
        pts = [(x, y, z - s), (x + s * .87, y, z + s * .5), (x - s * .87, y, z + s * .5), (x, y, z - s)]
        tube('Glyph', pts, .00035, material)
    elif kind == 'circle':
        pts = [(x + s * math.cos(a), y, z + s * math.sin(a)) for a in [math.tau * i / 20 for i in range(21)]]
        tube('Glyph', pts, .00035, material)
    elif kind == 'square':
        pts = [(x - s, y, z - s), (x + s, y, z - s), (x + s, y, z + s), (x - s, y, z + s), (x - s, y, z - s)]
        tube('Glyph', pts, .00035, material)
    else:
        tube('Glyph', [(x - s, y, z - s), (x + s, y, z + s)], .00035, material)
        tube('Glyph', [(x + s, y, z - s), (x - s, y, z + s)], .00035, material)


def ds4_body(material, voxel=.0009):
    """DS4 shell: top-view deck outline (bridge + wings) fused with two
    tapered grips that rest on the table, voxel remeshed into one surface.
    Coordinates: x right, z towards the player, front edge (light bar) z=0."""
    half = [(0, .003), (.030, .001), (.052, -.001), (.068, .003), (.077, .011), (.081, .024), (.080, .040),
            (.074, .056), (.060, .066), (.040, .066), (.026, .061), (.012, .063), (0, .064)]
    outline = half + [(-x, z) for x, z in reversed(half[1:-1])]
    area = sum(outline[i][0] * outline[(i + 1) % len(outline)][1] - outline[(i + 1) % len(outline)][0] * outline[i][1]
               for i in range(len(outline)))
    if area > 0:
        outline.reverse()
    parts = [prism('DS4 deck', outline, .011, .030, material, bev=.0075, seg=4)]
    for side in (-1, 1):
        # Grips leave the wings down and slightly outwards; ends touch the table.
        a0, a1 = Vector((side * .058, .0115, .046)), Vector((side * .064, .0165, .094))
        for i in range(25):
            t = i / 24
            r = .0185 * (1 - t) + .0165 * t
            parts.append(sphere('DS4 grip', tuple(a0 * (1 - t) + a1 * t), r, material,
                                scale=(1., .92, 1.), seg=20))
    for o in bpy.context.selected_objects:
        o.select_set(False)
    for o in parts:
        o.select_set(True)
    bpy.context.view_layer.objects.active = parts[0]
    bpy.ops.object.join()
    body = bpy.context.view_layer.objects.active
    body.name = 'DualShock 4 body'
    rm = body.modifiers.new('remesh', 'REMESH')
    rm.mode = 'VOXEL'
    rm.voxel_size = voxel
    sm = body.modifiers.new('smooth', 'SMOOTH')
    sm.factor = .6
    sm.iterations = 8
    dec = body.modifiers.new('dec', 'DECIMATE')
    dec.ratio = .3
    apply_mods(body)
    for p in body.data.polygons:
        p.use_smooth = True
    assign(body, material)
    low = min((body.matrix_world @ v.co).z for v in body.data.vertices)
    body.location.z -= low
    bpy.context.view_layer.update()
    apply_transform(body)
    box_uv(body, .3)
    tree = BVHTree.FromObject(body, bpy.context.evaluated_depsgraph_get())

    def top(x, z):
        hit = tree.ray_cast(P(x, 1., z), Vector((0, 0, -1)))
        return hit[0].z if hit[0] else .03
    return body, tree, top


def dpad_key(cx, cz, y, angle, material):
    """One D-pad arrow key: a pentagon whose point faces the pad centre."""
    w, inner, outer = .0084, .0042, .0142
    shape = [(-w / 2, -outer), (w / 2, -outer), (w / 2, -inner - .0032), (0, -inner), (-w / 2, -inner - .0032)]
    c, s = math.cos(angle), math.sin(angle)
    pts = [(cx + x * c - z * s, cz + x * s + z * c) for x, z in shape]
    if sum(pts[i][0] * pts[(i + 1) % 5][1] - pts[(i + 1) % 5][0] * pts[i][1] for i in range(5)) > 0:
        pts.reverse()
    return prism('D-pad key', pts, y - .0012, y + .0026, material, bev=.0009, seg=3)


def dualshock4():
    """Black DualShock 4 (CUH-ZCT2, 162 x 52 x 98 mm) traced from the Commons
    Sony-PlayStation-4-PS4-DualShock-4 photo: grey circular wells under the
    four arrow keys and the face buttons, a large touchpad on the front
    centre with SHARE/OPTIONS at its corners, speaker grille below it, the
    sticks low and inboard with the PS button between them, short fat grips,
    L1/R1 and L2/R2 on the front edge, light bar along the front face.
    Front (towards the TV) faces -z; origin on the table."""
    body, tree, top = ds4_body('ds4_body')
    for side, keys in ((-1, 'dpad'), (1, 'face')):
        cx, cz = side * .0595, .026
        overlay_shape('Button well', tree, (cx, cz), circle_pts(cx, cz, .0162), 'ds4_well', offset=.0004,
                      thick=.0006)
        if keys == 'dpad':
            for k in range(4):
                dpad_key(cx, cz, top(cx, cz) + .0010, k * math.pi / 2, 'ds4_key')
        else:
            for (dx, dz), (kind, m_) in zip(((0, -.0106), (.0106, 0), (0, .0106), (-.0106, 0)),
                                            (('triangle', 'glyph_green'), ('circle', 'glyph_red'),
                                             ('cross', 'glyph_blue'), ('square', 'glyph_pink'))):
                yb = top(cx + dx, cz + dz) + .0010
                cylinder('Face button', (cx + dx, yb, cz + dz), .0049, .0040, 'ds4_key', segments=28, bev=.0011)
                symbol(kind, cx + dx, yb + .0021, cz + dz, .0024, m_)
    overlay_shape('Touchpad', tree, (0, .0185), rounded_rect(.061, .030, .004, steps=4, cz=.0185), 'touchpad',
                  offset=.0009, thick=.0010, rings=6)
    for side in (-1, 1):
        x, z = side * .0385, .0085
        cube('Share Options', (x, top(x, z) + .0008, z), (.0042, .0024, .0088), 'ds4_key', bev=.0011,
             shear=lambda a, b, c, s=side: (a + s * .12 * c, b, c))
        sx, sz = side * .0275, .0495
        sy = top(sx, sz)
        cylinder('Stick ring', (sx, sy - .0005, sz), .0136, .0030, 'ds4_dark', segments=40, bev=.0008)
        cylinder('Stick shaft', (sx, sy + .0025, sz), .0062, .006, 'ds4_dark', segments=20)
        cylinder('Stick cap', (sx, sy + .0070, sz), .0101, .0046, 'ds4_rubber', segments=40, bev=.0016)
        cylinder('Stick dish', (sx, sy + .00935, sz), .0072, .0004, 'ds4_dark', segments=32)
        # L1/R1 on the shoulder, L2/R2 below and in front of it.
        bx = side * .056
        cube('Bumper', (bx, .0262, .0030), (.027, .0055, .0075), 'ds4_key', bev=.0022, seg=4)
        cube('Trigger', (bx, .0140, .0020), (.025, .0120, .0120), 'ds4_body', bev=.0040, seg=4,
             shear=lambda a, b, c: (a, b, c - .25 * b))
    for i in range(5):
        for j in range(3):
            if i in (0, 4) and j != 1:
                continue
            x, z = -.0048 + i * .0024, .0395 + j * .0022
            cylinder('Speaker hole', (x, top(x, z) - .0002, z), .00065, .0009, 'ds4_dark', segments=8)
    py = top(0, .058)
    cylinder('PS button', (0, py + .0005, .058), .0040, .0026, 'ds4_key', segments=28, bev=.0009)
    cylinder('PS ring', (0, py + .0001, .058), .0047, .0016, 'logo_silver', segments=28)
    cube('Light bar', (0, .0215, .0030), (.050, .0036, .0024), 'light_blue', bev=.0010)
    cube('EXT port', (0, top(0, .063) - .006, .0635), (.010, .0025, .002), 'ds4_dark', bev=.0006)
    export('dualshock4')


def disc_case(tag):
    """PS4 game case (Blu-ray Elite style, 135 x 14 x 171 mm), opaque blue
    with an original placeholder cover. Lies flat, front cover up, top
    (PS4 band) towards -z."""
    cube('Case', (0, .007, 0), (.135, .014, .171), 'case_blue', bev=.0025)
    bm = bmesh.new()
    vs = [bm.verts.new(P(x, .01415, z)) for x, z in ((-.066, .0845), (.066, .0845), (.066, -.0845), (-.066, -.0845))]
    bm.faces.new(vs)
    cover = new_object('Cover', bm)
    me = cover.data
    if me.polygons[0].normal.z < 0:
        me.flip_normals()
    mat('cover_' + tag, 'cover_' + tag, rough=.3)
    assign(cover, 'cover_' + tag)
    me.uv_layers.new(name='UVMap')
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            uv[li].uv = (co.x / .132 + .5, co.y / .169 + .5)
    # Covers made from the user's own games (tools/xr/game_covers.py) stay
    # in the git-ignored models/local/.
    export(('local/' if tag.startswith('CUSA') else '') + 'disc-case-' + tag)


def soundbar():
    """Low soundbar under the TV, 0.95 x 0.056 x 0.10 m: matte body, fabric
    front, small status LED."""
    prism('Soundbar body', rounded_rect(.95, .10, .03), 0, .056, 'plastic_matte', bev=.006, tile=.5)
    cube('Soundbar grille', (0, .028, .0505), (.89, .044, .003), 'speaker_grille', bev=.0012, tile=.5)
    cube('Soundbar LED', (.40, .049, .0525), (.006, .0015, .001), 'xbox_glow')
    export('soundbar')


def apply_transform(ob):
    for o in bpy.context.selected_objects:
        o.select_set(False)
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.transform_apply(location=True, rotation=True, scale=True)


def psv_dock():
    """Cradle for the 2.5x PSV status display. Origin: table surface under
    the display centre (World places it at the PSV x/z). The PSV is centred
    at y=.14, rotated -0.32 rad about x, scale 2.5."""
    a = -.32
    ca, sa = math.cos(a), math.sin(a)
    yv, zv = Vector((0, ca, sa)), Vector((0, -sa, ca))  # rotated axes
    centre = Vector((0, .14, 0))
    half_h, back = .0423 * 2.5, -.0108 * 2.5
    bottom = centre - yv * half_h
    bottom_back = bottom + zv * back
    # Base plate on the table.
    prism('PSV dock base', rounded_rect(.30, .17, .025, cz=-.01), 0, .016, 'dock', bev=.003)
    prism('PSV dock felt', rounded_rect(.29, .16, .022, cz=-.01), -.0015, 0, 'felt')
    # Ledge under the display's lower edge.
    ledge_top = bottom.y - .001
    cube('PSV dock ledge', (0, (ledge_top + .016) / 2, bottom.z + .002), (.24, ledge_top - .016, .034), 'dock',
         bev=.003)
    # Backrest plate parallel to the display back.
    rest_c = centre + zv * (back - .008) - yv * .035
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.)
    rot = Matrix.Rotation(a, 4, 'X')
    for v in bm.verts:
        loc = Vector((v.co.x * .16, v.co.z * .14, -v.co.y * .010))
        g = Vector((loc.x, loc.y * ca - loc.z * sa, loc.y * sa + loc.z * ca)) + rest_c
        v.co = P(*g)
    ob = new_object('PSV dock backrest', bm)
    assign(ob, 'dock')
    bevel(ob, .003)
    # Strut joining backrest to base.
    strut_top = rest_c - yv * .06
    cube('PSV dock strut', (0, (strut_top.y + .016) / 2, strut_top.z - .005), (.05, strut_top.y - .016, .02),
         'dock', bev=.003)
    cube('PSV dock LED', (.11, .0165, .065), (.010, .0012, .003), 'light_blue')
    export('psv-dock')
    return bottom, bottom_back


def tv():
    """Thin-bezel display; aperture SW x SH at local z=0 (front face),
    centred at the origin. Wall-mount bracket box behind."""
    w, h = SW, SH
    cube('TV panel', (0, 0, -.012), (w + .016, h + .016, .022), 'tv_edge', bev=.004, tile=.5)
    cube('TV glass', (0, 0, -.0005), (w, h, .002), 'screen_glass')
    cube('TV back', (0, -.05, -.05), (2.6, 1.3, .055), 'plastic_matte', bev=.01, tile=.5)
    cube('TV logo bar', (0, -h / 2 - .006, .0005), (.14, .004, .002), 'chrome_dark')
    export('tv')


def outdoor_screen_frame():
    """Black steel easel for the SW x SH aperture (origin = aperture centre,
    SCY above the deck)."""
    w, h = SW, SH
    cube('Screen panel', (0, 0, -.025), (w + .06, h + .06, .05), 'black_powder', bev=.008)
    cube('Screen glass', (0, 0, -.0005), (w, h, .002), 'screen_glass')
    for s in (-1, 1):
        x = s * (w / 2 - .45)
        cube('Screen post', (x, -SCY / 2, -.09), (.08, SCY, .08), 'black_powder', bev=.006)
        cube('Screen foot', (x, -SCY + .02, -.09), (.12, .04, .7), 'black_powder', bev=.006)
    cube('Screen crossbar', (0, -h / 2 + .2, -.09), (w - .9 + .08, .06, .05), 'black_powder', bev=.006)
    export('outdoor-screen')


def media_console():
    """Low floating walnut console, 5.2 x 0.24 x 0.42, fluted doors, top at
    0.30 so it stays under the bigger screen's lower edge."""
    W, H, D = 5.2, .24, .42
    y0 = .06
    cube('Console carcass', (0, y0 + H / 2, 0), (W, H, D), 'walnut', bev=.006, tile=1.2)
    pitch = .024
    n = int((W - .06) / pitch)
    bm = bmesh.new()
    for i in range(n):
        x = -W / 2 + .03 + pitch * (i + .5)
        ret = bmesh.ops.create_cone(bm, cap_ends=False, segments=12, radius1=pitch * .48, radius2=pitch * .48,
                                    depth=H - .04)
        for v in ret['verts']:
            v.co.y = max(v.co.y, 0) * .7
            v.co = P(x + v.co.x, y0 + H / 2 + v.co.z, D / 2 + v.co.y)
    ob = new_object('Console flutes', bm)
    assign(ob, 'walnut')
    smooth(ob, 60)
    box_uv(ob, 1.2, grain=1)
    for x in (-1.3, 0, 1.3):
        cube('Console door gap', (x, y0 + H / 2, D / 2 + .012), (.004, H - .04, .006), 'ds4_dark')
    cube('Console cleat', (0, y0 + .02, -D / 2 - .01), (W - .1, .03, .02), 'black_powder')
    # Decor only beyond the screen's edge (x > 2.2).
    cylinder('Vase', (2.50, y0 + H + .0575, -.02), .055, .115, 'ceramic', segments=36, radius_top=.04, bev=.004)
    export('media-console')


def side_cabinet():
    """Outdoor teak side cabinet 0.44 x 0.42 x 0.40 on black steel legs."""
    cube('Cabinet top', (0, .405, 0), (.44, .03, .40), 'teak', bev=.006, tile=1.)
    cube('Cabinet box', (0, .28, 0), (.42, .22, .38), 'teak', bev=.006, tile=1., grain=1)
    for x in (-.19, .19):
        for z in (-.17, .17):
            cube('Cabinet leg', (x, .085, z), (.025, .17, .025), 'black_powder', bev=.003)
    export('side-cabinet')


def tower_speaker():
    """Floor speaker 0.22 x 1.02 x 0.30: walnut cabinet, fabric grille, plinth."""
    w, h, d = .22, 1.02, .30
    cube('Speaker plinth', (0, .015, 0), (w + .06, .03, d + .06), 'black_powder', bev=.004)
    for x in (-.1, .1):
        for z in (-.12, .12):
            cylinder('Speaker spike', (x, .03 + .012, z), .008, .024, 'chrome_dark', segments=12, radius_top=.004)
    cube('Speaker cabinet', (0, .054 + h / 2, 0), (w, h, d), 'walnut', bev=.008, tile=1.2, grain=1)
    cube('Speaker grille', (0, .054 + h / 2, d / 2 + .006), (w - .02, h - .02, .012), 'speaker_grille', bev=.005,
         tile=.5)
    export('tower-speaker')


def plant():
    """Potted indoor plant ~1.35 m: tapered pot, soil, stems with leaves."""
    import random
    rnd = random.Random(5)
    cylinder('Pot', (0, .21, 0), .17, .42, 'ceramic_dark', segments=40, radius_top=.20, bev=.01)
    cylinder('Soil', (0, .40, 0), .185, .01, 'soil', segments=40)
    bm = bmesh.new()
    leaves = bmesh.new()
    for s in range(9):
        ang = rnd.uniform(0, math.tau)
        lean = rnd.uniform(.08, .32)
        height = rnd.uniform(.55, .95)
        base = Vector((rnd.uniform(-.04, .04), .40, rnd.uniform(-.04, .04)))
        dirv = Vector((math.cos(ang) * lean, 1, math.sin(ang) * lean)).normalized()
        pts = [base + dirv * height * t + Vector((0, -.05 * t * t, 0)) for t in (0, .5, 1)]
        tube_points = pts
        for a_, b_ in zip(tube_points, tube_points[1:]):
            ret = bmesh.ops.create_cone(bm, cap_ends=False, segments=6, radius1=.006, radius2=.005,
                                        depth=(b_ - a_).length)
            mid = (a_ + b_) / 2
            up = (b_ - a_).normalized()
            q = Vector((0, 0, 1)).rotation_difference(P(*up) - P(0, 0, 0))
            for v in ret['verts']:
                v.co = q @ v.co + P(*mid)
        # leaves along the upper stem
        for k in range(rnd.randint(3, 5)):
            t = .45 + .55 * k / 4
            at = base + dirv * height * t
            la = ang + rnd.uniform(-1.6, 1.6)
            L, Wd = rnd.uniform(.20, .30), rnd.uniform(.11, .16)
            out = Vector((math.cos(la), rnd.uniform(.1, .5), math.sin(la))).normalized()
            side = Vector((-math.sin(la), 0, math.cos(la)))
            rows = []
            for i in range(7):
                u = i / 6
                width = math.sin(math.pi * min(u * 1.1, 1)) * Wd / 2
                droop = Vector((0, -.12 * u * u, 0))
                c = at + out * L * u + droop
                cup = Vector((0, .015, 0))
                rows.append([leaves.verts.new(P(*(c - side * width + cup))), leaves.verts.new(P(*c)),
                             leaves.verts.new(P(*(c + side * width + cup)))])
            for r0, r1 in zip(rows, rows[1:]):
                leaves.faces.new([r0[0], r0[1], r1[1], r1[0]])
                leaves.faces.new([r0[1], r0[2], r1[2], r1[1]])
    st = new_object('Plant stems', bm)
    assign(st, 'stem')
    for p in st.data.polygons:
        p.use_smooth = True
    box_uv(st, .3)
    lf = new_object('Plant leaves', leaves)
    assign(lf, 'leaf')
    sol = lf.modifiers.new('sol', 'SOLIDIFY')
    sol.thickness = .002
    for p in lf.data.polygons:
        p.use_smooth = True
    apply_mods(lf)
    box_uv(lf, .3)
    export('plant')


def grass_planter(length=2.4):
    """Concrete planter 0.40 wide x 0.55 high with ornamental grass."""
    import random
    rnd = random.Random(9)
    cube('Planter', (0, .275, 0), (.42, .55, length), 'concrete', bev=.012, tile=2.)
    cube('Planter soil', (0, .535, 0), (.36, .02, length - .06), 'soil')
    bm = bmesh.new()
    for c in range(int(length / .16)):
        cx, cz = rnd.uniform(-.1, .1), -length / 2 + .1 + c * .16 + rnd.uniform(-.03, .03)
        for b in range(14):
            ang = rnd.uniform(0, math.tau)
            lean = rnd.uniform(.15, .55)
            hgt = rnd.uniform(.35, .65)
            root = Vector((cx + rnd.uniform(-.03, .03), .54, cz + rnd.uniform(-.03, .03)))
            d = Vector((math.cos(ang) * lean, 1, math.sin(ang) * lean))
            side = Vector((-math.sin(ang), 0, math.cos(ang))) * .006
            prev = None
            for i in range(5):
                u = i / 4
                p = root + Vector((d.x * hgt * u * u, hgt * u - .05 * u * u * lean, d.z * hgt * u * u))
                w = (1 - u) + .1
                pair = (bm.verts.new(P(*(p - side * w))), bm.verts.new(P(*(p + side * w))))
                if prev:
                    bm.faces.new([prev[0], prev[1], pair[1], pair[0]])
                prev = pair
    g = new_object('Grass', bm)
    assign(g, 'grass')
    sol = g.modifiers.new('sol', 'SOLIDIFY')
    sol.thickness = .0015
    for p in g.data.polygons:
        p.use_smooth = True
    apply_mods(g)
    box_uv(g, .3)
    export('grass-planter')


# ---------------------------------------------------------------- rooms
def lounge_room():
    """Room shell 7.0 x 3.1, screen wall at z=BACK (floor y=0, eye (0,1.45,0))."""
    W, H, back, front = ROOM_W, ROOM_H, BACK, 2.4
    D = front - back
    zc = (back + front) / 2
    cube('Floor', (0, -.05, zc), (W, .1, D), 'oak_floor', tile=2.4)
    cube('Ceiling', (0, H + .05, zc), (W, .1, D), 'ceiling', tile=2.)
    cube('Back wall', (0, H / 2, back - .06), (W, H, .12), 'plaster', tile=2.)
    cube('Front wall', (0, H / 2, front + .06), (W, H, .12), 'plaster', tile=2.)
    for s in (-1, 1):
        cube('Side wall', (s * (W / 2 + .06), H / 2, zc), (.12, H, D), 'plaster', tile=2.)
        cube('Skirting side', (s * (W / 2 - .008), .04, zc), (.016, .08, D), 'walnut', tile=1.2, grain=2)
    cube('Skirting back', (0, .04, back + .008), (W, .08, .016), 'walnut', tile=1.2)
    cube('Acoustic panel', (0, SCY, back + .02), (5.2, 2.85, .04), 'fabric', bev=.01, tile=.5)
    for s in (-1, 1):
        x0, x1 = 2.72, 3.46
        cube('Slat backing', (s * (x0 + x1) / 2, H / 2, back + .01), (x1 - x0, H, .02), 'fabric', tile=.5)
        n = int((x1 - x0) / .055)
        for i in range(n):
            x = s * (x0 + .02 + i * .055)
            cube('Slat', (x, H / 2, back + .04), (.028, H - .002, .04), 'walnut', bev=.003, tile=1.2, grain=1)
    cube('Cove soffit', (0, H - .12, back + .22), (W, .04, .44), 'ceiling', tile=2.)
    cube('Cove LED', (0, H - .095, back + .42), (W - .2, .01, .012), 'led_warm')
    for x in (-2.0, 2.0):
        for z in (-1.4, .6):
            cylinder('Downlight trim', (x, H - .002, z), .06, .006, 'chrome_dark', segments=32)
            cylinder('Downlight', (x, H - .004, z), .045, .004, 'downlight', segments=32)
    export('lounge-room')


def lounge_rug():
    bm = bmesh.new()
    for v in bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=.5)['verts']:
        pass
    ob = cube('Rug', (0, .005, 0), (2.4, .01, 1.7), 'rug', bev=.004, seg=2)
    # One rug texture across the top: planar UV 0..1
    me = ob.data
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            uv[li].uv = (co.x / 2.4 + .5, (-co.y) / 1.7 * -1 + .5)
    bm.free()
    export('area-rug')


def terrace():
    """Rooftop deck: 6.0 x 6.2, glass balustrade, string-light posts."""
    W, back, front = 5.6, -3.45, 2.6
    D = front - back
    zc = (back + front) / 2
    cube('Deck', (0, -.06, zc), (W + .4, .12, D + .4), 'teak_deck', tile=2.4)
    cube('Deck edge', (0, -.30, zc), (W + .5, .36, D + .5), 'concrete', tile=2.)
    # Glass balustrade on three sides: steel posts, glass, teak handrail.
    rail_h = 1.05

    def run(p0, p1):
        L = (Vector(p1) - Vector(p0)).length
        n = max(1, round(L / 1.2))
        horiz = abs(p1[0] - p0[0]) > abs(p1[2] - p0[2])
        for i in range(n + 1):
            t = i / n
            x, z = p0[0] + (p1[0] - p0[0]) * t, p0[2] + (p1[2] - p0[2]) * t
            cube('Rail post', (x, rail_h / 2, z), (.05, rail_h, .05), 'black_powder', bev=.004)
        for i in range(n):
            t0, t1 = i / n, (i + 1) / n
            x0, z0 = p0[0] + (p1[0] - p0[0]) * t0, p0[2] + (p1[2] - p0[2]) * t0
            x1, z1 = p0[0] + (p1[0] - p0[0]) * t1, p0[2] + (p1[2] - p0[2]) * t1
            cx, cz = (x0 + x1) / 2, (z0 + z1) / 2
            size = (abs(x1 - x0) - .06, rail_h - .16, .012) if horiz else (.012, rail_h - .16, abs(z1 - z0) - .06)
            cube('Glass', (cx, .06 + size[1] / 2, cz), size, 'glass')
        size = (abs(p1[0] - p0[0]) + .08, .05, .09) if horiz else (.09, .05, abs(p1[2] - p0[2]) + .08)
        cube('Handrail', ((p0[0] + p1[0]) / 2, rail_h + .025, (p0[2] + p1[2]) / 2), size, 'ash', bev=.012,
             tile=1., grain=0 if horiz else 2)

    run((-W / 2, 0, front - .2), (-W / 2, 0, back))
    run((W / 2, 0, front - .2), (W / 2, 0, back))
    run((-W / 2, 0, back), (W / 2, 0, back))
    export('terrace')


def skyline_ring():
    """Distant city band (r=70 m) and sky dome (r=160 m), both unlit."""
    R = 70.
    bm = bmesh.new()
    seg = 96
    lo, hi = -14., 10.
    ring = []
    for i in range(seg + 1):
        a = math.tau * i / seg
        x, z = R * math.sin(a), -R * math.cos(a)
        ring.append((bm.verts.new(P(x, lo, z)), bm.verts.new(P(x, hi, z))))
    for (a0, a1), (b0, b1) in zip(ring, ring[1:]):
        bm.faces.new([a0, b0, b1, a1])
    ob = new_object('Skyline', bm)
    assign(ob, 'skyline')
    me = ob.data
    me.uv_layers.new(name='UVMap')
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        for li in poly.loop_indices:
            vi = me.loops[li].vertex_index
            co = me.vertices[vi].co
            a = math.atan2(co.x, co.y) / math.tau + .5
            # wrap-safe: repeat skyline 3x around
            uv[li].uv = (vi // 2 / seg * 3, (co.z - lo) / (hi - lo))
    bm3 = bmesh.new()
    bmesh.ops.create_circle(bm3, cap_ends=True, segments=96, radius=R + 2)
    for v in bm3.verts:
        v.co = P(v.co.x, lo + .5, v.co.y)
    ground = new_object('City floor', bm3)
    assign(ground, 'city_floor')
    ground.data.flip_normals() if ground.data.polygons[0].normal.z < 0 else None
    box_uv(ground, 10.)
    bm2 = bmesh.new()
    bmesh.ops.create_uvsphere(bm2, u_segments=64, v_segments=32, radius=160.)
    sky = new_object('Sky dome', bm2)
    assign(sky, 'sky')
    me = sky.data
    me.uv_layers.new(name='UVMap')
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        cx = sum(me.vertices[me.loops[li].vertex_index].co.x for li in poly.loop_indices)
        cy = sum(me.vertices[me.loops[li].vertex_index].co.y for li in poly.loop_indices)
        seam = math.atan2(cx, cy) / math.tau + .5
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            u = math.atan2(co.x, co.y) / math.tau + .5
            if abs(u - seam) > .5:
                u += 1 if seam > u else -1
            v = .5 - math.asin(max(-1, min(1, co.z / 160.))) / math.pi
            uv[li].uv = (u, 1 - v)
    for p in sky.data.polygons:
        p.use_smooth = True
    sky.data.flip_normals()
    export('dusk-sky-dome')


def sky_dome(name, material, radius):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=64, v_segments=32, radius=radius)
    sky = new_object(name, bm)
    assign(sky, material)
    me = sky.data
    me.uv_layers.new(name='UVMap')
    uv = me.uv_layers.active.data
    for poly in me.polygons:
        cx = sum(me.vertices[me.loops[li].vertex_index].co.x for li in poly.loop_indices)
        cy = sum(me.vertices[me.loops[li].vertex_index].co.y for li in poly.loop_indices)
        seam = math.atan2(cx, cy) / math.tau + .5
        for li in poly.loop_indices:
            co = me.vertices[me.loops[li].vertex_index].co
            u = math.atan2(co.x, co.y) / math.tau + .5
            if abs(u - seam) > .5:
                u += 1 if seam > u else -1
            v = .5 - math.asin(max(-1, min(1, co.z / radius))) / math.pi
            uv[li].uv = (u, 1 - v)
    for p in me.polygons:
        p.use_smooth = True
    me.flip_normals()
    return sky


def dark_room():
    """Plain dark room: charcoal plaster, smoked oak, faint cove; lit mostly by the screen."""
    W, H, back, front = ROOM_W, ROOM_H, BACK, 2.4
    D = front - back
    zc = (back + front) / 2
    cube('Floor', (0, -.05, zc), (W, .1, D), 'oak_dark', tile=2.4)
    cube('Ceiling', (0, H + .05, zc), (W, .1, D), 'plaster_dark', tile=2.)
    cube('Back wall', (0, H / 2, back - .06), (W, H, .12), 'plaster_dark', tile=2.)
    cube('Front wall', (0, H / 2, front + .06), (W, H, .12), 'plaster_dark', tile=2.)
    for s_ in (-1, 1):
        cube('Side wall', (s_ * (W / 2 + .06), H / 2, zc), (.12, H, D), 'plaster_dark', tile=2.)
        cube('Skirting side', (s_ * (W / 2 - .008), .04, zc), (.016, .08, D), 'black_powder', tile=1.)
    cube('Skirting back', (0, .04, back + .008), (W, .08, .016), 'black_powder')
    cube('Cove soffit', (0, H - .12, back + .22), (W, .04, .44), 'plaster_dark', tile=2.)
    cube('Cove LED', (0, H - .095, back + .42), (W - .2, .01, .012), 'led_warm')
    export('dark-room')


def seaside():
    """Teak platform on a beach; the sea runs to a sunset horizon behind the screen."""
    cube('Deck', (0, -.06, -.55), (5.4, .12, 5.6), 'teak_deck', tile=2.4)
    for x in (-2.6, 2.6):
        for z in (-3.25, 2.15):
            cube('Deck post', (x, -.2, z), (.12, .3, .12), 'teak', bev=.006)
    # Sand: flat around the deck, shelving into the water past z=-9.
    bm = bmesh.new()
    nx, nz = 48, 48
    xs = [-70 + 140 * i / nx for i in range(nx + 1)]
    zs = [40 - 60 * j / nz for j in range(nz + 1)]
    grid = []
    for z in zs:
        row = []
        for x in xs:
            shore = -10 + 1.6 * math.sin(x / 6.5) + .8 * math.sin(x / 2.3 + 1)
            y = -.14 - max(0., (shore + 3 - z)) * .07 + .03 * math.sin(x * .7) * math.sin(z * .5)
            row.append(bm.verts.new(P(x, y, z)))
        grid.append(row)
    for j in range(nz):
        for i in range(nx):
            bm.faces.new([grid[j][i], grid[j][i + 1], grid[j + 1][i + 1], grid[j + 1][i]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    sand = new_object('Sand', bm)
    if sand.data.polygons[0].normal.z < 0:
        sand.data.flip_normals()
    assign(sand, 'sand')
    for p in sand.data.polygons:
        p.use_smooth = True
    box_uv(sand, 3.)
    bm = bmesh.new()
    bmesh.ops.create_circle(bm, cap_ends=True, segments=96, radius=330.)
    for v in bm.verts:
        v.co = P(v.co.x, -.42, v.co.y)
    sea = new_object('Sea', bm)
    if sea.data.polygons[0].normal.z < 0:
        sea.data.flip_normals()
    assign(sea, 'water')
    box_uv(sea, 7.)
    # Two low deck lanterns.
    for x in (-2.45, 2.45):
        cube('Lantern post', (x, .3, 1.95), (.06, .6, .06), 'black_powder', bev=.004)
        sphere('Lantern bulb', (x, .64, 1.95), .045, 'bulb')
    sky_dome('Sky', 'sea_sky', 350.)
    export('seaside')


def void_space():
    """Near-black void: a glossy disc floor with a faint rim; the screen floats."""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=96, radius1=3.4, radius2=3.4, depth=.06)
    bmesh.ops.translate(bm, vec=P(0, -.03, -.9), verts=bm.verts)
    ob = new_object('Void floor', bm)
    assign(ob, 'void_floor')
    bevel(ob, .01, 2, 30)
    box_uv(ob, 2.)
    bm = bmesh.new()
    ret = bmesh.ops.create_circle(bm, cap_ends=False, segments=128, radius=3.405)
    for v in bm.verts:
        v.co = P(v.co.x, -.012, v.co.y - .9)
    ring = tube('Void rim', [tuple(Vector((3.405 * math.cos(a), -.012, -.9 + 3.405 * math.sin(a))))
                             for a in [math.tau * i / 128 for i in range(129)]], .008, 'rim_glow')
    bm.free()
    sky_dome('Sky', 'void_sky', 150.)
    export('void-space')
    cube('Plinth', (0, .21, 0), (.30, .42, .30), 'black_powder', bev=.006)
    export('plinth')


def main():
    """`-- --only dualshock4 tv` rebuilds just those builders."""
    argv = sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []
    if argv[:1] == ['--only']:
        clear()
        materials()
        for name in argv[1:]:
            globals()[name]()
        print('exported', EXPORTED)
        return
    clear()
    materials()
    coffee_table()
    ps4_pro()
    dualshock4()
    for t in 'abcd':
        disc_case(t)
    (OUT / 'local').mkdir(exist_ok=True)
    for cover in sorted(TEX.glob('cover_CUSA*.png')):
        disc_case(cover.stem[len('cover_'):])
    soundbar()
    dock = psv_dock()
    tv()
    media_console()
    side_cabinet()
    plant()
    grass_planter()
    lounge_room()
    lounge_rug()
    terrace()
    outdoor_screen_frame()
    skyline_ring()
    dark_room()
    seaside()
    void_space()
    print('exported', EXPORTED, 'psv bottom', tuple(dock[0]), tuple(dock[1]))


if __name__ == '__main__':
    try:
        main()
    except Exception:
        import traceback
        traceback.print_exc()
        sys.exit(1)
