// ad_toolbox.hh — shared Mac Toolbox / software-QuickDraw core for the After Dark
// hosts. The DRAWING LOGIC is identical whether the module is PowerPC (adhost.cc,
// args in registers) or 68K (adhost68k.cc, args on the stack); only the argument
// marshaling differs. This header holds the reusable core — a color GrafPort +
// PixMap over a host framebuffer, the CLUT, and the QuickDraw raster primitives —
// so each host's trap/import handler just unpacks its own calling convention and
// calls these methods.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>
#include <memory>
#include "Emulators/MemoryContext.hh"

namespace adtoolbox {

// Public-domain 8x8 bitmap font (font8x8_basic), ASCII 0x20-0x7F, bit0 = leftmost.
extern const uint8_t QDFONT[96][8];

// ---- Authentic Macintosh default 8-bit System palette ('clut' id 8) ----------
// Fill `out` with the real Mac 256-colour default CLUT. Structure (matches
// resource_dasm's default_icon_color_table_8bit byte-for-byte; md5 of the 768-byte
// RGB8 table = 7bb7a37a1af9e3b447628ea5a7fa1184):
//   0..214  = 6x6x6 colour cube, levels {FF,CC,99,66,33,00}, R outer/G mid/B inner
//             DESCENDING, with pure black (0,0,0) EXCLUDED (it is index 255).
//   215..224 = red ramp   (EE,DD,BB,AA,88,77,55,44,22,11 , 0,0)
//   225..234 = green ramp
//   235..244 = blue ramp
//   245..254 = gray ramp  (EE..11 on all channels)
//   255      = black (0,0,0)
// Index 0 is WHITE and index 255 is BLACK. Both matter: direct-index art is authored
// against those slots, and a table that ends at anything other than pure black leaves
// modules that erase to 255 (Warp!) with visible dark-gray trails.
inline void mac_sys7_palette(uint8_t out[256][3]) {
  static const int V[6]  = {0xFF,0xCC,0x99,0x66,0x33,0x00};
  static const int RV[10]= {0xEE,0xDD,0xBB,0xAA,0x88,0x77,0x55,0x44,0x22,0x11};
  int n=0;
  for(int r=0;r<6;r++)for(int g=0;g<6;g++)for(int b=0;b<6;b++){
    if(V[r]==0&&V[g]==0&&V[b]==0) continue;             // black reserved for index 255
    out[n][0]=(uint8_t)V[r]; out[n][1]=(uint8_t)V[g]; out[n][2]=(uint8_t)V[b]; n++; }
  // n == 215 here
  for(int k=0;k<10;k++){ out[215+k][0]=(uint8_t)RV[k]; out[215+k][1]=0;          out[215+k][2]=0; }
  for(int k=0;k<10;k++){ out[225+k][0]=0;              out[225+k][1]=(uint8_t)RV[k]; out[225+k][2]=0; }
  for(int k=0;k<10;k++){ out[235+k][0]=0;              out[235+k][1]=0;          out[235+k][2]=(uint8_t)RV[k]; }
  for(int k=0;k<10;k++){ out[245+k][0]=(uint8_t)RV[k]; out[245+k][1]=(uint8_t)RV[k]; out[245+k][2]=(uint8_t)RV[k]; }
  out[255][0]=0; out[255][1]=0; out[255][2]=0;
}

struct ToolboxCanvas {
  std::shared_ptr<ResourceDASM::MemoryContext> mem;
  int fb_w = 512, fb_h = 384;
  uint32_t g_fb=0, g_clut=0, g_pm=0, g_pmh=0, g_port=0, g_gdev=0, g_gdh=0, g_visrgn=0;
  // Software-QuickDraw pen/port state.
  uint8_t  qd_fg=1, qd_bg=0;
  int16_t  pen_h=0, pen_v=0;
  int16_t  qd_clip_t=0, qd_clip_l=0, qd_clip_b=32767, qd_clip_r=32767;
  int16_t  qd_org_h=0, qd_org_v=0;     // SetOrigin: local coord -> physical = local - origin
  uint32_t cur_port=0;                 // SetPort/SetGWorld target
  std::vector<uint32_t> gworlds;       // offscreen GWorld ports
  uint64_t copybits_count=0;

  // ---- pen model (PenSize / PenMode / PenPat / BackPat / HidePen) -----------
  // A faithful GrafPort pen. The DEFAULTS below are chosen so the pen rasterizers
  // (qd_penline/qd_penstamp/qd_penfill) collapse to the plain qd_line(...,qd_fg) /
  // qd_fillrect(...,qd_fg) paths: 1x1 pen, patCopy transfer, an all-black (all-ones)
  // pen pattern, pen visible. A module only diverges once it actually calls
  // PenSize/PenMode/PenPat, so modules that never touch the pen (Snake, Bogglins)
  // render identically either way.
  int16_t pen_sw=1, pen_sh=1;                  // pnSize (width,height)
  int16_t pen_mode=8;                          // pnMode; 8=patCopy (default)
  uint8_t pen_pat[8]={0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};  // pnPat: solid black
  uint8_t bk_pat[8]={0,0,0,0,0,0,0,0};                          // bkPat: solid white/bg
  int16_t pen_vis=0;                           // HidePen/ShowPen nesting; <0 => hidden

  inline bool pen_bit(int x,int y) const { return (pen_pat[y&7] >> (7-(x&7))) & 1; }
  // One pen pixel honoring pnMode (Copy/Or/Xor/Bic + the "not" bit) and pnPat.
  void qd_penpx(int x,int y){
    if(pen_vis<0) return;                                    // pen hidden
    // Only the classic transfer modes 0..15 (srcCopy..notPatBic) map to indexed raster ops. The
    // arithmetic/blend modes (blend=32, addPin=33, addOver/subPin/transparent/adMax=34..37,
    // subOver=38, adMin=39), hilite (50) and grayishTextOr (49) can't be evaluated on an 8-bit
    // indexed framebuffer — real Color QD needs true RGB there. We fall back to patCopy for those
    // (== the pre-pen "draw solid fg" behaviour these modules already relied on), which also avoids
    // the extra per-pixel read-back. Pearls (PenMode=38 subOver) depends on this fallback.
    int pm = (pen_mode>=0 && pen_mode<=15) ? pen_mode : 8;
    bool on = pen_bit(x,y);
    if(pm & 4) on=!on;                                       // notPat* modes invert the pattern
    switch(pm & 3){
      case 0: qd_px(x,y, on?qd_fg:qd_bg); break;             // patCopy
      case 1: if(on) qd_px(x,y,qd_fg); break;                // patOr
      case 2: if(on) qd_px(x,y,(uint8_t)(qd_getpx(x,y)^qd_fg)); break; // patXor (invert)
      case 3: if(on) qd_px(x,y,qd_bg); break;                // patBic (erase)
    }
  }
  // One pen pixel through a PRE-RESOLVED pixmap (base/rb/bt/bl/bb/br), replicating qd_px/qd_getpx's
  // clip + region + origin + bounds pipeline exactly, so a fat pen doesn't re-resolve cur_pm per pixel.
  void qd_penpx_res(uint32_t base,int rb,int bt,int bl,int bb,int br,int x,int y){
    if(pen_vis<0) return;
    if(x<qd_clip_l||x>=qd_clip_r||y<qd_clip_t||y>=qd_clip_b) return;      // clip (local)
    if(clip_rgn){ if(x>=0&&y>=0&&x<fb_w&&y<fb_h){ auto it=rgn_masks.find(clip_rgn);
      if(it!=rgn_masks.end() && !it->second[(size_t)y*fb_w+x]) return; } }
    int px=(x-qd_org_h)-bl, py=(y-qd_org_v)-bt; if(px<0||py<0||px>=(br-bl)||py>=(bb-bt)) return;
    uint32_t addr=base+(uint32_t)py*rb+px;
    int pm = (pen_mode>=0 && pen_mode<=15) ? pen_mode : 8;
    bool on = pen_bit(x,y); if(pm & 4) on=!on;
    switch(pm & 3){
      case 0: mem->write_u8(addr, on?qd_fg:qd_bg); break;                  // patCopy
      case 1: if(on) mem->write_u8(addr, qd_fg); break;                    // patOr
      case 2: if(on) mem->write_u8(addr, (uint8_t)(mem->read_u8(addr)^qd_fg)); break; // patXor
      case 3: if(on) mem->write_u8(addr, qd_bg); break;                    // patBic
    }
  }
  // Stamp the pen rectangle (pnSize) with its top-left at (x,y) — QD's pen hangs
  // right/down. 1x1 collapses to a single qd_penpx (identical to the plain path).
  void qd_penstamp(int x,int y){
    if(pen_sw<=1 && pen_sh<=1){ qd_penpx(x,y); return; }
    int w=pen_sw>0?pen_sw:0, h=pen_sh>0?pen_sh:0;
    uint32_t base;int rb,bt,bl,bb,br; if(!cur_pm(base,rb,bt,bl,bb,br)) return;  // resolve ONCE per stamp
    for(int dy=0;dy<h;dy++) for(int dx=0;dx<w;dx++) qd_penpx_res(base,rb,bt,bl,bb,br,x+dx,y+dy);
  }
  void qd_penline(int x0,int y0,int x1,int y1){
    int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    for(int guard=0;guard<20000;guard++){ qd_penstamp(x0,y0);
      if(x0==x1&&y0==y1)break; int e2=2*err;
      if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
  }
  // Pen-pattern/mode fill of a rect (PaintRect/PaintOval interior). Same clip
  // pipeline as qd_fillrect so the default pen collapses to qd_fillrect(...,qd_fg).
  void qd_penfill(int t,int l,int b,int rr){
    if(pen_vis<0) return;
    uint32_t base;int rbx,bt,bl,bb,br; if(!cur_pm(base,rbx,bt,bl,bb,br))return;
    if(l<bl)l=bl; if(t<bt)t=bt; if(rr>br)rr=br; if(b>bb)b=bb;
    if(l<qd_clip_l)l=qd_clip_l; if(t<qd_clip_t)t=qd_clip_t;
    if(rr>qd_clip_r)rr=qd_clip_r; if(b>qd_clip_b)b=qd_clip_b;
    for(int y=t;y<b;y++)for(int x=l;x<rr;x++)qd_penpx(x,y);
  }

  // ---- scan-converted region coverage masks --------------------------------
  // The bounds-only region model loses a region's actual SHAPE — it keeps only
  // the bbox — so a module that fills or clips through a region built from many
  // rects (Union/Diff of RectRgns) renders a solid rectangle instead of the
  // intended figure. This optional per-region W*H coverage mask (keyed by the
  // region DATA pointer, the value behind a RgnHandle) records real shape so
  // FillRgn can paint the figure and SetClip can install a non-rectangular clip.
  // Masks are created lazily; a region with no mask keeps the bounds-only path,
  // so the modules that only ever use full-screen/rect regions are unaffected.
  std::map<uint32_t,std::vector<uint8_t>> rgn_masks;
  uint32_t clip_rgn=0;                 // region-data ptr of a non-rect clip, or 0 (rect-only)
  bool masks_enabled=false; bool force_screen=false;            // scan-converted regions are opt-in (see ctor / adhost68k)

  std::vector<uint8_t>* rgn_mask(uint32_t rgn,bool create){
    if(!masks_enabled) return nullptr;
    auto it=rgn_masks.find(rgn);
    if(it!=rgn_masks.end()) return &it->second;
    if(!create) return nullptr;
    auto& v=rgn_masks[rgn]; v.assign((size_t)fb_w*fb_h,0); return &v;
  }
  void rgn_mask_drop(uint32_t rgn){ rgn_masks.erase(rgn); if(clip_rgn==rgn) clip_rgn=0; }
  // Set a region's mask to exactly one rect (RectRgn / SetRectRgn).
  void rgn_mask_setrect(uint32_t rgn,int t,int l,int b,int rr){
    auto* m=rgn_mask(rgn,true); if(!m) return;   // masks disabled -> no-op
    std::fill(m->begin(),m->end(),0);
    if(t<0)t=0; if(l<0)l=0; if(b>fb_h)b=fb_h; if(rr>fb_w)rr=fb_w;
    for(int y=t;y<b;y++)for(int x=l;x<rr;x++)(*m)[(size_t)y*fb_w+x]=1;
  }
  // dst = combine(a,b): op 0=union(|) 1=diff(a&~b) 2=sect(a&b). Missing operand
  // masks are treated as their bbox is unknown -> only combine when both exist;
  // if an operand has no mask it is taken as "full" (union) / handled by caller.
  void rgn_mask_combine(uint32_t dst,uint32_t a,uint32_t b,int op){
    auto* ma=rgn_mask(a,false); auto* mb=rgn_mask(b,false);
    if(!ma&&!mb){ rgn_masks.erase(dst); return; }   // both bounds-only -> keep bbox path
    auto* md=rgn_mask(dst,true); size_t n=md->size();
    for(size_t i=0;i<n;i++){
      uint8_t va=ma?(*ma)[i]:1, vb=mb?(*mb)[i]:0;
      (*md)[i]= op==0 ? (va|vb) : op==1 ? (va&~vb) : (va&vb);
    }
  }
  void rgn_mask_copy(uint32_t dst,uint32_t src){
    auto* ms=rgn_mask(src,false);
    if(!ms){ rgn_masks.erase(dst); return; }
    rgn_masks[dst]=*ms;
  }
  bool rgn_mask_any(uint32_t rgn){
    auto* m=rgn_mask(rgn,false); if(!m) return false;
    for(uint8_t v:*m) if(v) return true; return false;
  }
  // A region whose mask covers the entire framebuffer is indistinguishable from
  // "no clip" — used to skip installing a per-pixel clip (and its hot-path cost)
  // for modules that merely SetClip a full-screen region.
  bool rgn_mask_full(uint32_t rgn){
    auto* m=rgn_mask(rgn,false); if(!m) return false;
    for(uint8_t v:*m) if(!v) return false; return true;
  }
  void rgn_mask_offset(uint32_t rgn,int dh,int dv){
    auto* m=rgn_mask(rgn,false); if(!m) return;
    std::vector<uint8_t> out((size_t)fb_w*fb_h,0);
    for(int y=0;y<fb_h;y++){ int sy=y-dv; if(sy<0||sy>=fb_h) continue;
      for(int x=0;x<fb_w;x++){ int sx=x-dh; if(sx<0||sx>=fb_w) continue;
        out[(size_t)y*fb_w+x]=(*m)[(size_t)sy*fb_w+sx]; } }
    *m=std::move(out);
  }
  // Paint a region's mask with idx (used by FillRgn/PaintRgn/EraseRgn when a
  // real mask exists). Returns false if the region has no mask (caller falls
  // back to the bbox fill).
  bool rgn_mask_fill(uint32_t rgn,uint8_t idx){
    auto* m=rgn_mask(rgn,false); if(!m) return false;
    for(int y=0;y<fb_h;y++)for(int x=0;x<fb_w;x++)
      if((*m)[(size_t)y*fb_w+x]) qd_px(x,y,idx);
    return true;
  }
  // ---- exact-shape queries (region-exact pass) -----------------------------
  // These let every consumer honour a non-rect region's TRUE shape. A region with
  // NO mask is exactly its bbox rect (the mask-less fast path is authentic there);
  // a region WITH a mask is genuinely non-rectangular and must be tested per-pixel.
  //
  // Tight bounding box of the set pixels. false if no mask or the mask is empty.
  bool rgn_mask_bbox(uint32_t rgn,int&t,int&l,int&b,int&r){
    auto* m=rgn_mask(rgn,false); if(!m) return false;
    int mnx=fb_w,mny=fb_h,mxx=-1,mxy=-1;
    for(int y=0;y<fb_h;y++)for(int x=0;x<fb_w;x++) if((*m)[(size_t)y*fb_w+x]){
      if(x<mnx)mnx=x; if(x>mxx)mxx=x; if(y<mny)mny=y; if(y>mxy)mxy=y; }
    if(mxx<mnx) return false;                                 // empty mask
    t=mny; l=mnx; b=mxy+1; r=mxx+1; return true;
  }
  // true IFF the mask exactly fills its tight bbox — i.e. the region is RECTANGULAR,
  // so the dense mask is redundant and may be dropped (bbox becomes the exact model).
  bool rgn_mask_is_rect(uint32_t rgn){
    int t,l,b,r; if(!rgn_mask_bbox(rgn,t,l,b,r)) return false;
    auto* m=rgn_mask(rgn,false);
    for(int y=t;y<b;y++)for(int x=l;x<r;x++) if(!(*m)[(size_t)y*fb_w+x]) return false;
    return true;
  }
  // Point-in-region against the exact mask (PtInRgn). Mask coords are region/global.
  bool rgn_mask_pt(uint32_t rgn,int x,int y){
    auto* m=rgn_mask(rgn,false); if(!m) return false;
    if(x<0||y<0||x>=fb_w||y>=fb_h) return false;
    return (*m)[(size_t)y*fb_w+x]!=0;
  }
  // Rect-intersects-region against the exact mask (RectInRgn: true if any pixel of
  // rect[t,l,b,r) is inside the region).
  bool rgn_mask_rect_hits(uint32_t rgn,int t,int l,int b,int r){
    auto* m=rgn_mask(rgn,false); if(!m) return false;
    if(t<0)t=0; if(l<0)l=0; if(b>fb_h)b=fb_h; if(r>fb_w)r=fb_w;
    for(int y=t;y<b;y++)for(int x=l;x<r;x++) if((*m)[(size_t)y*fb_w+x]) return true;
    return false;
  }
  // Outline the region's exact boundary in idx (FrameRgn): a set pixel is on the
  // frame iff it has an unset (or off-canvas) 4-neighbour. Mirrors rgn_mask_fill's
  // qd_px path (port origin + clip). false if the region has no mask.
  bool rgn_mask_frame(uint32_t rgn,uint8_t idx){
    auto* m=rgn_mask(rgn,false); if(!m) return false;
    int W=fb_w,H=fb_h;
    for(int y=0;y<H;y++)for(int x=0;x<W;x++){ if(!(*m)[(size_t)y*W+x]) continue;
      bool edge = (x==0   || !(*m)[(size_t)y*W+x-1]) ||
                  (x==W-1 || !(*m)[(size_t)y*W+x+1]) ||
                  (y==0   || !(*m)[(size_t)(y-1)*W+x]) ||
                  (y==H-1 || !(*m)[(size_t)(y+1)*W+x]);
      if(edge) qd_px(x,y,idx); }
    return true;
  }

  explicit ToolboxCanvas(std::shared_ptr<ResourceDASM::MemoryContext> m): mem(m) {}

  // ---- CLUT / colour -------------------------------------------------------
  bool dm_white_black=false;   // Draw Morph: resolve pure white to the black index (paper fill)
  // DISPLAY-ONLY black-at-index-0 (Nirvana). Nirvana is a plasma/fire feedback saver whose
  // untouched-background pixels are framebuffer index 0, and its own installed CLUT leaves index 0
  // WHITE, so the backdrop renders white where the real module grows plasma on BLACK. Blackening
  // CLUT entry 0 instead is not an option: the module resolves qd_fg/qd_bg and its plasma colours
  // through nearest-CLUT-match, so it shifts qd_fg 192->0, qd_bg 0->22 and collapses the red band to
  // near-black idx255. Changing only the DISPLAY LUT entry 0 at snapshot time leaves the framebuffer
  // bytes, g_clut and rgb_to_index untouched — the plasma's frame-to-frame feedback is unaffected and
  // only the backdrop's shown colour flips white->black. Default-off; the host name-gates it.
  bool disp_black0=false;
  // Display-only black at index 255 (Warp!). Same mechanism as disp_black0. Warp! fades distant stars
  // and erases old star positions to index 255 expecting black, so a near-black index 255 leaves every
  // swept position a visible dark-gray pixel that accumulates into a gray trail. Editing the CLUT
  // itself would perturb rgb_to_index nearest-match (Bogglins' near-dark colours shift index), so this
  // is display-only: index bytes and rgb_to_index are unchanged. The host name-gates it.
  bool disp_black255=false;
  uint8_t rgb_to_index(uint16_t R,uint16_t G,uint16_t B) const {
    if(dm_white_black && R>=0xF000 && G>=0xF000 && B>=0xF000){ // pure-white paper -> black idx
      int rb=0; long bd=1L<<40; for(int i=0;i<256;i++){ uint32_t e=g_clut+8+i*8;
        int cr=mem->read_u16b(e+2)>>8,cg=mem->read_u16b(e+4)>>8,cb=mem->read_u16b(e+6)>>8;
        long d=(long)cr*cr+(long)cg*cg+(long)cb*cb; if(d<bd){bd=d;rb=i;} } return (uint8_t)rb; }
    int r8=R>>8,g8=G>>8,b8=B>>8,best=0; long bd=1L<<40;
    for(int i=0;i<256;i++){ uint32_t e=g_clut+8+i*8;
      int cr=mem->read_u16b(e+2)>>8,cg=mem->read_u16b(e+4)>>8,cb=mem->read_u16b(e+6)>>8;
      long d=(long)(cr-r8)*(cr-r8)+(long)(cg-g8)*(cg-g8)+(long)(cb-b8)*(cb-b8);
      if(d<bd){bd=d;best=i;} }
    return (uint8_t)best;
  }
  void clut_set(int idx,uint16_t R,uint16_t G,uint16_t B){
    if(idx<0||idx>255) return; uint32_t e=g_clut+8+idx*8;
    mem->write_u16b(e,(uint16_t)idx); mem->write_u16b(e+2,R); mem->write_u16b(e+4,G); mem->write_u16b(e+6,B);
  }

  // ---- current pixmap ------------------------------------------------------
  bool cur_pm(uint32_t& base,int& rb,int& bt,int& bl,int& bb,int& br) const {
    uint32_t port=cur_port?cur_port:g_port; if(!port) return false;
    // Distinguish a color CGrafPort (portVersion@+6 has bits 15/14 set -> 0xC000) from a classic
    // B&W GrafPort (offset +2 is an INLINE BitMap: baseAddr@+2, rowBytes@+6, bounds@+8). The old
    // code always assumed color and read +2 as a PixMapHandle, so any module that OpenPort's a B&W
    // port (Gravity) resolved to garbage. Detect and read the right layout.
    bool isColor = (mem->read_u16b(port+0x06) & 0xC000)==0xC000;
    if(isColor){
      uint32_t pmh=mem->read_u32b(port+0x02); if(!pmh) return false;
      uint32_t pm=mem->read_u32b(pmh); if(!pm) return false;
      base=mem->read_u32b(pm+0); rb=mem->read_u16b(pm+4)&0x3FFF;
      bt=(int16_t)mem->read_u16b(pm+6); bl=(int16_t)mem->read_u16b(pm+8);
      bb=(int16_t)mem->read_u16b(pm+10); br=(int16_t)mem->read_u16b(pm+12);
    } else {
      base=mem->read_u32b(port+0x02); rb=mem->read_u16b(port+0x06)&0x3FFF;
      bt=(int16_t)mem->read_u16b(port+0x08); bl=(int16_t)mem->read_u16b(port+0x0A);
      bb=(int16_t)mem->read_u16b(port+0x0C); br=(int16_t)mem->read_u16b(port+0x0E);
    }
    if(force_screen && g_fb && base!=g_fb){   // redirect offscreen draws to the screen fb
      base=g_fb; rb=fb_w; bt=0; bl=0; bb=fb_h; br=fb_w;   // ...with the screen's own geometry
    }
    return base!=0 && rb>0;
  }

  // ---- raster primitives ---------------------------------------------------
  void qd_px(int x,int y,uint8_t idx){
    uint32_t base;int rb,bt,bl,bb,br; if(!cur_pm(base,rb,bt,bl,bb,br))return;
    if(x<qd_clip_l||x>=qd_clip_r||y<qd_clip_t||y>=qd_clip_b)return;   // clip in local coords
    if(clip_rgn){ // non-rectangular clip: reject pixels outside the clip region's mask
      if(x>=0&&y>=0&&x<fb_w&&y<fb_h){ auto it=rgn_masks.find(clip_rgn);
        if(it!=rgn_masks.end() && !it->second[(size_t)y*fb_w+x]) return; } }
    int sx=x-qd_org_h, sy=y-qd_org_v;                                 // apply port origin (SetOrigin)
    int px=sx-bl,py=sy-bt; if(px<0||py<0||px>=(br-bl)||py>=(bb-bt))return;
    mem->write_u8(base+(uint32_t)py*rb+px,idx);
  }
  // Read one pixel's palette index from the current pixmap, using the SAME coordinate
  // transform as qd_px (port origin + pixmap bounds) so GetCPixel mirrors SetCPixel/PaintRect.
  // Out-of-bounds reads return 0 (the background/white index). Used by GetCPixel-based
  // collision logic (e.g. Snake reads the cell ahead to decide whether it can move).
  uint8_t qd_getpx(int x,int y) const {
    uint32_t base;int rb,bt,bl,bb,br; if(!cur_pm(base,rb,bt,bl,bb,br))return 0;
    int sx=x-qd_org_h, sy=y-qd_org_v;
    int px=sx-bl,py=sy-bt; if(px<0||py<0||px>=(br-bl)||py>=(bb-bt))return 0;
    return mem->read_u8(base+(uint32_t)py*rb+px);
  }
  void qd_fillrect(int t,int l,int b,int rr,uint8_t idx){
    uint32_t base;int rb,bt,bl,bb,br; if(!cur_pm(base,rb,bt,bl,bb,br))return;
    if(l<bl)l=bl; if(t<bt)t=bt; if(rr>br)rr=br; if(b>bb)b=bb;
    if(l<qd_clip_l)l=qd_clip_l; if(t<qd_clip_t)t=qd_clip_t;
    if(rr>qd_clip_r)rr=qd_clip_r; if(b>qd_clip_b)b=qd_clip_b;
    for(int y=t;y<b;y++)for(int x=l;x<rr;x++)qd_px(x,y,idx);
  }
  void qd_line(int x0,int y0,int x1,int y1,uint8_t idx){
    int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    for(int guard=0;guard<20000;guard++){ qd_px(x0,y0,idx);
      if(x0==x1&&y0==y1)break; int e2=2*err;
      if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
  }
  void qd_char(int ch,int x,int y,uint8_t idx){
    if(ch<0x20||ch>0x7F)return; const uint8_t* g=QDFONT[ch-0x20];
    for(int row=0;row<8;row++){ uint8_t bits=g[row];
      for(int col=0;col<8;col++) if(bits&(1<<col)) qd_px(x+col,y-7+row,idx); }
  }
  void qd_string(uint32_t sp,bool pascal){
    if(!sp)return; int n; uint32_t p;
    if(pascal){ n=mem->read_u8(sp); p=sp+1; } else { n=0; p=sp; while(mem->read_u8(p+n))n++; }
    for(int k=0;k<n;k++){ int c=mem->read_u8(p+k); qd_char(c,pen_h,pen_v,qd_fg); pen_h+=6; }
  }
  // TETextBox: draw `len` bytes of text (raw, not Pascal) word-wrapped inside box[t,l,b,r] with
  // justification (teJustLeft=0, teJustCenter=1, teJustRight=-1). Our bitmap font is 8px tall /
  // ~6px advance, so we approximate line layout at cw=6, lineHeight=11, ascent=8. Modules such as
  // Nonsense render every phrase through this call; without it the screen stays blank.
  void qd_textbox(uint32_t textp,int len,int bt,int bl,int bb,int br,int just,uint8_t idx){
    if(!textp||len<=0) return;
    const int cw=6, lh=11, ascent=8; int boxw=br-bl; if(boxw<cw) boxw=cw;
    std::string s; s.reserve(len);
    for(int i=0;i<len;i++) s+=(char)mem->read_u8(textp+i);
    int y=bt+ascent; size_t i=0;
    while(i<s.size() && y<=bb+ascent){
      size_t lineStart=i, lastSpace=std::string::npos, j=i; int w=0;
      while(j<s.size() && s[j]!='\n' && (w+cw)<=boxw){ if(s[j]==' ') lastSpace=j; w+=cw; j++; }
      size_t lineEnd;
      if(j>=s.size() || s[j]=='\n') lineEnd=j;
      else if(lastSpace!=std::string::npos && lastSpace>lineStart) lineEnd=lastSpace;
      else lineEnd=j;                                      // long word: hard break
      int nchars=(int)(lineEnd-lineStart), linew=nchars*cw, x=bl;
      if(just==1) x=bl+(boxw-linew)/2;                     // center
      else if(just==-1) x=br-linew;                        // right
      // Real Toolbox TextBox EraseRect's the text area to the BACKGROUND colour before stamping
      // glyphs — that white erase is what makes Nonsense's black text readable in its white boxes.
      // Erase each drawn line's own band (the module often passes a degenerate/0-height box).
      if(linew>0) qd_fillrect(y-ascent, x, y-ascent+lh, x+linew, qd_bg);
      for(size_t k=lineStart;k<lineEnd;k++){ qd_char((unsigned char)s[k],x,y,idx); x+=cw; }
      y+=lh; i=lineEnd;
      while(i<s.size() && (s[i]==' '||s[i]=='\n')) i++;     // consume the break
    }
  }

  // ---- build the screen canvas: framebuffer + CLUT + PixMap + CGrafPort -----
  // Mirrors adhost.cc §6a2. CLUT starts as a grayscale ramp; callers may overlay
  // a module 'clut' resource via clut_set().
  void setup(int W,int Hh){
    fb_w=W; fb_h=Hh;
    g_fb   = mem->allocate(W*Hh); mem->memset(g_fb,0,W*Hh);
    g_clut = mem->allocate(8+256*8); mem->memset(g_clut,0,8+256*8);
    mem->write_u16b(g_clut+6, 255);
    // Seed the authentic Macintosh default 8-bit System palette (mac_sys7_palette). Callers
    // may overlay a module 'clut' via clut_set() on top; a partial clut keeps this substrate
    // for the indices it does not cover. idx0=WHITE, idx255=BLACK.
    {
      uint8_t pal[256][3]; mac_sys7_palette(pal);
      for(int i=0;i<256;i++){ uint32_t e=g_clut+8+i*8;
        mem->write_u16b(e,(uint16_t)i);
        mem->write_u16b(e+2,(uint16_t)(pal[i][0]*257));
        mem->write_u16b(e+4,(uint16_t)(pal[i][1]*257));
        mem->write_u16b(e+6,(uint16_t)(pal[i][2]*257)); }
    }
    uint32_t cth=mem->allocate(4); mem->write_u32b(cth,g_clut);
    g_pm = mem->allocate(0x32); mem->memset(g_pm,0,0x32);
    mem->write_u32b(g_pm+0x00, g_fb);
    mem->write_u16b(g_pm+0x04, 0x8000 | (uint16_t)W);
    mem->write_u16b(g_pm+0x06, 0); mem->write_u16b(g_pm+0x08, 0);
    mem->write_u16b(g_pm+0x0A, (uint16_t)Hh); mem->write_u16b(g_pm+0x0C, (uint16_t)W);
    // PixMap fields at the CORRECT Mac offsets (Boris reads pixelSize@0x20 to compute a divisor
    // and divided by zero because 0x20 was 0; Mandelbrot reads it for colour depth). Correct
    // layout: hRes@0x16, vRes@0x1A (Fixed), pixelType@0x1E, pixelSize@0x20, cmpCount@0x22,
    // cmpSize@0x24, pmTable@0x2A.
    //
    // ADPMLEGACY=1 additionally writes four alias fields (pixelSize@0x13, cmpCount@0x16,
    // cmpSize@0x18, pmTable@0x1C). They are NOT a second valid layout: they land INSIDE the
    // architectural fields and destroy them — 0x13/0x16/0x18 sit in packSize/hRes, and the
    // pmTable LONG at 0x1C covers vRes' low half AND pixelType@0x1E. Because 0x1C is written
    // last, pixelType then reads the LOW HALF OF THE COLOUR-TABLE HANDLE on every host pixmap
    // including the screen — never 0 (chunky indexed), the one value an 8bpp indexed pixmap
    // must report. The lever exists only for A/B; no module in the corpus reads these bytes.
    const bool pm_legacy = (getenv("ADPMLEGACY")!=nullptr);
    if(!pm_legacy){
      mem->write_u32b(g_pm+0x16, 0x00480000);      // hRes = 72 dpi (Fixed)
      mem->write_u32b(g_pm+0x1A, 0x00480000);      // vRes = 72 dpi (Fixed)
    }
    mem->write_u16b(g_pm+0x1E, 0);                 // pixelType = 0 (indexed/chunky)
    mem->write_u16b(g_pm+0x20, 8);                 // pixelSize = 8bpp
    mem->write_u16b(g_pm+0x22, 1);                 // cmpCount = 1 (indexed)
    mem->write_u16b(g_pm+0x24, 8);                 // cmpSize = 8
    mem->write_u32b(g_pm+0x2A, cth);               // pmTable (CTabHandle)
    if(pm_legacy){                                 // the alias fields, in their original order
      mem->write_u16b(g_pm+0x13, 8);               // legacy pixelSize
      mem->write_u16b(g_pm+0x16, 1); mem->write_u16b(g_pm+0x18, 1); // legacy cmpCount/cmpSize
      mem->write_u32b(g_pm+0x1C, cth);             // legacy pmTable (clobbers pixelType@0x1E)
    }
    g_pmh = mem->allocate(4); mem->write_u32b(g_pmh, g_pm);
    uint32_t vis = mem->allocate(0x0A); mem->write_u16b(vis,10);
    mem->write_u16b(vis+2,0); mem->write_u16b(vis+4,0);
    mem->write_u16b(vis+6,(uint16_t)Hh); mem->write_u16b(vis+8,(uint16_t)W);
    uint32_t vish=mem->allocate(4); mem->write_u32b(vish,vis); g_visrgn=vish;
    g_port = mem->allocate(0x100); mem->memset(g_port,0,0x100);
    mem->write_u32b(g_port+0x02, g_pmh);           // portPixMap
    mem->write_u16b(g_port+0x06, 0xC000);          // portVersion: color port
    mem->write_u16b(g_port+0x10,0); mem->write_u16b(g_port+0x12,0);
    mem->write_u16b(g_port+0x14,(uint16_t)Hh); mem->write_u16b(g_port+0x16,(uint16_t)W);
    mem->write_u32b(g_port+0x18, vish);
    // The SCREEN port's clipRgn(+0x1C). QuickDraw's invariant is that a port owns a clipRgn from
    // birth: SetClip and ClipRect copy a shape INTO the port's existing region rather than
    // adopting the caller's handle, which is only definable if the field is never NULL. The PPC
    // host builds both fields the same way, as two independent rectangular records.
    // Guest code READS this field: MacCanvas::SetPenOrigin (LIB510_Canvas @0x0100E4E6) fetches the
    // port's clipRgn at +0x1C and OffsetRgns it to hold the clip over the same pixels across the
    // SetOrigin two instructions later. With the field NULL the host's OffsetRgn takes its
    // `rg = h ? read(h) : 0` guard and silently does nothing.
    // Nothing HOST-side reads +0x1C — the host's clip is qd_clip_* plus the per-port store — so
    // this is a structural repair of the port record, not a change to what is drawn.
    // ADNOSCRCLIPRGN=1 restores the NULL. (getenv here rather than a host lever because this is
    // the header; ADPMLEGACY above reads its lever the same way.)
    if(getenv("ADNOSCRCLIPRGN")==nullptr){
      uint32_t clp = mem->allocate(0x0A); mem->write_u16b(clp,10);   // rgnSize = 10 (rectangular)
      mem->write_u16b(clp+2,0); mem->write_u16b(clp+4,0);
      mem->write_u16b(clp+6,(uint16_t)Hh); mem->write_u16b(clp+8,(uint16_t)W);   // rgnBBox = screen
      uint32_t clph = mem->allocate(4); mem->write_u32b(clph,clp);
      mem->write_u32b(g_port+0x1C, clph);
    }
    cur_port = g_port;
  }

  // ---- CopyBits: srcPixMap -> dstPixMap, honouring bounds (index copy) ------
  // Simplified: same-size 1:1 blit used by the sprite engine; the host maps the
  // engine's screen dst to g_fb via the GDevice so sprites land on-screen.
  void copybits(uint32_t srcPM,uint32_t dstPM){
    copybits_count++;
    uint32_t sb=mem->read_u32b(srcPM+0); int srb=mem->read_u16b(srcPM+4)&0x3FFF;
    int st=(int16_t)mem->read_u16b(srcPM+6),sl=(int16_t)mem->read_u16b(srcPM+8),
        sbot=(int16_t)mem->read_u16b(srcPM+10),sr=(int16_t)mem->read_u16b(srcPM+12);
    uint32_t db=mem->read_u32b(dstPM+0); int drb=mem->read_u16b(dstPM+4)&0x3FFF;
    int w=sr-sl,h=sbot-st; if(!sb||!db||w<=0||h<=0) return;
    for(int y=0;y<h;y++) for(int x=0;x<w;x++)
      mem->write_u8(db+(uint32_t)y*drb+x, mem->read_u8(sb+(uint32_t)y*srb+x));
  }

  // ---- dump framebuffer as an 8-bit-through-CLUT PPM -----------------------
  // The DISPLAY surface (P0 present model): the screen pixmap g_pm is the display ONLY while it
  // holds a full-screen page. Page-flipping modules swap g_pm.baseAddr among full-screen buffers
  // (each becomes the live page); modules that temporarily bind g_pm to a SMALL scratch pixmap
  // (e.g. Gravity's 352x51 sprite strip) are mid-composite, not presenting — so we keep showing
  // the last full-screen page. Present reads that page at true screen geometry (fb_w x fb_h),
  // never assuming a stale rowBytes. For single-buffer modules (the working set) present_base
  // tracks g_fb and output is byte-identical.
  uint32_t present_base=0;   // last full-screen display page; 0 => g_fb
  void update_present(){
    if(!g_pm) return;
    uint32_t bp=mem->read_u32b(g_pm+0x00); int rb=mem->read_u16b(g_pm+4)&0x3FFF;
    int t=(int16_t)mem->read_u16b(g_pm+6), l=(int16_t)mem->read_u16b(g_pm+8),
        b=(int16_t)mem->read_u16b(g_pm+10), r=(int16_t)mem->read_u16b(g_pm+12);
    bool fullscreen = bp && rb>=fb_w && (r-l)>=fb_w && (b-t)>=fb_h;
    if(fullscreen) present_base=bp;    // this is a live full-screen display page
  }
  // Reusable per-frame scratch (avoid re-alloc every snap). ppm_idx holds the
  // raw 8-bit index plane, ppm_rgb the encoded RGB body, ppm_lut a 256*3 RGB LUT.
  std::vector<uint8_t> ppm_idx, ppm_rgb; uint8_t ppm_lut[256*3];

  // Choose the source page + read the whole index plane into `ppm_idx` once.
  // (Bulk read; falls back to per-pixel on a partially-mapped buffer, with the
  // same output.)
  uint32_t snap_pick_src(){
    uint32_t src;
    if(const char* fa=getenv("ADSNAPADDR")){ src=(uint32_t)strtoul(fa,0,16); }
    else { update_present(); src = present_base? present_base : g_fb; }
    return src;
  }
  void snap_read_indices(uint32_t src){
    size_t n=(size_t)fb_w*fb_h; ppm_idx.resize(n);
    try { mem->memcpy(ppm_idx.data(), src, n); }        // single bulk read
    catch(...) { for(size_t i=0;i<n;i++){ uint8_t ix=0; try{ix=mem->read_u8(src+(uint32_t)i);}catch(...){} ppm_idx[i]=ix; } }
  }
  // Build a 256-entry RGB LUT from the live CLUT (256 reads instead of 3/pixel).
  void snap_build_lut(){
    for(int i=0;i<256;i++){ uint32_t e=g_clut+8+(uint32_t)i*8;
      ppm_lut[i*3+0]=(uint8_t)(mem->read_u16b(e+2)>>8);
      ppm_lut[i*3+1]=(uint8_t)(mem->read_u16b(e+4)>>8);
      ppm_lut[i*3+2]=(uint8_t)(mem->read_u16b(e+6)>>8); }
    // Display-only index overrides: change the SHOWN colour of index-0 / index-255 pixels without
    // touching g_clut or rgb_to_index. Every display path (P6 stream, P8 stream, shm) sources
    // ppm_lut here, so this is the one place both need applying.
    if(disp_black0){ ppm_lut[0]=0; ppm_lut[1]=0; ppm_lut[2]=0; }
    if(disp_black255){ ppm_lut[255*3]=0; ppm_lut[255*3+1]=0; ppm_lut[255*3+2]=0; } // Warp! erase/fade colour
  }
  // Write one P6 frame to an already-open stream (used both for file snaps and
  // for the ADSTREAM stdout pipe — the app consumes concatenated P6 frames).
  // One bulk index read + a 256-entry CLUT LUT + one buffered fwrite of the whole frame.
  void snap_ppm_stream(FILE* o){
    fprintf(o,"P6\n%d %d\n255\n",fb_w,fb_h);
    size_t n=(size_t)fb_w*fb_h;
    snap_read_indices(snap_pick_src());
    ppm_rgb.resize(n*3);
    if(getenv("ADRAWIDX")){ // raw-index dump: index value straight to gray, bypass CLUT
      for(size_t i=0;i<n;i++){ uint8_t ix=ppm_idx[i]; ppm_rgb[i*3+0]=ix; ppm_rgb[i*3+1]=ix; ppm_rgb[i*3+2]=ix; }
    } else {
      snap_build_lut();
      for(size_t i=0;i<n;i++){ const uint8_t* c=&ppm_lut[(size_t)ppm_idx[i]*3];
        ppm_rgb[i*3+0]=c[0]; ppm_rgb[i*3+1]=c[1]; ppm_rgb[i*3+2]=c[2]; }
    }
    fwrite(ppm_rgb.data(),1,n*3,o);
    fflush(o);
  }
  // ADSTREAMIDX=1: private "P8" indexed frame — header "P8\n<w> <h>\n", a 768-byte
  // RGB palette (256 CLUT entries), then w*h raw index bytes. The app builds an
  // indexed CGImage with zero per-pixel work. Host side is a palette dump + one
  // bulk index read + two fwrites — minimal CPU.
  void snap_p8_stream(FILE* o){
    fprintf(o,"P8\n%d %d\n",fb_w,fb_h);
    if(getenv("ADRAWIDX")){ for(int i=0;i<256;i++){ uint8_t v=(uint8_t)i; ppm_lut[i*3]=v;ppm_lut[i*3+1]=v;ppm_lut[i*3+2]=v; } }
    else snap_build_lut();
    fwrite(ppm_lut,1,256*3,o);
    size_t n=(size_t)fb_w*fb_h;
    snap_read_indices(snap_pick_src());
    fwrite(ppm_idx.data(),1,n,o);
    fflush(o);
  }
  // Zero-copy shared-memory transport. Same payload as snap_p8_stream (768-byte RGB
  // palette + w*h index bytes) but written straight into a caller-provided memory region
  // (an mmap'd POSIX shm slot) instead of an stdout stream — no header, no fwrite, no
  // pipe. `pal_dst` must have room for 256*3 bytes and `idx_dst` for fb_w*fb_h bytes.
  // Reuses the existing scratch/helpers so a shm frame is byte-for-byte identical to the
  // P8 stream's palette+index payload. Unused unless the host opts into ADSHM mode.
  void snap_p8_shm(uint8_t* pal_dst, uint8_t* idx_dst){
    if(getenv("ADRAWIDX")){ for(int i=0;i<256;i++){ uint8_t v=(uint8_t)i; pal_dst[i*3]=v;pal_dst[i*3+1]=v;pal_dst[i*3+2]=v; } }
    else { snap_build_lut(); for(int i=0;i<256*3;i++) pal_dst[i]=ppm_lut[i]; }
    size_t n=(size_t)fb_w*fb_h;
    snap_read_indices(snap_pick_src());
    for(size_t i=0;i<n;i++) idx_dst[i]=ppm_idx[i];
  }
  void snap_ppm(const std::string& path){
    FILE* o=fopen(path.c_str(),"wb"); if(!o) return;
    snap_ppm_stream(o);
    fclose(o);
  }
};

} // namespace adtoolbox
