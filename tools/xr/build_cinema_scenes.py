#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Original cinema layouts for Lite Editor / Lite Engine. Metres, Y up, -Z forward.
The preview screen and PSV status texture are placeholders, never sampled telemetry.
Run from any directory; generated World documents remain editable in Lite Editor.
"""
from pathlib import Path
import json, math, struct, uuid, io
from PIL import Image, ImageDraw, ImageFont
ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'assets/xr/cinema'
MODELS=OUT/'models'
MODELS.mkdir(parents=True,exist_ok=True)

def guid(name): return str(uuid.uuid5(uuid.NAMESPACE_URL,'shadps4/cinema/'+name))
def quat_x(a): return [math.sin(a/2),0,0,math.cos(a/2)]
def qmul(a,b):
    ax,ay,az,aw=a;bx,by,bz,bw=b
    return [aw*bx+ax*bw+ay*bz-az*by,aw*by-ax*bz+ay*bw+az*bx,aw*bz+ax*by-ay*bx+az*bw,aw*bw-ax*bx-ay*by-az*bz]
def quat_y(a): return [0,math.sin(a/2),0,math.cos(a/2)]

def write_glb(path,doc,data):
    data=bytes(data)+b'\0'*((-len(data))%4)
    doc['buffers']=[{'byteLength':len(data)}]
    j=json.dumps(doc,separators=(',',':')).encode(); j+=b' '*((-len(j))%4)
    path.write_bytes(struct.pack('<III',0x46546c67,2,28+len(j)+len(data))+struct.pack('<I4s',len(j),b'JSON')+j+struct.pack('<I4s',len(data),b'BIN\0')+data)

class Mesh:
    def __init__(self): self.pos=[]; self.norm=[]; self.col=[]; self.uv=[]; self.idx=[]
    def vertex(self,p,n,c,uv=(0,0)):
        self.pos.extend(p); self.norm.extend(n); self.col.extend([*c,1]); self.uv.extend(uv)
    def box(self,c):
        for axis in range(3):
            for sign in [-1,1]:
                start=len(self.pos)//3
                shade=(.9 if sign>0 else .52) if axis==1 else (1 if axis==2 and sign>0 else .65)
                for u,v in [(-.5,-.5),(.5,-.5),(.5,.5),(-.5,.5)]:
                    p=[0,0,0]; n=[0,0,0]; p[axis]=sign*.5; n[axis]=sign
                    p[(axis+1)%3]=u;p[(axis+2)%3]=v
                    self.vertex(p,n,[k*shade for k in c],(u+.5,.5-v))
                faces=[0,1,2,0,2,3] if sign>0 else [0,2,1,0,3,2]
                self.idx.extend(start+i for i in faces)
    def sphere(self,c,sky=False):
        rings,segments=(32,64) if sky else (12,24)
        for r in range(rings+1):
            lat=-math.pi/2+math.pi*r/rings;y=math.sin(lat); radius=math.cos(lat)
            for s in range(segments+1):
                a=math.tau*s/segments;n=[radius*math.cos(a),y,radius*math.sin(a)]
                shade=.65+.22*y+.13*n[2]
                glow=math.exp(-abs(y)*5)
                colour=[.013+.035*glow,.022+.044*glow,.046+.055*glow] if sky else [v*shade for v in c]
                self.vertex([v*.5 for v in n],n,colour)
        for r in range(rings):
            for s in range(segments):
                a=r*(segments+1)+s;b=a+segments+1
                self.idx.extend([a,a+1,b,a+1,b+1,b])
    def save(self,name,texture=None,blend=False):
        doc={'asset':{'version':'2.0','generator':'shadPS4 cinema authoring'},'scene':0,'scenes':[{'nodes':[0]}], 'nodes':[{'name':name,'mesh':0}], 'bufferViews':[],'accessors':[], 'extensionsUsed':['KHR_materials_unlit']}
        data=bytearray()
        def accessor(values,kind,n,typ=5126,fmt='f'):
            off=len(data);data.extend(struct.pack('<'+fmt*len(values),*values));data.extend(b'\0'*((-len(data))%4))
            vi=len(doc['bufferViews']);doc['bufferViews'].append({'buffer':0,'byteOffset':off,'byteLength':len(values)*4})
            item={'bufferView':vi,'componentType':typ,'count':len(values)//n,'type':kind}
            if kind=='VEC3': item.update(min=[min(values[i::n]) for i in range(n)],max=[max(values[i::n]) for i in range(n)])
            ai=len(doc['accessors']);doc['accessors'].append(item);return ai
        attrs={'POSITION':accessor(self.pos,'VEC3',3),'NORMAL':accessor(self.norm,'VEC3',3),'COLOR_0':accessor(self.col,'VEC4',4),'TEXCOORD_0':accessor(self.uv,'VEC2',2)}
        prim={'attributes':attrs,'indices':accessor(self.idx,'SCALAR',1,5125,'I'),'material':0}
        mat={'name':name,'doubleSided':True,'pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1],'metallicFactor':0,'roughnessFactor':.8},'extensions':{'KHR_materials_unlit':{}}}
        if blend: mat['alphaMode']='BLEND'
        if texture:
            blob=texture; vi=len(doc['bufferViews']);doc['bufferViews'].append({'buffer':0,'byteOffset':len(data),'byteLength':len(blob)});data.extend(blob)
            doc['images']=[{'bufferView':vi,'mimeType':'image/png'}];doc['textures']=[{'source':0}]
            mat['pbrMetallicRoughness']['baseColorTexture']={'index':0}
        doc['materials']=[mat];doc['meshes']=[{'name':name,'primitives':[prim]}]
        write_glb(MODELS/(name+'.glb'),doc,data)
        return 'models/'+name+'.glb'

def png_status():
    im=Image.new('RGB',(960,544),(10,17,28));d=ImageDraw.Draw(im)
    font=lambda s:ImageFont.truetype('C:/Windows/Fonts/segoeui.ttf',s)
    d.text((30,18),'shadPS4',font=font(34),fill=(235,242,250));d.text((680,26),'PREVIEW',font=font(23),fill=(107,200,255));d.line((28,65,932,65),fill=(107,200,255),width=2)
    d.text((30,83),'GAME FPS',font=font(22),fill=(149,168,190));d.text((28,108),'--',font=font(82),fill=(107,200,255));d.text((475,102),'XR / CINEMA',font=font(33),fill=(235,242,250));d.text((475,155),'Live on device',font=font(25),fill=(149,168,190))
    for i,label in enumerate(['CPU','GPU','MEMORY','BATTERY','POWER','BATTERY TEMP']):
        x=28+(i%2)*466;y=235+(i//2)*86;d.rounded_rectangle((x,y,x+438,y+77),radius=7,fill=(19,29,43));d.text((x+16,y+7),label,font=font(21),fill=(149,168,190));d.text((x+16,y+34),'--',font=font(30),fill=(235,242,250))
    d.text((30,509),'Editor preview - no live measurements',font=font(20),fill=(149,168,190));out=io.BytesIO();im.save(out,format='PNG');return out.getvalue()

def preview_psv():
    src=ROOT/'android/shadps4-app/app/src/main/assets/xr/psv_status.glb';b=src.read_bytes();n=struct.unpack_from('<I',b,12)[0];doc=json.loads(b[20:20+n]);data=bytearray(b[28+n:]);blob=png_status();vi=len(doc['bufferViews']);doc['bufferViews'].append({'buffer':0,'byteOffset':len(data),'byteLength':len(blob)});data.extend(blob)
    ii=len(doc.setdefault('images',[]));doc['images'].append({'bufferView':vi,'mimeType':'image/png'});ti=len(doc.setdefault('textures',[]));doc['textures'].append({'source':ii})
    for mat in doc['materials']:
        if mat['name']=='status_screen': mat['pbrMetallicRoughness']['baseColorTexture']={'index':ti}
    write_glb(MODELS/'psv-preview.glb',doc,data)

m=Mesh()
for radius,alpha in [(0,.55),(.30,.42),(.42,.18),(.5,0)]:
    for i in range(49):
        angle=math.tau*i/48;m.vertex((radius*math.cos(angle),0,radius*math.sin(angle)),(0,1,0),(0,0,0));m.col[-1]=alpha
for r in range(3):
    for i in range(48):
        a=r*49+i;b=a+49;m.idx.extend([a,b,a+1,a+1,b,b+1])
m.save('contact-shadow',blend=True)
# A technical display placeholder with the production 16:9 aperture.
im=Image.new('RGB',(1600,900));d=ImageDraw.Draw(im)
for y in range(900):
    t=y/899;d.line((0,y,1600,y),fill=(int(20+13*t),int(41+10*t),int(61+14*t)))
f=lambda s:ImageFont.truetype('C:/Windows/Fonts/segoeuil.ttf',s)
d.text((120,115),'shadPS4',font=f(45),fill=(155,191,210));d.text((116,342),'XR CINEMA',font=f(116),fill=(225,235,241));d.text((123,515),'Your game. A bigger view.',font=f(31),fill=(152,181,199));d.line((125,730,1475,730),fill=(79,112,132),width=2);d.text((126,765),'EDITOR PREVIEW     /     4.4 m SCREEN     /     3.0 m DISTANCE',font=f(22),fill=(138,166,184));out=io.BytesIO();im.save(out,format='PNG')
(MODELS/'screen-preview.png').write_bytes(out.getvalue())
m=Mesh()
for p,uv in [((-.5,-.5,0),(0,1)),((.5,-.5,0),(1,1)),((.5,.5,0),(1,0)),((-.5,.5,0),(0,0))]:m.vertex(p,(0,0,1),(1,1,1),uv)
m.idx=[0,1,2,0,2,3];m.save('screen-preview',out.getvalue())
preview_psv()
# Lit PBR props/rooms come from tools/xr/blender_cinema_assets.py.
for stale in ['button','button_round','cyan','dark','dark_round','dusk-sky','floor','graphite','graphite_round','metal','rug_box','wall','warm','wood']:
    (MODELS/(stale+'.glb')).unlink(missing_ok=True)

class Scene:
    def __init__(self,key,name):
        self.key=key;self.g={k:[] for k in ['objects','mesh_renderers','cameras','lights','opaque_components','prefab_instances']}
        self.doc={'header':{'magic':'SPWD','format_version':1,'type':'world'},'id':guid(key),'name':name,'environment':{'sky_colour':[.3,.36,.48],'ground_colour':[.20,.17,.14],'ambient_intensity':.6,'gi':False,'skybox':False,'exposure':1},'graph':self.g}
    def obj(self,name,p=(0,0,0),s=(1,1,1),model=None,parent='',q=(0,0,0,1)):
        model=model and ('models/'+model+'.glb' if '/' not in model else model)
        q=[v/math.sqrt(sum(x*x for x in q)) for v in q]
        id=guid(self.key+'/'+name);self.g['objects'].append({'id':id,'name':name,'parent':parent,'enabled':True,'translation':p,'rotation':q,'scale':s})
        if model:self.g['mesh_renderers'].append({'object':id,'component':{'enabled':True,'primitive':{'shape':'none'},'model':model}})
        return id
    def box(self,name,p,s,col,parent='',q=(0,0,0,1)):return self.obj(name,p,s,col,parent,q)
    def camera(self,name,p,q,fov=76,main=False):
        id=self.obj(name,p,q=q);self.g['cameras'].append({'object':id,'component':{'projection':'perspective','vfov_degrees':fov,'near_meters':.03,'far_meters':400,'is_main':main}})
    def light(self,name,p,q,kind,colour,intensity,parent='',**extra):
        id=self.obj(name,p,q=q,parent=parent)
        comp={'enabled':True,'type':kind,'colour':list(colour),'intensity':intensity}
        comp.update(extra);self.g['lights'].append({'object':id,'component':comp});return id
    def save(self): (OUT/(self.key+'.world.json')).write_text(json.dumps(self.doc,indent=2)+'\n',encoding='utf-8')


EYE=1.45
DOWN=quat_x(-math.pi/2)          # rect/spot lights emit along local -Z
TOWARD_VIEWER=quat_y(math.pi)
# Shared with tools/xr/blender_cinema_assets.py: a bigger screen for VR.
SW,SH,SD,SCY=4.40,2.475,3.0,1.55   # flat 16:9 ~200 in, 3 m away (~72 x 45 deg)
BACK=-3.12
CONSOLE_Z=BACK+.04+.21+.005
CONSOLE_TOP=.30

def ps4_vertical(a,p):
    """Standing PS4 Pro beside the screen; front light strip faces the viewer."""
    ps=a.obj('PS4 console',p,q=quat_y(-.15),model='ps4-pro')
    a.obj('PS4 contact shadow',(0,.0004,0),(.20,.003,.38),'contact-shadow',ps)
    a.light('PS4 light strip glow',(-.02,.16,.26),(0,0,0,1),'point',(.18,.48,1.),.5,range=1.2,parent=ps)
    return ps

# Case covers from the user's own games (tools/xr/game_covers.py ->
# git-ignored models/local/); an original placeholder stands in when absent.
GAME_CASES={'CUSA03023':'Bloodborne','CUSA09554':'Monster Hunter: World','CUSA34119':'Monster Hunter Rise'}
def case_model(title,fallback):
    local=MODELS/'local'/('disc-case-%s.glb'%title)
    return 'models/local/disc-case-%s.glb'%title if local.exists() else 'disc-case-'+fallback

def cases(a,name,p,yaw,titles):
    for i,(t,f) in enumerate(titles):
        a.obj('%s %d'%(name,i),(p[0],p[1]+.0141*i,p[2]),q=quat_y(yaw+.09*((i*7)%3-1)),model=case_model(t,f))

def common(a):
    """Seated viewer, screen aperture and its light, coffee table and props."""
    a.camera('Viewer - seated',(0,EYE,0),quat_x(-.10),86,True)
    # The production game quad replaces this named editor-only screen.
    a.obj('Game screen - preview',(0,SCY,-SD+.0015),(SW,SH,1),'screen-preview')
    a.light('Screen light',(0,SCY,-SD+.003),TOWARD_VIEWER,'rect',(1,1,1),1.4,width=SW,height=SH,range=10,
            screen_source='image',screen_image='models/screen-preview.png',screen_grid=[4,3])
    a.obj('Rug',(0,0,-.95),model='area-rug')
    a.obj('Table contact shadow',(0,.0105,-1.10),(1.62,.004,.78),'contact-shadow')
    a.obj('Coffee table',(0,0,-1.10),model='coffee-table')
    top=.52
    a.obj('PSV dock',(0,top,-1.05),model='psv-dock')
    a.obj('PSV status display',(0,top+.14,-1.05),(2.5,2.5,2.5),'models/psv-preview.glb',q=quat_x(-.32))
    for name,x,yaw in (('DualShock 4 L',-.38,.28),('DualShock 4 R',.38,-.30)):
        pad=a.obj(name,(x,top,-1.06),q=quat_y(yaw),model='dualshock4')
        a.obj(name+' contact shadow',(0,.0004,.004),(.20,.003,.13),'contact-shadow',pad)
    # The three games laid out flat side by side behind the left pad.
    for i,(x,z,yaw,(t,f)) in enumerate(((-.615,-1.270,.07,('CUSA09554','a')),(-.465,-1.262,-.03,('CUSA34119','c')),
                                        (-.315,-1.274,.05,('CUSA03023','b')))):
        a.obj('Game case table %d'%i,(x,top,z),q=quat_y(yaw),model=case_model(t,f))

def room_props(a):
    a.obj('TV',(0,SCY,-SD),model='tv')
    a.obj('Media console',(0,0,CONSOLE_Z),model='media-console')
    ps4_vertical(a,(-2.45,CONSOLE_TOP,CONSOLE_Z))
    a.obj('Soundbar',(0,CONSOLE_TOP,CONSOLE_Z+.11),model='soundbar')
    cases(a,'Game case console',(2.27,CONSOLE_TOP,CONSOLE_Z+.02),-.12,[('CUSA34119','c'),('CUSA03023','d'),('CUSA09554','b')])

def lounge(key,title):
    a=Scene(key,title);common(a)
    a.doc['environment'].update(sky_colour=[.24,.20,.16],ground_colour=[.13,.09,.06],ambient_intensity=.30,
        gi=True,gi_voxel_size=.12,gi_probes_per_frame=512,gi_hysteresis=.7,exposure=1.6)
    a.obj('Room',model='lounge-room')
    room_props(a)
    a.obj('Plant',(-2.9,0,-1.6),model='plant')
    a.light('Cove wash',(0,3.0,BACK+.37),quat_x(-math.pi/2-.45),'rect',(1,.72,.45),14,width=6.6,height=.08,range=5)
    for i,(x,z) in enumerate([(-2.0,-1.4),(2.0,-1.4),(-2.0,.6),(2.0,.6)]):
        a.light('Downlight %d'%i,(x,3.08,z),DOWN,'spot',(1,.84,.66),9 if z<0 else 6,range=6,
                inner_cone_degrees=18,outer_cone_degrees=42)
    a.light('Console glow',(0,.05,CONSOLE_Z+.06),DOWN,'rect',(1,.70,.42),3,width=5.0,height=.06,range=1.2)
    a.save()

# 01: walnut-and-fabric media wall, wall-mounted thin-bezel display.
lounge('tv-lounge','01 / Quiet TV Lounge')

# 02: rooftop deck at dusk, steel-framed outdoor screen, quiet skyline.
# Outdoor Worlds keep voxel GI off: the sky dome would stretch its volume
# to hundreds of metres (~290k voxels / 254k probes measured on Swan).
a=Scene('dusk-terrace','02 / Dusk Cinema Terrace');common(a)
a.doc['environment'].update(sky_colour=[.24,.28,.46],ground_colour=[.22,.15,.10],ambient_intensity=.75,
    gi=False,gi_voxel_size=.15,gi_probes_per_frame=512,gi_hysteresis=.7,exposure=1.1)
a.obj('Sky and skyline',model='dusk-sky-dome')
a.obj('Terrace',model='terrace')
a.obj('Outdoor screen',(0,SCY,-SD),model='outdoor-screen')
for s_ in (-1,1):
    a.obj('Planter '+('L' if s_<0 else 'R'),(s_*2.55,0,-1.6),model='grass-planter')
a.obj('Side cabinet',(-2.48,0,-3.05),q=quat_y(.10),model='side-cabinet')
ps4_vertical(a,(-2.48,.42,-3.05))
a.light('Dusk sun',(0,10,-30),quat_x(-.12),'directional',(1,.55,.32),.6)
a.save()

# 03: plain dark room - same subject, lit almost only by the screen.
a=Scene('dark-room','03 / Dark Room');common(a)
a.doc['environment'].update(sky_colour=[.07,.07,.08],ground_colour=[.04,.03,.025],ambient_intensity=.07,
    gi=True,gi_voxel_size=.12,gi_probes_per_frame=512,gi_hysteresis=.7,exposure=1.6)
a.obj('Room',model='dark-room')
room_props(a)
a.light('Cove wash',(0,3.0,BACK+.37),quat_x(-math.pi/2-.45),'rect',(1,.70,.42),2.5,width=6.6,height=.08,range=4)
a.light('Console glow',(0,.05,CONSOLE_Z+.06),DOWN,'rect',(1,.70,.42),1.2,width=5.0,height=.06,range=1.0)
a.save()

# 04: seaside platform at sunset.
a=Scene('seaside','04 / Seaside Sunset');common(a)
a.doc['environment'].update(sky_colour=[.48,.55,.78],ground_colour=[.46,.35,.24],ambient_intensity=.80,
    gi=False,gi_voxel_size=.15,gi_probes_per_frame=512,gi_hysteresis=.7,exposure=1.0)
a.obj('Seaside',model='seaside')
a.obj('Outdoor screen',(0,SCY,-SD),model='outdoor-screen')
a.obj('Side cabinet',(-2.45,0,-3.0),q=quat_y(.10),model='side-cabinet')
ps4_vertical(a,(-2.45,.42,-3.0))
a.light('Sunset sun',(0,10,-30),qmul(quat_y(math.pi),quat_x(-.10)),'directional',(1,.62,.36),2.5)
for i,x in enumerate((-2.45,2.45)):
    a.light('Lantern %d'%i,(x,.64,1.95),(0,0,0,1),'point',(1,.6,.3),.5,range=3)
a.save()

# 05: void - floating screen over a dark glossy disc.
a=Scene('void','05 / Void');common(a)
a.doc['environment'].update(sky_colour=[.05,.06,.10],ground_colour=[.0,.0,.0],ambient_intensity=.12,
    gi=False,gi_voxel_size=.15,gi_probes_per_frame=512,gi_hysteresis=.7,exposure=1.5)
a.obj('Void space',model='void-space')
a.obj('TV',(0,SCY,-SD),model='tv')
a.obj('Side plinth',(-2.45,0,-3.0),q=quat_y(.10),model='plinth')
ps4_vertical(a,(-2.45,.42,-3.0))
a.save()
print('Wrote Lite Editor World scenes:',OUT)
