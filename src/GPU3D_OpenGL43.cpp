/*
    Copyright 2016-2019 Arisotura

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdio.h>
#include <string.h>
#include "NDS.h"
#include "GPU.h"
#include "OpenGLSupport.h"

namespace GPU3D
{
namespace GLRenderer43
{

// GL version requirements
// * explicit uniform location: 4.3 (or extension)
// * texelFetch: 3.0 (GLSL 1.30)     (3.2/1.50 for MS)
// * UBO: 3.1
// * glMemoryBarrier: 4.2

// TODO: consider other way to handle uniforms (UBO?)

#define kShaderHeader "#version 430"


const char* kClearVS = kShaderHeader R"(

layout(location=0) in vec2 vPosition;

layout(location=1) uniform uint uDepth;

void main()
{
    float fdepth = (float(uDepth) / 8388608.0) - 1.0;
    gl_Position = vec4(vPosition, fdepth, 1.0);
}
)";

const char* kClearFS = kShaderHeader R"(

layout(location=0) uniform uvec4 uColor;
layout(location=2) uniform uint uOpaquePolyID;
layout(location=3) uniform uint uFogFlag;

layout(location=0) out vec4 oColor;
layout(location=1) out uvec3 oAttr;

void main()
{
    oColor = vec4(uColor).bgra / 31.0;
    oAttr.r = 0;
    oAttr.g = uOpaquePolyID;
    oAttr.b = 0;
}
)";


const char* kRenderVSCommon = R"(

layout(std140, binding=0) uniform uConfig
{
    vec2 uScreenSize;
    uint uDispCnt;
    vec4 uToonColors[32];
};

layout(location=0) in uvec4 vPosition;
layout(location=1) in uvec4 vColor;
layout(location=2) in ivec2 vTexcoord;
layout(location=3) in uvec3 vPolygonAttr;

smooth out vec4 fColor;
smooth out vec2 fTexcoord;
flat out uvec3 fPolygonAttr;
)";

const char* kRenderFSCommon = R"(

layout(binding=0) uniform usampler2D TexMem;
layout(binding=1) uniform sampler2D TexPalMem;

layout(std140, binding=0) uniform uConfig
{
    vec2 uScreenSize;
    uint uDispCnt;
    vec4 uToonColors[32];
};

smooth in vec4 fColor;
smooth in vec2 fTexcoord;
flat in uvec3 fPolygonAttr;

layout(location=0) out vec4 oColor;
layout(location=1) out uvec3 oAttr;

int TexcoordWrap(int c, int maxc, uint mode)
{
    if ((mode & (1<<0)) != 0)
    {
        if ((mode & (1<<2)) != 0 && (c & maxc) != 0)
            return (maxc-1) - (c & (maxc-1));
        else
            return (c & (maxc-1));
    }
    else
        return clamp(c, 0, maxc-1);
}

vec4 TextureFetch_A3I5(ivec2 addr, ivec4 st, uint wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x);
    uvec4 pixel = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);

    pixel.a = (pixel.r & 0xE0);
    pixel.a = (pixel.a >> 3) + (pixel.a >> 6);
    pixel.r &= 0x1F;

    addr.y = (addr.y << 3) + int(pixel.r);
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, float(pixel.a)/31.0);
}

vec4 TextureFetch_I2(ivec2 addr, ivec4 st, uint wrapmode, float alpha0)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x) >> 2;
    uvec4 pixel = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);
    pixel.r >>= (2 * (st.x & 3));
    pixel.r &= 0x03;

    addr.y = (addr.y << 2) + int(pixel.r);
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, max(step(1,pixel.r),alpha0));
}

vec4 TextureFetch_I4(ivec2 addr, ivec4 st, uint wrapmode, float alpha0)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x) >> 1;
    uvec4 pixel = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);
    if ((st.x & 1) != 0) pixel.r >>= 4;
    else                 pixel.r &= 0x0F;

    addr.y = (addr.y << 3) + int(pixel.r);
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, max(step(1,pixel.r),alpha0));
}

vec4 TextureFetch_I8(ivec2 addr, ivec4 st, uint wrapmode, float alpha0)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x);
    uvec4 pixel = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);

    addr.y = (addr.y << 3) + int(pixel.r);
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, max(step(1,pixel.r),alpha0));
}

vec4 TextureFetch_Compressed(ivec2 addr, ivec4 st, uint wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y & 0x3FC) * (st.z>>2)) + (st.x & 0x3FC) + (st.y & 0x3);
    uvec4 p = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);
    uint val = (p.r >> (2 * (st.x & 0x3))) & 0x3;

    int slot1addr = 0x20000 + ((addr.x & 0x1FFFC) >> 1);
    if (addr.x >= 0x40000) slot1addr += 0x10000;

    uint palinfo;
    p = texelFetch(TexMem, ivec2(slot1addr&0x3FF, slot1addr>>10), 0);
    palinfo = p.r;
    slot1addr++;
    p = texelFetch(TexMem, ivec2(slot1addr&0x3FF, slot1addr>>10), 0);
    palinfo |= (p.r << 8);

    addr.y = (addr.y << 3) + ((int(palinfo) & 0x3FFF) << 1);
    palinfo >>= 14;

    if (val == 0)
    {
        vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
        return vec4(color.rgb, 1.0);
    }
    else if (val == 1)
    {
        addr.y++;
        vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
        return vec4(color.rgb, 1.0);
    }
    else if (val == 2)
    {
        if (palinfo == 1)
        {
            vec4 color0 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            addr.y++;
            vec4 color1 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4((color0.rgb + color1.rgb) / 2.0, 1.0);
        }
        else if (palinfo == 3)
        {
            vec4 color0 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            addr.y++;
            vec4 color1 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4((color0.rgb*5.0 + color1.rgb*3.0) / 8.0, 1.0);
        }
        else
        {
            addr.y += 2;
            vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4(color.rgb, 1.0);
        }
    }
    else
    {
        if (palinfo == 2)
        {
            addr.y += 3;
            vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4(color.rgb, 1.0);
        }
        else if (palinfo == 3)
        {
            vec4 color0 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            addr.y++;
            vec4 color1 = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);
            return vec4((color0.rgb*3.0 + color1.rgb*5.0) / 8.0, 1.0);
        }
        else
        {
            return vec4(0.0);
        }
    }
}

vec4 TextureFetch_A5I3(ivec2 addr, ivec4 st, uint wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x);
    uvec4 pixel = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);

    pixel.a = (pixel.r & 0xF8) >> 3;
    pixel.r &= 0x07;

    addr.y = (addr.y << 3) + int(pixel.r);
    vec4 color = texelFetch(TexPalMem, ivec2(addr.y&0x3FF, addr.y>>10), 0);

    return vec4(color.rgb, float(pixel.a)/31.0);
}

vec4 TextureFetch_Direct(ivec2 addr, ivec4 st, uint wrapmode)
{
    st.x = TexcoordWrap(st.x, st.z, wrapmode>>0);
    st.y = TexcoordWrap(st.y, st.w, wrapmode>>1);

    addr.x += ((st.y * st.z) + st.x) << 1;
    uvec4 pixelL = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);
    addr.x++;
    uvec4 pixelH = texelFetch(TexMem, ivec2(addr.x&0x3FF, addr.x>>10), 0);

    vec4 color;
    color.r = float(pixelL.r & 0x1F) / 31.0;
    color.g = float((pixelL.r >> 5) | ((pixelH.r & 0x03) << 3)) / 31.0;
    color.b = float((pixelH.r & 0x7C) >> 2) / 31.0;
    color.a = float(pixelH.r >> 7);

    return color;
}

vec4 TextureLookup_Nearest(vec2 st)
{
    uint attr = fPolygonAttr.y;
    uint paladdr = fPolygonAttr.z;

    float alpha0;
    if ((attr & (1<<29)) != 0) alpha0 = 0.0;
    else                       alpha0 = 1.0;

    int tw = 8 << int((attr >> 20) & 0x7);
    int th = 8 << int((attr >> 23) & 0x7);
    ivec4 st_full = ivec4(ivec2(st), tw, th);

    ivec2 vramaddr = ivec2(int(attr & 0xFFFF) << 3, int(paladdr));
    uint wrapmode = attr >> 16;

    uint type = (attr >> 26) & 0x7;
    if      (type == 5) return TextureFetch_Compressed(vramaddr, st_full, wrapmode);
    else if (type == 2) return TextureFetch_I2        (vramaddr, st_full, wrapmode, alpha0);
    else if (type == 3) return TextureFetch_I4        (vramaddr, st_full, wrapmode, alpha0);
    else if (type == 4) return TextureFetch_I8        (vramaddr, st_full, wrapmode, alpha0);
    else if (type == 1) return TextureFetch_A3I5      (vramaddr, st_full, wrapmode);
    else if (type == 6) return TextureFetch_A5I3      (vramaddr, st_full, wrapmode);
    else                return TextureFetch_Direct    (vramaddr, st_full, wrapmode);
}

vec4 TextureLookup_Linear(vec2 texcoord)
{
    ivec2 intpart = ivec2(texcoord);
    vec2 fracpart = fract(texcoord);

    uint attr = fPolygonAttr.y;
    uint paladdr = fPolygonAttr.z;

    float alpha0;
    if ((attr & (1<<29)) != 0) alpha0 = 0.0;
    else                       alpha0 = 1.0;

    int tw = 8 << int((attr >> 20) & 0x7);
    int th = 8 << int((attr >> 23) & 0x7);
    ivec4 st_full = ivec4(intpart, tw, th);

    ivec2 vramaddr = ivec2(int(attr & 0xFFFF) << 3, int(paladdr));
    uint wrapmode = attr >> 16;

    vec4 A, B, C, D;
    uint type = (attr >> 26) & 0x7;
    if (type == 5)
    {
        A = TextureFetch_Compressed(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_Compressed(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_Compressed(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_Compressed(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }
    else if (type == 2)
    {
        A = TextureFetch_I2(vramaddr, st_full                 , wrapmode, alpha0);
        B = TextureFetch_I2(vramaddr, st_full + ivec4(1,0,0,0), wrapmode, alpha0);
        C = TextureFetch_I2(vramaddr, st_full + ivec4(0,1,0,0), wrapmode, alpha0);
        D = TextureFetch_I2(vramaddr, st_full + ivec4(1,1,0,0), wrapmode, alpha0);
    }
    else if (type == 3)
    {
        A = TextureFetch_I4(vramaddr, st_full                 , wrapmode, alpha0);
        B = TextureFetch_I4(vramaddr, st_full + ivec4(1,0,0,0), wrapmode, alpha0);
        C = TextureFetch_I4(vramaddr, st_full + ivec4(0,1,0,0), wrapmode, alpha0);
        D = TextureFetch_I4(vramaddr, st_full + ivec4(1,1,0,0), wrapmode, alpha0);
    }
    else if (type == 4)
    {
        A = TextureFetch_I8(vramaddr, st_full                 , wrapmode, alpha0);
        B = TextureFetch_I8(vramaddr, st_full + ivec4(1,0,0,0), wrapmode, alpha0);
        C = TextureFetch_I8(vramaddr, st_full + ivec4(0,1,0,0), wrapmode, alpha0);
        D = TextureFetch_I8(vramaddr, st_full + ivec4(1,1,0,0), wrapmode, alpha0);
    }
    else if (type == 1)
    {
        A = TextureFetch_A3I5(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_A3I5(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_A3I5(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_A3I5(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }
    else if (type == 6)
    {
        A = TextureFetch_A5I3(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_A5I3(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_A5I3(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_A5I3(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }
    else
    {
        A = TextureFetch_Direct(vramaddr, st_full                 , wrapmode);
        B = TextureFetch_Direct(vramaddr, st_full + ivec4(1,0,0,0), wrapmode);
        C = TextureFetch_Direct(vramaddr, st_full + ivec4(0,1,0,0), wrapmode);
        D = TextureFetch_Direct(vramaddr, st_full + ivec4(1,1,0,0), wrapmode);
    }

    float fx = fracpart.x;
    vec4 AB;
    if (A.a < (0.5/31.0) && B.a < (0.5/31.0))
        AB = vec4(0);
    else
    {
        //if (A.a < (0.5/31.0) || B.a < (0.5/31.0))
        //    fx = step(0.5, fx);

        AB = mix(A, B, fx);
    }

    fx = fracpart.x;
    vec4 CD;
    if (C.a < (0.5/31.0) && D.a < (0.5/31.0))
        CD = vec4(0);
    else
    {
        //if (C.a < (0.5/31.0) || D.a < (0.5/31.0))
        //    fx = step(0.5, fx);

        CD = mix(C, D, fx);
    }

    fx = fracpart.y;
    vec4 ret;
    if (AB.a < (0.5/31.0) && CD.a < (0.5/31.0))
        ret = vec4(0);
    else
    {
        //if (AB.a < (0.5/31.0) || CD.a < (0.5/31.0))
        //    fx = step(0.5, fx);

        ret = mix(AB, CD, fx);
    }

    return ret;
}

vec4 FinalColor()
{
    vec4 col;
    vec4 vcol = fColor;
    uint blendmode = (fPolygonAttr.x >> 4) & 0x3;

    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) == 0)
        {
            // toon
            vec3 tooncolor = uToonColors[int(vcol.r * 31)].rgb;
            vcol.rgb = tooncolor;
        }
        else
        {
            // highlight
            vcol.rgb = vcol.rrr;
        }
    }

    if ((((fPolygonAttr.y >> 26) & 0x7) == 0) || ((uDispCnt & (1<<0)) == 0))
    {
        // no texture
        col = vcol;
    }
    else
    {
        vec4 tcol = TextureLookup_Nearest(fTexcoord);
        //vec4 tcol = TextureLookup_Linear(fTexcoord);

        if ((blendmode & 1) != 0)
        {
            // decal
            col.rgb = (tcol.rgb * tcol.a) + (vcol.rgb * (1.0-tcol.a));
            col.a = vcol.a;
        }
        else
        {
            // modulate
            col = vcol * tcol;
        }
    }

    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) != 0)
        {
            vec3 tooncolor = uToonColors[int(vcol.r * 31)].rgb;
            col.rgb = min(col.rgb + tooncolor, 1.0);
        }
    }

    return col.bgra;
}
)";


const char* kRenderVS_Z = R"(

void main()
{
    uint attr = vPolygonAttr.x;
    uint zshift = (attr >> 16) & 0x1F;

    vec4 fpos;
    fpos.xy = ((vec2(vPosition.xy) * 2.0) / uScreenSize) - 1.0;
    fpos.z = (float(vPosition.z << zshift) / 8388608.0) - 1.0;
    fpos.w = float(vPosition.w) / 65536.0f;
    fpos.xyz *= fpos.w;

    fColor = vec4(vColor) / vec4(255.0,255.0,255.0,31.0);
    fTexcoord = vec2(vTexcoord) / 16.0;
    fPolygonAttr = vPolygonAttr;

    gl_Position = fpos;
}
)";

const char* kRenderVS_W = R"(

smooth out float fZ;

void main()
{
    uint attr = vPolygonAttr.x;
    uint zshift = (attr >> 16) & 0x1F;

    vec4 fpos;
    fpos.xy = ((vec2(vPosition.xy) * 2.0) / uScreenSize) - 1.0;
    fZ = float(vPosition.z << zshift) / 16777216.0;
    fpos.w = float(vPosition.w) / 65536.0f;
    fpos.xy *= fpos.w;

    fColor = vec4(vColor) / vec4(255.0,255.0,255.0,31.0);
    fTexcoord = vec2(vTexcoord) / 16.0;
    fPolygonAttr = vPolygonAttr;

    gl_Position = fpos;
}
)";


const char* kRenderFS_ZO = R"(

void main()
{
    vec4 col = FinalColor();
    if (col.a < 30.5/31) discard;

    oColor = col;
    oAttr.g = (fPolygonAttr.x >> 24) & 0x3F;
}
)";

const char* kRenderFS_WO = R"(

smooth in float fZ;

void main()
{
    vec4 col = FinalColor();
    if (col.a < 30.5/31) discard;

    oColor = col;
    oAttr.g = (fPolygonAttr.x >> 24) & 0x3F;
    gl_FragDepth = fZ;
}
)";

const char* kRenderFS_ZT = R"(

void main()
{
    vec4 col = FinalColor();
    if (col.a < 0.5/31) discard;
    if (col.a >= 30.5/31) discard;

    oColor = col;
    oAttr.g = 0xFF;
}
)";

const char* kRenderFS_WT = R"(

smooth in float fZ;

void main()
{
    vec4 col = FinalColor();
    if (col.a < 0.5/31) discard;
    if (col.a >= 30.5/31) discard;

    oColor = col;
    oAttr.g = 0xFF;
    gl_FragDepth = fZ;
}
)";

const char* kRenderFS_ZSM = R"(

void main()
{
    oColor = vec4(0,0,0,1);
    oAttr.g = 0xFF;
    oAttr.b = 1;
}
)";

const char* kRenderFS_WSM = R"(

smooth in float fZ;

void main()
{
    oColor = vec4(0,0,0,1);
    oAttr.g = 0xFF;
    oAttr.b = 1;
    gl_FragDepth = fZ;
}
)";

const char* kRenderFS_ZS = R"(

layout(binding=2) uniform usampler2D iAttrTex;
//layout(origin_upper_left) in vec4 gl_FragCoord;

void main()
{
    vec4 col = FinalColor();
    if (col.a < 0.5/31) discard;
    if (col.a >= 30.5/31) discard;

    uvec4 iAttr = texelFetch(iAttrTex, ivec2(gl_FragCoord.xy), 0);
    if (iAttr.b != 1) discard;
    if (iAttr.g == ((fPolygonAttr.x >> 24) & 0x3F)) discard;

    oColor = col;
}
)";

const char* kRenderFS_WS = R"(

layout(binding=2) uniform usampler2D iAttrTex;
//layout(origin_upper_left) in vec4 gl_FragCoord;

smooth in float fZ;

void main()
{
    vec4 col = FinalColor();
    if (col.a < 0.5/31) discard;
    if (col.a >= 30.5/31) discard;

    uvec4 iAttr = texelFetch(iAttrTex, ivec2(gl_FragCoord.xy), 0);
    if (iAttr.b != 1) discard;
    if (iAttr.g == ((fPolygonAttr.x >> 24) & 0x3F)) discard;

    oColor = col;
    gl_FragDepth = fZ;
}
)";


enum
{
    RenderFlag_WBuffer     = 0x01,
    RenderFlag_Trans       = 0x02,
    RenderFlag_ShadowMask  = 0x04,
    RenderFlag_Shadow      = 0x08,
};


GLuint ClearShaderPlain[3];

GLuint RenderShader[16][3];

struct
{
    float uScreenSize[2];
    u32 uDispCnt;
    u32 __pad0;
    float uToonColors[32][4];

} ShaderConfig;

GLuint ShaderConfigUBO;

typedef struct
{
    Polygon* PolyData;

    u16* Indices;
    u32 RenderKey;

} RendererPolygon;

RendererPolygon PolygonList[2048];
int NumFinalPolys, NumOpaqueFinalPolys;

GLuint ClearVertexBufferID, ClearVertexArrayID;

// vertex buffer
// * XYZW: 4x16bit
// * RGBA: 4x8bit
// * ST: 2x16bit
// * polygon data: 3x32bit (polygon/texture attributes)
//
// polygon attributes:
// * bit4-7, 11, 14-15, 24-29: POLYGON_ATTR
// * bit16-20: Z shift
// * bit8: front-facing (?)
// * bit9: W-buffering (?)

GLuint VertexBufferID;
u32 VertexBuffer[10240 * 7];
u32 NumVertices;

GLuint VertexArrayID;
u16 IndexBuffer[2048 * 10];
u32 NumTriangles;

GLuint TexMemID;
GLuint TexPalMemID;

int ScaleFactor;
int ScreenW, ScreenH;

GLuint FramebufferTex[4];
GLuint FramebufferID[2], PixelbufferID;
u32* Framebuffer = NULL;

bool ChunkedRendering = false;


bool InitGLExtensions()
{
    // TODO move this elsewhere!!
    //if (!OpenGL_Init()) return false;
    return true;
}

bool BuildRenderShader(u32 flags, const char* vs, const char* fs)
{
    char shadername[32];
    sprintf(shadername, "RenderShader%02X", flags);

    int headerlen = strlen(kShaderHeader);

    int vslen = strlen(vs);
    int vsclen = strlen(kRenderVSCommon);
    char* vsbuf = new char[headerlen + vsclen + vslen + 1];
    strcpy(&vsbuf[0], kShaderHeader);
    strcpy(&vsbuf[headerlen], kRenderVSCommon);
    strcpy(&vsbuf[headerlen + vsclen], vs);

    int fslen = strlen(fs);
    int fsclen = strlen(kRenderFSCommon);
    char* fsbuf = new char[headerlen + fsclen + fslen + 1];
    strcpy(&fsbuf[0], kShaderHeader);
    strcpy(&fsbuf[headerlen], kRenderFSCommon);
    strcpy(&fsbuf[headerlen + fsclen], fs);

    bool ret = OpenGL_BuildShaderProgram(vsbuf, fsbuf, RenderShader[flags], shadername);

    delete[] vsbuf;
    delete[] fsbuf;

    return ret;
}

void UseRenderShader(u32 flags)
{
    glUseProgram(RenderShader[flags][2]);
}

bool Init()
{
    if (!InitGLExtensions()) return false;

    const GLubyte* renderer = glGetString(GL_RENDERER); // get renderer string
    const GLubyte* version = glGetString(GL_VERSION); // version as a string
    printf("OpenGL: renderer: %s\n", renderer);
    printf("OpenGL: version: %s\n", version);

    int barg1, barg2;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &barg1);
    glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &barg2);
    printf("max texture: %d\n", barg1);
    printf("max comb. texture: %d\n", barg2);

    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &barg1);
    printf("max tex size: %d\n", barg1);

    glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &barg1);
    printf("max arraytex levels: %d\n", barg1);

    /*glGetIntegerv(GL_NUM_EXTENSIONS, &barg1);
    printf("extensions: %d\n", barg1);
    for (int i = 0; i < barg1; i++)
    {
        const GLubyte* ext = glGetStringi(GL_EXTENSIONS, i);
        printf("- %s\n", ext);
    }*/

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);


    glDepthRange(0, 1);
    glClearDepth(1.0);


    if (!OpenGL_BuildShaderProgram(kClearVS, kClearFS, ClearShaderPlain, "ClearShader"))
        return false;

    memset(RenderShader, 0, sizeof(RenderShader));

    if (!BuildRenderShader(0,
                           kRenderVS_Z, kRenderFS_ZO)) return false;
    if (!BuildRenderShader(RenderFlag_WBuffer,
                           kRenderVS_W, kRenderFS_WO)) return false;
    if (!BuildRenderShader(RenderFlag_Trans,
                           kRenderVS_Z, kRenderFS_ZT)) return false;
    if (!BuildRenderShader(RenderFlag_Trans | RenderFlag_WBuffer,
                           kRenderVS_W, kRenderFS_WT)) return false;
    if (!BuildRenderShader(RenderFlag_ShadowMask,
                           kRenderVS_Z, kRenderFS_ZSM)) return false;
    if (!BuildRenderShader(RenderFlag_ShadowMask | RenderFlag_WBuffer,
                           kRenderVS_W, kRenderFS_WSM)) return false;
    if (!BuildRenderShader(RenderFlag_Shadow,
                           kRenderVS_Z, kRenderFS_ZS)) return false;
    if (!BuildRenderShader(RenderFlag_Shadow | RenderFlag_WBuffer,
                           kRenderVS_W, kRenderFS_WS)) return false;


    memset(&ShaderConfig, 0, sizeof(ShaderConfig));

    glGenBuffers(1, &ShaderConfigUBO);
    glBindBuffer(GL_UNIFORM_BUFFER, ShaderConfigUBO);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(ShaderConfig), &ShaderConfig, GL_STATIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, ShaderConfigUBO);

    for (int i = 0; i < 16; i++)
    {
        if (!RenderShader[i][2]) continue;
        glUniformBlockBinding(RenderShader[i][2], 0, 0);
    }


    float clearvtx[6*2] =
    {
        -1.0, -1.0,
        1.0, 1.0,
        -1.0, 1.0,

        -1.0, -1.0,
        1.0, -1.0,
        1.0, 1.0
    };

    glGenBuffers(1, &ClearVertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(clearvtx), clearvtx, GL_STATIC_DRAW);

    glGenVertexArrays(1, &ClearVertexArrayID);
    glBindVertexArray(ClearVertexArrayID);
    glEnableVertexAttribArray(0); // position
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, (void*)(0));


    glGenBuffers(1, &VertexBufferID);
    glBindBuffer(GL_ARRAY_BUFFER, VertexBufferID);
    glBufferData(GL_ARRAY_BUFFER, sizeof(VertexBuffer), NULL, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &VertexArrayID);
    glBindVertexArray(VertexArrayID);
    glEnableVertexAttribArray(0); // position
    glVertexAttribIPointer(0, 4, GL_UNSIGNED_SHORT, 7*4, (void*)(0));
    glEnableVertexAttribArray(1); // color
    glVertexAttribIPointer(1, 4, GL_UNSIGNED_BYTE, 7*4, (void*)(2*4));
    glEnableVertexAttribArray(2); // texcoords
    glVertexAttribIPointer(2, 2, GL_SHORT, 7*4, (void*)(3*4));
    glEnableVertexAttribArray(3); // attrib
    glVertexAttribIPointer(3, 3, GL_UNSIGNED_INT, 7*4, (void*)(4*4));


    glGenFramebuffers(2, &FramebufferID[0]);
    glBindFramebuffer(GL_FRAMEBUFFER, FramebufferID[0]);

    // color buffer
    glGenTextures(4, &FramebufferTex[0]);
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, FramebufferTex[0], 0);

    // depth/stencil buffer
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, FramebufferTex[1], 0);

    // attribute buffer
    // R: opaque polyID (for edgemarking)
    // G: opaque polyID (for shadows, suppressed when rendering translucent polygons)
    // B: stencil flag
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[2]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, FramebufferTex[2], 0);

    // downscale framebuffer, for antialiased mode
    glBindFramebuffer(GL_FRAMEBUFFER, FramebufferID[1]);
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[3]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, FramebufferTex[3], 0);

    glBindFramebuffer(GL_FRAMEBUFFER, FramebufferID[0]);

    GLenum fbassign[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, fbassign);

    glEnable(GL_BLEND);
    glBlendFuncSeparatei(0, GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE);
    glBlendEquationSeparatei(0, GL_ADD, GL_MAX);
    glBlendFuncSeparatei(1, GL_ONE, GL_ZERO, GL_ONE, GL_ZERO);

    glGenBuffers(1, &PixelbufferID);

    glActiveTexture(GL_TEXTURE0);
    glGenTextures(1, &TexMemID);
    glBindTexture(GL_TEXTURE_2D, TexMemID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8UI, 1024, 512, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, NULL);

    glActiveTexture(GL_TEXTURE1);
    glGenTextures(1, &TexPalMemID);
    glBindTexture(GL_TEXTURE_2D, TexPalMemID);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, 1024, 48, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, NULL);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[2]); // opaque polyID / shadow bits

    return true;
}

void DeInit()
{
    // TODO CLEAN UP SHIT!!!!
}

void Reset()
{
    //
}

void SetScale(int scale)
{
    ScaleFactor = scale;

    // TODO: antialiasing setting
    ScreenW = 256 << scale;
    ScreenH = 192 << scale;

    glBindTexture(GL_TEXTURE_2D, FramebufferTex[0]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenW, ScreenH, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[1]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH24_STENCIL8, ScreenW, ScreenH, 0, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8, NULL);
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[2]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8UI, ScreenW, ScreenH, 0, GL_RGB_INTEGER, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, FramebufferTex[3]);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ScreenW/2, ScreenH/2, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);

    glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelbufferID);
    glBufferData(GL_PIXEL_PACK_BUFFER, ScreenW*ScreenH*4, NULL, GL_DYNAMIC_READ);

    if (Framebuffer) delete[] Framebuffer;
    Framebuffer = new u32[ScreenW*ScreenH];
}


void SetupPolygon(RendererPolygon* rp, Polygon* polygon)
{
    rp->PolyData = polygon;

    // render key: depending on what we're drawing
    // opaque polygons:
    // - depthfunc
    // -- alpha=0
    // regular translucent polygons:
    // - depthfunc
    // -- depthwrite
    // --- polyID
    // shadow mask polygons:
    // - depthfunc?????
    // shadow polygons:
    // - depthfunc
    // -- depthwrite
    // --- polyID

    rp->RenderKey = (polygon->Attr >> 14) & 0x1; // bit14 - depth func
    if (!polygon->IsShadowMask)
    {
        if (polygon->Translucent)
        {
            if (polygon->IsShadow) rp->RenderKey |= 0x20000;
            else                   rp->RenderKey |= 0x10000;
            rp->RenderKey |= (polygon->Attr >> 10) & 0x2; // bit11 - depth write
            rp->RenderKey |= (polygon->Attr & 0x3F000000) >> 16; // polygon ID
        }
        else
        {
            if ((polygon->Attr & 0x001F0000) == 0)
                rp->RenderKey |= 0x2;
        }
    }
    else
    {
        rp->RenderKey |= 0x30000;
    }
}

void BuildPolygons(RendererPolygon* polygons, int npolys)
{
    u32* vptr = &VertexBuffer[0];
    u32 vidx = 0;

    u16* iptr = &IndexBuffer[0];
    u32 numtriangles = 0;

    for (int i = 0; i < npolys; i++)
    {
        RendererPolygon* rp = &polygons[i];
        Polygon* poly = rp->PolyData;

        rp->Indices = iptr;

        u32 vidx_first = vidx;

        u32 polyattr = poly->Attr;

        u32 alpha = (polyattr >> 16) & 0x1F;

        u32 vtxattr = polyattr & 0x1F00C8F0;
        if (poly->FacingView) vtxattr |= (1<<8);
        if (poly->WBuffer)    vtxattr |= (1<<9);

        // assemble vertices
        for (int j = 0; j < poly->NumVertices; j++)
        {
            Vertex* vtx = poly->Vertices[j];

            u32 z = poly->FinalZ[j];
            u32 w = poly->FinalW[j];

            // Z should always fit within 16 bits, so it's okay to do this
            u32 zshift = 0;
            while (z > 0xFFFF) { z >>= 1; zshift++; }

            u32 x, y;
            if (ScaleFactor > 0)
            {
                x = vtx->HiresPosition[0] >> (4-ScaleFactor);
                y = vtx->HiresPosition[1] >> (4-ScaleFactor);
            }
            else
            {
                x = vtx->FinalPosition[0];
                y = vtx->FinalPosition[1];
            }

            *vptr++ = x | (y << 16);
            *vptr++ = z | (w << 16);

            *vptr++ =  (vtx->FinalColor[0] >> 1) |
                      ((vtx->FinalColor[1] >> 1) << 8) |
                      ((vtx->FinalColor[2] >> 1) << 16) |
                      (alpha << 24);

            *vptr++ = (u16)vtx->TexCoords[0] | ((u16)vtx->TexCoords[1] << 16);

            *vptr++ = vtxattr | (zshift << 16);
            *vptr++ = poly->TexParam;
            *vptr++ = poly->TexPalette;

            if (j >= 2)
            {
                // build a triangle
                *iptr++ = vidx_first;
                *iptr++ = vidx - 1;
                *iptr++ = vidx;
                numtriangles++;
            }

            vidx++;
        }
    }

    NumTriangles = numtriangles;
    NumVertices = vidx;
}

void RenderSceneChunk(int y, int h)
{
    u32 flags = 0;
    if (RenderPolygonRAM[0]->WBuffer) flags |= RenderFlag_WBuffer;

    if (h != 192) glScissor(0, y<<ScaleFactor, 256<<ScaleFactor, h<<ScaleFactor);

    // pass 1: opaque pixels

    UseRenderShader(flags);

    glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);

    glDepthFunc(GL_LESS);
    glDepthMask(GL_TRUE);

    glStencilFunc(GL_ALWAYS, 0xFF, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

    glBindVertexArray(VertexArrayID);
    glDrawElements(GL_TRIANGLES, NumTriangles*3, GL_UNSIGNED_SHORT, IndexBuffer);


    glEnable(GL_BLEND);
    UseRenderShader(flags | RenderFlag_Trans);

    u16* iptr;
    u32 curkey;
    bool lastwasshadow;bool darp;
//printf("morp %08X\n", RenderClearAttr1);
    if (NumOpaqueFinalPolys > -1)
    {
        glColorMaski(1, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
if (PolygonList[NumOpaqueFinalPolys].PolyData->IsShadow) printf("!! GLORG!!! %08X\n", PolygonList[NumOpaqueFinalPolys].PolyData->Attr);
        // pass 2: if needed, render translucent pixels that are against background pixels
        // when background alpha is zero, those need to be rendered with blending disabled

        if ((RenderClearAttr1 & 0x001F0000) == 0)
        {
            iptr = PolygonList[NumOpaqueFinalPolys].Indices;
            curkey = 0xFFFFFFFF;

            glDisable(GL_BLEND);

            for (int i = NumOpaqueFinalPolys; i < NumFinalPolys; i++)
            {
                RendererPolygon* rp = &PolygonList[i];
                if (rp->RenderKey != curkey)
                {
                    u16* endptr = rp->Indices;
                    u32 num = (u32)(endptr - iptr);
                    if (num) glDrawElements(GL_TRIANGLES, num, GL_UNSIGNED_SHORT, iptr);

                    iptr = rp->Indices;
                    curkey = rp->RenderKey;

                    // configure new one

                    // shadows aren't likely to pass against the clear-plane, so
                    if (rp->PolyData->IsShadow) continue;

                    // zorp
                    glDepthFunc(GL_LESS);

                    u32 polyattr = rp->PolyData->Attr;
                    u32 polyid = (polyattr >> 24) & 0x3F;

                    glStencilFunc(GL_EQUAL, 0, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_INVERT);
                    glStencilMask(0x40|polyid); // heheh

                    if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                    else                    glDepthMask(GL_FALSE);
                }
            }

            {
                u16* endptr = &IndexBuffer[NumTriangles*3];
                u32 num = (u32)(endptr - iptr);
                if (num) glDrawElements(GL_TRIANGLES, num, GL_UNSIGNED_SHORT, iptr);
            }

            glEnable(GL_BLEND);
            glStencilMask(0xFF);
        }

        // pass 3: translucent pixels

        iptr = PolygonList[NumOpaqueFinalPolys].Indices;
        curkey = 0xFFFFFFFF;
        lastwasshadow = false; darp = false;

        for (int i = NumOpaqueFinalPolys; i < NumFinalPolys; i++)
        {
            RendererPolygon* rp = &PolygonList[i];
            //printf("PASS 3 POLYGON %i: ATTR %08X (%d) | KEY %08X\n", i, rp->PolyData->Attr, (rp->PolyData->Attr>>4)&0x3, rp->RenderKey);
            if (rp->RenderKey != curkey)
            {
                u16* endptr = rp->Indices;
                u32 num = (u32)(endptr - iptr);
                if (num) glDrawElements(GL_TRIANGLES, num, GL_UNSIGNED_SHORT, iptr);

                iptr = rp->Indices;
                curkey = rp->RenderKey;

                // configure new one

                if (rp->PolyData->IsShadowMask)
                {
                    //printf("beginning shadowmask batch: %d, %d\n", lastwasshadow, darp);
                    /*if (!lastwasshadow)
                    {
                        //glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

                        glDisable(GL_BLEND);
                        UseRenderShader(flags | RenderFlag_ShadowMask);

                        // shadow bits are set where the depth test fails
                        // sure enough, if GL_LESS fails, the opposite function would pass
                        glDepthFunc(GL_GEQUAL);

                        //glStencilFunc(GL_ALWAYS, 0x80, 0x80);
                        //glStencilOp(GL_REPLACE, GL_REPLACE, GL_KEEP);

                        glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                        glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);
                        glDepthMask(GL_FALSE);

                        lastwasshadow = true;
                    }*/

                    glDisable(GL_BLEND);
                    UseRenderShader(flags | RenderFlag_ShadowMask);

                    glColorMaski(0, GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    glColorMaski(1, GL_FALSE, GL_FALSE, GL_TRUE, GL_FALSE);
                    glDepthMask(GL_FALSE);
                    glDepthFunc(GL_GEQUAL);
                    glStencilFunc(GL_ALWAYS,0,0);
                    glStencilOp(GL_KEEP,GL_KEEP,GL_KEEP);
                    lastwasshadow=true;

                    darp = false;
                }
                else
                {
                    if (rp->PolyData->IsShadow) glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
                    //if (rp->PolyData->IsShadow) printf("beginning shadow batch: %d, %d\n", lastwasshadow, darp);

                    if (rp->PolyData->IsShadow)
                        UseRenderShader(flags | RenderFlag_Shadow);
                    else
                        UseRenderShader(flags | RenderFlag_Trans);

                    if (lastwasshadow)
                    {
                        glEnable(GL_BLEND);

                        glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                        glColorMaski(1, GL_FALSE, GL_TRUE, GL_FALSE, GL_FALSE);

                        lastwasshadow = false;
                    }

                    // zorp
                    glDepthFunc(GL_LESS);

                    u32 polyattr = rp->PolyData->Attr;
                    u32 polyid = (polyattr >> 24) & 0x3F;

                    glStencilFunc(GL_NOTEQUAL, 0x40|polyid, 0xFF);
                    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

                    if (polyattr & (1<<11)) glDepthMask(GL_TRUE);
                    else                    glDepthMask(GL_FALSE);

                    darp = rp->PolyData->IsShadow;
                }
            }
        }

        {
            u16* endptr = &IndexBuffer[NumTriangles*3];
            u32 num = (u32)(endptr - iptr);
            if (num) glDrawElements(GL_TRIANGLES, num, GL_UNSIGNED_SHORT, iptr);
        }
    }

    glFlush();
}


void VCount144()
{
}

void RenderFrame()
{
    ShaderConfig.uScreenSize[0] = ScreenW;
    ShaderConfig.uScreenSize[1] = ScreenH;
    ShaderConfig.uDispCnt = RenderDispCnt;

    for (int i = 0; i < 32; i++)
    {
        u16 c = RenderToonTable[i];
        u32 r = c & 0x1F;
        u32 g = (c >> 5) & 0x1F;
        u32 b = (c >> 10) & 0x1F;

        ShaderConfig.uToonColors[i][0] = (float)r / 31.0;
        ShaderConfig.uToonColors[i][1] = (float)g / 31.0;
        ShaderConfig.uToonColors[i][2] = (float)b / 31.0;
    }

    glBindBuffer(GL_UNIFORM_BUFFER, ShaderConfigUBO);
    void* unibuf = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
    if (unibuf) memcpy(unibuf, &ShaderConfig, sizeof(ShaderConfig));
    glUnmapBuffer(GL_UNIFORM_BUFFER);

    // SUCKY!!!!!!!!!!!!!!!!!!
    // TODO: detect when VRAM blocks are modified!
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, TexMemID);
    for (int i = 0; i < 4; i++)
    {
        u32 mask = GPU::VRAMMap_Texture[i];
        u8* vram;
        if (!mask) continue;
        else if (mask & (1<<0)) vram = GPU::VRAM_A;
        else if (mask & (1<<1)) vram = GPU::VRAM_B;
        else if (mask & (1<<2)) vram = GPU::VRAM_C;
        else if (mask & (1<<3)) vram = GPU::VRAM_D;

        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i*128, 1024, 128, GL_RED_INTEGER, GL_UNSIGNED_BYTE, vram);
    }

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, TexPalMemID);
    for (int i = 0; i < 6; i++)
    {
        // 6 x 16K chunks
        u32 mask = GPU::VRAMMap_TexPal[i];
        u8* vram;
        if (!mask) continue;
        else if (mask & (1<<4)) vram = &GPU::VRAM_E[(i&3)*0x4000];
        else if (mask & (1<<5)) vram = GPU::VRAM_F;
        else if (mask & (1<<6)) vram = GPU::VRAM_G;

        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, i*8, 1024, 8, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, vram);
    }

    glDisable(GL_SCISSOR_TEST);
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_STENCIL_TEST);

    glViewport(0, 0, ScreenW, ScreenH);

    glBindFramebuffer(GL_FRAMEBUFFER, FramebufferID[0]);
    glDisable(GL_BLEND);
    glColorMaski(0, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glColorMaski(1, GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);

    // clear buffers
    // TODO: clear bitmap
    // TODO: check whether 'clear polygon ID' affects translucent polyID
    // (for example when alpha is 1..30)
    {
        glUseProgram(ClearShaderPlain[2]);
        glDepthFunc(GL_ALWAYS);
        glDepthMask(GL_TRUE);

        u32 r = RenderClearAttr1 & 0x1F;
        u32 g = (RenderClearAttr1 >> 5) & 0x1F;
        u32 b = (RenderClearAttr1 >> 10) & 0x1F;
        u32 fog = (RenderClearAttr1 >> 15) & 0x1;
        u32 a = (RenderClearAttr1 >> 16) & 0x1F;
        u32 polyid = (RenderClearAttr1 >> 24) & 0x3F;
        u32 z = ((RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;

        glStencilFunc(GL_ALWAYS, 0, 0xFF);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);

        /*if (r) r = r*2 + 1;
        if (g) g = g*2 + 1;
        if (b) b = b*2 + 1;*/

        glUniform4ui(0, r, g, b, a);
        glUniform1ui(1, z);
        glUniform1ui(2, polyid);
        glUniform1ui(3, fog);

        glBindBuffer(GL_ARRAY_BUFFER, ClearVertexBufferID);
        glBindVertexArray(ClearVertexArrayID);
        glDrawArrays(GL_TRIANGLES, 0, 2*3);
    }

    if (RenderNumPolygons)
    {
        // render shit here
        u32 flags = 0;
        if (RenderPolygonRAM[0]->WBuffer) flags |= RenderFlag_WBuffer;

        int npolys = 0;
        int firsttrans = -1;
        for (int i = 0; i < RenderNumPolygons; i++)
        {
            if (RenderPolygonRAM[i]->Degenerate) continue;

            // zog.
            //if (RenderPolygonRAM[i]->YBottom <= 96 || RenderPolygonRAM[i]->YTop >= 144) continue;

            SetupPolygon(&PolygonList[npolys], RenderPolygonRAM[i]);
            if (firsttrans < 0 && RenderPolygonRAM[i]->Translucent)
                firsttrans = npolys;

            npolys++;
        }
        NumFinalPolys = npolys;
        NumOpaqueFinalPolys = firsttrans;

        BuildPolygons(&PolygonList[0], npolys);
        glBindBuffer(GL_ARRAY_BUFFER, VertexBufferID);
        glBufferSubData(GL_ARRAY_BUFFER, 0, NumVertices*7*4, VertexBuffer);

        if (!ChunkedRendering)
        {
            RenderSceneChunk(0, 192);
        }
        else
        {
            glEnable(GL_SCISSOR_TEST);
            RenderSceneChunk(0, 48);
        }
    }

    if (false)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferID[0]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FramebufferID[1]);
        glBlitFramebuffer(0, 0, ScreenW, ScreenH, 0, 0, ScreenW/2, ScreenH/2, GL_COLOR_BUFFER_BIT, GL_LINEAR);

        glBindFramebuffer(GL_FRAMEBUFFER, FramebufferID[1]);
    }
    else
    {
        glBindFramebuffer(GL_FRAMEBUFFER, FramebufferID[0]);
    }

    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, PixelbufferID);

    if (!ChunkedRendering)
        glReadPixels(0, 0, 256<<ScaleFactor, 192<<ScaleFactor, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
    else
        glReadPixels(0, 0, 256<<ScaleFactor, 48<<ScaleFactor, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
}

u32* GetLine(int line)
{
    int stride = 256 << (ScaleFactor*2);

    if (!ChunkedRendering)
    {
        if (line == 0)
        {
            u8* data = (u8*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (data) memcpy(&Framebuffer[stride*0], data, 4*stride*192);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }
    else
    {
        if (line == 0)
        {
            u8* data = (u8*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (data) memcpy(&Framebuffer[stride*0], data, 4*stride*48);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

            if (RenderNumPolygons) RenderSceneChunk(48, 48);
            glReadPixels(0, 48<<ScaleFactor, 256<<ScaleFactor, 48<<ScaleFactor, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        }
        else if (line == 48)
        {
            u8* data = (u8*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (data) memcpy(&Framebuffer[stride*48], data, 4*stride*48);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

            if (RenderNumPolygons) RenderSceneChunk(96, 48);
            glReadPixels(0, 96<<ScaleFactor, 256<<ScaleFactor, 48<<ScaleFactor, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        }
        else if (line == 96)
        {
            u8* data = (u8*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (data) memcpy(&Framebuffer[stride*96], data, 4*stride*48);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);

            if (RenderNumPolygons) RenderSceneChunk(144, 48);
            glReadPixels(0, 144<<ScaleFactor, 256<<ScaleFactor, 48<<ScaleFactor, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
        }
        else if (line == 144)
        {
            u8* data = (u8*)glMapBuffer(GL_PIXEL_PACK_BUFFER, GL_READ_ONLY);
            if (data) memcpy(&Framebuffer[stride*144], data, 4*stride*48);
            glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        }
    }

    u64* ptr = (u64*)&Framebuffer[stride * line];
    for (int i = 0; i < stride; i+=2)
    {
        u64 rgb = *ptr & 0x00FCFCFC00FCFCFC;
        u64 a = *ptr & 0xF8000000F8000000;

        *ptr++ = (rgb >> 2) | (a >> 3);
    }

    return &Framebuffer[stride * line];
}

}
}