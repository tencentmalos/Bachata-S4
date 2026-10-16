#include "../ue426-reference/GnmGraphicsReference.h"
#include "../ue4-reference/GnmGraphicsCore.h"
#include <iostream>
#include <string>
#include <functional>
#define REQUIRE(X) do { if (!(X)) throw std::runtime_error(#X); } while (false)
using namespace Tetris426::Graphics;

struct RecordingSink {
    std::vector<std::vector<u32>> packets, tables;
    std::vector<std::vector<u8>> constants;
    std::vector<std::string> events;
    bool fail_upload = false;
    u32 count = 0, offset = 0, selector_low = 0, selector_high = 0;
    u64 address = 0;
    unsigned texture_touches = 0, static_checks = 0;
    void Emit(const u32* p, unsigned n) { packets.emplace_back(p, p+n); events.push_back("packet"); }
    u64 UploadTable(const u32* p, unsigned n) {
        if (fail_upload) throw std::runtime_error("allocation failed");
        tables.emplace_back(p, p+n); events.push_back("table");
        return 0x123400000000ull + tables.size()*0x1000;
    }
    u64 UploadConstants(const u8* p, unsigned n) {
        constants.emplace_back(p,p+n); events.push_back("constants");
        return 0x2200000000ull + constants.size()*0x1000;
    }
    void Shader(Stage s, const Program* p, bool update, u32) {
        events.push_back(std::string(update ? "update" : "set") + std::to_string(Index(s)) + (p ? "" : "null"));
    }
    void Prefetch(u64, u32) { events.push_back("prefetch"); }
    void ExportShader(const Program&, const std::array<u32,7>&, u32) { events.push_back("ES"); }
    void HullShader(const Program&, const std::array<u32,7>&, u32, bool) { events.push_back("HS"); }
    void PixelInputs(const u32* p, unsigned n) { packets.emplace_back(p,p+n); events.push_back("inputs"); }
    void TouchTexture(const Resource&) { ++texture_touches; }
    void BeginPipeline(const Pipeline&, const Pipeline*) { events.push_back("begin-pipeline"); }
    void FixedState(const Pipeline&, const Pipeline*) { events.push_back("fixed-state"); }
    void RetainPipeline(const Pipeline*, const Pipeline*) { events.push_back("retain-pipeline"); }
    void RetainUniform(Frequency, unsigned, const UniformBuffer*, const UniformBuffer*) { events.push_back("retain-ub"); }
    void ValidateStaticUniform(const UniformBuffer*, unsigned slot, u32 hash) {
        REQUIRE(slot == 2 && hash == 0x12345678); ++static_checks;
    }
    void BaseVertex(u32 v) { events.push_back("base:"+std::to_string(v)); }
    void Instances(u32 v) { events.push_back("instances:"+std::to_string(v)); }
    void IndexType(u32 v, u32 mode) { REQUIRE(mode==2); events.push_back("index:"+std::to_string(v)); }
    void DrawAuto(u32 n, u32 f) { REQUIRE(f==0); count=n; events.push_back("draw"); }
    void DrawIndex(u32 n, u64 a, u32 f) { REQUIRE(f==0); count=n; address=a; events.push_back("draw-index"); }
    void SetBase(u32 s, u64 a) { REQUIRE(s==0); address=a; events.push_back("set-base"); }
    void IndexBase(u64 a) { address=a; events.push_back("index-base"); }
    void IndexSize(u32 n) { count=n; events.push_back("index-size"); }
    void DrawIndirect(u32 o, Stage s, u32 lo, u32 hi, u32 f) {
        REQUIRE(s==Stage::Vertex && f==0); offset=o; selector_low=lo; selector_high=hi;
        events.push_back("indirect");
    }
    void DrawIndexIndirect(u32 o, Stage s, u32 lo, u32 hi, u32 f) {
        DrawIndirect(o,s,lo,hi,f); events.back()="index-indirect";
    }
};
template<class F> void Rejects(F f) {
    bool failed=false; try { f(); } catch (const std::exception&) { failed=true; }
    REQUIRE(failed);
}

void VertexVersionDifferences() {
    const auto stream=SnapshotStream(0x12345678000,1024,64);
    VertexElement e; e.format=kh3_gfx::VertexFormat(2); e.stride=32;
    e.offset=4; e.per_instance=1;
    auto d=VertexDescriptor(stream,e,3);
    REQUIRE((d==Descriptor{{0x456780a4,0x200123,30,0x2005f22c}}));
    e.stride=0;
    REQUIRE(VertexDescriptor(stream,e,3)[2]==8);
    REQUIRE(kh3_gfx::SnapshotStream(0x12345678000,1024,0,64).Records==960);
    e.stride=1; e.offset=0;
    d=VertexDescriptor(SnapshotStream(0x100000000ull,64,0),e,0x80000000);
    REQUIRE(d[0]==0x80000000 && (d[1]&0xfff)==0);
    u32 old[4]; const auto old_stream=kh3_gfx::SnapshotStream(0x100000000ull,64,1,0);
    kh3_gfx::VertexDescriptor(old_stream,0,2,1,true,0x80000000,old);
    REQUIRE((old[1]&0xfff)==1); // KH3 zero-extends the same 32-bit product
    REQUIRE(SnapshotStream(0,123,4,false).remaining_bytes==0);
    REQUIRE(SnapshotStream(0,123,4,true).address==4); // null object != zero backing
    REQUIRE(FormatBytes(15)==0x1fffffff && FormatBytes(61)==0);
}

void ActualBindingWordsAndSnapshots() {
    RecordingSink sink; Lcue l; Reflection r;
    r.SetWord(0,16); r.SetWord(0x1e,4); r.SetWord(0x9e,0x8003);
    r.SetWord(0xbe,0xffff); r.bytes[7]=2;
    l.stages[2].selected.reflection=&r;
    const u32 data[8]={1,2,3,4,5,6,7,8};
    l.stages[2].Bind(sink,Stage::Vertex,Binding::Sampler,0,data);
    REQUIRE((sink.packets[0]==std::vector<u32>{0xc0001000,0x6875000d,0xc0047600,0x4f,1,2,3,4}));
    l.stages[2].Bind(sink,Stage::Vertex,Binding::ReadTexture,0,data);
    l.stages[2].Bind(sink,Stage::Vertex,Binding::ConstantBuffer,0,nullptr); // unused map does not dereference
    REQUIRE(l.stages[2].selected.resources_dirty);
    CommitTable(sink,Stage::Vertex,l.stages[2],Extras{});
    REQUIRE((sink.packets[1]==std::vector<u32>{0xc0027600,0x4e,0x1000,0x1234}));
    l.stages[2].staging[4]=99;
    CommitTable(sink,Stage::Vertex,l.stages[2],Extras{});
    REQUIRE(sink.tables[0][4]==1 && sink.tables[1][4]==99);
    REQUIRE(sink.packets[2][2]==0x2000);
    r.SetWord(0x1e,0x4004); // bit14 is not the direct flag
    l.stages[2].Bind(sink,Stage::Vertex,Binding::ReadBuffer,0,data);
    REQUIRE(l.stages[2].staging[4]==1);
    Rejects([&]{l.stages[2].Bind(sink,Stage::Vertex,Binding::Sampler,16,data);});
}

void ReflectionSelectorOffsets() {
    RecordingSink sink; StageBindings s; Reflection r; s.selected.reflection=&r;
    r.SetWord(0,64);
    for(unsigned i=4;i<16;++i) r.bytes[i]=u8(i);
    for(unsigned i=0;i<6;++i) r.SetWord(0x12+2*i,u16(10+i));
    Extras extra; extra.shared_pointer=0x55; extra.fetch_shader=0x66;
    extra.scalar_d=17; extra.scalar_e=18; extra.factor_a=7; extra.factor_b=9;
    CommitTable(sink,Stage::Vertex,s,extra);
    REQUIRE(sink.packets.size()==12);
    const unsigned fields[]={7,8,9,10,11,12,13,14,4,5,6,15};
    for(unsigned i=0;i<12;++i) {
        const auto& p=sink.packets[i]; const bool scalar=fields[i]==13||fields[i]==14||fields[i]==15;
        REQUIRE(p[scalar?3:1]==0x4c+fields[i]);
    }
    REQUIRE(sink.packets[1][2]==0x1028); // selector8 -> WORD+12
    REQUIRE(sink.packets[2][2]==0x1030); // selector9 -> WORD+16 (not +14)
    REQUIRE(sink.packets[9][2]==0x102c); // selector5 -> WORD+14
    REQUIRE(sink.packets[10][2]==0x103c && sink.packets[11][4]==63);
    sink.packets.clear(); CommitTable(sink,Stage::Pixel,s,extra);
    REQUIRE(sink.packets.size()==8); // PS has no +4/+5/+6/+f emissions
}

void ShaderCommitAndLink() {
    RecordingSink sink; Lcue l; Program a,b,c,p; Reflection r;
    a.identity=1; b.identity=2; c.identity=3; p.identity=4;
    a.semantics={0x0507,0x0907}; b.semantics=a.semantics;
    p.semantics={0x1107,0x0308}; b.update_key[7]=9;
    l.SelectVS(a,r,0,0); l.SelectPS(&p,&r); l.prefetch=true;
    l.CommitOrdinary(sink);
    REQUIRE((sink.events==std::vector<std::string>{"set2","prefetch","set1","prefetch","table","table","inputs"}));
    REQUIRE((sink.packets.back()==std::vector<u32>{0x525,0x320}));
    l.SelectVS(b,r,0,0); REQUIRE(l.stages[2].selected.update); // VS compares only 3 DWORDs
    l.SelectVS(c,r,0,0); REQUIRE(!l.stages[2].selected.update); // double select
    l.CommitOrdinary(sink);
    l.SelectPS(nullptr,nullptr); sink.events.clear(); l.CommitOrdinary(sink);
    REQUIRE((sink.events==std::vector<std::string>{"set1null"}));
    l.SelectVS(a,r,0,0); sink.fail_upload=true;
    Rejects([&]{l.CommitOrdinary(sink);}); REQUIRE(l.stages[2].selected.resources_dirty);
    l.mode=0xb0; sink.events.clear(); Rejects([&]{l.CommitOrdinary(sink);}); REQUIRE(sink.events.empty());
    const std::vector<u16> v={0xc507,0x0907,0x0203};
    for(bool neo:{false,true}) for(unsigned x=0;x<65536;++x)
        REQUIRE(LinkPixelInput(u16(x),v,neo)==kh3_gfx::LinkPixelInput(u16(x),v.data(),v.size(),neo));
}

std::vector<u32> ResourceMap(unsigned ub,unsigned resource,unsigned slot) {
    std::vector<u32> map(20); map[ub]=20;
    map.push_back((ub<<24)|(resource<<8)|slot); map.push_back(0xff000000); return map;
}
void UniformResourceOrderAndStages() {
    RecordingSink sink; Lcue l; UniformBindings bindings; Reflection r;
    r.SetWord(0x9e,0x8000); r.SetWord(0x1e,0x8004); r.SetWord(0x20,0x800c); r.SetWord(0x7e,0x8008);
    Resource sampler,texture,srv,uav;
    sampler.words[0]=10; texture.kind=Resource::Kind::Texture; texture.words[0]=20;
    srv.kind=uav.kind=Resource::Kind::ShaderView;
    srv.view.kind=3; srv.view.words[0]=30; uav.view.kind=2; uav.view.words[0]=40;
    UniformBuffer ub; ub.resources={&sampler,&texture,&srv,&uav};
    ResourceMaps maps; maps.used_uniforms=1u<<2;
    maps.samplers=ResourceMap(2,0,0); maps.textures=ResourceMap(2,1,0);
    maps.srvs=ResourceMap(2,2,1); maps.uavs=ResourceMap(2,3,0);
    for (auto stage:{Stage::Pixel,Stage::Export,Stage::Geometry}) {
        const auto freq=stage==Stage::Pixel?Frequency::Pixel:stage==Stage::Export?Frequency::Vertex:Frequency::Geometry;
        l.stages[Index(stage)].selected.reflection=&r;
        bindings.Set(sink,l,freq,stage,2,ub); bindings.dirty[Index(freq)]|=1u<<19;
        sink.packets.clear(); bindings.Commit(sink,l,freq,stage,maps);
        REQUIRE(sink.packets.size()==(stage==Stage::Pixel?4:3));
        REQUIRE(sink.packets[0][4]==10 && sink.packets[1][4]==20 && sink.packets[2][4]==30);
        if(stage==Stage::Pixel) REQUIRE(sink.packets[3][4]==40);
        REQUIRE(bindings.dirty[Index(freq)]==0);
    }
    REQUIRE(sink.texture_touches==3);
    REQUIRE(HardwareStage(Frequency::Vertex,Stage::Export)==Stage::Export);
    Rejects([]{HardwareStage(Frequency::Hull,Stage::Vertex);});
}

void LooseConstantExtents() {
    RecordingSink sink; LooseConstants loose(128); Descriptor d;
    const u32 v=0x11223344;
    REQUIRE(!loose.Commit(sink,false,d));
    loose.Write(0x1003c,0x10004,&v); // native low16 truncation -> offset60,size4
    REQUIRE(loose.Commit(sink,false,d));
    REQUIRE(loose.previous_extent==64 && d[2]==4 && d[3]==0x20077fac && d[1]==0x100022);
    loose.Acknowledge(); loose.Write(0,4,&v); loose.Commit(sink,false,d);
    REQUIRE(sink.constants.back().size()==64); loose.Acknowledge();
    loose.Write(0,4,&v); loose.Commit(sink,true,d); REQUIRE(sink.constants.back().size()==16);
    REQUIRE(sink.constants.front()[60]==0x44 && sink.constants.front()[63]==0x11);
    Rejects([&]{loose.Write(127,4,&v);});
}

void ContextPipelineToDraw() {
    RecordingSink sink; CommandContext<RecordingSink> c(sink,128);
    Shader vs,ps; vs.program.identity=10; ps.program.identity=20; ps.frequency=Frequency::Pixel;
    vs.reflection.SetWord(0,8); vs.reflection.SetWord(0xe6,0); vs.reflection.SetWord(0xbe,4);
    vs.static_uniform_slots={255,2}; vs.static_uniform_hashes={0,0x12345678};
    UniformBuffer ub; c.static_uniforms.resize(3); c.static_uniforms[2]=&ub;
    Pipeline p; p.vertex=&vs; p.pixel=&ps; p.topology=1; p.loose_mask=1;
    VertexElement e; e.stride=16; e.binding=0; e.format=4; p.elements.push_back(e);
    c.RHISetGraphicsPipelineState(p,true);
    REQUIRE(sink.static_checks==1 && c.uniforms.dirty[0]==0xfffff && !c.loose[0].dirty);
    VertexBuffer vb; vb.current_address=0x1000; vb.size=128; c.RHISetStreamSource(0,&vb,16);
    vb.current_address=0x9000; // snapshots only on setter, not every draw
    const u32 value=7; c.RHISetShaderParameter(vs,99,0,4,&value);
    sink.events.clear(); c.RHIDrawPrimitive(3,4,2);
    REQUIRE(sink.count==6 && c.draw_state.base_vertex==0 && c.draw_state.instances==1);
    REQUIRE(sink.tables[0][0]==0x1010 && sink.tables[0][2]==7);
    REQUIRE(sink.tables[0][4]==0x1000 && sink.tables[0][6]==1);
    const auto shader=std::find(sink.events.begin(),sink.events.end(),"set2");
    const auto constants=std::find(sink.events.begin(),sink.events.end(),"constants");
    const auto draw=std::find(sink.events.begin(),sink.events.end(),"draw");
    REQUIRE(constants<shader && shader<draw && *(draw+1)=="instances:1" && *(draw+2)=="base:0");
    const auto uploads=sink.constants.size(); const auto tables=sink.tables.size();
    Pipeline same=p; same.fetch=0xfeed;
    c.RHISetGraphicsPipelineState(same,false); // same shader objects: no reselect
    REQUIRE(c.lcue.extras[2].fetch_shader==0 && !c.loose[0].dirty);
    c.RHIDrawPrimitive(0,0,1);
    REQUIRE(sink.count==2 && sink.constants.size()==uploads && sink.tables.size()==tables+1);
    c.RHISetShaderTexture(ps,0,nullptr); c.RHISetShaderResourceView(ps,0,nullptr);
    REQUIRE(!c.lcue.stages[1].selected.resources_dirty);
    Resource sampler;
    c.RHISetShaderSampler(ps,0,sampler);
    REQUIRE(c.lcue.stages[1].selected.resources_dirty); // even unused reflection
    IndexBuffer ib; ib.current_address=0x6000; ib.size=128; ib.stride=2;
    c.RHIDrawIndexedPrimitive(ib,0,0,16,3,4,1);
    REQUIRE(sink.count==6 && sink.address==0x6006);
    ArgumentBuffer args; args.current_address=0x7000;
    c.RHIDrawPrimitiveIndirect(args,28); REQUIRE(sink.offset==28);
    c.RHIDrawIndexedIndirect(ib,args,2); REQUIRE(sink.offset==40);
    c.RHIDrawIndexedPrimitiveIndirect(ib,args,24); REQUIRE(sink.offset==24);
    Pipeline gs=p; gs.geometry=&vs; Rejects([&]{c.RHISetGraphicsPipelineState(gs,false);});
}

struct DrawSink : RecordingSink {
    IndexBuffer* mutate=nullptr;
    void PrepareGraphics(u32 n) { events.push_back("prepare:"+std::to_string(n)); if(mutate) mutate->current_address=0x2000; }
    void CommitGraphics() { events.push_back("commit"); if(mutate) mutate->current_address=0x3000; }
};
void DirectAndIndirectDrawABI() {
    DrawSink sink; DrawState state; IndexBuffer ib; ib.current_address=0x1000; ib.type=1;
    sink.mutate=&ib;
    DrawIndexedPrimitive(sink,state,0,ib,7,4,999,0x40000001,2,3);
    REQUIRE(sink.address==0x2004 && sink.count==6); // uint32 wrap, backing after Prepare/before LCUE
    REQUIRE((sink.events==std::vector<std::string>{"prepare:4","instances:3","index:1","base:7","commit","draw-index","instances:1","base:0"}));
    sink.mutate=nullptr; sink.events.clear(); ArgumentBuffer args; args.current_address=0x9000;
    ib.size=72; ib.stride=4; state.instances=2;
    DrawIndexedIndirect(sink,state,ib,args,IndexedArgumentOffset(3),0xa3);
    REQUIRE(sink.count==18 && sink.offset==60 && sink.selector_low==3 && sink.selector_high==10);
    REQUIRE((sink.events==std::vector<std::string>{"prepare:0","set-base","index-base","index-size","commit","index-indirect","instances:1","instances:1"}));
    sink.events.clear(); DrawPrimitiveIndirect(sink,state,args,28,0x42);
    REQUIRE(sink.offset==28 && sink.address==0x9000 && sink.selector_low==2 && sink.selector_high==4);
    REQUIRE((sink.events==std::vector<std::string>{"prepare:0","set-base","commit","indirect","instances:1"}));
    REQUIRE(ElementCount(5,2)==6 && ElementCount(8,2)==6 && ElementCount(2,3)==6);
}

int main() {
    const std::pair<const char*,std::function<void()>> cases[]={
        {"vertex version differences",VertexVersionDifferences},
        {"actual binding words and snapshots",ActualBindingWordsAndSnapshots},
        {"reflection selector offsets",ReflectionSelectorOffsets},
        {"shader commit and linker",ShaderCommitAndLink},
        {"UB resource order and stages",UniformResourceOrderAndStages},
        {"loose constant extents",LooseConstantExtents},
        {"pipeline to draw",ContextPipelineToDraw},
        {"direct and indirect ABI",DirectAndIndirectDrawABI}};
    unsigned passed=0;
    for(const auto& c:cases) {
        try { c.second(); ++passed; std::cout<<"PASS "<<c.first<<'\n'; }
        catch(const std::exception& e) { std::cerr<<"FAIL "<<c.first<<": "<<e.what()<<'\n'; return 1; }
    }
    std::cout<<passed<<"/"<<sizeof(cases)/sizeof(cases[0])<<" scenarios passed\n";
}
