#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <algorithm>

static unsigned nullFailures=0, explicitFailures=0, uploadFailures=0;
template<class T> static void release(T *&p){if(p){p->Release();p=nullptr;}}
static unsigned rows(unsigned n){return (n+3u)/4u;}
static unsigned pitch(unsigned n){return rows(n)*16u;}
static std::vector<uint8_t> payload(unsigned size,unsigned level){
    std::vector<uint8_t> b(pitch(size)*rows(size));
    for(unsigned off=0;off<b.size();off+=16){b[off]=0x40;for(unsigned j=1;j<16;++j)b[off+j]=uint8_t(0x35u^(level*0x29u)^((off/16u)*0x13u)^(j*0x0Du));}
    return b;
}
static ID3D11Texture2D *chain(ID3D11Device *d,DXGI_FORMAT fmt,bool staging,bool initial){
    D3D11_TEXTURE2D_DESC desc{};desc.Width=24;desc.Height=24;desc.MipLevels=5;desc.ArraySize=1;desc.Format=fmt;desc.SampleDesc.Count=1;
    desc.Usage=staging?D3D11_USAGE_STAGING:D3D11_USAGE_DEFAULT;
    desc.CPUAccessFlags=staging?(D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE):0;
    desc.BindFlags=staging?0:D3D11_BIND_SHADER_RESOURCE;
    std::vector<std::vector<uint8_t>> bytes;D3D11_SUBRESOURCE_DATA data[5]{};
    for(unsigned level=0;level<5;++level){unsigned n=std::max(1u,24u>>level);bytes.push_back(payload(n,level));}
    for(unsigned level=0;level<5;++level){unsigned n=std::max(1u,24u>>level);data[level].pSysMem=bytes[level].data();data[level].SysMemPitch=pitch(n);data[level].SysMemSlicePitch=pitch(n)*rows(n);}
    ID3D11Texture2D *t{};auto hr=d->CreateTexture2D(&desc,initial?data:nullptr,&t);
    std::printf("CREATE_CHAIN: format=%u staging=%u initial=%u hr=0x%08lX\n",unsigned(fmt),staging,initial,(unsigned long)hr);return t;
}
static bool readback(ID3D11Device *d,ID3D11DeviceContext *ctx,ID3D11Texture2D *gpu,DXGI_FORMAT fmt,unsigned level,bool nullBox,char const *phase){
    unsigned n=std::max(1u,24u>>level),prefix=0;while(((n<<prefix)&3u)&&prefix<2)++prefix;
    D3D11_TEXTURE2D_DESC desc{};desc.Width=n<<prefix;desc.Height=n<<prefix;desc.MipLevels=prefix+1;desc.ArraySize=1;desc.Format=fmt;desc.SampleDesc.Count=1;
    desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ|D3D11_CPU_ACCESS_WRITE;
    ID3D11Texture2D *stage{};auto hr=d->CreateTexture2D(&desc,nullptr,&stage);
    if(FAILED(hr)){std::printf("FAIL: staging creation hr=0x%08lX\n",(unsigned long)hr);return false;}
    D3D11_MAPPED_SUBRESOURCE map{};hr=ctx->Map(stage,prefix,D3D11_MAP_WRITE,0,&map);
    if(FAILED(hr)||!map.pData){std::printf("FAIL: staging initialize map hr=0x%08lX\n",(unsigned long)hr);release(stage);return false;}
    for(unsigned row=0;row<rows(n);++row)std::memset((uint8_t*)map.pData+row*map.RowPitch,0xA5,pitch(n));
    ctx->Unmap(stage,prefix);
    D3D11_BOX box{0,0,0,n,n,1};ctx->CopySubresourceRegion(stage,prefix,0,0,0,gpu,level,nullBox?nullptr:&box);
    map={};hr=ctx->Map(stage,prefix,D3D11_MAP_READ,0,&map);
    bool match=SUCCEEDED(hr)&&map.pData;auto expect=payload(n,level);
    if(match){for(unsigned row=0;row<rows(n);++row)if(std::memcmp((uint8_t*)map.pData+row*map.RowPitch,expect.data()+row*pitch(n),pitch(n)))match=false;}
    std::printf("NATIVE_BC_CASE: phase=%s mode=%s format=%u mip=%u extent=%ux%u dstNativeMip=%u mapHr=0x%08lX result=%s\n",phase,nullBox?"whole-null-box":"explicit-box",unsigned(fmt),level,n,n,prefix,(unsigned long)hr,match?"EXACT_MATCH":"MISMATCH_OR_MAP_FAILURE");
    if(!match&&SUCCEEDED(hr)&&map.pData){std::printf("EXPECTED:");for(unsigned i=0;i<16;++i)std::printf(" %02X",expect[i]);std::printf("\nACTUAL:");for(unsigned i=0;i<16;++i)std::printf(" %02X",((uint8_t*)map.pData)[i]);std::puts("");}
    if(SUCCEEDED(hr)&&map.pData)ctx->Unmap(stage,prefix);release(stage);std::fflush(stdout);return match;
}
int main(int argc,char **argv){
    bool relax=false,compareExplicit=false;for(int i=1;i<argc;++i){if(!std::strcmp(argv[i],"--allow-non-intel-test"))relax=true;else if(!std::strcmp(argv[i],"--compare-explicit"))compareExplicit=true;else{return 2;}}
    std::setvbuf(stdout,nullptr,_IONBF,0);std::puts("Native BC7 directed D3D11 comparison. No Vulkan loader or game is used.");
    IDXGIFactory1 *factory{};auto hr=CreateDXGIFactory1(__uuidof(IDXGIFactory1),(void**)&factory);if(FAILED(hr))return 3;
    IDXGIAdapter1 *chosen{};
    for(unsigned i=0;;++i){IDXGIAdapter1 *a{};if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);
        std::printf("ADAPTER: vendor=0x%04X device=0x%04X flags=0x%X\n",desc.VendorId,desc.DeviceId,desc.Flags);
        if(!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)&&desc.VendorId!=0x1414&&(desc.VendorId==0x8086||relax)){chosen=a;break;}release(a);}
    release(factory);if(!chosen){std::puts("FAIL: no hardware Intel adapter; no WARP fallback attempted.");return 6;}
    ID3D11Device *d{};ID3D11DeviceContext *ctx{};D3D_FEATURE_LEVEL fl{};
    hr=D3D11CreateDevice(chosen,D3D_DRIVER_TYPE_UNKNOWN,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&d,&fl,&ctx);release(chosen);
    if(FAILED(hr)){std::printf("FAIL: hardware device hr=0x%08lX\n",(unsigned long)hr);return 7;}
    std::printf("DEVICE: featureLevel=0x%X allowNonIntelSandbox=%u noSoftwareFallback=1\n",unsigned(fl),relax);
    for(auto fmt:{DXGI_FORMAT_BC7_UNORM,DXGI_FORMAT_BC7_UNORM_SRGB}){
        auto known=chain(d,fmt,false,true);if(!known){++nullFailures;continue;}
        bool control=true;
        for(unsigned level=0;level<5;++level){if(!readback(d,ctx,known,fmt,level,true,"initial-GPU-readback")){++nullFailures;control=false;}}
        release(known);
        if(!control){std::puts("UPLOAD_PHASE: INCONCLUSIVE because initialized-GPU null readback control failed.");++uploadFailures;continue;}
        auto src=chain(d,fmt,true,true);auto dst=chain(d,fmt,false,false);
        if(!src||!dst){++uploadFailures;release(src);release(dst);continue;}
        for(unsigned level=0;level<5;++level)ctx->CopySubresourceRegion(dst,level,0,0,0,src,level,nullptr);
        for(unsigned level=0;level<5;++level)if(!readback(d,ctx,dst,fmt,level,true,"staging-upload-plus-readback"))++uploadFailures;
        release(src);release(dst);
    }
    auto removed=d->GetDeviceRemovedReason();std::printf("DEVICE_REMOVED_REASON: 0x%08lX\n",(unsigned long)removed);
    bool pass=!nullFailures&&!uploadFailures&&SUCCEEDED(removed);
    if(compareExplicit){
        std::puts("COMPARISON_ONLY: legacy explicit-box calls run after the required whole-copy controls; their results do not replace those controls.");
        for(auto fmt:{DXGI_FORMAT_BC7_UNORM,DXGI_FORMAT_BC7_UNORM_SRGB}){
            auto known=chain(d,fmt,false,true);if(!known){++explicitFailures;continue;}
            for(unsigned level=0;level<5;++level)if(!readback(d,ctx,known,fmt,level,false,"optional-legacy-box-comparison"))++explicitFailures;
            release(known);
        }
        std::printf("POST_COMPARISON_DEVICE_REASON: 0x%08lX\n",(unsigned long)d->GetDeviceRemovedReason());
    }else std::puts("LEGACY_EXPLICIT_COMPARISON: skipped; use --compare-explicit only for a separate comparison run.");
    std::printf("NATIVE_BC_SUMMARY: explicitReadbackFailures=%u nullReadbackFailures=%u nullUploadRoundtripFailures=%u\n",explicitFailures,nullFailures,uploadFailures);
    std::printf("FINAL: %s; explicit box results are comparison-only; no Intel/full-game claim from a sandbox run.\n",pass?"PASS":"FAIL");
    release(ctx);release(d);return pass?0:10;
}
