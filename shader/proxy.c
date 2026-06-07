#include <windows.h>
#define COBJMACROS
#include <dxgi.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <math.h>

// ---------------------------------------------------------------------------
// Stage 1 — pure passthrough proxy.
//
// Loads the real system dxgi.dll by absolute path (System32 — WOW64 file
// redirection resolves this to SysWOW64 automatically for a 32-bit process)
// and forwards every export untouched. Proves the proxy-load mechanism works
// before any hooking is added.
//
// Stage 2 — swap chain / Present hook.
//
// COM vtables for a given interface + runtime version are shared across all
// instances, so patching one instance's vtable slot affects every instance.
// We patch IDXGIFactory::CreateSwapChain (slot 10) on the first factory we
// see, then patch IDXGISwapChain::Present (slot 8) on the first swap chain
// that factory creates. Logging only for now — no rendering changes.
// ---------------------------------------------------------------------------

// vtable slot indices (fixed by COM interface inheritance order — verified
// against the actual ID3D11DeviceContextVtbl layout in d3d11.h)
#define VT_FACTORY_CREATESWAPCHAIN     10
#define VT_SWAPCHAIN_PRESENT            8
#define VT_CONTEXT_OMSETRENDERTARGETS  31
#define VT_CONTEXT_OMSETRENDERTARGETSANDUAV 32

static void log_line(const char *fmt, ...);

static void *patch_vtable_slot(void *com_obj, int slot, void *hook) {
    if (!com_obj) return NULL;
    void **vtbl = *(void ***)com_obj;
    void *original = vtbl[slot];
    if (original == hook) return NULL;          // already patched — idempotent
    DWORD old_protect;
    if (!VirtualProtect(&vtbl[slot], sizeof(void *), PAGE_EXECUTE_READWRITE, &old_protect))
        return NULL;
    vtbl[slot] = hook;
    VirtualProtect(&vtbl[slot], sizeof(void *), old_protect, &old_protect);
    return original;
}

// ---------------------------------------------------------------------------
// Depth-buffer capture via OMSetRenderTargets hook.
//
// By the time Present() runs, the engine has already unbound its depth buffer
// (confirmed via diagnostic logging — OMGetRenderTargets returns no DSV at
// Present time). So instead we patch ID3D11DeviceContext::OMSetRenderTargets
// and record whichever depth-stencil view is bound that matches the output
// resolution — that's our best heuristic for "the main scene's depth buffer"
// (shadow maps / reflection probes / UI compositing won't match the size).
// ---------------------------------------------------------------------------

typedef void (WINAPI *PFN_OMSetRenderTargets)(ID3D11DeviceContext *, UINT,
                                              ID3D11RenderTargetView *const *,
                                              ID3D11DepthStencilView *);
typedef void (WINAPI *PFN_OMSetRenderTargetsAndUAV)(ID3D11DeviceContext *, UINT,
                                                    ID3D11RenderTargetView *const *,
                                                    ID3D11DepthStencilView *, UINT, UINT,
                                                    ID3D11UnorderedAccessView *const *,
                                                    const UINT *);
static PFN_OMSetRenderTargets orig_OMSetRenderTargets;
static PFN_OMSetRenderTargetsAndUAV orig_OMSetRenderTargetsAndUAV;
static ID3D11DepthStencilView *g_candidate_dsv;   // most recent DSV matching backbuffer size (cached ref)
static UINT g_w, g_h;                             // current output resolution (set in build_pipeline)

// Inspect a depth-stencil view bound by either OM-binding entry point and, if its
// backing texture matches the output resolution, cache it as our capture candidate.
// Also logs the size/format of every DSV seen (throttled) so we can see what the
// engine actually binds if our size-match heuristic turns out to be wrong.
static void consider_dsv(ID3D11DepthStencilView *dsv, const char *src) {
    ID3D11Resource *res = NULL;
    if (!dsv || !g_w || !g_h) return;
    ID3D11View_GetResource((ID3D11View *)dsv, &res);
    if (res) {
        ID3D11Texture2D *tex = NULL;
        if (SUCCEEDED(ID3D11Resource_QueryInterface(res, &IID_ID3D11Texture2D, (void **)&tex))) {
            D3D11_TEXTURE2D_DESC d;
            static LONG seen_n;
            LONG sn;
            ID3D11Texture2D_GetDesc(tex, &d);
            sn = InterlockedIncrement(&seen_n);
            if (sn <= 20 || (sn % 300) == 0)
                log_line("[shader] omrt-diag #%ld via %s: dsv tex %ux%u fmt=%d (target %ux%u)",
                         sn, src, d.Width, d.Height, (int)d.Format, g_w, g_h);
            if (d.Width == g_w && d.Height == g_h) {
                ID3D11DepthStencilView *prev;
                ID3D11DepthStencilView_AddRef(dsv);
                prev = g_candidate_dsv;
                g_candidate_dsv = dsv;
                if (prev) ID3D11DepthStencilView_Release(prev);
            }
            ID3D11Texture2D_Release(tex);
        }
        ID3D11Resource_Release(res);
    }
}

static void WINAPI Hook_OMSetRenderTargets(ID3D11DeviceContext *ctx, UINT num_views,
                                           ID3D11RenderTargetView *const *views,
                                           ID3D11DepthStencilView *dsv) {
    consider_dsv(dsv, "OMSetRenderTargets");
    orig_OMSetRenderTargets(ctx, num_views, views, dsv);
}

static void WINAPI Hook_OMSetRenderTargetsAndUAV(ID3D11DeviceContext *ctx, UINT num_rtvs,
                                                 ID3D11RenderTargetView *const *rtvs,
                                                 ID3D11DepthStencilView *dsv,
                                                 UINT uav_start, UINT num_uavs,
                                                 ID3D11UnorderedAccessView *const *uavs,
                                                 const UINT *uav_init_counts) {
    consider_dsv(dsv, "OMSetRTAndUAV");
    orig_OMSetRenderTargetsAndUAV(ctx, num_rtvs, rtvs, dsv, uav_start, num_uavs, uavs, uav_init_counts);
}

static void hook_context(ID3D11DeviceContext *ctx) {
    void *prev = patch_vtable_slot(ctx, VT_CONTEXT_OMSETRENDERTARGETS, (void *)Hook_OMSetRenderTargets);
    if (prev) {
        orig_OMSetRenderTargets = (PFN_OMSetRenderTargets)prev;
        log_line("[shader] context hooked -> OMSetRenderTargets");
    }
    prev = patch_vtable_slot(ctx, VT_CONTEXT_OMSETRENDERTARGETSANDUAV, (void *)Hook_OMSetRenderTargetsAndUAV);
    if (prev) {
        orig_OMSetRenderTargetsAndUAV = (PFN_OMSetRenderTargetsAndUAV)prev;
        log_line("[shader] context hooked -> OMSetRenderTargetsAndUnorderedAccessViews");
    }
}

// ---------------------------------------------------------------------------
// Many engines record their main scene draws on deferred contexts (worker
// threads) and submit them via ExecuteCommandList — those contexts have their
// own vtable instance, separate from the immediate context's, so OM calls
// recorded on them never reach our immediate-context hooks above. Patch
// ID3D11Device::CreateDeferredContext so every deferred context gets hooked
// the moment it's created.
// ---------------------------------------------------------------------------
#define VT_DEVICE_CREATEDEFERREDCONTEXT 27

typedef HRESULT (WINAPI *PFN_CreateDeferredContext)(ID3D11Device *, UINT, ID3D11DeviceContext **);
static PFN_CreateDeferredContext orig_CreateDeferredContext;

static HRESULT WINAPI Hook_CreateDeferredContext(ID3D11Device *dev, UINT flags,
                                                 ID3D11DeviceContext **out_ctx) {
    HRESULT hr = orig_CreateDeferredContext(dev, flags, out_ctx);
    if (SUCCEEDED(hr) && out_ctx && *out_ctx) {
        log_line("[shader] deferred context created -> hooking");
        hook_context(*out_ctx);
    }
    return hr;
}

// ---------------------------------------------------------------------------
// Direct depth-stencil resource capture: hook ID3D11Device::CreateDepthStencilView.
// A depth-stencil resource MUST be created before it can ever be bound, so this
// is strictly more fundamental than tracking OM bindings — it fires rarely
// (once per buffer, not per frame), and hands us the resource directly so we
// can CopyResource from it on demand without needing to guess which binding
// call/context/heuristic ever touches it.
// ---------------------------------------------------------------------------
#define VT_DEVICE_CREATEDEPTHSTENCILVIEW 10

// Assumed projection parameters for SSR view-space reconstruction. The engine's
// actual projection matrix isn't accessible to us (same blind-guess situation
// ReShade is in, which is why its depth-based effects expose these as user-tunable
// sliders). Roblox's default vertical FOV is 70 degrees; near/far are typical
// guesses for its render distance — these may need visual tuning.
#define SSR_FOV_DEGREES   70.0f
#define SSR_NEAR_PLANE    0.1f
#define SSR_FAR_PLANE     1000.0f

typedef HRESULT (WINAPI *PFN_CreateDepthStencilView)(ID3D11Device *, ID3D11Resource *,
                                                     const D3D11_DEPTH_STENCIL_VIEW_DESC *,
                                                     ID3D11DepthStencilView **);
static PFN_CreateDepthStencilView orig_CreateDepthStencilView;
static ID3D11Resource *g_dsv_resource;   // best-matching depth-stencil resource seen so far (cached ref)
static UINT g_dsv_resource_samples;      // its sample count, for the preference check below

static HRESULT WINAPI Hook_CreateDepthStencilView(ID3D11Device *dev, ID3D11Resource *resource,
                                                  const D3D11_DEPTH_STENCIL_VIEW_DESC *desc,
                                                  ID3D11DepthStencilView **out_view) {
    HRESULT hr = orig_CreateDepthStencilView(dev, resource, desc, out_view);
    if (resource) {
        ID3D11Texture2D *tex = NULL;
        if (SUCCEEDED(ID3D11Resource_QueryInterface(resource, &IID_ID3D11Texture2D, (void **)&tex))) {
            D3D11_TEXTURE2D_DESC d;
            ID3D11Texture2D_GetDesc(tex, &d);
            log_line("[shader] CreateDepthStencilView: tex %ux%u fmt=%d mip=%u arr=%u sample=%u (target %ux%u) hr=0x%lx",
                     d.Width, d.Height, (int)d.Format, d.MipLevels, d.ArraySize, d.SampleDesc.Count,
                     g_w, g_h, hr);
            // Prefer the resource matching our output size with the HIGHEST sample
            // count: diagnostics showed a same-size 1x buffer that's created but
            // never written (reads back as constant zero) — almost certainly an
            // unused fallback, while the engine actually renders its main scene
            // into an MSAA depth buffer (4x observed). Higher samples = more
            // likely to be the live one actually used for 3D scene depth testing.
            if (SUCCEEDED(hr) && d.Width == g_w && d.Height == g_h &&
                (!g_dsv_resource || d.SampleDesc.Count > g_dsv_resource_samples)) {
                ID3D11Resource *prev;
                ID3D11Resource_AddRef(resource);
                prev = g_dsv_resource;
                g_dsv_resource = resource;
                g_dsv_resource_samples = d.SampleDesc.Count;
                if (prev) ID3D11Resource_Release(prev);
                log_line("[shader] -> cached as depth-resource candidate (samples=%u, direct creation hook)",
                         d.SampleDesc.Count);
            }
            ID3D11Texture2D_Release(tex);
        }
    }
    return hr;
}

static void hook_device(ID3D11Device *dev) {
    void *prev = patch_vtable_slot(dev, VT_DEVICE_CREATEDEFERREDCONTEXT, (void *)Hook_CreateDeferredContext);
    if (prev) {
        orig_CreateDeferredContext = (PFN_CreateDeferredContext)prev;
        log_line("[shader] device hooked -> CreateDeferredContext");
    }
    prev = patch_vtable_slot(dev, VT_DEVICE_CREATEDEPTHSTENCILVIEW, (void *)Hook_CreateDepthStencilView);
    if (prev) {
        orig_CreateDepthStencilView = (PFN_CreateDepthStencilView)prev;
        log_line("[shader] device hooked -> CreateDepthStencilView");
    }
}

typedef HRESULT (WINAPI *PFN_Present)(IDXGISwapChain *, UINT, UINT);
typedef HRESULT (WINAPI *PFN_CreateSwapChain)(IDXGIFactory *, IUnknown *, DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **);

static PFN_Present          orig_Present;
static PFN_CreateSwapChain  orig_CreateSwapChain;
static LONG                 g_present_count;

// ---------------------------------------------------------------------------
// Stage 3 — post-process render pipeline.
//
// Each Present: copy the real backbuffer into a sampleable texture, then draw
// a fullscreen triangle through our pixel shader (bloom + saturation/contrast
// boost) straight back onto the backbuffer before handing it to the OS.
// ---------------------------------------------------------------------------

typedef HRESULT (WINAPI *PFN_D3DCompile)(
    LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *,
    LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

static HMODULE         g_d3dcompiler;
static PFN_D3DCompile  p_D3DCompile;

static ID3D11Device             *g_dev;
static ID3D11DeviceContext      *g_ctx;
static ID3D11Texture2D          *g_copy_tex;
static ID3D11ShaderResourceView *g_copy_srv;
static ID3D11VertexShader       *g_vs;
static ID3D11PixelShader        *g_ps;
static ID3D11SamplerState       *g_sampler;
static ID3D11Buffer             *g_cbuf;
static ID3D11BlendState         *g_blend;
static ID3D11RasterizerState    *g_raster;
static ID3D11DepthStencilState  *g_depthstencil;

// Captured copy of whatever depth-stencil buffer is bound when Present fires
// (heuristically the main scene's depth buffer, for screen-space reflections).
static ID3D11Texture2D          *g_depth_tex;
static ID3D11ShaderResourceView *g_depth_srv;
static ID3D11Texture2D          *g_depth_staging;   // small CPU-readable readback texture for diagnostics
static DXGI_FORMAT g_depth_fmt;
static UINT g_depth_w, g_depth_h;
static UINT g_depth_samples = 1;          // sample count of the captured depth resource
static UINT g_compiled_ps_samples = 0;    // sample count the currently-bound g_ps was built for (0 = none yet)
static BOOL g_depth_available;
static enum { PIPE_NONE, PIPE_OK, PIPE_FAILED } g_pipe_state;

static const char *g_vs_src =
"struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"VSOut main(uint id : SV_VertexID) {\n"
"    VSOut o;\n"
"    o.uv = float2((id << 1) & 2, id & 2);\n"
"    o.pos = float4(o.uv * float2(2,-2) + float2(-1,1), 0, 1);\n"
"    return o;\n"
"}\n";

// Template for the post-process pixel shader, now implementing real screen-space
// reflections (SSR) on top of the captured depth buffer:
//   1. reconstruct view-space position from depth + assumed projection params
//      (FOV/near/far — fed in via the cbuffer, same blind-guess approach ReShade
//      uses since the engine's actual projection matrix isn't accessible to us)
//   2. derive the surface normal from screen-space derivatives of that position
//   3. reflect the view vector off the normal
//   4. ray-march in view space, projecting each step back to screen UV and
//      comparing against the depth buffer to find a hit
//   5. sample the color buffer at the hit point, blended in via a fresnel term
//
// The depth-texture declaration and a SampleDepth() helper are filled in at
// runtime via build_ps_source(), because the captured depth buffer might be a
// regular Texture2D or a multisampled Texture2DMS<float,N> — and HLSL requires
// the sample count to be a compile-time constant, so we can't use one static
// shader for both cases. SampleDepth() lets the ray-march sample at arbitrary
// UVs without the main loop caring which variant is active.
static const char *g_ps_template =
"Texture2D tex      : register(t0);\n"
"%s\n"
"SamplerState samp  : register(s0);\n"
"cbuffer Params : register(b0) {\n"
"    float2 texel;\n"
"    float  hasDepth;\n"
"    float  tanHalfFovY;\n"
"    float  aspect;\n"
"    float  zNear;\n"
"    float  zFar;\n"
"    float2 _pad;\n"
"};\n"
"%s\n"
"struct PSIn { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
"\n"
"// Reversed-Z convention (near=1.0, far=0.0) — the modern-engine standard for\n"
"// floating-point depth buffers (D32_FLOAT, which is what we captured), used\n"
"// because it distributes precision far more evenly than the traditional\n"
"// near=0/far=1 mapping. Linearizing inverts back to a real view-space distance.\n"
"float LinearizeDepth(float d) {\n"
"    return (zNear * zFar) / (zNear + d * (zFar - zNear));\n"
"}\n"
"\n"
"float3 ReconstructViewPos(float2 uv, float d) {\n"
"    float ndcX = uv.x * 2.0 - 1.0;\n"
"    float ndcY = 1.0 - uv.y * 2.0;\n"
"    float viewZ = LinearizeDepth(d);\n"
"    return float3(ndcX * viewZ * aspect * tanHalfFovY, ndcY * viewZ * tanHalfFovY, viewZ);\n"
"}\n"
"\n"
"float2 ProjectToUV(float3 viewPos) {\n"
"    float ndcX = viewPos.x / (viewPos.z * aspect * tanHalfFovY);\n"
"    float ndcY = viewPos.y / (viewPos.z * tanHalfFovY);\n"
"    return float2((ndcX + 1.0) * 0.5, (1.0 - ndcY) * 0.5);\n"
"}\n"
"\n"
"// Reconstruct a view-space normal at the given UV using the closest-depth\n"
"// neighbor on each axis. Plain ddx/ddy bridges across silhouette edges and\n"
"// produces garbage; picking the nearer neighbor keeps the normal on the same\n"
"// surface. Also returns an 'edge' magnitude (how much the opposing neighbors\n"
"// disagree in depth) so the caller can reject unreliable silhouette pixels.\n"
"float3 ReconstructNormal(float2 uv, float3 viewPos, out float edge) {\n"
"    float2 du = float2(texel.x, 0.0);\n"
"    float2 dv = float2(0.0, texel.y);\n"
"    float3 posR = ReconstructViewPos(uv + du, SampleDepth(uv + du));\n"
"    float3 posL = ReconstructViewPos(uv - du, SampleDepth(uv - du));\n"
"    float3 posU = ReconstructViewPos(uv + dv, SampleDepth(uv + dv));\n"
"    float3 posD = ReconstructViewPos(uv - dv, SampleDepth(uv - dv));\n"
"    float3 xVec = (abs(posR.z - viewPos.z) < abs(posL.z - viewPos.z)) ? (posR - viewPos) : (viewPos - posL);\n"
"    float3 yVec = (abs(posU.z - viewPos.z) < abs(posD.z - viewPos.z)) ? (posU - viewPos) : (viewPos - posD);\n"
"    edge = abs(posR.z - posL.z) + abs(posU.z - posD.z);\n"
"    float3 n = normalize(cross(yVec, xVec));\n"
"    if (n.z > 0.0) n = -n;\n"   // force the normal to face the camera (origin)
"    return n;\n"
"}\n"
"\n"
"// Per-pixel rotation hash so the small AO sample kernel doesn't produce visible\n"
"// banding — each pixel rotates its sample disk by a different angle.\n"
"float InterleavedGradientNoise(float2 p) {\n"
"    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));\n"
"}\n"
"\n"
"// Screen-space ambient occlusion (Alchemy/HBAO-style). Samples a depth-scaled\n"
"// disk around the pixel, reconstructs each sample's view position, and sums how\n"
"// much nearby geometry rises above this surface's tangent plane. Returns an\n"
"// occlusion factor in [0,1] (1 = fully lit, lower = more occluded). This is the\n"
"// contact-shadow / crevice darkening that adds a lot of perceived depth.\n"
"float ComputeAO(float2 uv, float3 P, float3 N) {\n"
"    const int   AO_SAMPLES  = 12;\n"
"    const float GOLDEN      = 2.39996323;\n"   // golden angle, radians
"    const float worldRadius = 0.6;\n"          // AO footprint in world units (tunable)
"    const float bias        = 0.03;\n"
"    const float intensity   = 1.6;\n"
"\n"
"    // UV-space radius matching worldRadius at this depth, so the AO footprint\n"
"    // is roughly constant in world space regardless of distance from camera.\n"
"    float radUVy = worldRadius / (P.z * tanHalfFovY) * 0.5;\n"
"    float2 radUV = float2(radUVy / aspect, radUVy);\n"
"\n"
"    float rot = InterleavedGradientNoise(uv / texel) * 6.2831853;\n"
"    float occ = 0.0;\n"
"\n"
"    [loop]\n"
"    for (int i = 0; i < AO_SAMPLES; i++) {\n"
"        float ang = rot + (float)i * GOLDEN;\n"
"        float r01 = sqrt(((float)i + 0.5) / (float)AO_SAMPLES);\n"   // even disk coverage
"        float2 sUV = uv + float2(cos(ang), sin(ang)) * r01 * radUV;\n"
"        if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) continue;\n"
"\n"
"        float3 Ps = ReconstructViewPos(sUV, SampleDepth(sUV));\n"
"        float3 V  = Ps - P;\n"
"        float  vv = dot(V, V);\n"
"        float  vn = dot(V, N);\n"
"        float  falloff = saturate(1.0 - vv / (worldRadius * worldRadius));\n"
"        occ += falloff * max(0.0, vn - bias) / (vv + 0.0001);\n"
"    }\n"
"\n"
"    return saturate(1.0 - occ * intensity / (float)AO_SAMPLES);\n"
"}\n"
"\n"
"float4 main(PSIn input) : SV_TARGET {\n"
"    float3 c = tex.Sample(samp, input.uv).rgb;\n"
"    float3 outc = c;\n"
"    float  ao = 1.0;\n"
"\n"
"    if (hasDepth > 0.5) {\n"
"        float d = SampleDepth(input.uv);\n"
"        if (d > 0.0001) {\n"
"            float3 viewPos = ReconstructViewPos(input.uv, d);\n"
"            float  edge;\n"
"            float3 normal  = ReconstructNormal(input.uv, viewPos, edge);\n"
"            ao = ComputeAO(input.uv, viewPos, normal);\n"
"            float3 viewDir = normalize(viewPos);\n"
"            float3 reflDir = reflect(viewDir, normal);\n"
"\n"
"            // Fresnel with NO constant floor: surfaces we look at head-on\n"
"            // (the sand directly below the camera) reflect almost nothing,\n"
"            // while grazing/distant surfaces reflect strongly — which is both\n"
"            // physically correct and exactly what keeps matte ground from\n"
"            // smearing. Power 5 makes the falloff sharp.\n"
"            float facing  = saturate(dot(-viewDir, normal));\n"
"            float fresnel = pow(1.0 - facing, 5.0);\n"
"\n"
"            // Reject silhouette-edge pixels where the normal is unreliable.\n"
"            float edgeReject = saturate(1.0 - edge / max(viewPos.z * 0.35, 0.001));\n"
"\n"
"            // Only bother marching if this pixel could plausibly reflect.\n"
"            float gate = fresnel * edgeReject;\n"
"            if (gate > 0.01 && reflDir.z > -0.999) {\n"
"                float  curStep  = max(viewPos.z * 0.02, 0.04);\n"
"                float3 prevPos  = viewPos + normal * (curStep * 0.5);\n"
"                float3 rayPos   = prevPos;\n"
"                bool   hit      = false;\n"
"                float2 hitUV    = float2(0.0, 0.0);\n"
"\n"
"                [loop]\n"
"                for (int i = 0; i < 32; i++) {\n"
"                    rayPos = prevPos + reflDir * curStep;\n"
"                    if (rayPos.z <= zNear) break;\n"
"\n"
"                    float2 sUV = ProjectToUV(rayPos);\n"
"                    if (sUV.x < 0.0 || sUV.x > 1.0 || sUV.y < 0.0 || sUV.y > 1.0) break;\n"
"\n"
"                    float sceneZ = LinearizeDepth(SampleDepth(sUV));\n"
"                    float diff   = rayPos.z - sceneZ;\n"
"                    if (diff > 0.0 && diff < curStep * 2.0) {\n"
"                        // Binary-search refine for a crisp intersection.\n"
"                        float3 lo = prevPos, hi = rayPos;\n"
"                        [loop]\n"
"                        for (int j = 0; j < 6; j++) {\n"
"                            float3 mid = (lo + hi) * 0.5;\n"
"                            float  mZ  = LinearizeDepth(SampleDepth(ProjectToUV(mid)));\n"
"                            if (mid.z > mZ) hi = mid; else lo = mid;\n"
"                        }\n"
"                        hitUV = ProjectToUV(hi);\n"
"                        hit = true;\n"
"                        break;\n"
"                    }\n"
"                    prevPos = rayPos;\n"
"                    curStep *= 1.20;\n"
"                }\n"
"\n"
"                if (hit) {\n"
"                    // Screen-edge fade: a reflection that lands near the frame\n"
"                    // border is unreliable (offscreen data missing) — fade it\n"
"                    // out smoothly instead of cutting off hard.\n"
"                    float2 ef = smoothstep(0.0, 0.12, hitUV) * smoothstep(0.0, 0.12, 1.0 - hitUV);\n"
"                    float edgeFade = ef.x * ef.y;\n"
"\n"
"                    // Blurred reflection sample (poor-man's roughness): a small\n"
"                    // cross of taps hides ray-march stepping and noise.\n"
"                    float3 reflColor = tex.Sample(samp, hitUV).rgb * 0.5;\n"
"                    reflColor += tex.Sample(samp, hitUV + float2( texel.x,  0.0) * 2.0).rgb * 0.125;\n"
"                    reflColor += tex.Sample(samp, hitUV + float2(-texel.x,  0.0) * 2.0).rgb * 0.125;\n"
"                    reflColor += tex.Sample(samp, hitUV + float2( 0.0,  texel.y) * 2.0).rgb * 0.125;\n"
"                    reflColor += tex.Sample(samp, hitUV + float2( 0.0, -texel.y) * 2.0).rgb * 0.125;\n"
"\n"
"                    float strength = saturate(gate * edgeFade * 0.7);\n"
"                    outc = lerp(c, reflColor, strength);\n"
"                }\n"
"            }\n"
"        }\n"
"    }\n"
"\n"
"    // Apply ambient occlusion: darken the (reflection-composited) color in\n"
"    // contact areas. aoStrength controls how pronounced the darkening is.\n"
"    float aoStrength = 0.85;\n"
"    outc *= lerp(1.0, ao, aoStrength);\n"
"\n"
"    float3 blur = 0;\n"
"    [unroll] for (int x = -1; x <= 1; x++)\n"
"    [unroll] for (int y = -1; y <= 1; y++)\n"
"        blur += tex.Sample(samp, input.uv + float2(x, y) * texel * 1.5).rgb;\n"
"    blur /= 9.0;\n"
"    float3 bloom = max(blur - 0.65, 0.0) * 0.8;\n"
"    outc += bloom;\n"
"    float gray = dot(outc, float3(0.299, 0.587, 0.114));\n"
"    outc = lerp(float3(gray, gray, gray), outc, 1.35);\n"
"    outc = (outc - 0.5) * 1.12 + 0.5;\n"
"    outc += 0.02;\n"
"    return float4(saturate(outc), 1.0);\n"
"}\n";

// Builds a complete PS source string for the given depth-buffer sample count.
// samples == 1 -> plain Texture2D + Sample(); samples > 1 -> Texture2DMS<float,N>
// + Load(pixelCoord, sampleIndex 0) (multisampled textures can't be filtered-sampled).
// Either way we emit a SampleDepth(uv) helper so the SSR ray-march can read the
// depth buffer at arbitrary screen positions without caring which variant is active.
// Caller must free() the returned buffer.
static char *build_ps_source(UINT samples) {
    char decl[160];
    char fn[384];
    char *out;
    int need;

    if (samples > 1) {
        _snprintf(decl, sizeof(decl), "Texture2DMS<float, %u> depthTex : register(t1);", samples);
        _snprintf(fn, sizeof(fn),
            "float SampleDepth(float2 uv) {\n"
            "    int2 px = int2(uv / texel);\n"
            "    return depthTex.Load(px, 0).r;\n"
            "}\n");
    } else {
        _snprintf(decl, sizeof(decl), "Texture2D depthTex : register(t1);");
        _snprintf(fn, sizeof(fn),
            "float SampleDepth(float2 uv) {\n"
            "    return depthTex.Sample(samp, uv).r;\n"
            "}\n");
    }
    decl[sizeof(decl) - 1] = '\0';
    fn[sizeof(fn) - 1] = '\0';

    need = (int)(strlen(g_ps_template) + strlen(decl) + strlen(fn) + 1);
    out = (char *)malloc((size_t)need);
    if (out) _snprintf(out, (size_t)need, g_ps_template, decl, fn);
    return out;
}

// (Re)compiles and swaps in the pixel shader for the given depth-buffer sample
// count. Called once at pipeline build (samples=1, "no depth yet" variant) and
// again whenever capture_depth() discovers the actual depth resource uses a
// different sample count (e.g. an MSAA scene depth buffer).
static BOOL recompile_pixel_shader(UINT samples) {
    char *src;
    ID3DBlob *ps_blob = NULL, *err_blob = NULL;
    ID3D11PixelShader *new_ps = NULL;
    HRESULT hr;

    if (!p_D3DCompile || !g_dev) return FALSE;

    src = build_ps_source(samples);
    if (!src) return FALSE;

    hr = p_D3DCompile(src, strlen(src), "ferns_ps", NULL, NULL, "main", "ps_4_0", 0, 0, &ps_blob, &err_blob);
    free(src);
    if (FAILED(hr)) {
        log_line("[shader] PS recompile (samples=%u) failed: %s", samples,
                 err_blob ? (char *)ID3D10Blob_GetBufferPointer(err_blob) : "?");
        if (err_blob) ID3D10Blob_Release(err_blob);
        return FALSE;
    }
    if (err_blob) ID3D10Blob_Release(err_blob);

    hr = ID3D11Device_CreatePixelShader(g_dev, ID3D10Blob_GetBufferPointer(ps_blob),
                                        ID3D10Blob_GetBufferSize(ps_blob), NULL, &new_ps);
    ID3D10Blob_Release(ps_blob);
    if (FAILED(hr)) {
        log_line("[shader] CreatePixelShader (samples=%u) failed hr=0x%lx", samples, hr);
        return FALSE;
    }

    if (g_ps) ID3D11PixelShader_Release(g_ps);
    g_ps = new_ps;
    g_compiled_ps_samples = samples;
    log_line("[shader] pixel shader (re)compiled for depth sample count = %u", samples);
    return TRUE;
}

static void release_pipeline(void) {
    if (g_depthstencil) { ID3D11DepthStencilState_Release(g_depthstencil); g_depthstencil = NULL; }
    if (g_raster)       { ID3D11RasterizerState_Release(g_raster);         g_raster = NULL; }
    if (g_blend)        { ID3D11BlendState_Release(g_blend);               g_blend = NULL; }
    if (g_cbuf)     { ID3D11Buffer_Release(g_cbuf);                 g_cbuf = NULL; }
    if (g_sampler)  { ID3D11SamplerState_Release(g_sampler);        g_sampler = NULL; }
    if (g_ps)       { ID3D11PixelShader_Release(g_ps);              g_ps = NULL; }
    if (g_vs)       { ID3D11VertexShader_Release(g_vs);             g_vs = NULL; }
    if (g_copy_srv) { ID3D11ShaderResourceView_Release(g_copy_srv); g_copy_srv = NULL; }
    if (g_copy_tex) { ID3D11Texture2D_Release(g_copy_tex);          g_copy_tex = NULL; }
    if (g_depth_srv) { ID3D11ShaderResourceView_Release(g_depth_srv); g_depth_srv = NULL; }
    if (g_depth_tex) { ID3D11Texture2D_Release(g_depth_tex);          g_depth_tex = NULL; }
    if (g_depth_staging) { ID3D11Texture2D_Release(g_depth_staging); g_depth_staging = NULL; }
    if (g_candidate_dsv) { ID3D11DepthStencilView_Release(g_candidate_dsv); g_candidate_dsv = NULL; }
    if (g_dsv_resource) { ID3D11Resource_Release(g_dsv_resource); g_dsv_resource = NULL; }
    g_dsv_resource_samples = 0;
    g_depth_samples = 1;
    g_compiled_ps_samples = 0;
    g_depth_w = g_depth_h = 0;
    g_depth_fmt = DXGI_FORMAT_UNKNOWN;
    g_depth_available = FALSE;

    if (g_ctx)      { ID3D11DeviceContext_Release(g_ctx);           g_ctx = NULL; }
    if (g_dev)      { ID3D11Device_Release(g_dev);                  g_dev = NULL; }
}

// Maps a depth-stencil format to a same-bits-layout typeless format (so we can
// create a shader-readable copy via CopyResource) and a depth-only SRV format
// to sample it as a plain grayscale value.
static BOOL map_depth_format(DXGI_FORMAT in, DXGI_FORMAT *typeless, DXGI_FORMAT *srv) {
    switch (in) {
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            *typeless = DXGI_FORMAT_R24G8_TYPELESS;
            *srv      = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
            return TRUE;
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
            *typeless = DXGI_FORMAT_R32_TYPELESS;
            *srv      = DXGI_FORMAT_R32_FLOAT;
            return TRUE;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            *typeless = DXGI_FORMAT_R32G8X24_TYPELESS;
            *srv      = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
            return TRUE;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            *typeless = DXGI_FORMAT_R16_TYPELESS;
            *srv      = DXGI_FORMAT_R16_UNORM;
            return TRUE;
        default:
            return FALSE;
    }
}

// Uses the depth-stencil resource directly cached by Hook_CreateDepthStencilView
// (matched against the output resolution at creation time) and copies it into a
// shader-readable texture for this frame's post-process pass. This bypasses OM
// binding-tracking entirely — we hold a ref-counted pointer to the resource
// itself, so we can copy from it regardless of how/when the engine binds it.
static void capture_depth(void) {
    ID3D11Texture2D  *dsv_tex = NULL;
    D3D11_TEXTURE2D_DESC ddesc;
    DXGI_FORMAT typeless_fmt = DXGI_FORMAT_UNKNOWN, srv_fmt = DXGI_FORMAT_UNKNOWN;

    static LONG diag_n;
    LONG dn = InterlockedIncrement(&diag_n);
    BOOL diag = (dn <= 8 || (dn % 600) == 0);

    g_depth_available = FALSE;

    if (!g_dsv_resource) {
        if (diag) log_line("[shader] depth-diag #%ld: no candidate depth resource captured yet", dn);
        return;
    }

    if (FAILED(ID3D11Resource_QueryInterface(g_dsv_resource, &IID_ID3D11Texture2D, (void **)&dsv_tex))) {
        if (diag) log_line("[shader] depth-diag #%ld: candidate resource isn't a Texture2D", dn);
        return;
    }

    ID3D11Texture2D_GetDesc(dsv_tex, &ddesc);

    if (diag) {
        log_line("[shader] depth-diag #%ld: candidate resource %ux%u fmt=%d (target %ux%u) mapped=%d",
                 dn, ddesc.Width, ddesc.Height, (int)ddesc.Format, g_w, g_h,
                 map_depth_format(ddesc.Format, &typeless_fmt, &srv_fmt));
    }

    // The hook already filtered by size; format must still be one we can copy
    // into a shader-readable typeless texture.
    if (ddesc.Width != g_w || ddesc.Height != g_h || !map_depth_format(ddesc.Format, &typeless_fmt, &srv_fmt)) {
        ID3D11Texture2D_Release(dsv_tex);
        return;
    }

    if (!g_depth_tex || g_depth_w != ddesc.Width || g_depth_h != ddesc.Height ||
        g_depth_fmt != typeless_fmt || g_depth_samples != ddesc.SampleDesc.Count) {
        D3D11_TEXTURE2D_DESC cdesc;
        D3D11_SHADER_RESOURCE_VIEW_DESC srvdesc;

        if (g_depth_srv) { ID3D11ShaderResourceView_Release(g_depth_srv); g_depth_srv = NULL; }
        if (g_depth_tex) { ID3D11Texture2D_Release(g_depth_tex);          g_depth_tex = NULL; }

        cdesc = ddesc;
        cdesc.Format = typeless_fmt;
        cdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        cdesc.Usage = D3D11_USAGE_DEFAULT;
        cdesc.CPUAccessFlags = 0;
        cdesc.MiscFlags = 0;
        // cdesc inherited SampleDesc from ddesc — must match the source resource
        // exactly (CopyResource requires identical multisample state).
        if (FAILED(ID3D11Device_CreateTexture2D(g_dev, &cdesc, NULL, &g_depth_tex))) {
            ID3D11Texture2D_Release(dsv_tex);
            return;
        }

        ZeroMemory(&srvdesc, sizeof(srvdesc));
        srvdesc.Format = srv_fmt;
        if (ddesc.SampleDesc.Count > 1) {
            srvdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            srvdesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srvdesc.Texture2D.MipLevels = 1;
        }
        if (FAILED(ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_depth_tex, &srvdesc, &g_depth_srv))) {
            ID3D11Texture2D_Release(g_depth_tex);
            g_depth_tex = NULL;
            ID3D11Texture2D_Release(dsv_tex);
            return;
        }

        g_depth_w = ddesc.Width;
        g_depth_h = ddesc.Height;
        g_depth_fmt = typeless_fmt;
        g_depth_samples = ddesc.SampleDesc.Count;
        log_line("[shader] depth capture ready: %ux%u srcfmt=%d samples=%u",
                 g_depth_w, g_depth_h, (int)ddesc.Format, g_depth_samples);

        if (g_compiled_ps_samples != g_depth_samples)
            recompile_pixel_shader(g_depth_samples);
    }

    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_depth_tex, (ID3D11Resource *)dsv_tex);
    ID3D11Texture2D_Release(dsv_tex);
    g_depth_available = TRUE;

    // Periodic CPU readback: log raw values from the center of the captured
    // buffer so we can tell whether it truly contains zeros or just visually
    // crushed data (the visualization shader alone can't prove which).
    // Only possible for non-multisampled buffers — staging textures and
    // CopySubresourceRegion can't be multisampled in D3D11.
    if (g_depth_samples == 1) {
        static LONG read_n;
        LONG rn = InterlockedIncrement(&read_n);
        if (rn == 1 || (rn % 300) == 0) {
            if (!g_depth_staging) {
                D3D11_TEXTURE2D_DESC sdesc;
                ZeroMemory(&sdesc, sizeof(sdesc));
                sdesc.Width = 4;
                sdesc.Height = 1;
                sdesc.MipLevels = 1;
                sdesc.ArraySize = 1;
                sdesc.Format = g_depth_fmt;
                sdesc.SampleDesc.Count = 1;
                sdesc.Usage = D3D11_USAGE_STAGING;
                sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                ID3D11Device_CreateTexture2D(g_dev, &sdesc, NULL, &g_depth_staging);
            }
            if (g_depth_staging) {
                D3D11_BOX box;
                D3D11_MAPPED_SUBRESOURCE mapped;
                box.left = g_depth_w / 2;  box.right = box.left + 4;
                box.top  = g_depth_h / 2;  box.bottom = box.top + 1;
                box.front = 0; box.back = 1;
                ID3D11DeviceContext_CopySubresourceRegion(g_ctx, (ID3D11Resource *)g_depth_staging, 0,
                                                           0, 0, 0, (ID3D11Resource *)g_depth_tex, 0, &box);
                if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_depth_staging, 0,
                                                       D3D11_MAP_READ, 0, &mapped))) {
                    if (g_depth_fmt == DXGI_FORMAT_R32_TYPELESS) {
                        float *v = (float *)mapped.pData;
                        log_line("[shader] depth-readback #%ld center row (float32): %.8f %.8f %.8f %.8f",
                                 rn, v[0], v[1], v[2], v[3]);
                    } else {
                        UINT32 *v = (UINT32 *)mapped.pData;
                        log_line("[shader] depth-readback #%ld center row (raw hex): %08lx %08lx %08lx %08lx",
                                 rn, (unsigned long)v[0], (unsigned long)v[1], (unsigned long)v[2], (unsigned long)v[3]);
                    }
                    ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_depth_staging, 0);
                }
            }
        }
    }
}

static BOOL build_pipeline(IDXGISwapChain *sc) {
    HRESULT hr;
    ID3D11Texture2D *backbuf = NULL;
    ID3DBlob *vs_blob = NULL, *err_blob = NULL;
    D3D11_TEXTURE2D_DESC bbdesc, cdesc;
    D3D11_SAMPLER_DESC sdesc;
    D3D11_BUFFER_DESC bdesc;

    release_pipeline();

    hr = IDXGISwapChain_GetDevice(sc, &IID_ID3D11Device, (void **)&g_dev);
    if (FAILED(hr)) { log_line("[shader] GetDevice failed hr=0x%lx", hr); return FALSE; }
    ID3D11Device_GetImmediateContext(g_dev, &g_ctx);
    hook_context(g_ctx);
    hook_device(g_dev);

    hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&backbuf);
    if (FAILED(hr)) { log_line("[shader] GetBuffer failed hr=0x%lx", hr); return FALSE; }

    ID3D11Texture2D_GetDesc(backbuf, &bbdesc);
    g_w = bbdesc.Width;
    g_h = bbdesc.Height;

    // NOTE: do NOT cache an RTV on the back buffer here — flip-model swap
    // chains rotate through a ring of distinct buffer resources, so
    // GetBuffer(0) returns a different underlying resource each frame.
    // The render target view is created fresh per-frame in render_effect().

    cdesc = bbdesc;
    cdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    cdesc.Usage = D3D11_USAGE_DEFAULT;
    cdesc.CPUAccessFlags = 0;
    cdesc.MiscFlags = 0;
    hr = ID3D11Device_CreateTexture2D(g_dev, &cdesc, NULL, &g_copy_tex);
    ID3D11Texture2D_Release(backbuf);
    if (FAILED(hr)) { log_line("[shader] CreateTexture2D(copy) failed hr=0x%lx", hr); return FALSE; }

    hr = ID3D11Device_CreateShaderResourceView(g_dev, (ID3D11Resource *)g_copy_tex, NULL, &g_copy_srv);
    if (FAILED(hr)) { log_line("[shader] CreateShaderResourceView failed hr=0x%lx", hr); return FALSE; }

    if (!p_D3DCompile) {
        g_d3dcompiler = LoadLibraryA("d3dcompiler_47.dll");
        if (g_d3dcompiler)
            p_D3DCompile = (PFN_D3DCompile)GetProcAddress(g_d3dcompiler, "D3DCompile");
    }
    if (!p_D3DCompile) { log_line("[shader] D3DCompile unavailable"); return FALSE; }

    hr = p_D3DCompile(g_vs_src, strlen(g_vs_src), "ferns_vs", NULL, NULL, "main", "vs_4_0", 0, 0, &vs_blob, &err_blob);
    if (FAILED(hr)) {
        log_line("[shader] VS compile failed: %s", err_blob ? (char *)ID3D10Blob_GetBufferPointer(err_blob) : "?");
        if (err_blob) ID3D10Blob_Release(err_blob);
        return FALSE;
    }
    hr = ID3D11Device_CreateVertexShader(g_dev, ID3D10Blob_GetBufferPointer(vs_blob), ID3D10Blob_GetBufferSize(vs_blob), NULL, &g_vs);
    ID3D10Blob_Release(vs_blob);
    if (FAILED(hr)) { log_line("[shader] CreateVertexShader failed hr=0x%lx", hr); return FALSE; }

    g_compiled_ps_samples = 0;   // force (re)compile of the depth-aware PS variant below
    if (!recompile_pixel_shader(1)) { log_line("[shader] initial PS compile failed"); return FALSE; }

    ZeroMemory(&sdesc, sizeof(sdesc));
    sdesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sdesc.AddressU = sdesc.AddressV = sdesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sdesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sdesc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = ID3D11Device_CreateSamplerState(g_dev, &sdesc, &g_sampler);
    if (FAILED(hr)) { log_line("[shader] CreateSamplerState failed hr=0x%lx", hr); return FALSE; }

    ZeroMemory(&bdesc, sizeof(bdesc));
    bdesc.ByteWidth = 32;
    bdesc.Usage = D3D11_USAGE_DYNAMIC;
    bdesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(g_dev, &bdesc, NULL, &g_cbuf);
    if (FAILED(hr)) { log_line("[shader] CreateBuffer(cbuf) failed hr=0x%lx", hr); return FALSE; }

    // Force known-good state for our draw — the game may leave blend/raster/
    // depth state bound in a way that would silently discard our fragments
    // (e.g. additive blending, scissor clipping, depth testing).
    {
        D3D11_BLEND_DESC bldesc;
        D3D11_RASTERIZER_DESC rsdesc;
        D3D11_DEPTH_STENCIL_DESC dsdesc;

        ZeroMemory(&bldesc, sizeof(bldesc));
        bldesc.RenderTarget[0].BlendEnable = FALSE;
        bldesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        hr = ID3D11Device_CreateBlendState(g_dev, &bldesc, &g_blend);
        if (FAILED(hr)) { log_line("[shader] CreateBlendState failed hr=0x%lx", hr); return FALSE; }

        ZeroMemory(&rsdesc, sizeof(rsdesc));
        rsdesc.FillMode = D3D11_FILL_SOLID;
        rsdesc.CullMode = D3D11_CULL_NONE;
        rsdesc.DepthClipEnable = TRUE;
        rsdesc.ScissorEnable = FALSE;
        hr = ID3D11Device_CreateRasterizerState(g_dev, &rsdesc, &g_raster);
        if (FAILED(hr)) { log_line("[shader] CreateRasterizerState failed hr=0x%lx", hr); return FALSE; }

        ZeroMemory(&dsdesc, sizeof(dsdesc));
        dsdesc.DepthEnable = FALSE;
        dsdesc.StencilEnable = FALSE;
        hr = ID3D11Device_CreateDepthStencilState(g_dev, &dsdesc, &g_depthstencil);
        if (FAILED(hr)) { log_line("[shader] CreateDepthStencilState failed hr=0x%lx", hr); return FALSE; }
    }

    log_line("[shader] pipeline built: %ux%u", g_w, g_h);
    return TRUE;
}

static void render_effect(IDXGISwapChain *sc) {
    ID3D11Texture2D *backbuf = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    D3D11_VIEWPORT vp;
    ID3D11RenderTargetView   *rtvs[1];
    ID3D11ShaderResourceView *srvs[1];
    ID3D11ShaderResourceView *null_srv[1] = { NULL };
    ID3D11SamplerState       *samplers[1];
    ID3D11Buffer             *cbufs[1];
    HRESULT hr;

    if (g_pipe_state == PIPE_FAILED) return;

    if (g_pipe_state == PIPE_NONE) {
        g_pipe_state = build_pipeline(sc) ? PIPE_OK : PIPE_FAILED;
        if (g_pipe_state == PIPE_FAILED) { release_pipeline(); return; }
    }

    // Read whatever depth-stencil buffer is currently bound BEFORE we touch
    // OM state ourselves — this is our only chance to see the game's state.
    capture_depth();

    if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&backbuf))) return;
    ID3D11Texture2D_GetDesc(backbuf, &desc);
    if (desc.Width != g_w || desc.Height != g_h) {
        ID3D11Texture2D_Release(backbuf);
        log_line("[shader] size changed -> rebuilding pipeline");
        g_pipe_state = build_pipeline(sc) ? PIPE_OK : PIPE_FAILED;
        if (g_pipe_state == PIPE_FAILED) { release_pipeline(); return; }
        if (FAILED(IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&backbuf))) return;
    }

    // Flip-model swap chains rotate distinct buffer resources through index 0,
    // so the render target view must be (re)created fresh from the CURRENT
    // back buffer every frame rather than cached across Present calls.
    hr = ID3D11Device_CreateRenderTargetView(g_dev, (ID3D11Resource *)backbuf, NULL, &rtv);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(backbuf);
        return;
    }

    ID3D11DeviceContext_CopyResource(g_ctx, (ID3D11Resource *)g_copy_tex, (ID3D11Resource *)backbuf);
    ID3D11Texture2D_Release(backbuf);

    if (SUCCEEDED(ID3D11DeviceContext_Map(g_ctx, (ID3D11Resource *)g_cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        float *p = (float *)mapped.pData;
        p[0] = 1.0f / (float)g_w;
        p[1] = 1.0f / (float)g_h;
        p[2] = g_depth_available ? 1.0f : 0.0f;
        p[3] = tanf(SSR_FOV_DEGREES * (3.14159265f / 180.0f) * 0.5f);
        p[4] = (float)g_w / (float)g_h;
        p[5] = SSR_NEAR_PLANE;
        p[6] = SSR_FAR_PLANE;
        p[7] = 0.0f;
        ID3D11DeviceContext_Unmap(g_ctx, (ID3D11Resource *)g_cbuf, 0);
    }

    vp.TopLeftX = 0.0f; vp.TopLeftY = 0.0f;
    vp.Width = (float)g_w; vp.Height = (float)g_h;
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(g_ctx, 1, &vp);

    rtvs[0] = rtv;
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 1, rtvs, NULL);

    {
        float blend_factor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ID3D11DeviceContext_OMSetBlendState(g_ctx, g_blend, blend_factor, 0xFFFFFFFF);
    }
    ID3D11DeviceContext_OMSetDepthStencilState(g_ctx, g_depthstencil, 0);
    ID3D11DeviceContext_RSSetState(g_ctx, g_raster);

    ID3D11DeviceContext_IASetInputLayout(g_ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11DeviceContext_VSSetShader(g_ctx, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_ctx, g_ps, NULL, 0);

    srvs[0] = g_copy_srv;
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, srvs);
    if (g_depth_available) {
        ID3D11ShaderResourceView *depth_srvs[1] = { g_depth_srv };
        ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, depth_srvs);
    }
    samplers[0] = g_sampler;
    ID3D11DeviceContext_PSSetSamplers(g_ctx, 0, 1, samplers);
    cbufs[0] = g_cbuf;
    ID3D11DeviceContext_PSSetConstantBuffers(g_ctx, 0, 1, cbufs);

    ID3D11DeviceContext_Draw(g_ctx, 3, 0);

    // unbind so the texture isn't simultaneously an SRV input and an RTV target next frame
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 0, 1, null_srv);
    ID3D11DeviceContext_PSSetShaderResources(g_ctx, 1, 1, null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(g_ctx, 0, NULL, NULL);
    ID3D11RenderTargetView_Release(rtv);
}

static HRESULT WINAPI Hook_Present(IDXGISwapChain *swapchain, UINT sync, UINT flags) {
    LONG n = InterlockedIncrement(&g_present_count);
    if (n <= 5 || (n % 600) == 0)   // first few + roughly once every ~10s @60fps
        log_line("[proxy] Present #%ld", n);
    render_effect(swapchain);
    return orig_Present(swapchain, sync, flags);
}

static HRESULT WINAPI Hook_CreateSwapChain(IDXGIFactory *factory, IUnknown *device,
                                           DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **out) {
    HRESULT hr = orig_CreateSwapChain(factory, device, desc, out);
    if (SUCCEEDED(hr) && out && *out) {
        log_line("[proxy] swap chain created -> hooking Present");
        void *prev = patch_vtable_slot(*out, VT_SWAPCHAIN_PRESENT, (void *)Hook_Present);
        if (prev) orig_Present = (PFN_Present)prev;
    }
    return hr;
}

static void hook_factory(void *factory) {
    void *prev = patch_vtable_slot(factory, VT_FACTORY_CREATESWAPCHAIN, (void *)Hook_CreateSwapChain);
    if (prev) {
        orig_CreateSwapChain = (PFN_CreateSwapChain)prev;
        log_line("[proxy] factory hooked -> CreateSwapChain");
    }
}

static HMODULE g_real;
static FILE   *g_log;

typedef HRESULT (WINAPI *PFN_CreateDXGIFactory)(REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory1)(REFIID, void **);
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(UINT, REFIID, void **);
typedef HRESULT (WINAPI *PFN_DXGIGetDebugInterface1)(UINT, REFIID, void **);
typedef HRESULT (WINAPI *PFN_DXGIDeclareAdapterRemovalSupport)(void);

static PFN_CreateDXGIFactory                p_CreateDXGIFactory;
static PFN_CreateDXGIFactory1               p_CreateDXGIFactory1;
static PFN_CreateDXGIFactory2               p_CreateDXGIFactory2;
static PFN_DXGIGetDebugInterface1           p_DXGIGetDebugInterface1;
static PFN_DXGIDeclareAdapterRemovalSupport p_DXGIDeclareAdapterRemovalSupport;

static void log_line(const char *fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfprintf(g_log, fmt, a);
    va_end(a);
    fputc('\n', g_log);
    fflush(g_log);
}

static void load_real(void) {
    if (g_real) return;

    char dir[MAX_PATH];
    if (GetModuleFileNameA(NULL, dir, MAX_PATH)) {
        char *slash = strrchr(dir, '\\');
        if (slash) *slash = '\0';
        char logpath[MAX_PATH];
        wsprintfA(logpath, "%s\\fernsshader.log", dir);
        g_log = fopen(logpath, "a");
    }

    char path[MAX_PATH];
    GetSystemDirectoryA(path, MAX_PATH);
    lstrcatA(path, "\\dxgi.dll");

    g_real = LoadLibraryA(path);
    log_line("[proxy] loading real dxgi from: %s -> %s", path, g_real ? "ok" : "FAILED");
    if (!g_real) return;

    p_CreateDXGIFactory  = (PFN_CreateDXGIFactory) GetProcAddress(g_real, "CreateDXGIFactory");
    p_CreateDXGIFactory1 = (PFN_CreateDXGIFactory1)GetProcAddress(g_real, "CreateDXGIFactory1");
    p_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)GetProcAddress(g_real, "CreateDXGIFactory2");
    p_DXGIGetDebugInterface1           = (PFN_DXGIGetDebugInterface1)GetProcAddress(g_real, "DXGIGetDebugInterface1");
    p_DXGIDeclareAdapterRemovalSupport = (PFN_DXGIDeclareAdapterRemovalSupport)GetProcAddress(g_real, "DXGIDeclareAdapterRemovalSupport");

    log_line("[proxy] resolved: factory=%p factory1=%p factory2=%p",
             (void*)p_CreateDXGIFactory, (void*)p_CreateDXGIFactory1, (void*)p_CreateDXGIFactory2);
}

HRESULT WINAPI Hook_CreateDXGIFactory(REFIID riid, void **ppFactory) {
    load_real();
    HRESULT hr = p_CreateDXGIFactory ? p_CreateDXGIFactory(riid, ppFactory) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory(*ppFactory);
    return hr;
}
HRESULT WINAPI Hook_CreateDXGIFactory1(REFIID riid, void **ppFactory) {
    load_real();
    HRESULT hr = p_CreateDXGIFactory1 ? p_CreateDXGIFactory1(riid, ppFactory) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory(*ppFactory);
    return hr;
}
HRESULT WINAPI Hook_CreateDXGIFactory2(UINT flags, REFIID riid, void **ppFactory) {
    load_real();
    HRESULT hr = p_CreateDXGIFactory2 ? p_CreateDXGIFactory2(flags, riid, ppFactory) : E_NOINTERFACE;
    if (SUCCEEDED(hr) && ppFactory && *ppFactory) hook_factory(*ppFactory);
    return hr;
}
HRESULT WINAPI Hook_DXGIGetDebugInterface1(UINT flags, REFIID riid, void **ppDebug) {
    load_real();
    return p_DXGIGetDebugInterface1 ? p_DXGIGetDebugInterface1(flags, riid, ppDebug) : E_NOINTERFACE;
}
HRESULT WINAPI Hook_DXGIDeclareAdapterRemovalSupport(void) {
    load_real();
    return p_DXGIDeclareAdapterRemovalSupport ? p_DXGIDeclareAdapterRemovalSupport() : S_OK;
}

BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID reserved) {
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(hinst);
        load_real();
        log_line("[proxy] attached to process");
    } else if (reason == DLL_PROCESS_DETACH) {
        log_line("[proxy] detaching");
        if (g_log) fclose(g_log);
        if (g_real) FreeLibrary(g_real);
    }
    return TRUE;
}
