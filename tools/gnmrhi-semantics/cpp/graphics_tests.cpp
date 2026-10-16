#include "../ue4-reference/GnmGraphicsCore.h"
#include <vector>
#include <iostream>
#include <stdexcept>
#define REQUIRE(X) do { if (!(X)) throw std::runtime_error(#X); } while (false)
using namespace kh3_gfx;

struct Sink
{
    std::vector<std::vector<uint32_t>> Packets, Uploads;
    void Emit(const uint32_t* P, uint32_t N) { Packets.emplace_back(P, P + N); }
    uint64_t UploadTable(const uint32_t* P, uint32_t N)
    {
        Uploads.emplace_back(P, P + N);
        return 0x123400000000ull + 0x1000 * Uploads.size();
    }
};

void StreamAndFetch()
{
    const Stream S = SnapshotStream(0x12345678000, 1024, 32, 64);
    REQUIRE(S.Base == 0x12345678040 && S.Records == 30);
    uint32_t D[4];
    VertexDescriptor(S, 4, 2, 16, true, 3, D);
    REQUIRE(D[0] == 0x45678074 && D[1] == 0x200123 && D[2] == 30);
    REQUIRE(D[3] == 0x2005f22c); // Type 2: Float2, packed table conversion
    VertexDescriptor(S, 4, 2, 16, false, 999, D);
    REQUIRE(D[0] == 0x45678044);
    const Stream Null = SnapshotStream(0, 0, 16, 99);
    REQUIRE(!Null.Base && !Null.Records && Null.Stride == 16);
    REQUIRE(SnapshotStream(100, 123, 0, 3).Records == 120);
}

void SemanticLink()
{
    const uint16_t VS[] = {0x0507, 0x0907}; // first match wins
    REQUIRE(LinkPixelInput(0x1107, VS, 2, false) == 0x525);
    REQUIRE(LinkPixelInput(0x0308, VS, 2, false) == 0x320);
    REQUIRE(LinkPixelInput(0x4007, VS, 2, true) == 0x1080025);
    REQUIRE(LinkPixelInput(0x8007, VS, 2, true) == 0x2080025);
    REQUIRE(LinkPixelInput(0xc307, VS, 2, true) == 0x3180325);
    REQUIRE(LinkPixelInput(0x4008, VS, 2, true) == 0x1080020);
    const uint16_t BothModes[] = {0xc507};
    REQUIRE(LinkPixelInput(0xc007, BothModes, 1, true) == 0x3080005);
}

void ShaderSelection()
{
    uint32_t A[8] = {}, B[8] = {}; B[7] = 9;
    Selection VS, PS;
    VS.Select(0, 100, nullptr, A, 3);
    PS.Select(0, 100, nullptr, A, 8);
    REQUIRE(VS.ShaderDirty && !VS.UpdateAllowed);
    VS.Acknowledge(); PS.Acknowledge();
    VS.Select(100, 200, A, B, 3); PS.Select(100, 200, A, B, 8);
    REQUIRE(VS.UpdateAllowed && !PS.UpdateAllowed); // same VS key, different PS key
    VS.Select(200, 300, B, B, 3);
    REQUIRE(!VS.UpdateAllowed && VS.ShaderDirty); // no emit between selections
    PS.Acknowledge(); PS.Select(200, 0, B, nullptr, 8);
    REQUIRE(PS.ShaderDirty && !PS.ResourcesDirty && !PS.UpdateAllowed);
}

void BindingAndSnapshots()
{
    Sink B; Selection State;
    uint32_t Staging[16] = {}, D[4] = {1,2,3,4};
    Bind(B, 2, 0xffff, D, 4, Staging, 16, State);
    REQUIRE(State.ResourcesDirty && B.Packets.empty());
    Bind(B, 2, 0x8003, D, 4, Staging, 16, State);
    REQUIRE((B.Packets[0] == std::vector<uint32_t>{0xc0001000,0x6875000d,0xc0047600,0x4f,1,2,3,4}));
    Bind(B, 2, 4, D, 4, Staging, 16, State);
    Reflection R; R.TableDwords = 16; R.Selector[7] = 2;
    CommitTable(B, 2, R, Extras{}, Staging, 16);
    Staging[4] = 99;
    CommitTable(B, 2, R, Extras{}, Staging, 16);
    REQUIRE(B.Uploads[0][4] == 1 && B.Uploads[1][4] == 99);
    REQUIRE(B.Packets[1][2] == 0x1000 && B.Packets[2][2] == 0x2000);
    REQUIRE(B.Packets[1][3] == 0x1234);
}

void LcueSelectors()
{
    Sink B; Reflection R; Extras E; uint32_t Staging[32] = {};
    R.TableDwords = 32;
    for (int I = 4; I < 16; ++I) R.Selector[I] = uint8_t(I);
    R.Offset16 = 2; R.Offset18 = 3; R.Offset1a = 4;
    R.Offset1c = 5; R.Offset1e = 6; R.Offset20 = 7;
    E.SharedPointer = 0xaabbccddeeff0011; E.FetchShader = 0x1122334455667788;
    E.ScalarD = 16; E.ScalarE = 17; E.ScalarF = 18;
    CommitTable(B, 2, R, E, Staging, 32);
    REQUIRE(B.Packets.size() == 12);
    REQUIRE(B.Packets[1][2] == 0x1008);
    REQUIRE(B.Packets[5][2] == 0xeeff0011 && B.Packets[5][3] == 0xaabbccdd);
    REQUIRE((B.Packets[6] == std::vector<uint32_t>{0xc0001000,0x6875000d,0xc0017600,0x59,16}));
    REQUIRE(B.Packets[8][1] == 0x50 && B.Packets[8][2] == 0x55667788);
    REQUIRE(B.Packets[9][2] == 0x100c && B.Packets[10][2] == 0x101c);
    REQUIRE(B.Packets[11][4] == 18);
    B.Packets.clear(); CommitTable(B, 1, R, E, Staging, 32);
    REQUIRE(B.Packets.size() == 8 && B.Packets[0][1] == 0x13); // PS ignores +4/+5/+6/+f
}

void DrawTransitions()
{
    Sink B; DrawState D;
    uint32_t Topology, Count;
    REQUIRE(DrawShape(0, 5, Topology, Count) && Count == 15);
    D.SetTopology(B, Topology, true); D.SetBase(B, uint32_t(-2));
    D.SetInstances(B, 3); D.SetIndexType(B, 0);
    REQUIRE((B.Packets[0] == std::vector<uint32_t>{0xc0017900,0x10000242,4}));
    REQUIRE(B.Packets[1][2] == 0xfffffffe);
    D.AfterDraw(B);
    REQUIRE(B.Packets[4][0] == 0xc0002f00 && B.Packets[4][1] == 1);
    REQUIRE(B.Packets[5][1] == 0x8102 && !B.Packets[5][2]);
    D.SetTopology(B, 4, true); D.AfterDraw(B); D.SetIndexType(B, 0);
    REQUIRE(B.Packets.size() == 6); // no redundant topology/reset/index packets
    REQUIRE(DrawShape(1, 0, Topology, Count) && Count == 2); // raw count, not guessed skip
    REQUIRE(!DrawShape(3, 1, Topology, Count) && !DrawShape(4, 1, Topology, Count));
}

int main()
{
    try {
        StreamAndFetch(); SemanticLink(); ShaderSelection(); BindingAndSnapshots();
        LcueSelectors(); DrawTransitions();
        std::cout << "PASS: 6 graphics scenarios (shared C++14 UE-reference core)\n";
    } catch (const std::exception& E) { std::cerr << E.what() << '\n'; return 1; }
}
