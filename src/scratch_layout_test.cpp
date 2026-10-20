#define NOMINMAX
#include "Miscellaneous/D3D11BlitScratch.hpp"
#include <cstdio>
#include <cstdlib>
#include <vector>
using ashes::d3d11::blitscratch::Layout;
using ashes::d3d11::blitscratch::LayoutKind;
static unsigned checks{};
static void need(bool ok, char const *name) { if(!ok){std::fprintf(stderr,"FAIL: %s\n",name);std::exit(1);}++checks;std::printf("PASS: %s\n",name); }
static VkImageBlit rect(int x,int y,int w,int h,unsigned mip=0,unsigned layer=0,unsigned layers=1) {
 VkImageBlit r{};r.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,layers};r.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,mip,layer,layers};r.srcOffsets[1]={11,9,1};r.dstOffsets[0]={x,y,0};r.dstOffsets[1]={x+w,y+h,1};return r;
}
int main() {
 VkImageCreateInfo info{};info.imageType=VK_IMAGE_TYPE_2D;info.extent={4096,4096,1};info.mipLevels=4;info.arrayLayers=6;info.format=VK_FORMAT_R8G8B8A8_UNORM;
 std::vector<VkImageBlit> regions{rect(1320,2792,256,256)};
 auto a=Layout::plan(info,regions);
 need(a.kind==LayoutKind::eCompact,"single nonzero atlas tile is compact");
 need(a.scratchExtent.width==256&&a.scratchExtent.height==256&&a.bboxOrigin.x==1320&&a.bboxOrigin.y==2792,"scratch follows tile bounds not full atlas");
 auto local=a.makeLocalDestination(regions[0]);auto back=a.makeCopyBack(regions[0]);
 need(local.dstOffsets[0].x==0&&local.dstOffsets[0].y==0&&local.dstOffsets[1].x==256,"destination rebased to scratch origin");
 need(back.srcOffset.x==0&&back.dstOffset.x==1320&&back.dstOffset.y==2792&&back.extent.width==256,"copyback retains original atlas origin");
 regions={rect(20,30,13,11,1,2,2),rect(80,90,15,9,1,4,1)};a=Layout::plan(info,regions);
 need(a.kind==LayoutKind::eCompact&&a.scratchExtent.width==75&&a.scratchExtent.height==69,"multiple same-mip regions share exact bounding rectangle");
 need(a.mipLevel==1&&a.baseArrayLayer==2&&a.layerCount==3,"scratch retains referenced layer interval");
 local=a.makeLocalDestination(regions[1]);back=a.makeCopyBack(regions[1]);
 need(local.dstSubresource.mipLevel==0&&local.dstSubresource.baseArrayLayer==2&&local.dstOffsets[0].x==60&&local.dstOffsets[0].y==60,"mip and layer transformed to scratch coordinates");
 need(back.dstSubresource.mipLevel==1&&back.dstSubresource.baseArrayLayer==4&&back.srcSubresource.mipLevel==0&&back.srcSubresource.baseArrayLayer==2,"copyback preserves original mip and layer");
 regions[1].dstSubresource.mipLevel=2;a=Layout::plan(info,regions);need(a.kind==LayoutKind::eFullFallback,"mixed destination mips use explicit valid fallback");
 regions={rect(0,0,1,1)};info.flags=VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;a=Layout::plan(info,regions);need(a.kind==LayoutKind::eFullFallback,"cube-compatible destination uses explicit fallback");info.flags=0;
 regions={rect(-1,0,2,2)};need(Layout::plan(info,regions).kind==LayoutKind::eInvalid,"negative destination rejected");
 regions={rect(4090,4090,10,10)};need(Layout::plan(info,regions).kind==LayoutKind::eInvalid,"out-of-image rectangle rejected");
 regions={rect(0,0,1,1,0,5,2)};need(Layout::plan(info,regions).kind==LayoutKind::eInvalid,"out-of-layer interval rejected");
 regions={rect(0,0,1,1,32)};need(Layout::plan(info,regions).kind==LayoutKind::eInvalid,"overshift mip rejected");
 regions.clear();need(Layout::plan(info,regions).kind==LayoutKind::eInvalid,"empty compute set rejected");
 info.imageType=VK_IMAGE_TYPE_3D;regions={rect(0,0,1,1)};need(Layout::plan(info,regions).kind==LayoutKind::eInvalid,"non-2D scratch request rejected");
 std::printf("FINAL: SCRATCH LAYOUT PASS checks=%u\n",checks);return 0;
}
