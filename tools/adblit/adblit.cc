// adblit — decode After Dark 4.0 RLE sprites to RGBA by running the original
// PowerPC blitter (in `After Dark 4.0 Shared`) under resource_dasm's emulator.
//
// Usage:
//   adblit <ADShared.pef> <RLEP.bin> <Rdat.bin> <clut.bin> <out_prefix>
//
// Emits, for every CSTM animation frame in the RLEP:
//   <out_prefix>.NN.rgba   (raw W*H*4 RGBA8888, alpha 0 = transparent)
// and <out_prefix>.manifest (one "NN W H" line per frame).
//
// How it works: the PEF is loaded with relocations (imports stubbed); the 8-bit
// per-strip blitter at sec0+0x2838 is called once per row (RLE pointer chained
// via the r23 value captured at the 0x289C end-of-row handler), with the canvas
// plot-vectors pointed at a sentinel so every plotted pixel is captured as
// (x = width - r4, y = row, colorIndex = r6). An identity color map makes r6 the
// raw palette index, which we resolve through the module's clut.

#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <vector>
#include <string>
#include <array>
#include <map>

#include "Emulators/MemoryContext.hh"
#include "Emulators/PPC32Emulator.hh"
#include "ExecutableFormats/PEFFile.hh"
#include <phosg/Filesystem.hh>
#include <phosg/Strings.hh>

using namespace std;
using namespace ResourceDASM;

static const uint32_t HANDLER_OFFS[10] = {
    0x289C, 0x28A0, 0x28C0, 0x28FC, 0x292C, 0x297C, 0x29EC, 0x2A48, 0x2B74, 0x2C28};

static uint16_t be16(const string& s, size_t o) {
  return ((uint8_t)s[o] << 8) | (uint8_t)s[o + 1];
}

int main(int argc, char** argv) {
  if (argc < 6) {
    fprintf(stderr, "usage: %s <ADShared.pef> <RLEP.bin> <Rdat.bin> <clut.bin> <out_prefix>\n", argv[0]);
    return 2;
  }
  const char* pef_path = argv[1];
  string rlep = phosg::load_file(argv[2]);
  string rdat = phosg::load_file(argv[3]);
  string clutArgStr = argv[4];
  string clut = (clutArgStr == "IDENT" || clutArgStr == "CTAB")
                    ? string() : phosg::load_file(argv[4]);
  string out_prefix = argv[5];

  // Palette. Three sources:
  //  - "IDENT": identity ramp (R=G=B=index) — for recovering raw indices.
  //  - "CTAB":  parse the sprite's own color table embedded in the RLEP.
  //  - else:    a Mac clut file (id,r,g,b 16-bit; take high byte).
  array<array<uint8_t, 3>, 256> pal{};
  string clutArg = argv[4];
  if (clutArg == "IDENT") {
    for (int i = 0; i < 256; i++) pal[i] = {(uint8_t)i, (uint8_t)i, (uint8_t)i};
  } else if (clutArg == "CTAB") {
    // Trailing CTAB in the RLEP: 'CTAB', then a small header, then 4-byte
    // [x][R][G][B] entries indexed positionally by pixel value.
    size_t ct = rlep.rfind("CTAB");
    if (ct != string::npos) {
      size_t off = ct + 16;  // skip 'CTAB' + 12-byte header
      for (int i = 0; i < 256 && off + 4 <= rlep.size(); i++, off += 4)
        pal[i] = {(uint8_t)rlep[off + 1], (uint8_t)rlep[off + 2], (uint8_t)rlep[off + 3]};
    }
  } else {
    uint32_t ncol = be16(clut, 6) + 1;
    for (uint32_t i = 0, o = 8; i < ncol && o + 8 <= clut.size(); i++, o += 8) {
      uint16_t id = be16(clut, o);
      if (id < 256) pal[id] = {(uint8_t)clut[o + 2], (uint8_t)clut[o + 4], (uint8_t)clut[o + 6]};
    }
  }
  // Sprite cel box from Rdat (0x28 = width, 0x2A = height). BOTH matter: the RLE
  // stream carries only the rows it needs, so a frame whose art stops early has no
  // trailing rows at all. Emitting such a frame at its own decoded height would
  // change the cel box frame to frame — the art would no longer register against a
  // fixed origin, and a renderer that keeps one node size across an animation
  // stretches every short frame. Every frame is emitted at the declared box, art
  // anchored at the top-left, rows past the data left transparent.
  int WIDTH = rdat.size() >= 0x2C ? be16(rdat, 0x28) : 128;
  if (WIDTH <= 0 || WIDTH > 1024) WIDTH = 128;
  int CEL_H = rdat.size() >= 0x2C ? be16(rdat, 0x2A) : 0;
  if (CEL_H <= 0 || CEL_H > 1024) CEL_H = 0;   // 0 = fall back to the decoded height

  // Per-cel dimensions. The Rdat box above is only a bank-wide fallback: Deluxe
  // sprite banks carry each cel's OWN width/height in an 'IHDR' chunk (one per
  // cel, keyed by cel index), and a bank's cels are NOT all the same width
  // (e.g. Guernsey 24000: 68,114,117,121,165,173,177,172,176,180,182,188...).
  // Decoding every cel at the bank width mis-strides all but the one cel that
  // happens to match it. The chunk stream starts at 0x20; each chunk is
  // tag(4) + flags16 + index16 + reserved32 + len16 @+14 (same field this file's
  // CSTM loop below already reads) + data, and an IHDR's 12-byte data holds
  // height at data+8 / width at data+0xA (i.e. chunk+0x18 / chunk+0x1A).
  std::map<uint16_t, std::pair<int, int>> cel_dims;  // cel index -> (width, height)
  for (size_t off = 0x20; off + 16 <= rlep.size();) {
    uint16_t clen = be16(rlep, off + 14);
    if (clen < 16 || off + clen > rlep.size()) break;
    if (clen >= 0x1C && !rlep.compare(off, 4, "IHDR"))
      cel_dims[be16(rlep, off + 6)] = {be16(rlep, off + 0x1A), be16(rlep, off + 0x18)};
    off += clen;
  }

  // --- Load the PEF ----------------------------------------------------------
  auto mem = make_shared<MemoryContext>();
  PEFFile pef(pef_path);
  {
    uint32_t i = 0;
    for (const auto& imp : pef.imports())
      mem->set_symbol_addr(imp.lib_name + ":" + imp.name, 0xF0000000 + (i++) * 4);
  }
  const uint32_t BASE = 0x10000000;
  pef.load_into("AfterDarkShared", mem, BASE);
  uint32_t page = mem->get_page_size();
  auto align = [&](uint32_t a) { return (a + page - 1) & ~(page - 1); };
  uint32_t sec0 = BASE, sec1 = BASE + align(0x3F670);
  // Locate the dispatch jump table -> TOC (r2).
  uint32_t want[10];
  for (int i = 0; i < 10; i++) want[i] = sec0 + HANDLER_OFFS[i];
  uint32_t table_addr = 0;
  for (uint32_t off = 0; off + 40 <= 0x8FBC; off += 4) {
    bool ok = true;
    for (int i = 0; i < 10; i++)
      if (mem->read_u32b(sec1 + off + i * 4) != want[i]) { ok = false; break; }
    if (ok) { table_addr = sec1 + off; break; }
  }
  if (!table_addr) { fprintf(stderr, "jump table not found\n"); return 1; }
  uint32_t toc = table_addr + 17680;

  const uint32_t STRIP_BLIT = sec0 + 0x2838;
  const uint32_t OP0_ENDROW = sec0 + 0x289C;
  // Two distinct plot primitives (different signatures), each behind its own
  // sentinel so we can tell them apart:
  //   r27 @canvas+0x2E : PixelPlot(canvas, x=r4, y=r5, color=r6)
  //   r26 @canvas+0x3A : RunFill  (canvas, xStart=r4, xEnd=r5, y=r6, color=r7)
  const uint32_t PLOT_PIXEL = 0xE0000000, PLOT_RUN = 0xE0000008, DONE_SENT = 0xE0000004;

  // Shared fabricated structures.
  uint32_t stack = mem->allocate(0x20000) + 0x18000;
  uint32_t colormap = mem->allocate(256 * 4);
  for (int i = 0; i < 256; i++) mem->write_u32b(colormap + i * 4, i);
  uint32_t pix_tvec = mem->allocate(8);
  mem->write_u32b(pix_tvec, PLOT_PIXEL); mem->write_u32b(pix_tvec + 4, toc);
  uint32_t run_tvec = mem->allocate(8);
  mem->write_u32b(run_tvec, PLOT_RUN);   mem->write_u32b(run_tvec + 4, toc);
  uint32_t canvas = mem->allocate(0x100);
  mem->write_u16b(canvas + 0x0E, 0);
  mem->write_u16b(canvas + 0x10, 0);
  mem->write_u32b(canvas + 0x2E, pix_tvec);   // r27 = single-pixel plot
  mem->write_u32b(canvas + 0x3A, run_tvec);   // r26 = run fill
  uint32_t tag = mem->allocate(0x20);
  mem->write_u16b(tag + 0x00, 0);
  mem->write_u32b(tag + 0x02, colormap);
  mem->write_u32b(tag + 0x06, canvas);
  mem->write_u16b(tag + 0x10, WIDTH);
  uint32_t rle_buf = mem->allocate(0x10000);

  PPC32Emulator emu(mem);
  // capture state (reset per frame)
  struct Px { int x, y, idx; };
  vector<Px> pixels;
  int cur_y = 0; uint32_t next_ptr = 0; bool saw_endrow = false; uint64_t steps = 0;
  emu.set_debug_hook([&](PPC32Emulator& e) {
    auto& r = e.registers();
    if (r.pc == PLOT_PIXEL) {
      pixels.push_back({WIDTH - (int)r.r[4].s, cur_y, (int)r.r[6].u});
      r.pc = r.lr;
    } else if (r.pc == PLOT_RUN) {
      // fill descending coords [r4, r5) with color r7
      int d0 = (int)r.r[4].s, d1 = (int)r.r[5].s, col = (int)r.r[7].u;
      for (int d = d0; d < d1; d++) pixels.push_back({WIDTH - d, cur_y, col});
      r.pc = r.lr;
    } else if (r.pc == OP0_ENDROW) { next_ptr = r.r[23].u; saw_endrow = true; }
    else if (r.pc == DONE_SENT) throw EmulatorBase::terminate_emulation();
    if (++steps > 50000000) throw EmulatorBase::terminate_emulation();
  });

  // --- Decode every CSTM frame ----------------------------------------------
  string manifest;
  int frame_no = 0;
  size_t pos = 0;
  while ((pos = rlep.find("CSTM", pos)) != string::npos) {
    uint16_t clen = be16(rlep, pos + 14);
    if (clen < 16 || pos + clen > rlep.size()) break;
    // This CSTM's own cel index (chunk+6, same field IHDR is keyed by) may have
    // an IHDR entry with dimensions that differ from the bank-wide fallback —
    // switch the strip blitter to them for this frame only.
    auto dims = cel_dims.find(be16(rlep, pos + 6));
    if (dims != cel_dims.end() && dims->second.first > 0 && dims->second.first <= 1024) {
      WIDTH = dims->second.first;
      CEL_H = dims->second.second;
      mem->write_u16b(tag + 0x10, WIDTH);
    }
    string bodystr = rlep.substr(pos + 16, clen - 16);
    pos += clen;

    // Stage the frame in a scratch buffer that is ZEROED first: a row that reads
    // past the frame body would otherwise decode the PREVIOUS frame's bytes, which
    // is what produced the stale-pixel scanlines this staging used to emit.
    static const string zeros(0x10000, '\0');
    mem->memcpy(rle_buf, zeros.data(), zeros.size());
    mem->memcpy(rle_buf, bodystr.data(), bodystr.size());
    uint32_t rle_addr = rle_buf, rle_end = rle_buf + (uint32_t)bodystr.size();
    pixels.clear();
    uint32_t ptr = rle_addr; cur_y = 0;
    while (ptr < rle_end && cur_y < 512) {
      size_t before = pixels.size();
      auto& regs = emu.registers();
      regs = PPC32Emulator::Regs();
      regs.r[1].u = stack; regs.r[2].u = toc; regs.r[3].u = tag;
      regs.r[4].u = ptr; regs.lr = DONE_SENT; regs.pc = STRIP_BLIT;
      saw_endrow = false;
      try { emu.execute(); } catch (const exception&) { break; }
      if (!saw_endrow) break;
      // A frame's rows are chained head to tail and the last one lands exactly on
      // the end of the body. A row that consumes bytes BEYOND it is not a row at
      // all — it is the row decoder started on the chunk's tail padding and run
      // off the end. Drop its pixels and stop.
      if (next_ptr > rle_end) { pixels.resize(before); break; }
      ptr = next_ptr; cur_y++;
    }
    int H = CEL_H ? CEL_H : cur_y;
    if (cur_y <= 0) { frame_no++; continue; }
    if (cur_y > H) H = cur_y;      // art taller than the declared box: never clip
    vector<uint8_t> img(WIDTH * H * 4, 0);
    for (auto& p : pixels)
      if (p.x >= 0 && p.x < WIDTH && p.y >= 0 && p.y < H) {
        auto& c = pal[p.idx & 0xFF];
        uint8_t* d = &img[(p.y * WIDTH + p.x) * 4];
        d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = 255;
      }
    char path[1024];
    snprintf(path, sizeof(path), "%s.%02d.rgba", out_prefix.c_str(), frame_no);
    FILE* f = fopen(path, "wb");
    fwrite(img.data(), 1, img.size(), f);
    fclose(f);
    { char mline[64]; snprintf(mline, sizeof(mline), "%02d %d %d\n", frame_no, WIDTH, H); manifest += mline; }
    frame_no++;
  }
  phosg::save_file(out_prefix + ".manifest", manifest);
  fprintf(stderr, "[adblit] %s: %d frames, width=%d\n", argv[2], frame_no, WIDTH);
  return 0;
}
