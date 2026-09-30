#pragma once

#include <cstdint>

static const uint64_t kLighthouseShaderPrewarmList[][2] = {
    { 0x1080108ULL, 0xfffffffffffe0001ULL },          { 0x1080108ULL, 0xfffffffffffe0021ULL },
    { 0x1082821ULL, 0xfffffffffffe0001ULL },          { 0x1088000ULL, 0xfffffffffffe0001ULL },
    { 0x10001000ULL, 0xfffffffffffe0000ULL },         { 0x10001000ULL, 0xfffffffffffe0001ULL },
    { 0x80008000ULL, 0xfffffffffffe0001ULL },         { 0x20d020d0108818aULL, 0xfffffffffffe0010ULL },
    { 0x20d020d0108818aULL, 0xfffffffffffe0011ULL },  { 0x20d020d0108818aULL, 0xfffffffffffe0031ULL },
    { 0x20d030d01081218ULL, 0xfffffffffffe0010ULL },  { 0x20d030d01081218ULL, 0xfffffffffffe0011ULL },
    { 0x20d030d01081218ULL, 0xfffffffffffe0031ULL },  { 0x20d030d1000121cULL, 0xfffffffffffe0010ULL },
    { 0x20d030d1000121cULL, 0xfffffffffffe0011ULL },  { 0x20dd00001088000ULL, 0xfffffffffffe0010ULL },
    { 0x20dd00001088000ULL, 0xfffffffffffe0011ULL },  { 0xc000d00000002108ULL, 0xfffffffffffe0012ULL },
    { 0xd000020d80000108ULL, 0xfffffffffffe0017ULL }, { 0xd000020dc0000108ULL, 0xfffffffffffe0012ULL },
    { 0xd000020dc0001000ULL, 0xfffffffffffe0012ULL }, { 0xd000d0000a088000ULL, 0xfffffffffffe0011ULL },
    { 0xd000d0000a088000ULL, 0xfffffffffffe0811ULL }, { 0xd000d00010001000ULL, 0xfffffffffffe0011ULL },
};
