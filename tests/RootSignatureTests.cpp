// RTSky - host tests of root-signature register resolution (src/Track/RootSignatureTracker.cpp):
// root descriptors, descriptor tables with explicit and APPEND offsets, register spaces, visibility.
#include "Track/RootSignatureTracker.h"

#include <cstdio>

using namespace rtsky::track;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        ++g_failures;
}

D3D12_DESCRIPTOR_RANGE1 Range(D3D12_DESCRIPTOR_RANGE_TYPE type, UINT count, UINT base, UINT space, UINT offset)
{
    D3D12_DESCRIPTOR_RANGE1 r = {};
    r.RangeType = type;
    r.NumDescriptors = count;
    r.BaseShaderRegister = base;
    r.RegisterSpace = space;
    r.OffsetInDescriptorsFromTableStart = offset;
    return r;
}

} // namespace

int main()
{
    std::printf("root signature register resolution (a layout shaped like PS_directional_standard's)\n");

    // p0 root CBV b4 | p1 root CBV b5 | p2 table: CBV b10,s1 (APPEND after t0-t31) | p3 table: SRV t0-t31
    // then SRV t10-t29,s1 (APPEND) | p4 root CBV b6 vertex-only | p5 constants b0 x4
    D3D12_DESCRIPTOR_RANGE1 cbRanges[] = {
        Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 2, 0),
        Range(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 10, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND),
    };
    D3D12_DESCRIPTOR_RANGE1 srvRanges[] = {
        Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, 0),
        Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 20, 10, 1, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND),
        Range(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 5, 100),
    };
    D3D12_ROOT_PARAMETER1 params[6] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 4;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 5;
    params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[2].DescriptorTable.NumDescriptorRanges = 2;
    params[2].DescriptorTable.pDescriptorRanges = cbRanges;
    params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[3].DescriptorTable.NumDescriptorRanges = 3;
    params[3].DescriptorTable.pDescriptorRanges = srvRanges;
    params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[4].Descriptor.ShaderRegister = 6;
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    params[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[5].Constants.ShaderRegister = 0;
    params[5].Constants.Num32BitValues = 4;

    D3D12_VERSIONED_ROOT_SIGNATURE_DESC desc = {};
    desc.Version = D3D_ROOT_SIGNATURE_VERSION_1_1;
    desc.Desc_1_1.NumParameters = 6;
    desc.Desc_1_1.pParameters = params;
    const RootLayout layout = LayoutFromDesc(desc);
    Check(layout.params.size() == 6, "six parameters");

    RootLocation loc;
    Check(ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 5, 0, &loc) && loc.param == 1 && loc.descriptorOffset == -1,
          "cb5: root CBV p1");
    Check(ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 10, 1, &loc) && loc.param == 2 && loc.descriptorOffset == 32,
          "cb10,space1: table p2 at 32 (APPEND after 32 SRVs)");
    Check(ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 15, 0, &loc) && loc.param == 3 && loc.descriptorOffset == 15,
          "t15: table p3 at 15");
    Check(ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 24, 1, &loc) && loc.param == 3 && loc.descriptorOffset == 32 + 14,
          "t24,space1: table p3 at 46 (APPEND, register 24 of the range starting at 10)");
    Check(ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 7, 5, &loc) && loc.param == 3 && loc.descriptorOffset == 107,
          "t7,space5: unbounded range at an explicit offset (100 + 7)");
    Check(!ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 30, 1, &loc), "t30,space1: past the range -> not found");
    Check(!ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 6, 0, &loc), "cb6: vertex-only parameter is not the pixel shader's");
    Check(!ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, &loc) || loc.param == 3,
          "t4 is not the root CBV b4 (kinds are kept apart)");
    Check(ResolveRegister(layout, D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 0, 2, &loc) && loc.param == 2 && loc.descriptorOffset == 0,
          "t0,space2 in the CBV table's first range");
    std::printf("  layout: %s\n", DescribeLayout(layout).c_str());

    std::printf(g_failures == 0 ? "\nall root signature tests passed\n" : "\n%d root signature test(s) FAILED\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
