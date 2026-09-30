# SPDX-License-Identifier: GPL-2.0-or-later
"""Rebuild our original Vita-shaped status console with Blender (no downloaded mesh).
blender --background --python tools/xr/build_psv_model.py -- ROOT
Metres; +X right, +Y up, +Z front. Named material and screen contracts are tested.
PCH-1000 envelope: Sony's published 182 x 83.5 x 18.6 mm, excluding controls.
"""
import bpy, math, pathlib, sys, json, struct, re, xml.etree.ElementTree as ET
from mathutils import Vector, Matrix
root = pathlib.Path(sys.argv[sys.argv.index('--') + 1])
asset = root / 'android/shadps4-app/app/src/main/assets/xr/psv_status.glb'
out = root / 'build/validation/xr-psv-status-20260930'
asset.parent.mkdir(parents=True,exist_ok=True); out.mkdir(parents=True,exist_ok=True)
bpy.ops.object.select_all(action='SELECT'); bpy.ops.object.delete(use_global=False)

def mat(name, color, rough=.4, metal=0, unlit=False):
    m=bpy.data.materials.new(name); m.diffuse_color=(*color,1); m.use_nodes=True
    nt=m.node_tree; p=nt.nodes.get('Principled BSDF')
    p.inputs['Base Color'].default_value=(*color,1)
    p.inputs['Roughness'].default_value=rough; p.inputs['Metallic'].default_value=metal
    if unlit:
        nt.nodes.remove(p); e=nt.nodes.new('ShaderNodeEmission'); e.inputs['Color'].default_value=(*color,1)
        nt.links.new(e.outputs[0],nt.nodes.get('Material Output').inputs['Surface'])
    return m
shell=mat('shell',(.013,.019,.028),.20)
back=mat('rear_shell',(.022,.027,.035),.55)
rim=mat('metal_trim',(.50,.54,.59),.22,.78)
glass=mat('bezel',(.009,.014,.022),.17)
rubber=mat('rubber',(.012,.014,.019),.8)
button=mat('buttons',(.021,.028,.040),.3)
ink=mat('legends',(.6,.65,.73),.5)
indicator=mat('ps_indicator',(.6,.65,.73),.35)
indicator.node_tree.nodes.get('Principled BSDF').inputs['Emission Color'].default_value=(.005,.18,1,1)
indicator.node_tree.nodes.get('Principled BSDF').inputs['Emission Strength'].default_value=2
screen=mat('status_screen',(1,1,1),1,0,True)
clear=mat('clear_shoulders',(.72,.77,.82),.13)
clear.node_tree.nodes.get('Principled BSDF').inputs['Transmission Weight'].default_value=.78
clear.node_tree.nodes.get('Principled BSDF').inputs['IOR'].default_value=1.49

def finish(o,name,m):
    o.name=name; o.data.materials.append(m)
    return o

def round_body(name,w,h,r,profiles,m):
    verts=[]; faces=[]; N=64
    for z, inset in profiles:
        for c in range(4):
            a0=c*math.pi/2
            cx=(w/2-r)*(1 if c in (0,3) else -1)
            cy=(h/2-r)*(1 if c in (0,1) else -1)
            for j in range(16):
                a=a0+j*math.pi/32
                verts.append((cx+(r-inset)*math.cos(a),cy+(r-inset)*math.sin(a),z))
    faces.append(tuple(reversed(range(N))))
    for k in range(len(profiles)-1):
        for j in range(N):
            a=k*N+j; b=k*N+(j+1)%N; faces.append((a,b,b+N,a+N))
    faces.append(tuple(range((len(profiles)-1)*N,len(profiles)*N)))
    mesh=bpy.data.meshes.new(name); mesh.from_pydata(verts,[],faces); mesh.update()
    o=bpy.data.objects.new(name,mesh); bpy.context.collection.objects.link(o); finish(o,name,m)
    for p in mesh.polygons: p.use_smooth=len(p.vertices)==4
    return o

# Continuous rounded shell, not a rectangular rail behind the front face.
round_body('rear_case',.182,.0835,.036,[(-.0093,.0048),(-.009,.0035),(-.008,.002),(-.0065,.0008),(-.0045,.0002),(-.002,0)],back)
round_body('silver_edge',.182,.0835,.036,[(-.002,.0002),(-.0017,0),(.003,0),(.0034,.0002)],rim)
round_body('front_shell',.182,.0835,.036,[(.0033,.0002),(.005,.0003),(.007,.0007),(.0087,.0013),(.0093,.002)],shell)

def box(name,loc,size,m,bevel=.001):
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.mesh.primitive_cube_add(size=1,location=loc); o=bpy.context.object; o.dimensions=size
    bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    if bevel:
        mod=o.modifiers.new('moulded_radius','BEVEL'); mod.width=bevel; mod.segments=3
        bpy.context.view_layer.objects.active=o; bpy.ops.object.modifier_apply(modifier=mod.name)
        mod=o.modifiers.new('weighted_normals','WEIGHTED_NORMAL'); bpy.ops.object.modifier_apply(modifier=mod.name)
    return finish(o,name,m)

def cylinder(name,x,y,z,r,d,m):
    bpy.ops.object.select_all(action='DESELECT')
    bpy.ops.mesh.primitive_cylinder_add(vertices=32,radius=r,depth=d,location=(x,y,z))
    o=bpy.context.object; mod=o.modifiers.new('edge','BEVEL'); mod.width=min(d/4,.0006); mod.segments=2
    bpy.ops.object.modifier_apply(modifier=mod.name)
    for p in o.data.polygons: p.use_smooth=abs(p.normal.z)<.7
    return finish(o,name,m)

def line(name,coords,m,r=.00024):
    c=bpy.data.curves.new(name,'CURVE'); c.dimensions='3D'; c.bevel_depth=r; c.bevel_resolution=1
    sp=c.splines.new('POLY'); sp.points.add(len(coords)-1)
    for p,v in zip(sp.points,coords): p.co=(*v,1)
    o=bpy.data.objects.new(name,c); bpy.context.collection.objects.link(o); o.data.materials.append(m)
    bpy.ops.object.select_all(action='DESELECT'); o.select_set(True); bpy.context.view_layer.objects.active=o
    bpy.ops.object.convert(target='MESH'); return bpy.context.object

def label(name,text,x,y,z,size,m):
    c=bpy.data.curves.new(name,'FONT'); c.body=text; c.align_x='CENTER'; c.align_y='CENTER'; c.size=size; c.extrude=.00003
    o=bpy.data.objects.new(name,c); bpy.context.collection.objects.link(o); o.location=(x,y,z); o.data.materials.append(m)
    bpy.ops.object.select_all(action='DESELECT'); o.select_set(True); bpy.context.view_layer.objects.active=o; bpy.ops.object.convert(target='MESH')

def oval(name,loc,width,height,depth,m):
    # True elliptical outline, including the low-profile oval Home/Select/Start keys.
    o=cylinder(name,*loc,width/2,depth,m)
    o.scale.y=height/width
    bpy.ops.object.transform_apply(location=False,rotation=False,scale=True)
    return o

def prism(name,outline,z0,z1,m,bevel=0):
    n=len(outline); vs=[(x,y,z) for z in (z0,z1) for x,y in outline]
    fs=[tuple(reversed(range(n))),tuple(range(n,2*n))]
    fs += [(i,(i+1)%n,(i+1)%n+n,i+n) for i in range(n)]
    me=bpy.data.meshes.new(name); me.from_pydata(vs,[],fs); me.update()
    o=bpy.data.objects.new(name,me); bpy.context.collection.objects.link(o); finish(o,name,m)
    bpy.ops.object.select_all(action='DESELECT'); o.select_set(True); bpy.context.view_layer.objects.active=o
    if bevel:
        mod=o.modifiers.new('rounded_edges','BEVEL'); mod.width=bevel; mod.segments=3
        bpy.ops.object.modifier_apply(modifier=mod.name)
        mod=o.modifiers.new('weighted_normals','WEIGHTED_NORMAL'); bpy.ops.object.modifier_apply(modifier=mod.name)
    return o

def ps_logo(name,cx,cy,z,width):
    # Font Awesome's PlayStation brand silhouette, CC BY 4.0; attribution next to SVG.
    path=ET.parse(root/'tools/xr/playstation.svg').find('{http://www.w3.org/2000/svg}path').attrib['d']
    ts=re.findall(r'[A-Za-z]|[-+]?(?:\d*\.\d+|\d+)',path); i=0; cur=(0,0); start=(0,0); contours=[]; pts=[]; cmd=None
    while i<len(ts):
        if ts[i].isalpha(): cmd=ts[i]; i+=1
        rel=cmd.islower(); op=cmd.upper()
        if op=='Z':
            contours.append(pts); pts=[]; cur=start; cmd=None; continue
        count={'M':2,'L':2,'H':1,'V':1,'C':6}[op]
        vals=list(map(float,ts[i:i+count])); i+=count; px,py=cur
        if op in ('M','L'):
            cur=(vals[0]+(px if rel else 0),vals[1]+(py if rel else 0)); pts.append(cur)
            if op=='M': start=cur; cmd='l' if rel else 'L'
        elif op=='H': cur=(vals[0]+(px if rel else 0),py); pts.append(cur)
        elif op=='V': cur=(px,vals[0]+(py if rel else 0)); pts.append(cur)
        else:
            controls=[cur]+[(vals[j]+(px if rel else 0),vals[j+1]+(py if rel else 0)) for j in (0,2,4)]
            for j in range(1,9):
                t=j/8; pts.append(tuple(sum(controls[k][a]*math.comb(3,k)*t**k*(1-t)**(3-k) for k in range(4)) for a in (0,1)))
            cur=controls[-1]
    for index,contour in enumerate(contours):
        coords=[(cx+(x-288)*width/576,cy+(256-y)*width/576) for x,y in contour]
        if sum(coords[k][0]*coords[(k+1)%len(coords)][1]-coords[(k+1)%len(coords)][0]*coords[k][1] for k in range(len(coords)))<0: coords.reverse()
        prism(name+str(index),coords,z,z+.000035,indicator)

# Flush laminated front face; the active OLED is inset inside the glass, not a raised tablet.
box('screen_bezel',(0,.0025,.0094),(.122,.074,.0002),glass,.0006)
# Exact quad, so no curved UVs, clipping or glancing-angle protrusion. UV v=0 is top.
w,h=.1107,.06273; y=.0025; z=.00965
mesh=bpy.data.meshes.new('display_quad'); mesh.from_pydata([(-w/2,y-h/2,z),(w/2,y-h/2,z),(w/2,y+h/2,z),(-w/2,y+h/2,z)],[],[(0,1,2,3)])
mesh.uv_layers.new(name='UVMap')
for li,uv in zip(mesh.polygons[0].loop_indices,[(0,1),(1,1),(1,0),(0,0)]): mesh.uv_layers[0].data[li].uv=uv
# Blender GLB export flips UV V: set input accordingly for the top-left raster.
for uv in mesh.uv_layers[0].data: uv.uv.y=1-uv.uv.y
ob=bpy.data.objects.new('status_display',mesh); bpy.context.collection.objects.link(ob); finish(ob,'status_display',screen)
for sign in [-1,1]:
    x=sign*.069
    cylinder('stick_socket_'+str(sign),x,-.009,.0095,.0065,.0014,glass)
    cylinder('stick_stem_'+str(sign),x,-.009,.0115,.0025,.004,rubber)
    cylinder('stick_cap_'+str(sign),x,-.009,.0142,.0053,.0024,rubber)
    bpy.ops.mesh.primitive_torus_add(major_radius=.0045,minor_radius=.00035,major_segments=32,minor_segments=6,location=(x,-.009,.0155)); finish(bpy.context.object,'stick_grip_'+str(sign),button)
    # Two transparent shoulder caps follow the curved upper corners. The old
    # axis-aligned 27 x 8 mm boxes overhung the rounded shell like a long pen.
    outline=[]
    for radius,angles in [(.0366,range(44,89,2)),(.0315,range(88,43,-2))]:
        outline += [(sign*(.055+radius*math.cos(math.radians(a))),.00575+radius*math.sin(math.radians(a))) for a in angles]
    if sign<0: outline.reverse()
    prism('shoulder_'+str(sign),outline,-.0065,.0035,clear,.00065)
    for row in range(2):
        for col in range(3):
            cylinder(f'speaker_{sign}_{row}_{col}',sign*(.082+col*.0023),-.0085-row*.0025,.00945,.00065,.0004,rubber)
    # Exposed crescent-shaped metal corners of the perimeter frame.
    for top in [-1,1]:
        def bezier(points,t):
            return tuple(sum(points[k][a] * (math.comb(3,k)*t**k*(1-t)**(3-k)) for k in range(4)) for a in range(2))
        outer=[(.057,.0403),(.067,.041),(.076,.035),(.081,.0278)]
        inner=[(.057,.0403),(.067,.0342),(.076,.032),(.081,.0278)]
        verts=[]
        for points in (outer,inner):
            for j in range(17):
                x,y=bezier(points,j/16); verts.append((sign*x,top*y,.00985))
        faces=[(j,j+1,j+18,j+17) for j in range(16)]
        me=bpy.data.meshes.new('corner_trim'); me.from_pydata(verts,[],faces); me.update()
        ob=bpy.data.objects.new(f'corner_trim_{sign}_{top}',me); bpy.context.collection.objects.link(ob); finish(ob,ob.name,rim)
        # Both orientations of a mirrored cap must face out.
        for poly in me.polygons:
            if poly.normal.z<0: poly.flip()

    oval('rear_grip_'+str(sign),(sign*.070,-.006,-.0084),.016,.038,.003,rubber)
# Direction cross, with recessed centre and separated arm seams.
cylinder('dpad_seat',-.074,.014,.0095,.010,.001,glass)
for i,(dx,dy) in enumerate([(0,1),(1,0),(0,-1),(-1,0)]):
    # Four tapered arms meet around the small diamond-shaped central recess.
    arm=[(-.0028,.008),(-.0028,.003), (0,.0007),(.0028,.003),(.0028,.008)]
    points=[(-.074+dy*u+dx*v,.014-dx*u+dy*v) for u,v in arm]
    prism('dpad_'+str(i),points,.0098,.0126,button,.00045)
    cx=-.074+dx*.006; cy=.014+dy*.006
    line('direction_'+str(i),[(cx-dy*.0008,cy+dx*.0008,.01265),(cx+dx*.001,cy+dy*.001,.01265),(cx+dy*.0008,cy-dx*.0008,.01265),(cx-dy*.0008,cy+dx*.0008,.01265)],ink,.00010)
for i,(dx,dy) in enumerate([(0,1),(1,0),(0,-1),(-1,0)]):
    x=.074+dx*.0077; y=.014+dy*.0077; z=.0129
    cylinder('face_button_'+str(i),x,y,.0112,.00365,.0026,button)
    s=.0016
    if i==0: pts=[(x,y+s,z),(x-s,y-s,z),(x+s,y-s,z),(x,y+s,z)]
    elif i==1: pts=[(x+s*math.cos(a*math.pi/12),y+s*math.sin(a*math.pi/12),z) for a in range(25)]
    elif i==2:
        line('cross_a',[(x-s,y-s,z),(x+s,y+s,z)],ink,.00018); pts=[(x-s,y+s,z),(x+s,y-s,z)]
    else: pts=[(x-s,y-s,z),(x+s,y-s,z),(x+s,y+s,z),(x-s,y+s,z),(x-s,y-s,z)]
    line('button_legend_'+str(i),pts,ink,.00018)
cylinder('front_camera',.065,.026,.0092,.0017,.001,glass)
cylinder('camera_lens',.065,.026,.0096,.00085,.0004,rim)
oval('home_rim',(-.071,-.026,.00955),.010,.0062,.0006,glass)
oval('home',(-.071,-.026,.0099),.0089,.0052,.0006,button)
ps_logo('home_mark',-.071,-.026,.01025,.0037)
for x,t in [(.066,'SELECT'),(.076,'START')]:
    oval(t+'_rim',(x,-.026,.00955),.0078,.0042,.0005,glass)
    oval(t,(x,-.026,.00985),.007,.0035,.0005,button)
    label(t+'_ink',t,x,-.026,.0102,.00135,ink)
label('model_name','PS VITA',0,-.0355,.0096,.004,ink)
label('brand','SONY',-.067,.029,.0094,.0032,ink)
round_body('rear_touchpad',.160,.061,.025,[(-.0095,.0005),(-.0093,0)],glass)
# Back camera and the tactile panel's fine geometric pattern.
cylinder('rear_camera',0,.036,-.0098,.0037,.0012,button)
cylinder('rear_lens',0,.036,-.0106,.0016,.0004,glass)
for row in range(14):
    for col in range(30):
        x=-.050+col*.00345; y=-.022+row*.0034; r=.00055
        z=-.0096; kind=(row+col)%4
        if kind==0: points=[(x+r*math.cos(a*math.pi/6),y+r*math.sin(a*math.pi/6),z) for a in range(13)]
        elif kind==1: points=[(x-r,y-r,z),(x-r,y+r,z),(x+r,y+r,z),(x+r,y-r,z),(x-r,y-r,z)]
        elif kind==2: points=[(x-r,y-r,z),(x,y+r,z),(x+r,y-r,z),(x-r,y-r,z)]
        else:
            line(f'rear_cross_{row}_{col}',[(x-r,y-r,z),(x+r,y+r,z)],back,.00007)
            points=[(x-r,y+r,z),(x+r,y-r,z)]
        line(f'rear_pattern_{row}_{col}',points,back,.00007)

for x in [-.078,.078]:
    for y in [-.025,.025]:
        cylinder('rear_screw',x,y,-.0088,.001,.0004,rubber)
        line('screw_slot',[(x-.0005,y,-.00905),(x+.0005,y,-.00905)],back,.00012)
box('multi_port',(0,-.0413,-.002),(.017,.002,.005),rubber,.0008)
# A single body mesh with named material slots avoids one draw per speaker hole
# or rear-panel mark. Keep the screen independent and preserve control anchors.
bpy.ops.object.select_all(action='DESELECT')
for ob in bpy.context.scene.objects:
    if ob.type=='MESH' and ob.name!='status_display' and not ob.name.startswith('shoulder_'): ob.select_set(True)
bpy.context.view_layer.objects.active=bpy.data.objects['front_shell']
bpy.ops.object.join(); bpy.context.object.name='psv_body'
for name,pos in [('left_stick_anchor',(-.069,-.009,.0155)),('right_stick_anchor',(.069,-.009,.0155)),('screen_anchor',(0,.0025,.00965))]:
    anchor=bpy.data.objects.new(name,None); bpy.context.collection.objects.link(anchor); anchor.location=pos
# Keep contract node names / material slots, apply triangulation at export.
bpy.ops.object.select_all(action='SELECT')
bpy.ops.export_scene.gltf(filepath=str(asset),export_format='GLB',export_yup=False,export_apply=True,export_cameras=False,export_lights=False,export_animations=False)
# Export the display/LED as explicit unlit glTF materials. Blender's emission
# nodes are otherwise exported as PBR emission, which darkens a live screen.
data=asset.read_bytes(); length=struct.unpack_from('<I',data,12)[0]; doc=json.loads(data[20:20+length])
for m in doc['materials']:
    if m['name'] in ('status_screen','power_led'):
        m.setdefault('extensions',{})['KHR_materials_unlit']={}
        m['pbrMetallicRoughness']={'baseColorFactor':[1,1,1,1] if m['name']=='status_screen' else [.025,.45,1,1],'metallicFactor':0,'roughnessFactor':1}
        m.pop('emissiveFactor',None)
doc.setdefault('extensionsUsed',[]).append('KHR_materials_unlit')
chunk=json.dumps(doc,separators=(',',':')).encode(); chunk+=b' '*((-len(chunk))%4)
tail=data[20+length:]; asset.write_bytes(struct.pack('<III',0x46546c67,2,20+len(chunk)+len(tail))+struct.pack('<II',len(chunk),0x4e4f534a)+chunk+tail)
bpy.ops.wm.save_as_mainfile(filepath=str(out/'psv-status.blend'))
# Preview is Blender lighting only; the headset uses Lite Engine's LTC rect + voxel GI.
screen.node_tree.nodes.get('Emission').inputs['Color'].default_value=(.008,.018,.033,1)
scene=bpy.context.scene; scene.render.engine='CYCLES'; scene.cycles.samples=32
scene.world.color=(.035,.035,.035)
for name,loc,energy,size in [('key',(-.1,.16,.22),1.0,.20),('fill',(.13,-.02,.10),.35,.15)]:
    ld=bpy.data.lights.new(name,'AREA'); ld.energy=energy; ld.shape='DISK'; ld.size=size
    o=bpy.data.objects.new(name,ld); scene.collection.objects.link(o); o.location=loc; o.rotation_euler=(-o.location).to_track_quat('-Z','Y').to_euler()
cam=bpy.data.cameras.new('preview'); co=bpy.data.objects.new('preview',cam); scene.collection.objects.link(co); co.location=(.028,.06,.30); z=co.location.normalized(); x=Vector((0,1,0)).cross(z).normalized(); y=z.cross(x); co.rotation_euler=Matrix((x,y,z)).transposed().to_euler(); cam.type='ORTHO'; cam.ortho_scale=.218; scene.camera=co
scene.render.resolution_x=1400; scene.render.resolution_y=850; scene.render.resolution_percentage=100
scene.render.film_transparent=False
for filename,position in [('psv-model-preview',(.028,.06,.30)),('psv-front',(0,0,.30)),('psv-back',(0,0,-.30)),('psv-top',(0,.30,.04))]:
    co.location=position; z=co.location.normalized(); x=Vector((0,1,0)).cross(z).normalized(); y=z.cross(x)
    co.rotation_euler=Matrix((x,y,z)).transposed().to_euler()
    # Move the same neutral softbox behind for the back inspection.
    if filename=='psv-back':
        for ob in [bpy.data.objects['key'],bpy.data.objects['fill']]:
            ob.location.z=-abs(ob.location.z); ob.rotation_euler=(-ob.location).to_track_quat('-Z','Y').to_euler()
    else:
        for ob in [bpy.data.objects['key'],bpy.data.objects['fill']]:
            ob.location.z=abs(ob.location.z); ob.rotation_euler=(-ob.location).to_track_quat('-Z','Y').to_euler()
    scene.render.filepath=str(out/(filename+'.png')); bpy.ops.render.render(write_still=True)
print('PSV_ASSET',asset,'triangles',sum(len(o.data.polygons) for o in bpy.data.objects if o.type=='MESH'))
