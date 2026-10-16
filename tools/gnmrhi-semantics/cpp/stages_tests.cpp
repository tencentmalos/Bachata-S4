#include "../ue4-reference/GnmStagesCore.h"
#include <vector>
#include <string>
#include <iostream>
#include <stdexcept>
#define REQUIRE(X) do { if (!(X)) throw std::runtime_error(#X); } while (false)
using namespace kh3_gfx;
struct Sink
{
    std::vector<std::vector<uint32_t>> Packets, Uploads;
    std::vector<std::string> Events;
    uint32_t HullBody[7] = {}, HullConfig = 0; bool HullUpdate = false;
    void Emit(const uint32_t* P, uint32_t N)
    { Packets.emplace_back(P,P+N); Events.emplace_back("packet"); }
    uint64_t UploadTable(const uint32_t* P, uint32_t N)
    { Uploads.emplace_back(P,P+N); Events.emplace_back("upload"); return 0x12300001000ull+Uploads.size()*0x1000; }
    void SetLs(uint64_t Program, uint32_t Modifier)
    { REQUIRE(Program == 0x6000 && Modifier == 7); Events.emplace_back("LS"); }
    void SetHs(uint64_t Program,const uint32_t Body[7],uint32_t Config,bool Update)
    {
        REQUIRE(Program == 0x5000); Events.emplace_back("HS");
        for(int I=0; I<7; ++I) HullBody[I]=Body[I]; HullConfig=Config; HullUpdate=Update;
    }
};
void CopyProfiles()
{
    REQUIRE(CopyShaderAddress(0x1000,3)==0x1044);
    for (int On=0; On<2; ++On) for (int Compact=0; Compact<2; ++Compact)
    {
        auto R=CopyReflection(On!=0,Compact!=0);
        REQUIRE(R.TableDwords==(Compact?0:64));
        REQUIRE(R.Selector[On?15:12]==0 && R.Selector[On?12:15]==255);
        REQUIRE(R.Selector[6]==(Compact?255:2));
        Sink S; uint32_t Staging[64]={}; Extras E; E.SharedPointer=0x1234; E.ScalarF=91;
        CommitTable(S,2,R,E,Staging,64);
        REQUIRE(S.Packets.size()==(Compact?1:2));
        REQUIRE(On ? S.Packets.back()[4]==91 : S.Packets[0][2]==0x1234);
    }
}
void OnChipLimits()
{
    GeometryMetadata G; G.Word34=1; G.ItemDwords[0]=1;
    auto B=OnChipBudget(G); REQUIRE(B.Groups==64 && B.Lds512Units==1);
    G.Word34|=31<<11; B=OnChipBudget(G); REQUIRE(B.Groups==63 && B.Lds512Units==1);
    G.Word34=4|(2<<11)|(8<<16); G.ItemDwords[0]=16;
    B=OnChipBudget(G); REQUIRE(B.Groups==64 && B.Lds512Units==44);
    G.Word34=2047; G.ItemDwords[0]=9; B=OnChipBudget(G); REQUIRE(!B.Groups && !B.Lds512Units);
    G.Word34=0; G.ItemDwords[0]=0; REQUIRE(!OnChipBudget(G).Groups);
    REQUIRE(PatchEsResource2(0xffffffff,0)==0xffffffff);
    REQUIRE(PatchEsResource2(0xffffffff,7)==0xe07fffff);
}
void CutsAndModes()
{
    GeometryMetadata G; G.Word34=1;
    G.ItemDwords[0]=128; REQUIRE(GeometryCut(G)==3);
    G.ItemDwords[0]=129; REQUIRE(GeometryCut(G)==2);
    G.ItemDwords[0]=257; REQUIRE(GeometryCut(G)==1);
    G.ItemDwords[0]=513; REQUIRE(GeometryCut(G)==0);
    Sink B; GeometryMode M; G.CopyFlags=0x22;
    M.Select(B,G,8); REQUIRE(B.Packets.size()==3);
    REQUIRE((B.Packets[0]==std::vector<uint32_t>{0xc0016900,0x291,0x4008}));
    REQUIRE(B.Packets[1][2]==0x600023 && B.Packets[2][2]==0xb0);
    M.Select(B,G,8); REQUIRE(B.Packets.size()==3);
    M.Select(B,G,9); REQUIRE(B.Packets[3].size()==6 && B.Packets[3][4]==0x2e5);
    M.Disable(B); REQUIRE(!M.Mode && M.OnChipKey==~0u && B.Packets.back()[2]==0);
    const auto N=B.Packets.size(); M.Disable(B); REQUIRE(B.Packets.size()==N);
}
void RingLayout()
{
    GeometryMetadata G; G.Word34=3;
    for (int I=0; I<4; ++I) G.ItemDwords[I]=I+1;
    uint32_t T[48]; for(auto& W:T) W=0xdeadbeef;
    GeometryRingDescriptors(T,0x23400000000ull,0x123fffffe00ull,G);
    REQUIRE(T[0]==0xdeadbeef && T[36]==0xdeadbeef && T[47]==0xdeadbeef);
    REQUIRE(T[8]==0 && T[9]==0x80003234 && T[10]==0x400000 && T[11]==0xea7204);
    REQUIRE(T[16]==0xfffffe00 && T[17]==0x800c3123 && T[18]==64);
    // Stream1 crosses 32-bit address boundary; high base must carry.
    REQUIRE(T[20]==0x100 && T[21]==0x80183124);
    REQUIRE(T[24]==0x700 && T[28]==0x1000);
    REQUIRE(T[32]==0xfffffe00 && T[33]==0x3123 && T[35]==0x27204);
    RingLayoutKey K; REQUIRE(!K.Matches(16,G)); K.Remember(16,G); REQUIRE(K.Matches(16,G));
    REQUIRE(!K.Matches(17,G)); G.ItemDwords[2]++; REQUIRE(!K.Matches(16,G));
}
void RingRegisters()
{
    GeometryMetadata G; G.Word34=3;
    for(int I=0; I<4; ++I) G.ItemDwords[I]=I+1;
    Sink B; GeometryRingRegisters(B,G);
    REQUIRE((B.Packets[0]==std::vector<uint32_t>{0xc0046900,0x2d7,1,2,3,4}));
    REQUIRE((B.Packets[1]==std::vector<uint32_t>{0xc0036900,0x298,3,9,18}));
    REQUIRE(B.Packets[2][1]==0x2ce && B.Packets[3][2]==30);
}
void StageSelectors()
{
    uint32_t Data[32]={}; Reflection R; R.TableDwords=32;
    for(int I=4; I<16; ++I) R.Selector[I]=uint8_t(I);
    const size_t Counts[]={0,8,12,9,11,8,11};
    for(uint32_t Stage=1; Stage<7; ++Stage)
    {
        Sink B; CommitTable(B,Stage,R,Extras{},Data,32); REQUIRE(B.Packets.size()==Counts[Stage]);
        if(Stage==3) REQUIRE(B.Packets.back()[3]==0x9b); // GS +f
        if(Stage==4) REQUIRE(B.Packets.back()[1]==0xd2); // ES +6, no +f
        if(Stage==6) REQUIRE(B.Packets.back()[1]==0x152);
    }
}
TessellationMetadata TessInput()
{
    TessellationMetadata T; T.LsWord1c=16; T.HsWord24=3; T.HsWord28=4;
    T.HsWord2c=2; T.HsWord30=32; T.HsWord34=6; T.HsWord3c=99; T.PatchGroupCount=8;
    T.HsOverride=int32_t(0x80000002); return T;
}
void TessConstants()
{
    auto T=TessInput(); uint32_t W[9]; TessellationConstants(T,W);
    REQUIRE((std::vector<uint32_t>(W,W+9)==std::vector<uint32_t>{16,32,8,384,32,1408,128,0x44000000,99}));
    T.HsWord28=0; TessellationConstants(T,W); REQUIRE(W[1]==16 && W[3]==0 && W[5]==384 && W[6]==0);
    T.HsWord28=4; T.HsWord30=0; TessellationConstants(T,W); REQUIRE(W[1]==16 && W[3]==0 && W[6]==64);
    REQUIRE(PatchHsTfParam(0xffffffff,0)==0xffffffff);
    REQUIRE(PatchHsTfParam(0xffffffff,int32_t(0x80000002))==0xfffdffff);
    REQUIRE(DomainHardwareStage(1)==2 && DomainHardwareStage(0xb1)==4);
}
void TessSubmission()
{
    Sink B; StageView LS,HS,DS; TessellationCache C;
    uint32_t L[32]={},H[32]={},D[32]={},Body[7]={1,2,3,4,0xffffffff,6,7};
    LS.Program=0x6000; LS.Modifier=7; HS.Program=0x5000;
    LS.Staging=L; HS.Staging=H; DS.Staging=D;
    LS.StagingCount=HS.StagingCount=DS.StagingCount=32;
    LS.Ref.TableDwords=HS.Ref.TableDwords=32;
    LS.State.ShaderDirty=HS.State.ShaderDirty=true;
    LS.State.ResourcesDirty=true; HS.State.UpdateAllowed=true;
    HS.TessellationSlot=4; DS.TessellationSlot=0x8002;
    CommitLsHs(B,1,TessInput(),LS,HS,DS,C,Body);
    REQUIRE(C.Valid && C.Descriptor[2]==3 && C.Descriptor[3]==0x20077fac);
    REQUIRE(H[4]==0x2000 && H[5]==0x100123 && H[6]==3);
    REQUIRE((B.Events==std::vector<std::string>{"upload","packet","LS","HS","upload","upload"}));
    REQUIRE(B.Packets[0][3]==0x4e && B.HullConfig==0x18308 && B.HullUpdate);
    REQUIRE(B.HullBody[4]==0xfffdffff && Body[4]==0xffffffff);
    REQUIRE(!LS.State.Emitted && HS.State.Emitted && DS.State.ResourcesDirty);
    B.Events.clear(); const auto N=B.Uploads.size();
    CommitLsHs(B,0xb1,TessInput(),LS,HS,DS,C,Body);
    REQUIRE(B.Uploads.size()==N+1 && B.Uploads.back().size()==32); // cached 9-DWORD constants reused
    REQUIRE(B.Packets.back()[3]==0xce); // combined mode binds domain at ES, retains dirty
    REQUIRE(DS.State.ResourcesDirty && B.Events[0]=="packet");
}
int main()
{
    try {
        CopyProfiles(); OnChipLimits(); CutsAndModes(); RingLayout(); RingRegisters();
        StageSelectors(); TessConstants(); TessSubmission();
        std::cout<<"PASS: 8 GS/ES/LS/HS scenarios (actual C++14 reference core)\n";
    } catch(const std::exception& E) { std::cerr<<E.what()<<'\n'; return 1; }
}
