#include "../ue426-reference/GnmGraphicsReference.h"
#include "../ue4-reference/GnmStagesCore.h"
#include <iostream>
#include <string>
#define REQUIRE(X) do { if (!(X)) throw std::runtime_error(#X); } while (false)
using namespace Tetris426::Graphics;
struct Sink {
    std::vector<std::vector<u32>> packets, tables;
    std::vector<std::string> events;
    std::array<u32,7> es_body{}, hs_body{};
    u32 hs_config=0, es_modifier=0, count=0; bool hs_update=false, fail=false;
    Stage indirect_stage=Stage::Compute; u32 lo=0,hi=0;
    void Emit(const u32* p,unsigned n) { packets.emplace_back(p,p+n); events.push_back("packet"); }
    u64 UploadTable(const u32* p,unsigned n) {
        if(fail) throw std::runtime_error("allocation failure");
        tables.emplace_back(p,p+n); events.push_back("table:"+std::to_string(n));
        return 0x123400000000ull+tables.size()*0x1000;
    }
    u64 UploadConstants(const u8*,unsigned) { events.push_back("loose"); return 0x22000000; }
    void Shader(Stage s,const Program*,bool u,u32) { events.push_back((u?"update":"set")+std::to_string(Index(s))); }
    void ExportShader(const Program&,const std::array<u32,7>& b,u32 m) { es_body=b;es_modifier=m;events.push_back("ES"); }
    void HullShader(const Program&,const std::array<u32,7>& b,u32 c,bool u) { hs_body=b;hs_config=c;hs_update=u;events.push_back("HS"); }
    void Prefetch(u64,u32) { events.push_back("prefetch"); }
    void PixelInputs(const u32* p,unsigned n) { packets.emplace_back(p,p+n);events.push_back("link"); }
    void BeginPipeline(const Pipeline&,const Pipeline*) { events.push_back("begin"); }
    void FixedState(const Pipeline&,const Pipeline*) { events.push_back("fixed"); }
    void RetainPipeline(const Pipeline*,const Pipeline*) { events.push_back("retain"); }
    void RetainUniform(Frequency,unsigned,const UniformBuffer*,const UniformBuffer*) { events.push_back("retain-ub"); }
    void ValidateStaticUniform(const UniformBuffer*,unsigned,u32) { events.push_back("static-ub"); }
    void TouchTexture(const Resource&) { events.push_back("texture"); }
    void BaseVertex(u32) { events.push_back("base"); }
    void Instances(u32) { events.push_back("instances"); }
    void IndexType(u32,u32) { events.push_back("index-type"); }
    void DrawAuto(u32 n,u32) { count=n;events.push_back("draw"); }
    void DrawIndex(u32 n,u64,u32) { count=n;events.push_back("indexed"); }
    void SetBase(u32,u64) { events.push_back("set-base"); }
    void IndexBase(u64) { events.push_back("index-base"); }
    void IndexSize(u32) { events.push_back("index-size"); }
    void DrawIndirect(u32,Stage s,u32 l,u32 h,u32) { indirect_stage=s;lo=l;hi=h;events.push_back("indirect"); }
    void DrawIndexIndirect(u32 o,Stage s,u32 l,u32 h,u32 f) { DrawIndirect(o,s,l,h,f); }
};
template<class F> void Rejects(F f) { bool caught=false;try{f();}catch(const std::exception&){caught=true;}REQUIRE(caught); }
std::size_t Event(const Sink& s,const std::string& name) {
    auto it=std::find(s.events.begin(),s.events.end(),name);REQUIRE(it!=s.events.end());return std::size_t(it-s.events.begin());
}
void ProfilesAndBudgets() {
    REQUIRE(CopyShaderAddress(0x1000,3)==0x1044);
    for(int on=0;on<2;++on) for(int compact=0;compact<2;++compact) {
        auto r=CopyReflection(on!=0,compact!=0);
        REQUIRE(r.Word(0)==(compact?0:64));REQUIRE(r.bytes[on?15:12]==0);
        REQUIRE(r.bytes[on?12:15]==255 && r.bytes[3]==2);
        REQUIRE(r.Word(0x106)==(compact?0xffff:0) && r.Word(0x10c)==(compact?0xffff:12));
        REQUIRE(r.Word(0xe6)==0xffff && r.bytes[0x111]==20 && r.bytes[6]==(compact?255:2));
    }
    GeometryMetadata g;g.items[0]=1;g.word34=1;REQUIRE(OnChipBudget(g).groups==64);
    g.word34|=31<<11;REQUIRE(OnChipBudget(g).groups==63);
    g.word34=2047;g.items[0]=9;REQUIRE(!OnChipBudget(g).groups);
    g.word34=0;Rejects([&]{OnChipBudget(g);});g.word34=1;
    for(u32 x:{128u,129u,257u,513u}) {
        g.items[0]=x;kh3_gfx::GeometryMetadata k;k.Word34=g.word34;k.ItemDwords[0]=x;
        REQUIRE(GeometryCut(g)==kh3_gfx::GeometryCut(k));
        auto a=OnChipBudget(g);auto b=kh3_gfx::OnChipBudget(k);
        REQUIRE(a.groups==b.Groups && a.lds512==b.Lds512Units);
    }
    REQUIRE(PatchEsResource2(~0u,7)==0xe07fffff && PatchEsResource2(~0u,0)==~0u);
}
void RingsAndRegisters() {
    GeometryMetadata g;g.word34=3;g.items={{1,2,3,4}};
    RingStorage rs;rs.esgs=0x23400000000;rs.gsvs=0x123fffffe00;
    RingTable t;t.fill(0xdeadbeef);FillRingTable(t,rs,g);
    REQUIRE(t[0]==0xdeadbeef && t[7]==0xdeadbeef && t[36]==0xdeadbeef && t[47]==0xdeadbeef);
    REQUIRE(t[8]==0 && t[9]==0x80003234 && t[10]==0x400000 && t[11]==0xea7204);
    REQUIRE(t[20]==0x100 && t[21]==0x80183124 && t[22]==64 && t[23]==0xaa7204);
    REQUIRE(t[32]==0xfffffe00 && t[33]==0x3123 && t[35]==0x27204);
    kh3_gfx::GeometryMetadata old;old.Word34=3;for(unsigned i=0;i<4;++i)old.ItemDwords[i]=g.items[i];
    u32 expected[48];std::fill(expected,expected+48,0xdeadbeef);
    kh3_gfx::GeometryRingDescriptors(expected,rs.esgs,rs.gsvs,old);REQUIRE(std::equal(t.begin(),t.end(),expected));
    rs.esgs_bytes=0x200000;FillRingTable(t,rs,g);REQUIRE(t[10]==0x200000 && t[14]==0x200000);
    Sink s;GeometryRingRegisters(s,g);
    REQUIRE((s.packets[0]==std::vector<u32>{0xc0046900,0x2d7,1,2,3,4}));
    REQUIRE((s.packets[1]==std::vector<u32>{0xc0036900,0x298,3,9,18}));
    REQUIRE(s.packets[2][2]==3 && s.packets[3][2]==30);
    REQUIRE(SameRingLayout(16,g,16,g) && !SameRingLayout(16,g,17,g));
    StageLcue l;l.UploadRings(s,t,rs,16,g);auto saved=s.tables.back();
    g.items[0]=9;l.UploadRings(s,t,rs,16,g);
    REQUIRE(saved[17]!=s.tables.back()[17] && l.extras[2].shared_pointer==0x123400002000);
}
void ModesAndSelection() {
    StageLcue l;Sink s;Program gs,copy;Reflection r;GeometryMetadata g;
    gs.identity=0x1000;copy.identity=0x1044;g.copy_flags=0x22;g.word34=4|(2<<11);r.bytes[15]=1;
    l.SetMode(s,0xb0,true);l.SelectGS(s,gs,r,copy,true,g,8);l.SetExportFactor(16);
    REQUIRE((s.packets[0]==std::vector<u32>{0xc0016900,0x2d5,0xb0}));
    REQUIRE(s.packets[1][1]==0x291 && s.packets[1][2]==0x4018);
    REQUIRE(s.packets[2][2]==0x600023 && l.extras[2].factor_a==24 && l.extras[2].factor_b==16);
    l.stages[3].selected.emitted=true;l.stages[2].selected.emitted=true;
    l.stages[3].selected.shader_dirty=false;l.stages[2].selected.shader_dirty=false;
    Program gs2=gs,copy2=copy;gs2.identity++;copy2.identity++;
    auto n=s.packets.size();l.SelectGS(s,gs2,r,copy2,true,g,8);
    REQUIRE(l.stages[3].selected.update && l.stages[2].selected.update && s.packets.size()==n);
    l.SelectGS(s,gs2,r,copy2,true,g,9);
    REQUIRE(!l.stages[3].selected.update && s.packets[n].size()==6);
    g.copy_flags=0;l.SelectGS(s,gs2,r,copy2,true,g,0);
    REQUIRE(l.geometry_mode.key!=~0u && l.geometry_mode.mode==0x180003);
    l.SetMode(s,0);REQUIRE(l.geometry_mode.key==~0u && !l.geometry_mode.mode);
    REQUIRE(s.packets.back()[1]==0x2d5 && !s.packets.back()[2]);Rejects([&]{l.SetMode(s,3);});
}
void AllStageSelectors() {
    const unsigned counts[]={0,8,12,9,11,8,11};Reflection r;r.SetWord(0,32);
    for(unsigned i=4;i<16;++i)r.bytes[i]=u8(i);
    StageBindings state;state.selected.reflection=&r;Extras e;e.factor_a=7;e.factor_b=9;
    for(unsigned i=1;i<=6;++i) {
        Sink s;CommitTable(s,Stage(i),state,e);REQUIRE(s.packets.size()==counts[i]);
        if(i==2 || i==3)REQUIRE(s.packets.back().back()==63);
        if(i==4)REQUIRE(s.packets.back()[1]==0xd2);
        if(i==6)REQUIRE(s.packets.back()[1]==0x152);
    }
}
struct Replay {
    StageLcue l;Program es,gs,vs,ps,ls,hs;Reflection er,gr,vr,pr,lr,hr;
    Replay() {
        es.identity=4;gs.identity=3;vs.identity=2;ps.identity=1;ls.identity=6;hs.identity=5;
        es.body[3]=~0u;hs.body[4]=~0u;
        er.SetWord(0,16);gr.SetWord(0,12);vr.SetWord(0,20);pr.SetWord(0,8);lr.SetWord(0,24);hr.SetWord(0,28);
        l.SelectES(es,er,0x1234,7,3);l.SelectVS(vs,vr,0,0);l.SelectPS(&ps,&pr);
        l.stages[3].selected.Select(&gs,&gr,true);
        l.stages[6].selected.Select(&ls,&lr,true);l.stages[5].selected.Select(&hs,&hr,true);
        l.tess={16,3,4,2,32,6,99,8,0x80000002};l.hull_config_previous=0xabc00000;
        hr.SetWord(0xe4,0x8009);vr.SetWord(0xe4,4);er.SetWord(0xe4,4);l.stages[5].selected.update=true;
    }
};
void GeometryCommitOrder() {
    Replay x;Sink s;x.l.mode=0xb0;x.l.prefetch=true;
    x.vs.semantics={0x707};x.ps.semantics={7};x.l.Commit(s);
    REQUIRE(Event(s,"ES")<Event(s,"set3") && Event(s,"set3")<Event(s,"table:16"));
    REQUIRE(Event(s,"table:12")<Event(s,"set2") && Event(s,"set2")<Event(s,"set1"));
    REQUIRE(Event(s,"table:8")<Event(s,"link") && s.packets.back()[0]==7);
    REQUIRE(s.es_body[3]==0xe03fffff && s.es_modifier==7 && x.es.body[3]==~0u);
    REQUIRE(!x.l.stages[4].selected.emitted && x.l.stages[3].selected.emitted && !x.l.stages[3].selected.update);
}
void TessellationReplayAndCache() {
    Replay x;Sink s;x.l.mode=0x45;auto c=TessellationConstants(x.l.tess);
    REQUIRE((c==std::array<u32,9>{{16,32,8,384,32,1408,128,0x44000000,99}}));x.l.Commit(s);
    REQUIRE(s.tables.front()==std::vector<u32>(c.begin(),c.end()));
    REQUIRE((s.packets.front()==std::vector<u32>{0xc0001000,0x6875000d,0xc0047600,0x115,0x1000,0x100234,3,0x20077fac}));
    REQUIRE(Event(s,"table:9")<Event(s,"set6") && Event(s,"set6")<Event(s,"HS"));
    REQUIRE(Event(s,"HS")<Event(s,"table:24") && Event(s,"table:28")<Event(s,"set2"));
    REQUIRE(s.hs_body[4]==0xfffdffff && x.hs.body[4]==~0u && s.hs_update);
    REQUIRE(s.hs_config==(0xabc00000|8|(3<<8)|(6<<14)));
    REQUIRE(x.l.stages[2].staging[4]==0x1000 && x.l.stages[2].staging[6]==3);
    REQUIRE(!x.l.stages[5].selected.update && x.l.stages[5].selected.emitted);
    auto n=s.tables.size();x.l.Commit(s);REQUIRE(s.tables.size()==n+2);
    REQUIRE(s.tables[n].size()==28 && s.tables[n+1].size()==20 && x.l.tess_descriptor[0]==0x1000);
    x.l.tess.hs28=0;auto no=TessellationConstants(x.l.tess);
    REQUIRE(no[1]==16 && no[3]==0 && no[5]==384 && no[6]==0);
    Replay y;y.l.mode=0x45;y.l.tess_constants_enabled=false;Sink off;y.l.Commit(off);
    REQUIRE(off.tables.front().size()==24 && !y.l.tess_descriptor_valid);
}
void CombinedModeOrdering() {
    Replay x;Sink s;x.l.mode=0xad;x.l.Commit(s);REQUIRE(Event(s,"ES")<Event(s,"table:9"));
    REQUIRE(x.l.stages[4].selected.resources_dirty && x.l.stages[4].staging[4]==0x3000);
    REQUIRE(s.tables[0][4]==0); // internal domain binding happens after ES table commit
    auto n=s.tables.size();x.l.Commit(s);
    REQUIRE(s.tables[n].size()==16 && s.tables[n][4]==0x3000 && x.l.stages[4].selected.resources_dirty);
}
void UeGeometryContext() {
    Sink s;CommandContext<Sink> ctx(s,64);Shader v,g,p;
    v.vertex_stage=Stage::Export;v.export_program.identity=0x2000;v.export_program.body[5]=16;
    v.export_program.indirect_selectors=0x72;v.export_reflection.SetWord(0,32);
    v.export_reflection.SetWord(0xe6,0);v.export_reflection.SetWord(0xbe,4);
    g.frequency=Frequency::Geometry;g.program.identity=0x3000;g.geometry_header3=3;
    g.copy_program.identity=0x3044;g.copy_program.semantics={0x907};
    g.geometry_metadata.word34=3;g.geometry_metadata.items={{1,2,3,4}};
    g.reflection.SetWord(0,8);g.reflection.SetWord(0xbe,0);
    p.frequency=Frequency::Pixel;p.program.identity=0x4000;p.program.semantics={7};
    Pipeline pipe;pipe.vertex=&v;pipe.geometry=&g;pipe.pixel=&p;pipe.fetch=0x6000;pipe.ConfigureStages();
    pipe.elements.push_back({4,16,0,0,0,0});ctx.rings.esgs=0x40000000;ctx.rings.gsvs=0x50000000;
    ctx.RHISetGraphicsPipelineState(pipe,false);REQUIRE(s.tables[0].size()==48);
    VertexBuffer vb;vb.current_address=0x70000000;vb.size=256;ctx.RHISetStreamSource(0,&vb,0);
    UniformBuffer ub;ub.descriptor={{11,22,33,44}};
    ctx.RHISetShaderUniformBuffer(v,0,ub);ctx.RHISetShaderUniformBuffer(g,0,ub);ctx.RHIDrawPrimitive(0,1,1);
    REQUIRE(ctx.lcue.stages[4].staging[0]==0x70000000 && ctx.lcue.stages[4].staging[4]==11);
    REQUIRE(ctx.lcue.stages[3].staging[0]==11 && ctx.lcue.stages[2].staging[4]==0);
    REQUIRE(s.count==3 && Event(s,"link")<Event(s,"draw") && s.packets.back()[0]==9);
    auto n=s.tables.size();ctx.RHISetGraphicsPipelineState(pipe,false);REQUIRE(s.tables.size()==n);
    ArgumentBuffer args;args.current_address=0x90000000;ctx.RHIDrawPrimitiveIndirect(args,4);
    REQUIRE(s.indirect_stage==Stage::Export && s.lo==2 && s.hi==7);
    IndexBuffer ib;ib.current_address=0xa0000000;ib.size=256;
    ctx.RHIDrawIndexedIndirect(ib,args,3);REQUIRE(s.indirect_stage==Stage::Export);
    ctx.RHIDrawIndexedPrimitiveIndirect(ib,args,20);
    Shader on=g;on.program.identity=0x4000;on.copy_program.identity=0x4044;on.geometry_metadata.copy_flags=0x12;
    on.reflection.bytes[15]=1;Pipeline onpipe=pipe;onpipe.geometry=&on;onpipe.ConfigureStages();
    ctx.RHISetGraphicsPipelineState(onpipe,false);REQUIRE(ctx.lcue.geometry_mode.mode==0x600003);
    REQUIRE(!onpipe.needs_ring_table && ctx.lcue.extras[2].factor_b==16);
    ctx.RHIDrawIndexedPrimitive(ib,0,0,3,0,1,1);
    n=s.tables.size();ctx.RHISetGraphicsPipelineState(pipe,false);
    REQUIRE(s.tables.size()==n+1 && s.tables.back().size()==48); // on-chip PSO ring key was zero
    Shader ordinary=v;ordinary.vertex_stage=Stage::Vertex;ordinary.program.identity=0xb000;
    Pipeline simple;simple.vertex=&ordinary;simple.pixel=&p;simple.ConfigureStages();
    ctx.RHISetGraphicsPipelineState(simple,false);ctx.RHIDrawPrimitive(0,1,1);
    REQUIRE(ctx.lcue.mode==0 && !ctx.lcue.geometry_mode.mode);
    simple.mode=0x45;Rejects([&]{ctx.RHISetGraphicsPipelineState(simple,false);});
}
void FailureAndUnsupportedStage() {
    Replay x;Sink s;s.fail=true;x.l.mode=0x45;Rejects([&]{x.l.Commit(s);});
    REQUIRE(x.l.stages[5].selected.shader_dirty && !x.l.tess_descriptor_valid && s.events.empty());
    Rejects([&]{HardwareStage(Frequency::Hull,Stage::Local);});
    Rejects([&]{HardwareStage(Frequency::Domain,Stage::Vertex);});
    StageLcue invalid;invalid.mode=0x45;Rejects([&]{invalid.Commit(s);});
}
int main() {
    try {
        ProfilesAndBudgets();RingsAndRegisters();ModesAndSelection();AllStageSelectors();
        GeometryCommitOrder();TessellationReplayAndCache();CombinedModeOrdering();
        UeGeometryContext();FailureAndUnsupportedStage();
        std::cout<<"9 stage426 scenarios passed\n";return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
