// adhost — runtime host for the PowerPC After Dark modules (A4gm PEFs).
//
// Loads the real `After Dark 4.0 Shared` engine (adxpl510) and a real module
// PEF into one emulated PowerPC address space, links them (module imports ->
// adxpl510 exports; adxpl510 imports -> host OS implementations), calls the
// module's `main` factory, and drives the module frame-by-frame, streaming
// frames to the app over the ADSHM/stdout pipeline. Every Mac Toolbox call the
// engine makes is trapped and serviced by the host (ADTRACE logs them).
//
// Usage:
//   adhost <ADShared.pef> <module.pef> [maxcalls]
// Behavior levers are environment variables; see the KEYENV table below.
//
#include <stdio.h>
#include <stdlib.h>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include <unordered_map>

#include "Emulators/MemoryContext.hh"
#include "Emulators/PPC32Emulator.hh"
#include "ExecutableFormats/PEFFile.hh"
#include "ResourceFile.hh"
#include "IndexFormats/Formats.hh"
#include <phosg/Filesystem.hh>
#include <optional>
#include <cmath>

using namespace std;
using namespace ResourceDASM;

// ---- address map -----------------------------------------------------------
static const uint32_t BASE_A = 0x10000000;   // adxpl510 (After Dark 4.0 Shared)
static const uint32_t BASE_M = 0x30000000;   // the module
static const uint32_t SENT_BASE = 0xE0000000;// import code sentinels (PC markers)
static const uint32_t RET_SENT  = 0xE1000000;// "top-level call returned" marker

// ===================== STUB CENSUS INSTRUMENTATION =========================
// Zero-overhead when ADSTUBCENSUS is unset (g_census stays false; ADSTUB is a
// single branch on a global bool that is never taken). When set, every host
// STUB/PARTIAL-fallback site that returns a hardcoded constant / no-ops where a
// real Mac would compute state is counted, keyed by a static tag. The catch-all
// for named-but-unimplemented OS imports is tagged with the RESOLVED import name
// (UNIMPL:<Lib:Name>) so we get an exact census of every faked API.
//
// Census runs are typically bounded by an external SIGKILL timeout; SIGKILL is uncatchable, so we
// cannot rely on atexit/end-of-main. Instead we arm alarm() to fire well before
// the kill; the handler only sets a flag (async-signal-safe), and do_import —
// invoked on EVERY OS call, i.e. continuously by every frame loop — notices the
// flag and dumps in normal context (map is consistent) then _exit()s.
#include <csignal>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>   // shm_open/mmap for the zero-copy shared-memory transport
#include <sys/stat.h>
#include <chrono>
#include <ctime>
#include <cerrno>         // errno/strerror for the timed-cycle execv fallback
#include <mach-o/dyld.h>  // _NSGetExecutablePath for execv(self) on the timed re-init cycle
// WALL-CLOCK TIME (stream mode): in ADSTREAM (live app) mode the virtual Mac time sources
// (TickCount / Microseconds / GetDateTime) advance at the REAL wall-clock rate, not a fixed step
// PER CALL. A per-call step makes on-screen clocks (Time Flies) run fast: the module reads a time
// source once per DRAW, so at the stream consumer's ~30 fps an ADTICKSTEP=10 step is ~300 ticks/s
// against the true 60.15 Hz. Headless census runs (no ADSTREAM) keep the per-call step — they
// intentionally run faster than realtime. Opt-out ADNOWALLCLOCK.
static double ad_wall_elapsed(){
  static const auto t0 = std::chrono::steady_clock::now();
  return std::chrono::duration<double>(std::chrono::steady_clock::now()-t0).count();
}
static bool ad_wallclock_on(){
  static const bool on = getenv("ADSTREAM") && !getenv("ADNOWALLCLOCK");
  return on;
}
// IDLE TICK CATCH-UP boost. Wall-clock time starves tick-deadline-gated module phases that need
// accelerated virtual time: Art Critic's init needs BOTH ~900 unpaced DRAW calls AND stepped
// virtual time (1.8s with both; distinct=1 over 62s if either wall-clock or pacing throttles it).
// The frame loop accrues this monotonic offset while the module provably idles (advance draws leave
// the fb unchanged); TickCount/Microseconds add it on top of wall time. 0 unless the loop accrues.
static uint32_t g_adff_ticks = 0;
// BANNER OVERLAY — PPC side, symmetric with the 68K host. The real engine composites the shared
// library's (adxpl510) DoUserMessage banner [BASE_A+0xEB34,+0xEC80) as a TIMED OVERLAY: drawn over the
// animation, cleared on a timeout, never baked into the sprite background. The host captures it at the
// (suppressed) draw site and composites it onto the PRESENTED frame for the first N frames, then clears.
static bool g_banner_have=false; static std::string g_banner_text; static int g_banner_x=0, g_banner_y=0; static uint8_t g_banner_fg=0;
static std::map<std::string,uint64_t> g_stubhits;   // tag -> hit count
static bool g_census = false;                        // set from ADSTUBCENSUS
static volatile sig_atomic_t g_dump_now = 0;         // set by alarm handler
#define ADSTUB(tag) do{ if(g_census) g_stubhits[tag]++; }while(0)

// ---- live keyboard/mouse input from the app (stdin line protocol) ---------------
// Same channel as SET/GO: "KEY <kc> <0|1>", "CAPS <0|1>", "MOUSE <x> <y> <btn>".
// Live Mac KeyMap (flat 16-byte order), caps + mouse state. Starts headless-zero (all
// keys up, caps off, mouse (0,0) up) and is only mutated by app protocol lines, so an
// input-less run behaves exactly as if there were no input channel at all.
static uint8_t g_km[16] = {0};
static bool    g_caps   = false;
static int     g_msx=0, g_msy=0;
static bool    g_msbtn  = false;
static bool    g_input_seen = false;
static const int kCapsKeycode = 57;
static inline void ad_km_set(int kc,bool dn){ if(kc<0||kc>127) return;
  uint8_t m=(uint8_t)(1u<<(kc&7)); if(dn) g_km[kc>>3]|=m; else g_km[kc>>3]&=(uint8_t)~m; }
// ---- Rodger Dodger PPC keypump ---------------------------------------------------
// The shared AD 4.0 library (ADShared40.pef = adxpl510) exports the UserInput class; its
// buffered key path is a global circular ring of static members addressed r2(TOC)-relative:
//   theirKeyPressed@TOC+0x448 (flag,w), theirKeyWrite@+0x44A (head,w), theirKeyRead@+0x44C
//   (tail,w), theirKeyBufferSize@+0x44E (w), theirKeyBuffer@+0x450 (4-byte KeyType slots).
// AddKey(KeyType) (code 0x1002D8B0): buffer[write*4]=key; write=(write+1)%size; if
// read==write read=(read+1)%size; pressed=1.  (PPC increments write by +1; the 68K build
// uses +2.)  The real AD shell called AddKey on each keyDown; this host's BLANK/DRAW drive
// never reaches it, so the host reproduces AddKey itself.
static int g_rd_dir = 0;   // currently-held RD KeyType (0 = none)
// Map a Mac virtual keycode -> RD KeyType (the four values RD's KeyToMove recognises:
// 0x26/0x68->dir1(up) 0x27/0x66->dir2(right) 0x28/0x62->dir3(down) 0x25/0x64->dir4(left)).
static int ad_rd_keytype(int kc){
  // KeyType is {short keycode; short mods} big-endian and every reached consumer reads the
  // HIGH halfword (lha at the word base), so the keycode goes in the TOP 16 bits; a keycode
  // packed into the low half is invisible to the module.
  switch(kc){
    case 0x7E: case 0x0D: return 0x260000; // Up arrow    / W
    case 0x7D: case 0x01: return 0x280000; // Down arrow  / S
    case 0x7B: case 0x00: return 0x250000; // Left arrow  / A
    case 0x7C: case 0x02: return 0x270000; // Right arrow / D
  }
  return 0;
}
static bool ad_input_line(const char* s){
  int a,b,c;
  if(sscanf(s,"KEY %d %d",&a,&b)==2){ ad_km_set(a,b!=0); g_input_seen=true;
    { int kt=ad_rd_keytype(a);   // maintain the held RD direction (keypump)
      if(kt){ if(b) g_rd_dir=kt; else if(g_rd_dir==kt) g_rd_dir=0; } }
    if(getenv("ADINPUTLOG")) fprintf(stderr,"[input] KEY kc=%d down=%d rd_dir=%02X\n",a,b,g_rd_dir); return true; }
  if(sscanf(s,"CAPS %d",&a)==1){ g_caps=(a!=0); ad_km_set(kCapsKeycode,g_caps); g_input_seen=true;
    if(getenv("ADINPUTLOG")) fprintf(stderr,"[input] CAPS=%d\n",g_caps); return true; }
  if(sscanf(s,"MOUSE %d %d %d",&a,&b,&c)==3){ g_msx=a; g_msy=b; g_msbtn=(c!=0); g_input_seen=true;
    if(getenv("ADINPUTLOG")) fprintf(stderr,"[input] MOUSE x=%d y=%d btn=%d\n",a,b,c); return true; }
  return false;
}

// ---- OPT-IN SEEDING OF THE adxpl510 LIBRARY RNG ----------------------------------
// WHAT THE RNG IS. A module's "Random" option does not roll Toolbox Random. It
// cross-TOC-calls the shared library's own export RandInt__FUl (adxpl510
// @1002A1C4), whose core @1002A140 is a 55-lag ADDITIVE LAGGED-FIBONACCI
// generator living in four adxpl510 data-section globals (library TOC 0x10048000):
//     TOC-0x254C uint32 tbl[55]     working state
//     TOC-0x2470 uint32 master[55]  baked constant key
//     TOC-0x2394 int16  i           \ lag cursors, reset to 24 / 0 on seeding
//     TOC+0x0442 int16  j           /
// Its single seeding entry point is the export SRand__FUi (@10029D74), which
// refills tbl[k] = master[k] ^ (two draws of the library's ANSI LCG @1003781C)
// and resets the cursors. Randomize__Fv / Set/GetFullSRand exist too, and the
// library never calls any of them itself — seeding is the client's job.
//
// WHY EVERY LAUNCH REPLAYS ONE FIXED SEQUENCE. The MODULE is that client, and it
// seeds from the clock inside its own INIT. Swirling Magic @300047E8 calls the
// glue for XTimer::GetMilliseconds() and hands the result straight to SRand at
// @300047F0, then makes its first "Random" style pick a few hundred instructions
// later — all still inside main(selector=0). On a real Mac Microseconds() counts
// from BOOT, so that seed was milliseconds of uptime: a different number every
// launch. Our InterfaceLib:Microseconds serves a virtual clock whose origin is 0
// at PROCESS START, and INIT happens at a fixed point in the startup sequence, so
// the seed is a constant — measured 0x00000064 under ADNOWALLCLOCK (one call x
// ADUSPERCALL) and 0x00000000 with the wall clock on (<1 ms elapsed at INIT).
// Same seed, same stream, same picks, every launch. It is a host-clock-ORIGIN
// artefact; neither the module nor the library is doing anything wrong.
// (ADFAKEEPOCH and ADREALCLOCK move GetDateTime and ADRANDSEED moves
// InterfaceLib:Random — three different generators, none of them this one.)
//
// THE LEVER. Opt-in, because the determinism is LOAD-BEARING: the byte-exact PPC
// census baselines are recorded against that constant seed. Unset, not one
// instruction below runs.
//     ADLIBRNGSEED=<hex>   fixed reproducible seed (RE, bisecting a roll)
//     ADLIBRNGSEED=random  true per-launch randomisation (arc4random)
//     ADLIBRNGSEED=probe   report the RNG state, seed nothing (read-only)
// Armed, it acts at exactly two points, both of which run the library's OWN
// SRand code — the generator, the master key and the cursors are never written
// by the host, only the seed ARGUMENT the machine's uptime would have supplied:
//   (1) a host call to SRand(seed) before main(selector=0), which covers modules
//       that never seed the library themselves; and
//   (2) an argument override on the module's own SRand call. Point (2) is what
//       decides the render: seeding earlier is overwritten by the module's own
//       INIT-time SRand, and seeding after INIT is too late for the first style
//       pick.
static bool     g_librng_armed = false;
static uint32_t g_librng_seed  = 0;
static uint32_t g_librng_srand_pc = 0;   // adxpl510 SRand__FUi code entry, resolved at load

// ---- OPT-IN REAL HARDWARE CAPS-LOCK POLLING (ADREALCAPS=1) -----------------------
// The consumer is PortableModule::GetCapsLockChange() in the AD 4.0 shared library, which
// reads the low-memory Mac KeyMap (68K: 0x174 directly; PPC: via the TOC pointer) and
// edge-detects it. The host only has to keep g_caps truthful and everything wired
// downstream fires. Instead of routing the state app->stdin, the host can ask the OS what
// caps is doing right now — which also works when there is no app at all.
//
// WHICH API, AND WHY NOT THE OBVIOUS ONE (measured):
//   CGEventSourceKeyState(state, 0x39 /*kVK_CapsLock*/) DOES NOT WORK for caps. Caps-Lock is
//   a LOCKING modifier: the physical key is not held down while the lock is engaged, so the
//   per-key state read returns 0 even with caps ON.
//   CGEventSourceFlagsState(...) & kCGEventFlagMaskAlphaShift IS correct and tracks the
//   toggle exactly (flags 0x00000100 -> 0x00010100 -> 0x00000100).
//
// NO PERMISSION PROMPT: a modifier-FLAGS read is not an event tap and is not a per-key
// query, so it does not engage the Input Monitoring (kTCCServiceListenEvent) gate.
// Confirmed from a fresh, never-before-run binary path (TCC is keyed per-binary, so a
// fresh path is the case that would prompt): no TCC log entries, no dialog.
//
// DECLARED BY HAND, NOT #included: pulling in <ApplicationServices/...> would drag the real
// Mac Rect/Point/GrafPort typedefs into a file that defines its OWN emulated versions of
// those very types. Two C symbols with a documented ABI are all that is needed; link with
// -framework ApplicationServices.
extern "C" uint64_t CGEventSourceFlagsState(int32_t stateID);
static const int32_t  kADCGCombinedSessionState = 0;          // kCGEventSourceStateCombinedSessionState
static const uint64_t kADCGFlagMaskAlphaShift   = 0x00010000ULL; // kCGEventFlagMaskAlphaShift

// Poll the real caps state and fold any CHANGE into exactly the state the "CAPS <0|1>"
// stdin line sets. INERT BY CONSTRUCTION, three ways:
//   1. off unless ADREALCAPS is set -> census/headless untouched;
//   2. even when armed, if the polled level equals g_caps NOTHING is written — so a run
//      with caps off never sets g_input_seen, and with g_input_seen false the host never
//      writes the KeyMap mirror;
//   3. it only ever assigns the same three variables ad_input_line already assigns.
static void ad_poll_real_caps(){
  static const bool armed = getenv("ADREALCAPS")!=nullptr;
  if(!armed) return;
  bool rc = (CGEventSourceFlagsState(kADCGCombinedSessionState) & kADCGFlagMaskAlphaShift)!=0;
  if(rc==g_caps) return;                       // no edge -> touch nothing at all
  g_caps = rc; ad_km_set(kCapsKeycode, rc); g_input_seen = true;
  if(getenv("ADINPUTLOG")) fprintf(stderr,"[input] REALCAPS=%d\n", rc?1:0);
}
// UserInput ring globals, resolved once at startup from the AddKey TVector's TOC
// (== the r2 the library methods use). 0 until resolved / until the module imports UserInput.
static bool     g_keypump_on = false;      // gated on RD + ring resolved + !ADNOKEYPUMP
static uint32_t g_ui_pressed=0,g_ui_write=0,g_ui_read=0,g_ui_size=0,g_ui_buffer=0;
static long     g_rd_feedlog = 0;

static void census_alarm(int){ g_dump_now = 1; }
static void census_dump(){
  static bool done=false; if(done) return; done=true;
  if(!g_census) return;
  uint64_t total=0;
  for(const auto& kv : g_stubhits){
    fprintf(stderr,"[stubhit] %s %llu\n", kv.first.c_str(), (unsigned long long)kv.second);
    total += kv.second;
  }
  fprintf(stderr,"[stubcensus] distinct_tags_hit=%zu total_hits=%llu\n",
          g_stubhits.size(), (unsigned long long)total);
  fflush(stderr);
}
// ===========================================================================

struct Host {
  shared_ptr<MemoryContext> mem;
  vector<string> import_names;                // index -> "Lib:Name"
  unordered_map<uint32_t,uint32_t> block_size;// ptr -> size (Memory Manager)
  uint64_t call_count = 0;
  uint64_t max_calls = 2000;
  bool verbose = true;
  uint32_t ad_info = 0;   // lazily-built After Dark Gestalt info struct

  // Screen canvas (a real color GrafPort + PixMap over a host framebuffer)
  int fb_w = getenv("ADSCREENW") ? atoi(getenv("ADSCREENW")) : 512;
  int fb_h = getenv("ADSCREENH") ? atoi(getenv("ADSCREENH")) : 384;
  uint32_t g_fb=0, g_clut=0, g_pm=0, g_pmh=0, g_port=0, g_gdev=0, g_gdh=0, g_visrgn=0;
  uint32_t g_addl=0;     // 'ADdl' Deluxe entry-point blob (for read-watch tracing)
  uint32_t g_frame=0;    // the engine "frame"/faceplate context blob (read-watch)
  // Software QuickDraw pen/port state (for modules that draw primitives to the port)
  uint8_t qd_fg=1, qd_bg=0;      // current fore/back palette index (fg default non-zero)
  int16_t pen_h=0, pen_v=0;      // pen position (local coords)
  int16_t pen_w=1, pen_hgt=1;    // pen size (PenSize) — line/frame thickness
  int16_t pen_mode=8;            // pen transfer mode (PenMode); 8=patCopy (tracked only)
  int16_t txt_font=0, txt_face=0, txt_size=12; // TextFont/Face/Size state (tracked)
  int16_t qd_org_h=0, qd_org_v=0;// SetOrigin offset (tracked; applied only if ADSETORIGIN)
  int16_t qd_clip_t=0,qd_clip_l=0,qd_clip_b=32767,qd_clip_r=32767; // clip rect
  uint32_t cur_port=0;   // current QuickDraw port (SetGWorld/SetPort target)
  vector<uint32_t> gworlds;   // offscreen GWorld ports created via NewGWorld
  uint64_t copybits_count=0;
  // RNG seed the Toolbox Random() reads (low-mem RndSeed); LMSetRndSeed writes it.
  uint32_t rnd_seed = getenv("ADRANDSEED")? strtoul(getenv("ADRANDSEED"),0,0) : 0x12345678;
  // Color Manager search-proc registrations (AddSearch/DelSearch). The host maps colors
  // through g_clut directly and never invokes these procs; the list is faithful bookkeeping.
  vector<uint32_t> color_searches;

  // Resource Manager — a chain of open resource files (module first, then the
  // companion 'After Dark 4.0 Library'). Get1* uses the current file; GetResource
  // / GetIndString fall through the whole chain (Mac semantics, simplified).
  vector<ResourceFile> res_files;
  int cur_file = 0;                                 // index into res_files
  map<pair<uint32_t,int16_t>,uint32_t> res_handle;  // (type,id) -> emulated Handle
  map<uint32_t,pair<uint32_t,int16_t>> handle_res;  // Handle -> (type,id)
  int16_t last_res_err = 0;                          // ResError: result of the last Resource Mgr call
  // Memory Manager handle state byte: handle -> flags. bit7(0x80)=locked,
  // bit6(0x40)=purgeable (Inside Macintosh: Memory). HGetState reads it; HLock/HUnlock/
  // HPurge/HNoPurge/HSetState maintain it.
  unordered_map<uint32_t,uint8_t> h_flags;

  uint32_t page_align(uint32_t a){ uint32_t p=mem->get_page_size(); return (a+p-1)&~(p-1); }

  // Find the file index holding (type,id): current file first, then the chain.
  int find_file(uint32_t type, int16_t id){
    if(cur_file>=0 && cur_file<(int)res_files.size() && res_files[cur_file].resource_exists(type,id)) return cur_file;
    for(int i=0;i<(int)res_files.size();i++) if(res_files[i].resource_exists(type,id)) return i;
    return -1;
  }
  // Make (or reuse) a Mac Handle for a resource: master ptr -> data block.
  uint32_t handle_for(uint32_t type, int16_t id){
    auto key=make_pair(type,id);
    auto it=res_handle.find(key); if(it!=res_handle.end()) return it->second;
    int fi=find_file(type,id); if(fi<0) return 0;
    auto res=res_files[fi].get_resource(type,id);
    uint32_t data = res->data.empty()? mem->allocate(1) : mem->allocate(res->data.size());
    if(!res->data.empty()) mem->memcpy(data, res->data.data(), res->data.size());
    uint32_t h=mem->allocate(4); mem->write_u32b(h,data);
    block_size[h]=res->data.size(); block_size[data]=res->data.size();
    res_handle[key]=h; handle_res[h]=key;
    return h;
  }
};

// A trivial bump heap inside a big preallocated arena, so NewPtr/NewHandle
// hand out addresses far from the fixed fragment bases.
struct Heap { uint32_t cur, end; };

int main(int argc, char** argv){
  if(argc<3){ fprintf(stderr,"usage: %s <ADShared.pef> <module.pef> [maxcalls]\n",argv[0]); return 2; }
  Host H; H.mem = make_shared<MemoryContext>();
  auto mem = H.mem;
  if(argc>=4) H.max_calls = strtoull(argv[3],nullptr,0);

  // ---- STUB CENSUS: arm counting + pre-KILL self-alarm ---------------------
  if(getenv("ADSTUBCENSUS")){
    g_census = true;
    // Number of STATIC ADSTUB(...) call sites instrumented in do_import. The
    // catch-all (UNIMPL:<name>) is one further site that emits many dynamic tags.
    const int ADSTUB_STATIC_SITES = 38;
    fprintf(stderr,"[stubcensus] total_tags_instrumented=%d (static stub sites; +1 name-resolved catch-all)\n",
            ADSTUB_STATIC_SITES);
    signal(SIGALRM, census_alarm);
    signal(SIGTERM, census_alarm);
    unsigned secs = getenv("ADCENSUSALARM")? (unsigned)strtoul(getenv("ADCENSUSALARM"),0,0) : 55;
    alarm(secs);
    atexit(census_dump);   // also dump on normal exit (module finished < alarm)
  }

  // Map a zeroed "low memory" page. Classic Mac globals live at 0..0x3000, and
  // it lets uninitialised engine pointers (null objects) read 0 and no-op
  // gracefully instead of faulting — invaluable for reconnaissance.
  mem->allocate_at(0x0, 0x4000);
  mem->memset(0x0, 0, 0x4000);

  // ---- 1. register OS import sentinels (must exist before loading adxpl510) --
  PEFFile ad(argv[1]);
  {
    // Build ordered import table + a TVector per import pointing at a code
    // sentinel. adxpl510 calls these via cross-TOC glue -> pc = sentinel.
    const auto imps = ad.imports();
    H.import_names.resize(imps.size());
    uint32_t tvec_region = mem->allocate(imps.size()*8);
    for(uint32_t i=0;i<imps.size();i++){
      H.import_names[i] = imps[i].lib_name + ":" + imps[i].name;
      uint32_t tv = tvec_region + i*8;
      mem->write_u32b(tv,   SENT_BASE + i*4);  // code entry (a bare PC marker)
      mem->write_u32b(tv+4, 0);                // toc (unused by host stubs)
      mem->set_symbol_addr((imps[i].lib_name+":"+imps[i].name).c_str(), tv);
    }
    fprintf(stderr,"[adhost] registered %zu OS import sentinels\n",imps.size());
  }

  // ---- 2. load adxpl510 (load_into registers all exports as adxpl510:name) --
  ad.load_into("adxpl510", mem, BASE_A);
  fprintf(stderr,"[adhost] adxpl510 loaded at %08X, %zu exports registered\n",
          BASE_A, ad.exports().size());

  // ---- 3. load the module, resolve its imports ----------------------------
  PEFFile mod(argv[2]);
  // Any module imports NOT satisfied by adxpl510 fall back to OS sentinels too.
  {
    uint32_t base_extra = 0; // (kept simple: FT imports are all adxpl510:*)
    for(const auto& im : mod.imports()){
      string key = im.lib_name+":"+im.name;
      try { mem->get_symbol_addr(key.c_str()); }
      catch(const out_of_range&){
        // unknown import: give it its own OS sentinel appended to the table
        uint32_t i = H.import_names.size();
        H.import_names.push_back(key);
        uint32_t tv = mem->allocate(8);
        mem->write_u32b(tv, SENT_BASE+i*4); mem->write_u32b(tv+4,0);
        mem->set_symbol_addr(key.c_str(), tv);
        (void)base_extra;
      }
    }
  }
  mod.load_into("module", mem, BASE_M);
  // Compute each section's runtime address exactly as load_into places them, by
  // parsing the PEF section headers from the file (PEFFile::sections is private).
  // PEF: 40-byte container header (sectionCount @+0x20, u16be), then 28-byte
  // section headers; each has totalSize @+0x08. load_into skips total_size==0
  // sections and page-aligns each non-empty section after the previous.
  // FT's code section was 0x13200, but this must be per-module general.
  uint32_t m_sec0 = BASE_M, m_sec1 = 0;
  { FILE* pf=fopen(argv[2],"rb");
    if(pf){ uint8_t hdr[40]; if(fread(hdr,1,40,pf)==40){
      int nsec=(hdr[0x20]<<8)|hdr[0x21];
      uint32_t a=BASE_M, pg=mem->get_page_size();
      for(int i=0;i<nsec;i++){ uint8_t sh[28]; if(fread(sh,1,28,pf)!=28) break;
        uint32_t sz=(sh[8]<<24)|(sh[9]<<16)|(sh[10]<<8)|sh[11];
        if(sz==0) continue;
        if(i==0) m_sec0=a; if(i==1) m_sec1=a;
        a=(a+sz+(pg-1))&~(pg-1); } }
      fclose(pf); } }
  if(!m_sec1) m_sec1 = H.page_align(BASE_M + 0x13200); // fallback
  const auto& mainsym = mod.main();
  uint32_t main_tvec = (mainsym.section_index==0? m_sec0 : m_sec1) + mainsym.value;
  fprintf(stderr,"[adhost] module loaded: sec0=%08X sec1=%08X  main TVector @%08X\n",
          m_sec0,m_sec1,main_tvec);
  uint32_t main_code = mem->read_u32b(main_tvec);
  uint32_t main_toc  = mem->read_u32b(main_tvec+4);
  if(getenv("ADMAINCODE")){ main_code = (uint32_t)strtoul(getenv("ADMAINCODE"),0,16);
    fprintf(stderr,"[adhost] ADMAINCODE override: main_code=%08X\n",main_code); }
  if(getenv("ADMAINTOC")){ main_toc = (uint32_t)strtoul(getenv("ADMAINTOC"),0,16); }
  fprintf(stderr,"[adhost] main: code=%08X toc=%08X\n",main_code,main_toc);

  // ---- 3c. load resource forks (module, then companion Library) ------------
  auto load_rf = [&](const char* path, const char* label){
    try { H.res_files.push_back(parse_resource_fork(phosg::load_file(path)));
          auto& rf=H.res_files.back();
          fprintf(stderr,"[adhost] %s: %zu resources of %zu types\n",label,
                  rf.all_resources().size(), rf.all_resource_types().size());
    } catch(const exception& ex){ fprintf(stderr,"[adhost] %s load failed: %s\n",label,ex.what()); }
  };
  if(const char* rp = getenv("ADRSRC")) load_rf(rp,"module fork");
  else if(!getenv("ADNORSRCAUTO")){
    // With ADRSRC unset the Resource Manager is empty and the module's OWN resource fork
    // (cel bitmaps, source photos, palettes) never loads: Points of View then projects an
    // empty field and Rock Paper Scissors' DoInitialize returns -1 and draws nothing. So
    // when ADRSRC is unset, auto-load the module's macOS resource named-fork
    // <argv[2]>/..namedfork/rsrc. Opt-out ADNORSRCAUTO. Passing ADRSRC explicitly is still
    // preferred. Note ADMAINCODE is per-module: 300074F4 is Points of View's dispatcher and
    // is a no-op entry in Rock Paper Scissors, whose real main auto-detects at 3001C680.
    std::string rf = std::string(argv[2]) + "/..namedfork/rsrc";
    fprintf(stderr,"[adhost_re] ADRSRC unset — auto-loading module resource fork %s\n",rf.c_str());
    load_rf(rf.c_str(),"module fork (auto)");
  }
  else fprintf(stderr,"[adhost] no ADRSRC set — Resource Manager empty\n");
  if(const char* lp = getenv("ADLIB"))  load_rf(lp,"library fork");

  // ---- 3b. dump the module's 8-selector jump table (main TOC + 0x27F8) ------
  // Skip/guard for GraphicsModule (no-main) modules: main_toc is garbage there.
  if(!getenv("ADGM")) try {
    uint32_t jt = main_toc + 10232;   // 0x27F8
    fprintf(stderr,"[adhost] module selector table @%08X:\n",jt);
    for(int i=0;i<8;i++){
      uint32_t h = mem->read_u32b(jt + i*4);
      fprintf(stderr,"    sel %d -> %08X (module+%06X)\n", i, h, h-BASE_M);
    }
  } catch(const exception&){ fprintf(stderr,"[adhost] (selector table unreadable — no-main module?)\n"); }

  if(const char* sw=getenv("ADSCANWORD")){  // scan sec1 for a 32-bit word (find TVectors)
    uint32_t want=(uint32_t)strtoul(sw,0,16);
    for(uint32_t a=m_sec1; a<m_sec1+0x8000; a+=4){
      if(mem->read_u32b(a)==want)
        fprintf(stderr,"[scan] %08X = %08X  (next word %08X)\n",a,want,mem->read_u32b(a+4));
    }
  }

  if(const char* dz=getenv("ADDASM")){   // ADDASM=startoff:endoff (module offsets, hex)
    uint32_t a0=0,a1=0; sscanf(dz,"%x:%x",&a0,&a1);
    for(uint32_t a=BASE_M+a0; a<BASE_M+a1; a+=4){
      uint32_t op=mem->read_u32b(a);
      fprintf(stderr,"mod+%05X  %08X  %s\n", a-BASE_M, op,
              PPC32Emulator::disassemble_one(a,op).c_str());
    }
  }

  if(const char* dz=getenv("ADDASMABS")){  // ADDASMABS=startaddr:endaddr (absolute, hex)
    uint32_t a0=0,a1=0; sscanf(dz,"%x:%x",&a0,&a1);
    for(uint32_t a=a0; a<a1; a+=4){
      uint32_t op=mem->read_u32b(a);
      fprintf(stderr,"%08X  %08X  %s\n", a, op,
              PPC32Emulator::disassemble_one(a,op).c_str());
    }
  }

  if(getenv("ADTOC")){
    for(uint32_t o : {0x1D8u,0x1DCu,0x384u,0x358u,0x35Cu,0x350u})
      fprintf(stderr,"[toc] r2+%03X = %08X\n", o, mem->read_u32b(main_toc+o));
  }

  // ---- 4. fabricate a stack + a scratch arg block --------------------------
  // Host allocations must NOT overlap the engine (BASE_A=0x10000000) or a module
  // image (BASE_M=0x30000000+). The general allocator can straddle BASE_M for
  // SMALL modules (Magic Turtle's stack landed ~0x2FFD3400..0x30013400, overlapping
  // sec0 -> the module's stmw register-saves corrupted its own data / theFrame ->
  // strcpy fault @0x320000). Detect that and relocate clear, at a fixed high base.
  // hi_alloc(size, prefBase): allocate `size` bytes, relocating to a high address
  // if the natural allocation would intersect the module region [BASE_M,+0x100000).
  auto overlaps_mod = [&](uint32_t a, uint32_t sz){ return a < BASE_M + 0x100000 && a + sz > BASE_M; };
  auto hi_alloc = [&](uint32_t sz, uint32_t hiBase)->uint32_t{
    uint32_t a = mem->allocate(sz);
    if(overlaps_mod(a, sz)){
      try { mem->allocate_at(hiBase, sz); a = hiBase;
            fprintf(stderr,"[adhost] relocated host alloc (%X bytes) to %08X (overlapped module)\n",sz,hiBase); }
      catch(const exception&){ fprintf(stderr,"[adhost] WARN: hi_alloc relocation to %08X failed\n",hiBase); }
    }
    return a;
  };
  // Emulated stack at a FIXED high base (0x60000000) clear of the engine (0x10000000),
  // modules (0x30000000+), and all other host allocations (0x50000000 region: canvas,
  // frame, GWorlds). A stack adjacent to the frame lets a module's stwu corrupt
  // frame/GMParamBlock fields (Points of View's DoInitialize then reads a clobbered flag
  // and returns -1). Fallback to hi_alloc.
  uint32_t stack;
  try { mem->allocate_at(0x60000000, 0x80000); stack = 0x60000000 + 0x38000; }
  catch(const exception&){ stack = hi_alloc(0x40000, 0x50000000) + 0x38000; }
  uint32_t scratch = hi_alloc(0x1000, 0x50100000);
  // ADMAPLOW=addr:size — pre-map a low memory region a module computes as a buffer
  // address but that our allocator never handed out (Magic Turtle strcpy's a name to
  // a consistent 0x003222A4 regardless of layout — mapping it lets the write succeed
  // and the module continue instead of faulting "not within any arena").
  // Comma-separated list of addr:size regions, e.g. "20000000:100000,80000000:100000".
  if(const char* ml=getenv("ADMAPLOW")){ string s(ml);
    size_t pos=0; while(pos<s.size()){ size_t c=s.find(',',pos); string one=s.substr(pos,c==string::npos?string::npos:c-pos);
      uint32_t a=0,sz=0; sscanf(one.c_str(),"%x:%x",&a,&sz);
      if(a&&sz){ try{ mem->allocate_at(a,sz); mem->memset(a,0,sz);
          fprintf(stderr,"[adhost] pre-mapped low region %08X..%08X\n",a,a+sz);}catch(const exception&){} }
      if(c==string::npos) break; pos=c+1; } }
  mem->memset(scratch,0,0x1000);

  // ---- 4b. map adxpl510 export code-addresses -> names (for call tracing) --
  // Each export is a TVector {code,toc}; index by its code entry so we can name
  // engine functions as they're entered.
  unordered_map<uint32_t,string> ad_fn_name;
  for(const auto& kv : ad.exports()){
    const auto& e = kv.second;
    uint32_t tvec = (e.section_index==0? BASE_A : H.page_align(BASE_A+0x3F670)) + e.value;
    // only data-section TVectors resolve to code; guard the read
    try { uint32_t code = mem->read_u32b(tvec); if(code>=BASE_A && code<BASE_A+0x40000) ad_fn_name.emplace(code, kv.first); }
    catch(...){}
  }
  bool trace_engine = getenv("ADENGINE")!=nullptr;

  // ---- 4d. software QuickDraw: raster primitives into the current port ------
  // Modules that draw lines/rects/text straight to the screen port (Messages,
  // Slow Burn, Shadow Agents) need real QuickDraw; the engine's sprite modules
  // don't. Font: 8x8, ASCII 0x20-0x7F, bit 0 = leftmost column (font8x8_basic).
  static const uint8_t QDFONT[96][8] = {
    {0,0,0,0,0,0,0,0},{0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00},{0x36,0x36,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x36,0x36,0x7F,0x36,0x7F,0x36,0x36,0x00},{0x0C,0x3E,0x03,0x1E,0x30,0x1F,0x0C,0x00},{0x00,0x63,0x33,0x18,0x0C,0x66,0x63,0x00},
    {0x1C,0x36,0x1C,0x6E,0x3B,0x33,0x6E,0x00},{0x06,0x06,0x03,0x00,0x00,0x00,0x00,0x00},{0x18,0x0C,0x06,0x06,0x06,0x0C,0x18,0x00},
    {0x06,0x0C,0x18,0x18,0x18,0x0C,0x06,0x00},{0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},{0x00,0x0C,0x0C,0x3F,0x0C,0x0C,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x06},{0x00,0x00,0x00,0x3F,0x00,0x00,0x00,0x00},{0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00},
    {0x60,0x30,0x18,0x0C,0x06,0x03,0x01,0x00},{0x3E,0x63,0x73,0x7B,0x6F,0x67,0x3E,0x00},{0x0C,0x0E,0x0C,0x0C,0x0C,0x0C,0x3F,0x00},
    {0x1E,0x33,0x30,0x1C,0x06,0x33,0x3F,0x00},{0x1E,0x33,0x30,0x1C,0x30,0x33,0x1E,0x00},{0x38,0x3C,0x36,0x33,0x7F,0x30,0x78,0x00},
    {0x3F,0x03,0x1F,0x30,0x30,0x33,0x1E,0x00},{0x1C,0x06,0x03,0x1F,0x33,0x33,0x1E,0x00},{0x3F,0x33,0x30,0x18,0x0C,0x0C,0x0C,0x00},
    {0x1E,0x33,0x33,0x1E,0x33,0x33,0x1E,0x00},{0x1E,0x33,0x33,0x3E,0x30,0x18,0x0E,0x00},{0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00},
    {0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x06},{0x18,0x0C,0x06,0x03,0x06,0x0C,0x18,0x00},{0x00,0x00,0x3F,0x00,0x00,0x3F,0x00,0x00},
    {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},{0x1E,0x33,0x30,0x18,0x0C,0x00,0x0C,0x00},{0x3E,0x63,0x7B,0x7B,0x7B,0x03,0x1E,0x00},
    {0x0C,0x1E,0x33,0x33,0x3F,0x33,0x33,0x00},{0x3F,0x66,0x66,0x3E,0x66,0x66,0x3F,0x00},{0x3C,0x66,0x03,0x03,0x03,0x66,0x3C,0x00},
    {0x1F,0x36,0x66,0x66,0x66,0x36,0x1F,0x00},{0x7F,0x46,0x16,0x1E,0x16,0x46,0x7F,0x00},{0x7F,0x46,0x16,0x1E,0x16,0x06,0x0F,0x00},
    {0x3C,0x66,0x03,0x03,0x73,0x66,0x7C,0x00},{0x33,0x33,0x33,0x3F,0x33,0x33,0x33,0x00},{0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},
    {0x78,0x30,0x30,0x30,0x33,0x33,0x1E,0x00},{0x67,0x66,0x36,0x1E,0x36,0x66,0x67,0x00},{0x0F,0x06,0x06,0x06,0x46,0x66,0x7F,0x00},
    {0x63,0x77,0x7F,0x7F,0x6B,0x63,0x63,0x00},{0x63,0x67,0x6F,0x7B,0x73,0x63,0x63,0x00},{0x1C,0x36,0x63,0x63,0x63,0x36,0x1C,0x00},
    {0x3F,0x66,0x66,0x3E,0x06,0x06,0x0F,0x00},{0x1E,0x33,0x33,0x33,0x3B,0x1E,0x38,0x00},{0x3F,0x66,0x66,0x3E,0x36,0x66,0x67,0x00},
    {0x1E,0x33,0x07,0x0E,0x38,0x33,0x1E,0x00},{0x3F,0x2D,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},{0x33,0x33,0x33,0x33,0x33,0x33,0x3F,0x00},
    {0x33,0x33,0x33,0x33,0x33,0x1E,0x0C,0x00},{0x63,0x63,0x63,0x6B,0x7F,0x77,0x63,0x00},{0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x00},
    {0x33,0x33,0x33,0x1E,0x0C,0x0C,0x1E,0x00},{0x7F,0x63,0x31,0x18,0x4C,0x66,0x7F,0x00},{0x1E,0x06,0x06,0x06,0x06,0x06,0x1E,0x00},
    {0x03,0x06,0x0C,0x18,0x30,0x60,0x40,0x00},{0x1E,0x18,0x18,0x18,0x18,0x18,0x1E,0x00},{0x08,0x1C,0x36,0x63,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},{0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},{0x00,0x00,0x1E,0x30,0x3E,0x33,0x6E,0x00},
    {0x07,0x06,0x06,0x3E,0x66,0x66,0x3B,0x00},{0x00,0x00,0x1E,0x33,0x03,0x33,0x1E,0x00},{0x38,0x30,0x30,0x3e,0x33,0x33,0x6E,0x00},
    {0x00,0x00,0x1E,0x33,0x3f,0x03,0x1E,0x00},{0x1C,0x36,0x06,0x0f,0x06,0x06,0x0F,0x00},{0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x1F},
    {0x07,0x06,0x36,0x6E,0x66,0x66,0x67,0x00},{0x0C,0x00,0x0E,0x0C,0x0C,0x0C,0x1E,0x00},{0x30,0x00,0x30,0x30,0x30,0x33,0x33,0x1E},
    {0x07,0x06,0x66,0x36,0x1E,0x36,0x67,0x00},{0x0E,0x0C,0x0C,0x0C,0x0C,0x0C,0x1E,0x00},{0x00,0x00,0x33,0x7F,0x7F,0x6B,0x63,0x00},
    {0x00,0x00,0x1F,0x33,0x33,0x33,0x33,0x00},{0x00,0x00,0x1E,0x33,0x33,0x33,0x1E,0x00},{0x00,0x00,0x3B,0x66,0x66,0x3E,0x06,0x0F},
    {0x00,0x00,0x6E,0x33,0x33,0x3E,0x30,0x78},{0x00,0x00,0x3B,0x6E,0x66,0x06,0x0F,0x00},{0x00,0x00,0x3E,0x03,0x1E,0x30,0x1F,0x00},
    {0x08,0x0C,0x3E,0x0C,0x0C,0x2C,0x18,0x00},{0x00,0x00,0x33,0x33,0x33,0x33,0x6E,0x00},{0x00,0x00,0x33,0x33,0x33,0x1E,0x0C,0x00},
    {0x00,0x00,0x63,0x6B,0x7F,0x7F,0x36,0x00},{0x00,0x00,0x63,0x36,0x1C,0x36,0x63,0x00},{0x00,0x00,0x33,0x33,0x33,0x3E,0x30,0x1F},
    {0x00,0x00,0x3F,0x19,0x0C,0x26,0x3F,0x00},{0x38,0x0C,0x0C,0x07,0x0C,0x0C,0x38,0x00},{0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    {0x07,0x0C,0x0C,0x38,0x0C,0x0C,0x07,0x00},{0x6E,0x3B,0x00,0x00,0x00,0x00,0x00,0x00},{0,0,0,0,0,0,0,0}
  };
  auto rgb_to_index=[&](uint16_t R,uint16_t G,uint16_t B)->uint8_t{
    int r8=R>>8,g8=G>>8,b8=B>>8,best=0; long bd=1L<<40;
    for(int i=0;i<256;i++){ uint32_t e=H.g_clut+8+i*8;
      int cr=mem->read_u16b(e+2)>>8,cg=mem->read_u16b(e+4)>>8,cb=mem->read_u16b(e+6)>>8;
      long d=(long)(cr-r8)*(cr-r8)+(long)(cg-g8)*(cg-g8)+(long)(cb-b8)*(cb-b8);
      if(d<bd){bd=d;best=i;} }
    return (uint8_t)best;
  };
  auto cur_pm=[&](uint32_t& base,int& rb,int& bt,int& bl,int& bb,int& br)->bool{
    uint32_t port=H.cur_port?H.cur_port:H.g_port; if(!port) return false;
    uint32_t pmh=mem->read_u32b(port+0x02); if(!pmh) return false;
    uint32_t pm=mem->read_u32b(pmh); if(!pm) return false;
    base=mem->read_u32b(pm+0); rb=mem->read_u16b(pm+4)&0x3FFF;
    bt=(int16_t)mem->read_u16b(pm+6); bl=(int16_t)mem->read_u16b(pm+8);
    bb=(int16_t)mem->read_u16b(pm+10); br=(int16_t)mem->read_u16b(pm+12);
    return base!=0 && rb>0;
  };
  auto qd_px=[&](int x,int y,uint8_t idx){
    uint32_t base;int rb,bt,bl,bb,br; if(!cur_pm(base,rb,bt,bl,bb,br))return;
    if(x<H.qd_clip_l||x>=H.qd_clip_r||y<H.qd_clip_t||y>=H.qd_clip_b)return;
    int px=x-bl,py=y-bt; if(px<0||py<0||px>=(br-bl)||py>=(bb-bt))return;
    mem->write_u8(base+(uint32_t)py*rb+px,idx);
  };
  auto qd_fillrect=[&](int t,int l,int b,int rr,uint8_t idx){
    // Clamp to the current pixmap bounds AND clip rect before iterating, so a
    // giant blank-rect (e.g. right=0x8300) can't spin billions of no-op pixels.
    uint32_t base;int rb,bt,bl,bb,br; if(!cur_pm(base,rb,bt,bl,bb,br))return;
    if(l<bl)l=bl; if(t<bt)t=bt; if(rr>br)rr=br; if(b>bb)b=bb;
    if(l<H.qd_clip_l)l=H.qd_clip_l; if(t<H.qd_clip_t)t=H.qd_clip_t;
    if(rr>H.qd_clip_r)rr=H.qd_clip_r; if(b>H.qd_clip_b)b=H.qd_clip_b;
    for(int y=t;y<b;y++)for(int x=l;x<rr;x++)qd_px(x,y,idx);
  };
  auto qd_line=[&](int x0,int y0,int x1,int y1,uint8_t idx){
    if(getenv("ADLINELOG")){
      uint32_t base=0;int rb=0,bt=0,bl=0,bb=0,br=0; bool ok=cur_pm(base,rb,bt,bl,bb,br);
      static uint32_t toFb=0,toGw=0,toOther=0; static uint32_t idxmask=0;
      if(ok){ if(base==H.g_fb)toFb++; else if(base>=0x50000&&base<0x60000)toGw++; else toOther++; }
      idxmask |= (1u<<(idx&31));
      static int n=0; if(++n<=6 || (n%2000)==0)
        fprintf(stderr,"[line%d] (%d,%d)->(%d,%d) idx=%d base=%08X toFb=%u toGw=%u toOther=%u idxseen=%08X\n",
          n,x0,y0,x1,y1,idx,base,toFb,toGw,toOther,idxmask); }
    int dx=abs(x1-x0),dy=-abs(y1-y0),sx=x0<x1?1:-1,sy=y0<y1?1:-1,err=dx+dy;
    // Honor pen_w/pen_hgt by stamping a small filled box at each step; only when >1, so
    // the default 1px pen (and every module that never calls PenSize) takes the cheap
    // single-pixel path.
    int pw=H.pen_w>1?H.pen_w:1, ph=H.pen_hgt>1?H.pen_hgt:1;
    for(int guard=0;guard<20000;guard++){
      if(pw==1&&ph==1) qd_px(x0,y0,idx);
      else for(int oy=0;oy<ph;oy++)for(int ox=0;ox<pw;ox++) qd_px(x0+ox,y0+oy,idx);
      if(x0==x1&&y0==y1)break; int e2=2*err;
      if(e2>=dy){err+=dy;x0+=sx;} if(e2<=dx){err+=dx;y0+=sy;} }
  };
  // Filled/framed ellipse inside a Rect (PaintOval/FrameOval/EraseOval). Midpoint
  // scan: for each row, x-extent from the ellipse equation. Art Critic paints its
  // brush strokes as ovals.
  auto qd_oval=[&](int t,int l,int b,int rr,uint8_t idx,bool frame){
    int w=rr-l,h=b-t; if(w<=0||h<=0)return;
    double cx=(l+rr)/2.0, cy=(t+b)/2.0, ax=w/2.0, ay=h/2.0;
    if(ax<=0||ay<=0)return;
    for(int y=t;y<b;y++){
      double ny=(y+0.5-cy)/ay; if(ny*ny>1.0)continue;
      double hx=ax*sqrt(1.0-ny*ny); int xl=(int)(cx-hx), xr=(int)(cx+hx);
      if(frame){ qd_px(xl,y,idx); qd_px(xr,y,idx); }
      else for(int x=xl;x<=xr;x++) qd_px(x,y,idx);
    }
    if(frame){ // close top/bottom arcs horizontally
      for(int x=l;x<rr;x++){ double nx=(x+0.5-cx)/ax; if(nx*nx>1.0)continue;
        double hy=ay*sqrt(1.0-nx*nx); qd_px(x,(int)(cy-hy),idx); qd_px(x,(int)(cy+hy),idx); } }
  };
  auto qd_char=[&](int ch,int x,int y,uint8_t idx){
    if(ch<0x20||ch>0x7F)return; const uint8_t* g=QDFONT[ch-0x20];
    for(int row=0;row<8;row++){ uint8_t bits=g[row];
      for(int col=0;col<8;col++) if(bits&(1<<col)) qd_px(x+col,y-7+row,idx); }
  };
  auto qd_string=[&](uint32_t sp,bool pascal){  // draw at pen, advance pen 6px/char
    if(!sp)return; int n; uint32_t p;
    if(pascal){ n=mem->read_u8(sp); p=sp+1; } else { n=0; p=sp; while(mem->read_u8(p+n))n++; }
    for(int k=0;k<n;k++){ int c=mem->read_u8(p+k); qd_char(c,H.pen_h,H.pen_v,H.qd_fg); H.pen_h+=6; }
  };

  // ---- 5. Memory Manager + trace hook -------------------------------------
  PPC32Emulator emu(mem);
  // Per-item opt-out levers: each reverts ONE implemented Toolbox item to a plain stub,
  // for isolated A/B. Cached once because some sit in hot paths (HGetState runs ~1.8M
  // times). ADNOCUP is still read inline at its lower-frequency site.
  static const bool NOHSTATE   = getenv("ADNOHSTATE");   // HGetState/HSetState/H* flags
  static const bool NORESERR   = getenv("ADNORESERR");   // ResError last-error
  static const bool NOGESTERR  = getenv("ADNOGESTERR");  // Gestalt undef-selector err
  static const bool NORGN      = getenv("ADNORGN");      // region bbox model
  static const bool NOUSERESFIX= getenv("ADNOUSERESFIX");// UseResFile remap
  static const bool NOFCB      = getenv("ADNOFCB");      // PBGetFCBInfoSync real block
  static const bool NODEVATTR  = getenv("ADNODEVATTR");  // TestDeviceAttribute real bits
  static const bool NOINDPAT   = getenv("ADNOINDPAT");   // GetIndPattern real PAT#
  // Flying Toasters' one-shot "Flying Toasters!:  <fun fact>" title is NOT FT's own drawing —
  // it is the shared library's (adxpl510) init-time USER-MESSAGE banner. During the module's
  // DoInitialize (message 0) the library routine at BASE_A+0xEB34 loads FT's fun-fact resource
  // (GetResource 0x55F0), builds "<sceneName>:  <fact>", MoveTo(pen+15,+25), and DrawStrings it
  // to the SCREEN port (caller lr == BASE_A+0xEC3C). Then — STILL inside message 0 — FT's sprite
  // engine captures the screen into its background page (CopyBits screen->003D8000, lib fn
  // BASE_A+0xCCAC), baking the banner into the sprite bg. No screen erase runs between the two,
  // so no post-init "engine blank" can remove it: the snapshot is INTRA-message-0. On a real Mac
  // these DoUserMessage banners are ENGINE-COMPOSITED TIMED OVERLAYS (drawn over the animation
  // each frame, cleared on timeout, never in a module's sprite background); that overlay/timeout
  // logic lives in the un-extracted faceplate engine app, so without an overlay layer the banner
  // leaks into the captured background. The gate is therefore mechanism-grounded rather than
  // name-based: suppress the DrawString iff the CALLER is the library's DoUserMessage banner
  // routine [BASE_A+0xEB34, BASE_A+0xEC80), which covers the same-class banner in every AD4 PPC
  // module (e.g. Fish World). Default ON; opt-out ADNOMSGBANNER.
  static const uint32_t MSGBANNER_LO = BASE_A + 0xEB34, MSGBANNER_HI = BASE_A + 0xEC80;
  static const bool MSGBANNERFIX = !getenv("ADNOMSGBANNER");
  // Psycho Deli's psychedelic CLUT cycle is computed by its OWN per-frame DoDrawFrame step,
  // which advances the module's float palette at objSub+0x58 and re-installs it via
  // SetEntries. A GM drive that calls only DoBlank each frame therefore yields a constant
  // palette and a static module. The GM loop instead reproduces the store the module's own
  // StyleDispatch first case performs (seed its per-style element-draw callback at
  // DoDrawFrame-TOC+0x64c from the seed TVector at +0xf0, normally installed by a setup
  // message this drive never sends) and drives DoDrawFrame each frame. Arming is keyed
  // STRUCTURALLY on that null-callback signal (GM module whose DoDrawFrame TOC+0x64c is NULL
  // with an in-module seed at +0xf0), not on the module name.
  auto do_import = [&](PPC32Emulator& e, uint32_t idx)->void{
    if(g_dump_now){ census_dump(); fflush(NULL); _exit(0); }   // census alarm fired
    auto& r = e.registers();
    const string& nm = (idx<H.import_names.size())? H.import_names[idx] : string("???");
    // ---- implement the handful the engine needs before it can do anything ---
    auto ret = [&](uint32_t v){ r.r[3].u=v; };
    bool handled=true;
    uint32_t redirect_pc=0, redirect_toc=0;   // for re-entrant CallUniversalProc dispatch
    if(nm=="InterfaceLib:NewPtr" || nm=="InterfaceLib:NewPtrSys" ||
       nm=="InterfaceLib:NewPtrClear" || nm=="InterfaceLib:NewPtrSysClear"){
      uint32_t sz=r.r[3].u; uint32_t p=mem->allocate(sz?sz:1); H.block_size[p]=sz;
      if(nm.find("Clear")!=string::npos) mem->memset(p,0,sz);
      ret(p);
    } else if(nm=="InterfaceLib:NewHandle" || nm=="InterfaceLib:NewHandleSys" ||
              nm=="InterfaceLib:NewHandleClear" || nm=="InterfaceLib:NewHandleSysClear" ||
              nm=="InterfaceLib:NewEmptyHandle"){
      uint32_t sz=r.r[3].u; uint32_t p = sz? mem->allocate(sz):0;
      uint32_t h=mem->allocate(4); mem->write_u32b(h,p);
      if(sz && nm.find("Clear")!=string::npos) mem->memset(p,0,sz);
      H.block_size[h]=sz; ret(h);
    } else if(nm=="InterfaceLib:HLock"||nm=="InterfaceLib:HUnlock"||nm=="InterfaceLib:HLockHi"||
              nm=="InterfaceLib:MoveHHi"||nm=="InterfaceLib:HNoPurge"||nm=="InterfaceLib:HPurge"||
              nm=="InterfaceLib:HGetState"||nm=="InterfaceLib:HSetState"){
      // Real handle state-byte model. r3 = Handle. bit7(0x80)=locked, bit6(0x40)=purgeable.
      if(NOHSTATE){ ADSTUB(string("STUB:")+nm); }   // opt-out: no-op
      else {
        uint32_t h=r.r[3].u;
        if(nm=="InterfaceLib:HLock"||nm=="InterfaceLib:HLockHi") H.h_flags[h]|=0x80;
        else if(nm=="InterfaceLib:HUnlock") H.h_flags[h]&=~0x80;
        else if(nm=="InterfaceLib:HPurge") H.h_flags[h]|=0x40;
        else if(nm=="InterfaceLib:HNoPurge") H.h_flags[h]&=~0x40;
        else if(nm=="InterfaceLib:HGetState") ret(H.h_flags.count(h)?H.h_flags[h]:0);
        else if(nm=="InterfaceLib:HSetState") H.h_flags[h]=(uint8_t)r.r[4].u;
        // MoveHHi: relocation hint only (our blocks never move) — no flag change.
      }
    } else if(nm=="InterfaceLib:DisposePtr"||nm=="InterfaceLib:DisposeHandle"){
      // leak-by-design: the bump arena has no free list.
      ADSTUB(string("STUB:")+nm);
    } else if(nm=="InterfaceLib:GetHandleSize"||nm=="InterfaceLib:GetPtrSize"){
      auto it=H.block_size.find(r.r[3].u); ret(it!=H.block_size.end()?it->second:0);
    } else if(nm=="InterfaceLib:SetHandleSize"){
      uint32_t h=r.r[3].u, ns=r.r[4].u, old=mem->read_u32b(h);
      uint32_t np = ns? mem->allocate(ns):0;
      if(old && np){ uint32_t osz=H.block_size.count(h)?H.block_size[h]:0; mem->memcpy(np,old,min(osz,ns)); }
      mem->write_u32b(h,np); H.block_size[h]=ns;
    } else if(nm=="InterfaceLib:BlockMove"||nm=="InterfaceLib:BlockMoveData"){
      // byte-wise so a copy that spans host allocation arenas doesn't trip the
      // "not entirely contained within one arena" guard
      uint32_t s=r.r[3].u,d=r.r[4].u,n=r.r[5].u;
      if(d>s) for(int64_t i=(int64_t)n-1;i>=0;i--) mem->write_u8(d+i, mem->read_u8(s+i));
      else    for(uint32_t i=0;i<n;i++) mem->write_u8(d+i, mem->read_u8(s+i));
    } else if(nm=="InterfaceLib:MemError"){
      ADSTUB(string("STUB:")+nm);   // constant noErr (all our allocations succeed)
      ret(0);
    } else if(nm=="InterfaceLib:ResError"){
      // Real last-error: a failed Get*Resource sets resNotFound(-192); a successful
      // resource call sets noErr. Modules distinguish "missing" from "OK" this way.
      if(NORESERR){ ADSTUB(string("STUB:")+nm); ret(0); }   // opt-out: constant noErr
      else ret((uint32_t)(int32_t)H.last_res_err);
    } else if(nm=="InterfaceLib:p2cstr"){
      uint32_t s=r.r[3].u; if(s){ uint8_t len=mem->read_u8(s);
        for(int i=0;i<len;i++) mem->write_u8(s+i, mem->read_u8(s+1+i)); mem->write_u8(s+len,0); }
      ret(s);
    } else if(nm=="InterfaceLib:c2pstr"){
      uint32_t s=r.r[3].u; if(s){ uint32_t len=0; while(mem->read_u8(s+len)) len++;
        for(int i=len-1;i>=0;i--) mem->write_u8(s+1+i, mem->read_u8(s+i)); mem->write_u8(s,(uint8_t)len); }
      ret(s);
    } else if(nm=="InterfaceLib:GetPort"){
      if(r.r[3].u) mem->write_u32b(r.r[3].u, H.g_port);   // GetPort(GrafPtr* -> port)
    } else if(nm=="InterfaceLib:GetGWorld"){
      // GetGWorld(CGrafPtr* port, GDHandle* gd)
      if(r.r[3].u) mem->write_u32b(r.r[3].u, H.g_port);
      if(r.r[4].u) mem->write_u32b(r.r[4].u, H.g_gdh);
    } else if(nm=="InterfaceLib:GetMainDevice"||nm=="InterfaceLib:GetGDevice"){
      ret(H.g_gdh);
    } else if(nm=="InterfaceLib:GetGWorldPixMap"){
      // GetGWorldPixMap(GWorldPtr) -> PixMapHandle (portPixMap field @+0x02)
      uint32_t gw=r.r[3].u;
      ret(gw ? mem->read_u32b(gw+0x02) : H.g_pmh);
    } else if(nm=="InterfaceLib:NewRoutineDescriptor"){
      // NewRoutineDescriptor(ProcPtr theProc, ProcInfo, ISA) -> UniversalProcPtr.
      // On native PPC a UPP is just the proc's TVector, so return r3 unchanged; otherwise
      // the module's draw-proc UPP is null and calling it faults at pc=0 (Psycho Deli).
      if(getenv("ADNRDLOG")){ static int n=0; if(n++<20)
        fprintf(stderr,"[nrd] proc=%08X procInfo=%08X lr=%08X\n",r.r[3].u,r.r[4].u,r.lr); }
      ret(r.r[3].u);
    } else if(nm=="InterfaceLib:DisposeRoutineDescriptor"){
      ADSTUB(string("STUB:")+nm);
      ret(0);
    } else if(nm=="InterfaceLib:CallUniversalProc" && !getenv("ADNOCUP")){
      // CallUniversalProc(UniversalProcPtr upp, ProcInfo, args...): actually DISPATCH the
      // proc (opt-out ADNOCUP). Modules make real UPP callbacks — GraphicsModules route
      // drawing through them — so a ret(0) stub swallows module code a real Mac runs.
      // On PPC a UPP is the proc's TVector. Redirect emulation INTO the proc: it returns
      // (blr) to r.lr = the module caller, so this trap effectively becomes the call.
      // Args shift by 2 (skip upp, procInfo).
      uint32_t upp=r.r[3].u;
      if(getenv("ADCBLOG")){ static int n=0; if(n++<24){
        uint32_t pi=r.r[4].u, a1=r.r[5].u, a2=r.r[6].u;
        fprintf(stderr,"[cup] upp=%08X procInfo=%08X a1=%08X a2=%08X lr=%08X",upp,pi,a1,a2,r.lr);
        if(a1>0x1000&&a1<0x60000000){ fprintf(stderr,"  [a1]=%08X %08X %08X %08X",
          mem->read_u32b(a1),mem->read_u32b(a1+4),mem->read_u32b(a1+8),mem->read_u32b(a1+12)); }
        fprintf(stderr,"\n"); } }
      if(upp){ uint32_t code=mem->read_u32b(upp), toc=mem->read_u32b(upp+4);
        uint32_t a3=r.r[5].u,a4=r.r[6].u,a5=r.r[7].u,a6=r.r[8].u,a7=r.r[9].u,a8=r.r[10].u;
        r.r[3].u=a3;r.r[4].u=a4;r.r[5].u=a5;r.r[6].u=a6;r.r[7].u=a7;r.r[8].u=a8;
        redirect_pc=code; redirect_toc=toc; }   // enter proc; returns to r.lr (module)
      else ret(0);
    } else if(nm=="InterfaceLib:PBGetFCBInfoSync"){
      // PBGetFCBInfo(FCBPBPtr pb, Boolean async): describe an open file control block.
      // A module enumerating open files reads these fields, so fill them for real.
      // Our only "open files" are the loaded resource forks (res_files); OpenResFile maps
      // file index i -> refNum i+2. FCBPBRec offsets (Inside Macintosh: Files):
      //   ioResult@16 ioNamePtr@18 ioVRefNum@22 ioRefNum@24 ioFCBIndx@28
      //   ioFCBFlNm@32 ioFCBFlags@36 ioFCBEOF@40 ioFCBPLen@44 ioFCBCrPs@48 ioFCBVRefNum@52
      if(NOFCB){ ADSTUB(string("STUB:")+nm); ret(0); }   // opt-out: noErr + untouched block
      else {
      // paramErr is returned ONLY to terminate a genuine index-walk past the last open
      // file (indx>N). For a specific refNum query, or an indeterminate/bogus param block,
      // answer noErr (the superset-residency model the resource no-ops also use): an error
      // here steers the AD engine into a direct-FSRead path this host cannot back, which
      // derails Rodger Dodger.
      uint32_t pb=r.r[3].u;
      int32_t err=0; int fi=-1;
      if(pb){
        int16_t refnum=(int16_t)mem->read_u16b(pb+24);
        int16_t indx  =(int16_t)mem->read_u16b(pb+28);
        int N=(int)H.res_files.size();
        if(indx>0){                            // by index (1-based enumeration)
          if(indx<=N) fi=indx-1; else err=-50; // past the last open fork -> paramErr (stop)
        } else if(refnum>=2 && (refnum-2)<N){  // by a refNum we own
          fi=refnum-2;
        }
        // else: indeterminate (refnum 0 / bogus pb) -> noErr, describe nothing.
      }
      if(fi>=0){                               // fill the real FCB fields for this open fork
        mem->write_u16b(pb+24,(uint16_t)(int16_t)(fi+2));  // ioRefNum
        mem->write_u16b(pb+22,(uint16_t)(int16_t)-1);      // ioVRefNum = boot volume
        mem->write_u16b(pb+52,(uint16_t)(int16_t)-1);      // ioFCBVRefNum
        mem->write_u32b(pb+32,(uint32_t)(fi+1));           // ioFCBFlNm (synthetic file number)
        mem->write_u16b(pb+36,0);                          // ioFCBFlags (resource fork, not dirty)
        mem->write_u32b(pb+40,0);                          // ioFCBEOF  (fork size not tracked)
        mem->write_u32b(pb+44,0);                          // ioFCBPLen
        mem->write_u32b(pb+48,0);                          // ioFCBCrPs (mark at 0)
        uint32_t np=mem->read_u32b(pb+18);                 // ioNamePtr: empty Pascal name if provided
        if(np) mem->write_u8(np,0);
      }
      if(pb) mem->write_u16b(pb+16,(uint16_t)(int16_t)err);  // ioResult (File Mgr always writes it)
      ret((uint32_t)err);
      }   // end !NOFCB
    } else if(nm=="InterfaceLib:SetPort"||nm=="InterfaceLib:SetGWorld"||
              nm=="InterfaceLib:SetGDevice"||
              nm=="InterfaceLib:CallUniversalProc"){
      // Track the current drawing port so CopyBits/FillRect target it.
      if(nm=="InterfaceLib:SetGWorld"||nm=="InterfaceLib:SetPort") H.cur_port=r.r[3].u;
      else ADSTUB(string("STUB:")+nm);  // SetGDevice discard / CallUniversalProc under ADNOCUP
      ret(0);  // benign
    } else if(nm.rfind("MathLib:",0)==0){
      // PowerPC FP ABI: double args in f1,f2; double result in f1. Modules compute
      // trig-based motion (Magic Turtle's spiral, orbit paths) through MathLib, so the
      // common transcendental/rounding fns come straight from libm.
      const string fn = nm.substr(8);
      double a = r.f[1].f, b = r.f[2].f, y = 0;
      if(fn=="sin") y=std::sin(a); else if(fn=="cos") y=std::cos(a);
      else if(fn=="tan") y=std::tan(a); else if(fn=="asin") y=std::asin(a);
      else if(fn=="acos") y=std::acos(a); else if(fn=="atan") y=std::atan(a);
      else if(fn=="atan2") y=std::atan2(a,b); else if(fn=="sqrt") y=std::sqrt(a);
      else if(fn=="exp") y=std::exp(a); else if(fn=="log") y=std::log(a);
      else if(fn=="log10") y=std::log10(a); else if(fn=="pow") y=std::pow(a,b);
      else if(fn=="fabs") y=std::fabs(a); else if(fn=="floor") y=std::floor(a);
      else if(fn=="ceil") y=std::ceil(a); else if(fn=="fmod") y=std::fmod(a,b);
      else if(fn=="hypot") y=std::hypot(a,b); else if(fn=="sinh") y=std::sinh(a);
      else if(fn=="cosh") y=std::cosh(a); else if(fn=="tanh") y=std::tanh(a);
      else if(fn=="rint"||fn=="nearbyint") y=std::rint(a);
      else handled=false; // unknown MathLib fn -> log as UNIMPLEMENTED
      if(handled) r.f[1].f = y;
    } else if(nm=="InterfaceLib:SetEntries"){
      // SetEntries(short start, short count, CSpecArray table): install `count+1`
      // ColorSpec {value:u16, r:u16,g:u16,b:u16} entries into the CLUT. When start<0,
      // each entry's own `value` is its palette index; else entries go start..start+count.
      // Modules like Super Guy install their real palette this way (we render 8-bit fb
      // through g_clut), and palette-cycling effects re-call it each frame. Honor the
      // module's index 0 here (full-screen primitive modules use it as a real color),
      // unlike the sprite-clut path that forces 0=black backdrop.
      int16_t start=(int16_t)r.r[3].u, count=(int16_t)r.r[4].u; uint32_t tab=r.r[5].u;
      if(getenv("ADPDLOG")) fprintf(stderr,"    SetEntries start=%d count=%d tab=%08X first={%04X,%04X,%04X} last={%04X,%04X,%04X}\n",
        start,count,tab, tab?mem->read_u16b(tab+2):0,tab?mem->read_u16b(tab+4):0,tab?mem->read_u16b(tab+6):0,
        tab&&count>=0?mem->read_u16b(tab+(uint32_t)count*8+2):0,tab&&count>=0?mem->read_u16b(tab+(uint32_t)count*8+4):0,tab&&count>=0?mem->read_u16b(tab+(uint32_t)count*8+6):0);
      if(tab && H.g_clut){
        for(int k=0;k<=count;k++){ uint32_t cs=tab+(uint32_t)k*8;
          int idx = (start<0) ? (int)mem->read_u16b(cs) : (start+k);
          if(idx<0||idx>255) continue;
          uint32_t e=H.g_clut+8+(uint32_t)idx*8;
          mem->write_u16b(e+2, mem->read_u16b(cs+2));
          mem->write_u16b(e+4, mem->read_u16b(cs+4));
          mem->write_u16b(e+6, mem->read_u16b(cs+6)); }
      }
    } else if(nm=="InterfaceLib:GetKeys"){
      // GetKeys(KeyMap theKeys) -> fill the caller's 16-byte keymap. Modules poll "is any
      // key down?" (Points of View gates its animation on it), so the map must be real.
      ADSTUB(string("STUB:")+nm);  // no real hardware keyboard behind it
      if(getenv("ADINPUTLOG")){ static long n=0; if(++n<=60) fprintf(stderr,"    [input] GetKeys km=%08X lr=%08X\n", r.r[3].u, r.lr); }
      // Copy the live KeyMap; g_km stays all-zero until the app sends input.
      uint32_t km=r.r[3].u; if(km){ for(int i=0;i<16;i++) mem->write_u8(km+i,g_km[i]); }
    } else if(nm=="InterfaceLib:HideCursor"||nm=="InterfaceLib:ShowCursor"||
              nm=="InterfaceLib:InitCursor"||nm=="InterfaceLib:ObscureCursor"||
              nm=="InterfaceLib:ShieldCursor"||nm=="InterfaceLib:SetCursor"){
      // no-op: no real cursor in the offscreen render.
      ADSTUB(string("STUB:")+nm);
    } else if(nm=="InterfaceLib:Button"||nm=="InterfaceLib:StillDown"||nm=="InterfaceLib:WaitMouseUp"){
      ADSTUB(string("STUB:")+nm);  // constant 0: mouse button never down
      if(getenv("ADINPUTLOG")){ static long n=0; if(++n<=60) fprintf(stderr,"    [input] %s lr=%08X\n", nm.c_str(), r.lr); }
      ret(g_msbtn?1:0); // live mouse button (default up = 0)
    } else if(nm=="InterfaceLib:GetMouse"){
      // GetMouse(Point* mouseLoc) — fill the caller's Point (v,h)=(y,x) with the live
      // mouse position; default (0,0). ADShared40 imports GetMouse.
      ADSTUB(string("STUB:")+nm);
      if(getenv("ADINPUTLOG")){ static long n=0; if(++n<=60) fprintf(stderr,"    [input] GetMouse pt=%08X lr=%08X\n", r.r[3].u, r.lr); }
      uint32_t pt=r.r[3].u; if(pt){ mem->write_u16b(pt,(uint16_t)g_msy); mem->write_u16b(pt+2,(uint16_t)g_msx); }
    } else if(nm=="InterfaceLib:Color2Index"){
      // Color2Index(const RGBColor* -> long index): nearest palette index for an RGB,
      // mapped via the CLUT. Modules build per-element colors this way (Swirling Magic's
      // pixies), so a constant return makes their drawing invisible.
      uint32_t cp=r.r[3].u;
      ret(cp ? rgb_to_index(mem->read_u16b(cp),mem->read_u16b(cp+2),mem->read_u16b(cp+4)) : 0);
    } else if(nm=="InterfaceLib:Index2Color"){
      // Index2Color(long index, RGBColor* -> fill from CLUT).
      uint32_t idx=r.r[3].u & 0xFF, cp=r.r[4].u;
      if(cp && H.g_clut){ uint32_t e=H.g_clut+8+idx*8;
        mem->write_u16b(cp,mem->read_u16b(e+2)); mem->write_u16b(cp+2,mem->read_u16b(e+4)); mem->write_u16b(cp+4,mem->read_u16b(e+6)); }
    } else if(nm=="InterfaceLib:RGBForeColor"||nm=="InterfaceLib:RGBBackColor"){
      uint32_t cp=r.r[3].u; if(cp){ uint16_t R=mem->read_u16b(cp),G=mem->read_u16b(cp+2),B=mem->read_u16b(cp+4);
        uint8_t ix=rgb_to_index(R,G,B);
        // Palette-index-in-red-channel (CLUT-animation) modules: Psycho Deli draws its
        // psychedelic chevrons with RGBForeColor((index,0,constBlue)) — the index is the
        // low byte of red, resolved on a real Mac through the module's inverse color table
        // (built from its SetEntries palette). Nearest-RGB match collapses the near-
        // identical dark ramp to one index (invisible), so we honor red-as-index instead.
        //
        // Detect the convention STRUCTURALLY from the color itself, not from a module name
        // or the ADGM flag: green == 0 AND red is a NONZERO value that fits in the low byte
        // (R in [1,255], i.e. an index packed in red with the high byte clear). The 17
        // true-RGB call_main modules never emit that — their green==0 colors are pure
        // blues/blacks with red == 0 ({0000,0000,FFFF} blue, {0000,0000,0000} black), never
        // a nonzero small red — while PD emits G==0 & R∈[1,255] on every draw and never
        // R==0. The R<0x100 bound leaves a real large red (high byte set) as a true colour.
        // ADNOPDINDEX forces off; ADPDINDEX forces on for any path.
        bool index_in_red = (G==0 && R!=0 && R<0x100);
        if(getenv("ADPDLOG")){ static int n=0; if(n++<16) fprintf(stderr,"    RGBForeColor R=%04X G=%04X B=%04X -> ix=%d\n",R,G,B,(int)(R&0xFF)); }
        if((getenv("ADPDINDEX") || index_in_red) && !getenv("ADNOPDINDEX")) ix=(uint8_t)(R & 0xFF);
        if(nm=="InterfaceLib:RGBForeColor")H.qd_fg=ix; else H.qd_bg=ix; }
    } else if(nm=="InterfaceLib:ForeColor"||nm=="InterfaceLib:BackColor"){
      // classic 8-color constants -> RGB -> nearest index
      uint32_t c=r.r[3].u; uint16_t R=0,G=0,B=0;
      switch(c){ case 30:R=G=B=0xFFFF;break; /*white*/ case 33:R=G=B=0;break; /*black*/
        case 205:R=0xDD00;break; /*red*/ case 341:G=0xAA00;break; /*green*/ case 409:B=0xEE00;break; /*blue*/
        case 273:G=B=0xFFFF;break; /*cyan*/ case 137:R=B=0xFFFF;break; /*magenta*/ case 69:R=G=0xFFFF;break; /*yellow*/
        default:R=G=B=0;break; }
      uint8_t ix=rgb_to_index(R,G,B); if(nm=="InterfaceLib:ForeColor")H.qd_fg=ix; else H.qd_bg=ix;
    } else if(nm=="InterfaceLib:PenSize"){
      // PenSize(short width, short height): set the pen dimensions used by Line/Frame.
      H.pen_w=(int16_t)r.r[3].u; H.pen_hgt=(int16_t)r.r[4].u;
      if(H.pen_w<1)H.pen_w=1; if(H.pen_hgt<1)H.pen_hgt=1;
    } else if(nm=="InterfaceLib:PenMode"){ H.pen_mode=(int16_t)r.r[3].u; // tracked; no XOR path
    } else if(nm=="InterfaceLib:PenNormal"){ H.pen_w=1; H.pen_hgt=1; H.pen_mode=8;
    } else if(nm=="InterfaceLib:TextFont"){ H.txt_font=(int16_t)r.r[3].u; // tracked (fixed bitmap font)
    } else if(nm=="InterfaceLib:TextFace"){ H.txt_face=(int16_t)r.r[3].u; // tracked
    } else if(nm=="InterfaceLib:TextSize"){ H.txt_size=(int16_t)r.r[3].u; // tracked
    } else if(nm=="InterfaceLib:TextMode"){ // tracked no-op (transfer mode for text)
    } else if(nm=="InterfaceLib:StringWidth"){
      // StringWidth(ConstStr255 s) -> pixel width. Our bitmap font advances 6px/char
      // (see qd_string), so report 6*len; text-layout code (Bad Dog) centers strings
      // with it.
      uint32_t s=r.r[3].u; int n=s?mem->read_u8(s):0; ret((uint32_t)(6*n));
    } else if(nm=="InterfaceLib:TextWidth"){
      // TextWidth(ptr, offset, len) -> width of len chars at 6px each.
      ret((uint32_t)(6*(int16_t)r.r[5].u));
    } else if(nm=="InterfaceLib:CharWidth"){ ret(6);
    } else if(nm=="InterfaceLib:GetFontInfo"){
      // GetFontInfo(FontInfo* -> ascent,descent,widMax,leading). Match our 8px cell.
      uint32_t p=r.r[3].u; if(p){ mem->write_u16b(p+0,8); mem->write_u16b(p+2,2);
        mem->write_u16b(p+4,6); mem->write_u16b(p+6,0); }
    } else if(nm=="InterfaceLib:SetOrigin"){
      // SetOrigin(short h, short v): move the port's coordinate origin. Modules call
      // this constantly to re-home local coords; our software QD treats coords as
      // pixmap-local so the default is identity. Track it and (opt-in ADSETORIGIN)
      // apply as a pixel offset, and reflect it in the current port's portRect so a
      // module that reads the origin back sees the value it set.
      H.qd_org_h=(int16_t)r.r[3].u; H.qd_org_v=(int16_t)r.r[4].u;
      uint32_t port=H.cur_port?H.cur_port:H.g_port;
      if(port){ mem->write_u16b(port+0x10,(uint16_t)H.qd_org_v); mem->write_u16b(port+0x12,(uint16_t)H.qd_org_h); }
    } else if(nm=="InterfaceLib:PaintOval"||nm=="InterfaceLib:FillOval"||
              nm=="InterfaceLib:EraseOval"||nm=="InterfaceLib:FrameOval"||
              nm=="InterfaceLib:InvertOval"){
      uint32_t rp=r.r[3].u; if(rp){ int t=(int16_t)mem->read_u16b(rp),l=(int16_t)mem->read_u16b(rp+2),
        b=(int16_t)mem->read_u16b(rp+4),rr=(int16_t)mem->read_u16b(rp+6);
        uint8_t ix=(nm=="InterfaceLib:EraseOval")?H.qd_bg:H.qd_fg;
        qd_oval(t,l,b,rr,ix,nm=="InterfaceLib:FrameOval"); }
    } else if(nm=="InterfaceLib:MoveTo"){ H.pen_h=(int16_t)r.r[3].u; H.pen_v=(int16_t)r.r[4].u;
    } else if(nm=="InterfaceLib:Move"){ H.pen_h+=(int16_t)r.r[3].u; H.pen_v+=(int16_t)r.r[4].u;
    } else if(nm=="InterfaceLib:LineTo"){ int th=(int16_t)r.r[3].u,tv=(int16_t)r.r[4].u;
      qd_line(H.pen_h,H.pen_v,th,tv,H.qd_fg); H.pen_h=th; H.pen_v=tv;
    } else if(nm=="InterfaceLib:Line"){ int th=H.pen_h+(int16_t)r.r[3].u,tv=H.pen_v+(int16_t)r.r[4].u;
      qd_line(H.pen_h,H.pen_v,th,tv,H.qd_fg); H.pen_h=th; H.pen_v=tv;
    } else if(nm=="InterfaceLib:PaintRect"||nm=="InterfaceLib:FillRect"||nm=="InterfaceLib:EraseRect"||
              nm=="InterfaceLib:InvertRect"||nm=="InterfaceLib:FrameRect"){
      uint32_t rp=r.r[3].u; if(rp){ int t=(int16_t)mem->read_u16b(rp),l=(int16_t)mem->read_u16b(rp+2),
        b=(int16_t)mem->read_u16b(rp+4),rr=(int16_t)mem->read_u16b(rp+6);
        uint8_t ix=(nm=="InterfaceLib:EraseRect")?H.qd_bg:H.qd_fg;
        // BACKDROP BLACK: the shared AD 4.0 engine (MacCanvas) erases its full-screen OFFSCREEN work
        // buffer with its default background colour (a light engine global: white for Guernsey Madness,
        // grey-238 for Rainforest) and then blits that buffer to the (correctly black) screen, so the
        // light backdrop shows through as a big white/grey box behind the sprites. After Dark's backdrop
        // is BLACK; the light value is only the engine's shipped default bg. So a LARGE-area EraseRect
        // (>=50% of the screen, top-left anchored) whose bg is (near-)white/light paints black instead,
        // keeping the composited backdrop black while sprites keep their colours. Small sprite-cell
        // buffers and non-light erases are untouched. Opt-out ADNOBLACKDROP=1.
        if(nm=="InterfaceLib:EraseRect" && !getenv("ADNOBLACKDROP")){
          uint32_t e0=H.g_clut+8+(uint32_t)ix*8;
          bool light = mem->read_u16b(e0+2)>=0xE000 && mem->read_u16b(e0+4)>=0xE000 && mem->read_u16b(e0+6)>=0xE000;
          int cw=min(rr,H.fb_w)-max(l,0), ch=min(b,H.fb_h)-max(t,0);
          bool big = l<=1 && t<=1 && cw>0 && ch>0 && (long)cw*ch >= (long)H.fb_w*H.fb_h/2;
          if(light && big) ix = rgb_to_index(0,0,0);
        }
        if(nm=="InterfaceLib:FrameRect"){ qd_line(l,t,rr-1,t,ix);qd_line(l,b-1,rr-1,b-1,ix);
          qd_line(l,t,l,b-1,ix);qd_line(rr-1,t,rr-1,b-1,ix); }
        else qd_fillrect(t,l,b,rr,ix); }
    } else if(nm=="InterfaceLib:FillRgn"||nm=="InterfaceLib:PaintRgn"||nm=="InterfaceLib:EraseRgn"||
              nm=="InterfaceLib:InvertRgn"||nm=="InterfaceLib:FrameRgn"){
      // Region ops approximated by the region's BOUNDING BOX (RgnHandle -> [rgn], then
      // rgnBBox Rect at rgn+2). Psycho Deli paints its pattern via FillRgn — without
      // this the whole module draws nothing. (Non-rect regions over-fill; acceptable.)
      uint32_t rh=r.r[3].u; uint32_t rgn=rh?mem->read_u32b(rh):0;
      if(rgn){ int t=(int16_t)mem->read_u16b(rgn+2),l=(int16_t)mem->read_u16b(rgn+4),
        b=(int16_t)mem->read_u16b(rgn+6),rr=(int16_t)mem->read_u16b(rgn+8);
        if(getenv("ADGMGWLOG")){ static int n=0; if(n++<6){ uint32_t base;int rb,bt,bl,bb,br;bool ok=cur_pm(base,rb,bt,bl,bb,br);
          fprintf(stderr,"    [%s] bbox=%d,%d,%d,%d fg=%d clip=%d,%d,%d,%d curport=%08X pm_ok=%d base=%08X bounds=%d,%d,%d,%d\n",
            nm.c_str()+12,t,l,b,rr,H.qd_fg,H.qd_clip_t,H.qd_clip_l,H.qd_clip_b,H.qd_clip_r,H.cur_port,ok,ok?base:0,bt,bl,bb,br); } }
        uint8_t ix=(nm=="InterfaceLib:EraseRgn")?H.qd_bg:H.qd_fg;
        if(nm=="InterfaceLib:FrameRgn"){ qd_line(l,t,rr-1,t,ix);qd_line(l,b-1,rr-1,b-1,ix);
          qd_line(l,t,l,b-1,ix);qd_line(rr-1,t,rr-1,b-1,ix); }
        else qd_fillrect(t,l,b,rr,ix); }
    } else if(nm=="InterfaceLib:ClipRect"){
      uint32_t rp=r.r[3].u; if(rp){ H.qd_clip_t=(int16_t)mem->read_u16b(rp);H.qd_clip_l=(int16_t)mem->read_u16b(rp+2);
        H.qd_clip_b=(int16_t)mem->read_u16b(rp+4);H.qd_clip_r=(int16_t)mem->read_u16b(rp+6); }
    } else if(nm=="InterfaceLib:GetClip"){
      // GetClip(RgnHandle): store the current clip rect into the region's bbox so a
      // module can save/restore it (paired with SetClip). An all-zero region restored
      // later installs an empty clip and drawing vanishes.
      uint32_t rh=r.r[3].u; uint32_t rgn=rh?mem->read_u32b(rh):0;
      if(rgn){ mem->write_u16b(rgn,10); mem->write_u16b(rgn+2,(uint16_t)H.qd_clip_t);
        mem->write_u16b(rgn+4,(uint16_t)H.qd_clip_l); mem->write_u16b(rgn+6,(uint16_t)H.qd_clip_b);
        mem->write_u16b(rgn+8,(uint16_t)H.qd_clip_r); }
    } else if(nm=="InterfaceLib:SetClip"){
      // SetClip(RgnHandle): adopt the region's bbox as the clip rect. A full-screen or
      // wide bbox (or an empty {0,0,0,0} from a freshly-made rgn) is treated as "no clip"
      // so we never accidentally clip everything to nothing.
      uint32_t rh=r.r[3].u; uint32_t rgn=rh?mem->read_u32b(rh):0;
      if(rgn){ int t=(int16_t)mem->read_u16b(rgn+2),l=(int16_t)mem->read_u16b(rgn+4),
        b=(int16_t)mem->read_u16b(rgn+6),rr=(int16_t)mem->read_u16b(rgn+8);
        if(b>t&&rr>l){ H.qd_clip_t=t;H.qd_clip_l=l;H.qd_clip_b=b;H.qd_clip_r=rr; }
        else { H.qd_clip_t=0;H.qd_clip_l=0;H.qd_clip_b=32767;H.qd_clip_r=32767; } }
    } else if(nm=="InterfaceLib:DrawString"){
      if(getenv("ADTXTLOG")){ uint32_t sp=r.r[3].u; int n=sp?mem->read_u8(sp):0; std::string s;
        for(int k=0;k<n&&k<80;k++){ int c=mem->read_u8(sp+1+k); s+=(c>=0x20&&c<0x7F)?(char)c:'.'; }
        fprintf(stderr,"[txt] DrawString pen=(%d,%d) fg=%d port=%08X lr=%08X \"%s\"\n",H.pen_h,H.pen_v,H.qd_fg,H.cur_port,r.lr,s.c_str()); }
      // Drop the shared library's init-time USER-MESSAGE banner (see MSGBANNERFIX): the caller
      // (r.lr) inside the library's DoUserMessage banner routine is the mechanism-grounded
      // discriminator. That draw is what leaks FT's "<name>: <fact>" title into the sprite
      // background; on real hardware these banners are engine-composited transient overlays,
      // never sprite-bg content.
      bool ft_title=false;
      if(MSGBANNERFIX && r.lr>=MSGBANNER_LO && r.lr<MSGBANNER_HI) ft_title=true;
      // Rather than merely dropping it, CAPTURE it (text/pen/fg) so the present-time overlay can
      // show it briefly then clear — the authentic timed overlay. Suppressing the draw here keeps
      // it out of g_fb and the sprite background; the overlay is a screen composite only.
      if(ft_title && !getenv("ADNOBANNEROVL")){ uint32_t sp=r.r[3].u; if(sp){ int ln=mem->read_u8(sp);
        std::string s; for(int k=0;k<ln&&k<255;k++){ int c=0; try{c=mem->read_u8(sp+1+k);}catch(...){} s+=(char)c; }
        g_banner_text=s; g_banner_x=H.pen_h; g_banner_y=H.pen_v; g_banner_fg=H.qd_fg; g_banner_have=true;
        if(getenv("ADBANNERLOG")) fprintf(stderr,"[banner] captured lr=%08X x=%d y=%d fg=%d \"%s\"\n",r.lr,g_banner_x,g_banner_y,g_banner_fg,s.c_str()); } }
      if(!ft_title) qd_string(r.r[3].u,true);
    } else if(nm=="InterfaceLib:DrawText"){ // DrawText(ptr, offset, len)
      uint32_t p=r.r[3].u+(int32_t)r.r[4].u; int n=(int16_t)r.r[5].u;
      if(getenv("ADTXTLOG")){ std::string s; for(int k=0;k<n&&k<80;k++){ int c=mem->read_u8(p+k); s+=(c>=0x20&&c<0x7F)?(char)c:'.'; }
        fprintf(stderr,"[txt] DrawText pen=(%d,%d) fg=%d port=%08X \"%s\"\n",H.pen_h,H.pen_v,H.qd_fg,H.cur_port,s.c_str()); }
      for(int k=0;k<n;k++){ qd_char(mem->read_u8(p+k),H.pen_h,H.pen_v,H.qd_fg); H.pen_h+=6; }
    } else if(nm=="InterfaceLib:DrawChar"){ qd_char(r.r[3].u&0xFF,H.pen_h,H.pen_v,H.qd_fg); H.pen_h+=6;
    } else if(nm=="InterfaceLib:SetRect"){
      // SetRect(Rect* r, short left, short top, short right, short bottom).
      // Rect fields: top@0, left@2, bottom@4, right@6.
      uint32_t rp=r.r[3].u;
      if(rp){ mem->write_u16b(rp+0,(uint16_t)r.r[5].u); mem->write_u16b(rp+2,(uint16_t)r.r[4].u);
              mem->write_u16b(rp+4,(uint16_t)r.r[7].u); mem->write_u16b(rp+6,(uint16_t)r.r[6].u); }
    } else if(nm=="InterfaceLib:OffsetRect"){
      uint32_t rp=r.r[3].u; int16_t dh=(int16_t)r.r[4].u, dv=(int16_t)r.r[5].u;
      if(rp){ mem->write_u16b(rp+0,(uint16_t)((int16_t)mem->read_u16b(rp+0)+dv));
              mem->write_u16b(rp+2,(uint16_t)((int16_t)mem->read_u16b(rp+2)+dh));
              mem->write_u16b(rp+4,(uint16_t)((int16_t)mem->read_u16b(rp+4)+dv));
              mem->write_u16b(rp+6,(uint16_t)((int16_t)mem->read_u16b(rp+6)+dh)); }
    } else if(nm=="InterfaceLib:InsetRect"){
      uint32_t rp=r.r[3].u; int16_t dh=(int16_t)r.r[4].u, dv=(int16_t)r.r[5].u;
      if(rp){ mem->write_u16b(rp+0,(uint16_t)((int16_t)mem->read_u16b(rp+0)+dv));
              mem->write_u16b(rp+2,(uint16_t)((int16_t)mem->read_u16b(rp+2)+dh));
              mem->write_u16b(rp+4,(uint16_t)((int16_t)mem->read_u16b(rp+4)-dv));
              mem->write_u16b(rp+6,(uint16_t)((int16_t)mem->read_u16b(rp+6)-dh)); }
    } else if(nm=="InterfaceLib:SectRect"){
      // SectRect(Rect* a, Rect* b, Rect* dst) -> Boolean (non-empty intersection)
      uint32_t a=r.r[3].u,b=r.r[4].u,d=r.r[5].u;
      int at=(int16_t)mem->read_u16b(a),al=(int16_t)mem->read_u16b(a+2),ab=(int16_t)mem->read_u16b(a+4),ar=(int16_t)mem->read_u16b(a+6);
      int bt=(int16_t)mem->read_u16b(b),bl=(int16_t)mem->read_u16b(b+2),bb=(int16_t)mem->read_u16b(b+4),br=(int16_t)mem->read_u16b(b+6);
      int t=max(at,bt),l=max(al,bl),bo=min(ab,bb),rr=min(ar,br);
      bool ne=(l<rr&&t<bo);
      if(d){ if(ne){mem->write_u16b(d,t);mem->write_u16b(d+2,l);mem->write_u16b(d+4,bo);mem->write_u16b(d+6,rr);}
             else {mem->write_u16b(d,0);mem->write_u16b(d+2,0);mem->write_u16b(d+4,0);mem->write_u16b(d+6,0);} }
      ret(ne?1:0);
    } else if(nm=="InterfaceLib:UnionRect"){
      uint32_t a=r.r[3].u,b=r.r[4].u,d=r.r[5].u;
      int at=(int16_t)mem->read_u16b(a),al=(int16_t)mem->read_u16b(a+2),ab=(int16_t)mem->read_u16b(a+4),ar=(int16_t)mem->read_u16b(a+6);
      int bt=(int16_t)mem->read_u16b(b),bl=(int16_t)mem->read_u16b(b+2),bb=(int16_t)mem->read_u16b(b+4),br=(int16_t)mem->read_u16b(b+6);
      if(d){ mem->write_u16b(d,min(at,bt));mem->write_u16b(d+2,min(al,bl));
             mem->write_u16b(d+4,max(ab,bb));mem->write_u16b(d+6,max(ar,br)); }
    } else if(nm=="InterfaceLib:EmptyRect"){
      uint32_t a=r.r[3].u; int t=(int16_t)mem->read_u16b(a),l=(int16_t)mem->read_u16b(a+2),
        b=(int16_t)mem->read_u16b(a+4),rr=(int16_t)mem->read_u16b(a+6); ret((l>=rr||t>=b)?1:0);
    } else if(nm=="InterfaceLib:PtInRect"){
      // PtInRect(Point pt, Rect* r) -> Boolean. Point passed as r3 (v<<16|h) on PPC.
      uint32_t pt=r.r[3].u, rp=r.r[4].u;
      int v=(int16_t)(pt>>16), h=(int16_t)(pt&0xFFFF);
      int t=(int16_t)mem->read_u16b(rp),l=(int16_t)mem->read_u16b(rp+2),
        b=(int16_t)mem->read_u16b(rp+4),rr=(int16_t)mem->read_u16b(rp+6);
      ret((h>=l&&h<rr&&v>=t&&v<b)?1:0);
    } else if(nm=="InterfaceLib:SetPt"){
      uint32_t p=r.r[3].u; if(p){ mem->write_u16b(p,(uint16_t)r.r[5].u); mem->write_u16b(p+2,(uint16_t)r.r[4].u); }
    } else if(nm=="InterfaceLib:LocalToGlobal"||nm=="InterfaceLib:GlobalToLocal"){
      // Full-screen port at origin (0,0): local == global, no-op.
    } else if(nm=="InterfaceLib:EqualRect"){
      uint32_t a=r.r[3].u,b=r.r[4].u; bool eq=true;
      for(int k=0;k<8;k+=2) if(mem->read_u16b(a+k)!=mem->read_u16b(b+k)) eq=false;
      ret(eq?1:0);
    } else if(nm=="InterfaceLib:GetMaxDevice"||nm=="InterfaceLib:GetDeviceList"){
      ret(H.g_gdh);   // single main device (head of the list)
    } else if(nm=="InterfaceLib:GetNextDevice"){
      // We expose exactly ONE GDevice. GetNextDevice(gd) MUST return 0 to end the
      // list, else `for(gd=..; gd; gd=GetNextDevice(gd))` walks forever (Psycho Deli
      // hung here at 9M calls). Return the device only if asked for something before
      // ours; simplest correct model for a single device: always 0 (no next).
      ret(0);
    } else if(nm=="InterfaceLib:TestDeviceAttribute"){
      // TestDeviceAttribute(GDHandle gdh, short attribute) -> Boolean, computed from the
      // GDevice's actual gdFlags bits (@0x14): screenDevice(13)/screenActive(15)/
      // mainScreen(11)/gdDevType(0) = true, ramInit(10)/allInit(12)/noDriver(14) = false.
      if(NODEVATTR){ ADSTUB(string("STUB:")+nm); ret(1); }   // opt-out: constant 1
      else {
        uint32_t gdh=r.r[3].u; uint32_t gd=gdh?mem->read_u32b(gdh):H.g_gdev;
        uint16_t flags = gd?mem->read_u16b(gd+0x14):0;
        int attr=(int)(int16_t)r.r[4].u;
        ret((attr>=0 && attr<16 && ((flags>>attr)&1)) ? 1 : 0);
      }
    } else if(nm=="InterfaceLib:MapRect"||nm=="InterfaceLib:MapPt"){
      // MapRect(Rect* r, Rect* srcMap, Rect* dstMap): map r proportionally from
      // srcMap to dstMap. Our canvases are 1:1 same-size, so this is identity.
      // (Implemented proportionally to be safe.)
      uint32_t rp=r.r[3].u, s=r.r[4].u, d=r.r[5].u;
      if(rp&&s&&d){
        int st=(int16_t)mem->read_u16b(s),sl=(int16_t)mem->read_u16b(s+2),sb=(int16_t)mem->read_u16b(s+4),sr=(int16_t)mem->read_u16b(s+6);
        int dt=(int16_t)mem->read_u16b(d),dl=(int16_t)mem->read_u16b(d+2),db=(int16_t)mem->read_u16b(d+4),dr=(int16_t)mem->read_u16b(d+6);
        int sw=sr-sl,sh=sb-st,dw=dr-dl,dh=db-dt;
        if(sw>0&&sh>0){ for(int k=0;k<8;k+=2){ int v=(int16_t)mem->read_u16b(rp+k); int nv;
          if(k==2||k==6){ nv=dl+(int)((int64_t)(v-sl)*dw/sw); } else { nv=dt+(int)((int64_t)(v-st)*dh/sh); }
          mem->write_u16b(rp+k,(uint16_t)nv); } }
      }
    } else if(nm=="InterfaceLib:EraseRect"||nm=="InterfaceLib:FillRect"||
              nm=="InterfaceLib:PaintRect"||nm=="InterfaceLib:InvertRect"||
              nm=="InterfaceLib:FrameRect"){
      // Fill a rect in the CURRENT port's pixmap. EraseRect uses the back color
      // (index 0 = black here); FillRect/PaintRect use a mid color. Good enough
      // to see the blank + any QuickDraw-drawn chrome land in the framebuffer.
      uint32_t rp=r.r[3].u, port=H.cur_port?H.cur_port:H.g_port;
      uint32_t pmh=mem->read_u32b(port+2), pm=pmh?mem->read_u32b(pmh):0;
      if(rp&&pm){
        uint32_t base=mem->read_u32b(pm); int rb=mem->read_u16b(pm+4)&0x3FFF;
        int pt=(int16_t)mem->read_u16b(pm+6),pl=(int16_t)mem->read_u16b(pm+8),
            pb=(int16_t)mem->read_u16b(pm+10),pr=(int16_t)mem->read_u16b(pm+12);
        int t=(int16_t)mem->read_u16b(rp),l=(int16_t)mem->read_u16b(rp+2),
            b=(int16_t)mem->read_u16b(rp+4),rr=(int16_t)mem->read_u16b(rp+6);
        uint8_t col = (nm=="InterfaceLib:EraseRect")?0:0;
        for(int y=max(t,pt);y<min(b,pb);y++) for(int x=max(l,pl);x<min(rr,pr);x++)
          mem->write_u8(base+(uint32_t)(y-pt)*rb+(x-pl), col);
      }
    } else if(nm=="InterfaceLib:NewRgn"){
      // Handle to a minimal empty QuickDraw region {rgnSize=10, rgnBBox=0}.
      uint32_t rgn=mem->allocate(10); mem->memset(rgn,0,10); mem->write_u16b(rgn,10);
      uint32_t h=mem->allocate(4); mem->write_u32b(h,rgn); ret(h);
    } else if(nm=="InterfaceLib:RectRgn"||nm=="InterfaceLib:SetRectRgn"||
              nm=="InterfaceLib:CopyRgn"||nm=="InterfaceLib:SetEmptyRgn"||
              nm=="InterfaceLib:UnionRgn"||nm=="InterfaceLib:SectRgn"||
              nm=="InterfaceLib:DiffRgn"||nm=="InterfaceLib:XorRgn"||
              nm=="InterfaceLib:OffsetRgn"||nm=="InterfaceLib:EmptyRgn"){
      // Real bounding-box region model: only the 10-byte {rgnSize, rgnBBox =
      // top,left,bottom,right @ +2/+4/+6/+8} form, no scanline data. EqualRgn/EmptyRgn READ
      // the bbox and RectRgn+SetClip must build a usable clip, so leaving every bbox
      // {0,0,0,0} fabricates "unchanged"/"empty" and a permanently empty clip.
      // Sect/Union/Diff/Xor are exact for rectangles and documented upper-bound
      // approximations for non-rect operands.
      if(NORGN){ ADSTUB(string("STUB:")+nm); }   // opt-out: no-op (EmptyRgn returns r3 unchanged)
      else {
      auto rgnOf=[&](uint32_t h)->uint32_t{ return h?mem->read_u32b(h):0; };
      auto getbb=[&](uint32_t rg,int16_t&t,int16_t&l,int16_t&b,int16_t&rr){
        t=(int16_t)mem->read_u16b(rg+2); l=(int16_t)mem->read_u16b(rg+4);
        b=(int16_t)mem->read_u16b(rg+6); rr=(int16_t)mem->read_u16b(rg+8); };
      auto setbb=[&](uint32_t rg,int t,int l,int b,int rr){
        mem->write_u16b(rg,10);
        mem->write_u16b(rg+2,(uint16_t)(int16_t)t); mem->write_u16b(rg+4,(uint16_t)(int16_t)l);
        mem->write_u16b(rg+6,(uint16_t)(int16_t)b); mem->write_u16b(rg+8,(uint16_t)(int16_t)rr); };
      if(nm=="InterfaceLib:RectRgn"){                       // RectRgn(rgn, Rect*)
        uint32_t rg=rgnOf(r.r[3].u), rp=r.r[4].u;
        if(rg&&rp){ int t=(int16_t)mem->read_u16b(rp),l=(int16_t)mem->read_u16b(rp+2),
          b=(int16_t)mem->read_u16b(rp+4),rr=(int16_t)mem->read_u16b(rp+6);
          if(b<=t||rr<=l) setbb(rg,0,0,0,0); else setbb(rg,t,l,b,rr); }
      } else if(nm=="InterfaceLib:SetRectRgn"){             // SetRectRgn(rgn,left,top,right,bottom)
        uint32_t rg=rgnOf(r.r[3].u);
        int l=(int16_t)r.r[4].u,t=(int16_t)r.r[5].u,rr=(int16_t)r.r[6].u,b=(int16_t)r.r[7].u;
        if(rg){ if(rr<=l||b<=t) setbb(rg,0,0,0,0); else setbb(rg,t,l,b,rr); }
      } else if(nm=="InterfaceLib:SetEmptyRgn"){
        uint32_t rg=rgnOf(r.r[3].u); if(rg) setbb(rg,0,0,0,0);
      } else if(nm=="InterfaceLib:CopyRgn"){                // CopyRgn(src,dst)
        uint32_t s=rgnOf(r.r[3].u),d=rgnOf(r.r[4].u);
        if(s&&d){ int16_t t,l,b,rr; getbb(s,t,l,b,rr); setbb(d,t,l,b,rr); }
      } else if(nm=="InterfaceLib:OffsetRgn"){              // OffsetRgn(rgn,dh,dv)
        uint32_t rg=rgnOf(r.r[3].u); int16_t dh=(int16_t)r.r[4].u,dv=(int16_t)r.r[5].u;
        if(rg){ int16_t t,l,b,rr; getbb(rg,t,l,b,rr);
          if(!(b<=t||rr<=l)) setbb(rg,t+dv,l+dh,b+dv,rr+dh); }   // empty rgn unaffected
      } else if(nm=="InterfaceLib:EmptyRgn"){               // EmptyRgn(rgn)->Boolean
        uint32_t rg=rgnOf(r.r[3].u); bool empty=true;
        if(rg){ int16_t t,l,b,rr; getbb(rg,t,l,b,rr); empty=(b<=t||rr<=l); }
        ret(empty?1:0);
      } else {                                              // SectRgn/UnionRgn/DiffRgn/XorRgn(a,b,dst)
        uint32_t a=rgnOf(r.r[3].u),b=rgnOf(r.r[4].u),d=rgnOf(r.r[5].u);
        if(a&&b&&d){
          int16_t at,al,ab,ar,bt,bl,bb2,br; getbb(a,at,al,ab,ar); getbb(b,bt,bl,bb2,br);
          bool ae=(ab<=at||ar<=al), be=(bb2<=bt||br<=bl);
          if(nm=="InterfaceLib:SectRgn"){
            int t=max<int>(at,bt),l=max<int>(al,bl),bo=min<int>(ab,bb2),rr=min<int>(ar,br);
            if(ae||be||bo<=t||rr<=l) setbb(d,0,0,0,0); else setbb(d,t,l,bo,rr);
          } else if(nm=="InterfaceLib:DiffRgn"){
            // a minus b: bbox can only shrink -> keep a's bbox (documented upper bound).
            if(ae) setbb(d,0,0,0,0); else setbb(d,at,al,ab,ar);
          } else { // UnionRgn / XorRgn -> bounding union (exact for Union of rects; upper bound for Xor)
            if(ae&&be) setbb(d,0,0,0,0);
            else if(ae) setbb(d,bt,bl,bb2,br);
            else if(be) setbb(d,at,al,ab,ar);
            else setbb(d,min<int>(at,bt),min<int>(al,bl),max<int>(ab,bb2),max<int>(ar,br));
          }
        }
      }
      }   // end !NORGN
    } else if(nm=="InterfaceLib:DisposeRgn"||nm=="InterfaceLib:OpenRgn"||
              nm=="InterfaceLib:CloseRgn"){
      // DisposeRgn: leak-by-design (the bump arena has no free list). OpenRgn/
      // CloseRgn: region path recording is not modelled in the bbox-only model — no-op.
      ADSTUB(string("STUB:")+nm);
    } else if(nm=="InterfaceLib:EqualRgn"){
      // EqualRgn(a,b) -> Boolean. Compare the two regions' 8-byte bounding boxes
      // (rgn+2..+8). Modules gate work on "region unchanged" (Magic Turtle), so a
      // constant "not equal" means they never settle.
      uint32_t ah=r.r[3].u,bh=r.r[4].u; uint32_t ra=ah?mem->read_u32b(ah):0, rb=bh?mem->read_u32b(bh):0;
      bool eq=(ra&&rb); if(ra&&rb) for(int k=2;k<10;k+=2) if(mem->read_u16b(ra+k)!=mem->read_u16b(rb+k)) eq=false;
      ret(eq?1:0);
    } else if(nm=="InterfaceLib:NewGWorld"){
      // NewGWorld(GWorldPtr* out, short depth, Rect* bounds, CTabHandle, GDHandle, flags)
      // Build a real offscreen CGrafPort with its own PixMap + pixel buffer, so
      // the module can composite sprites into it and later blit to the screen.
      uint32_t outp=r.r[3].u; int depth=(int16_t)r.r[4].u; uint32_t rectp=r.r[5].u;
      if(depth<=0) depth=8;
      int top=(int16_t)mem->read_u16b(rectp+0), left=(int16_t)mem->read_u16b(rectp+2);
      int bot=(int16_t)mem->read_u16b(rectp+4), rgt=(int16_t)mem->read_u16b(rectp+6);
      int w=rgt-left, h=bot-top; if(w<1)w=1; if(h<1)h=1;
      int rb=(((w*depth+7)/8)+1)&~1;
      uint32_t buf=mem->allocate((uint32_t)rb*h); mem->memset(buf,0,(uint32_t)rb*h);
      uint32_t cth=mem->allocate(4); mem->write_u32b(cth,H.g_clut);
      uint32_t pm=mem->allocate(0x32); mem->memset(pm,0,0x32);
      mem->write_u32b(pm+0x00, buf);
      mem->write_u16b(pm+0x04, 0x8000 | (uint16_t)rb);
      mem->write_u16b(pm+0x06, top); mem->write_u16b(pm+0x08, left);
      mem->write_u16b(pm+0x0A, bot); mem->write_u16b(pm+0x0C, rgt);
      mem->write_u32b(pm+0x16, 0x00480000); mem->write_u32b(pm+0x1A, 0x00480000);
      mem->write_u16b(pm+0x20, (uint16_t)depth);
      mem->write_u16b(pm+0x22, 1); mem->write_u16b(pm+0x24, (uint16_t)depth);
      mem->write_u32b(pm+0x2A, cth);
      uint32_t pmh=mem->allocate(4); mem->write_u32b(pmh, pm);
      uint32_t rg1=mem->allocate(10); mem->write_u16b(rg1,10);
      mem->write_u16b(rg1+2,top);mem->write_u16b(rg1+4,left);mem->write_u16b(rg1+6,bot);mem->write_u16b(rg1+8,rgt);
      uint32_t vh=mem->allocate(4); mem->write_u32b(vh,rg1);
      uint32_t rg2=mem->allocate(10); mem->write_u16b(rg2,10);
      mem->write_u16b(rg2+2,top);mem->write_u16b(rg2+4,left);mem->write_u16b(rg2+6,bot);mem->write_u16b(rg2+8,rgt);
      uint32_t ch=mem->allocate(4); mem->write_u32b(ch,rg2);
      uint32_t port=mem->allocate(0x100); mem->memset(port,0,0x100);
      mem->write_u32b(port+0x02, pmh);
      mem->write_u16b(port+0x06, 0xC000);
      mem->write_u16b(port+0x10, top); mem->write_u16b(port+0x12, left);
      mem->write_u16b(port+0x14, bot); mem->write_u16b(port+0x16, rgt);
      mem->write_u32b(port+0x18, vh);
      mem->write_u32b(port+0x1C, ch);
      if(outp) mem->write_u32b(outp, port);
      H.gworlds.push_back(port);
      if(getenv("ADALLOCLOG")) fprintf(stderr,"      NewGWorld bounds=%d,%d,%d,%d depth=%d -> port=%08X pm=%08X buf=%08X (%dx%d rb=%d)\n",
              top,left,bot,rgt,depth,port,pm,buf,w,h,rb);
      ret(0);
    } else if(nm=="InterfaceLib:LockPixels"||nm=="InterfaceLib:AllowPurgePixels"||
              nm=="InterfaceLib:NoPurgePixels"){
      ADSTUB(string("STUB:")+nm);  // constant 1 (pixels always resident in host)
      ret(1);  // Boolean true / noErr — pixels are always resident here
    } else if(nm=="InterfaceLib:GetPixBaseAddr"){
      // GetPixBaseAddr(PixMapHandle) -> Ptr (baseAddr @ PixMap+0x00)
      uint32_t pmh=r.r[3].u; uint32_t base=0;
      if(pmh){ uint32_t pm=mem->read_u32b(pmh); if(pm) base=mem->read_u32b(pm); }
      ret(base);
    } else if(nm=="InterfaceLib:CopyBits"||nm=="InterfaceLib:CopyMask"||
              nm=="InterfaceLib:CopyDeepMask"){
      // CopyBits(srcBits,dstBits,srcRect,dstRect,mode,maskRgn). srcBits/dstBits
      // are either a real (Pix|Bit)Map* or &CGrafPort.portBits (offset +2 with
      // portVersion 0xC000 @ +6, portPixMap handle @ +2). Copy 8-bit pixels
      // from srcRect (src-local coords) to dstRect (dst-local coords).
      auto resolve=[&](uint32_t bits, uint32_t& base, int& rb, int& t,int& l,int& b,int& rr, int& depth){
        uint16_t rbf=mem->read_u16b(bits+4);
        uint32_t pm;
        if((rbf&0xC000)==0xC000){ uint32_t pmh=mem->read_u32b(bits); pm=mem->read_u32b(pmh); }
        else if(rbf&0x8000){ pm=bits; }
        else { base=mem->read_u32b(bits); rb=rbf; t=(int16_t)mem->read_u16b(bits+6);
               l=(int16_t)mem->read_u16b(bits+8); b=(int16_t)mem->read_u16b(bits+10);
               rr=(int16_t)mem->read_u16b(bits+12); depth=1; return; }
        base=mem->read_u32b(pm+0); rb=mem->read_u16b(pm+4)&0x3FFF;
        t=(int16_t)mem->read_u16b(pm+6); l=(int16_t)mem->read_u16b(pm+8);
        b=(int16_t)mem->read_u16b(pm+10); rr=(int16_t)mem->read_u16b(pm+12);
        depth=mem->read_u16b(pm+0x20);
      };
      uint32_t sB=0,dB=0; int sRB=0,dRB=0,sT=0,sL=0,sBt=0,sRt=0,sD=0, dT=0,dL=0,dBt=0,dRt=0,dD=0;
      resolve(r.r[3].u,sB,sRB,sT,sL,sBt,sRt,sD);
      resolve(r.r[4].u,dB,dRB,dT,dL,dBt,dRt,dD);
      uint32_t srp=r.r[5].u, drp=r.r[6].u;
      int srT=(int16_t)mem->read_u16b(srp),srL=(int16_t)mem->read_u16b(srp+2),
          srB=(int16_t)mem->read_u16b(srp+4),srR=(int16_t)mem->read_u16b(srp+6);
      int drT=(int16_t)mem->read_u16b(drp),drL=(int16_t)mem->read_u16b(drp+2),
          drB=(int16_t)mem->read_u16b(drp+4),drR=(int16_t)mem->read_u16b(drp+6);
      int sw=srR-srL, sh=srB-srT, dw=drR-drL, dh=drB-drT;
      // Buffer extents (pixmap-local), used to clamp so a bad/oversized rect
      // can never read or write outside the allocated pixel buffers.
      int sExtW=sRt-sL, sExtH=sBt-sT, dExtW=dRt-dL, dExtH=dBt-dT;
      if(getenv("ADCBLOG"))
        fprintf(stderr,"      CopyBits src=%08X(base=%08X %dx%d rb=%d d=%d) -> dst=%08X(base=%08X %dx%d rb=%d d=%d) srcR=%d,%d,%d,%d dstR=%d,%d,%d,%d%s\n",
                r.r[3].u,sB,sExtW,sExtH,sRB,sD, r.r[4].u,dB,dExtW,dExtH,dRB,dD, srT,srL,srB,srR, drT,drL,drB,drR,
                (dB==H.g_fb)?" [->SCREEN]":"");
      if(sB&&dB&&sD==8&&dD==8&&sw>0&&sh>0&&dw>0&&dh>0&&sExtW>0&&sExtH>0&&dExtW>0&&dExtH>0){
        for(int y=0;y<dh;y++){
          int sy=srT + (dh==sh? y : (int)((int64_t)y*sh/dh)) - sT;
          int dy=drT + y - dT;
          if(sy<0||dy<0||sy>=sExtH||dy>=dExtH) continue;
          uint32_t srow=sB+(uint32_t)sy*sRB, drow=dB+(uint32_t)dy*dRB;
          for(int x=0;x<dw;x++){
            int sx=srL + (dw==sw? x : (int)((int64_t)x*sw/dw)) - sL;
            int dx=drL + x - dL;
            if(sx<0||dx<0||sx>=sExtW||dx>=dExtW) continue;
            mem->write_u8(drow+dx, mem->read_u8(srow+sx));
          }
        }
      }
      H.copybits_count++;
      ret(0);
    } else if(nm=="InterfaceLib:UpdateGWorld"||nm=="InterfaceLib:DisposeGWorld"||
              nm=="InterfaceLib:UnlockPixels"||nm=="InterfaceLib:CTabChanged"){
      ADSTUB(string("STUB:")+nm);  // UpdateGWorld should return flags; all no-op noErr
      ret(0);
    } else if(nm=="InterfaceLib:GetDeviceList"){
      ret(H.g_gdh);
    } else if(nm=="InterfaceLib:SetResLoad"||
              nm=="InterfaceLib:HSetVol"||nm=="InterfaceLib:SetVol"){
      // resource/file mgr residency hints — no-op (always-resident model). UseResFile is
      // deliberately NOT in this group: it has a real cur_file remap further down.
      ADSTUB(string("STUB:")+nm);
    } else if(nm=="InterfaceLib:Gestalt"){
      // r3 = selector, r4 = long* response. For the After Dark selector 'aYmm'
      // return a pointer to an AD-info struct whose version short (+0x0C) is 768,
      // so AfterDarkExists()'s version check passes. Other selectors: absent (0).
      if(r.r[3].u == 0x61596D6D){          // 'aYmm'
        if(!H.ad_info){ H.ad_info=mem->allocate(0x40); mem->memset(H.ad_info,0,0x40);
                        // AD version at +0x0C. 0x300=3.0; many 4.0 modules require >=4.0.
                        // Report 4.1 (0x0410) so version checks pass. ADADVER overrides.
                        uint16_t adver = getenv("ADADVER")? (uint16_t)strtoul(getenv("ADADVER"),0,0) : 0x0410;
                        mem->write_u16b(H.ad_info+0x0C, adver); }
        if(r.r[4].u) mem->write_u32b(r.r[4].u, H.ad_info);
        ret(0);   // noErr
      } else {
        // Unknown selector: a real Mac returns gestaltUndefSelectorErr(-5551) with response
        // 0. noErr+0 would fake "feature present, value 0".
        if(r.r[4].u) mem->write_u32b(r.r[4].u, 0);
        if(NOGESTERR){ ADSTUB("STUB:Gestalt_selector_absent"); ret(0); }  // opt-out: noErr
        else ret((uint32_t)(int32_t)-5551);
      }
    } else if(nm=="InterfaceLib:Get1Resource"||nm=="InterfaceLib:GetResource"){
      // r3 = ResType, r4 = short id -> Handle (0 if absent)
      uint32_t h=H.handle_for(r.r[3].u, (int16_t)r.r[4].u);
      H.last_res_err = h ? 0 : -192;   // resNotFound on a missing resource
      ret(h);
    } else if(nm=="InterfaceLib:Get1IndResource"||nm=="InterfaceLib:GetIndResource"){
      // r3 = ResType, r4 = short index (1-based) -> Handle (current file)
      uint32_t h=0;
      if(!H.res_files.empty()){ int fi=(H.cur_file<(int)H.res_files.size())?H.cur_file:0;
        auto ids=H.res_files[fi].all_resources_of_type(r.r[3].u); int idx=(int16_t)r.r[4].u;
        if(idx>=1 && idx<=(int)ids.size()) h=H.handle_for(r.r[3].u, ids[idx-1]); }
      H.last_res_err = h ? 0 : -192;
      ret(h);
    } else if(nm=="InterfaceLib:Count1Resources"||nm=="InterfaceLib:CountResources"){
      uint32_t c=0; if(!H.res_files.empty()){ int fi=(H.cur_file<(int)H.res_files.size())?H.cur_file:0;
        c=(uint32_t)H.res_files[fi].count_resources_of_type(r.r[3].u); }
      ret(c);
    } else if(nm=="InterfaceLib:Count1Types"||nm=="InterfaceLib:CountTypes"){
      ret(H.res_files.empty()?0:(uint32_t)H.res_files[0].all_resource_types().size());
    } else if(nm=="InterfaceLib:GetIndString"){
      // r3 = StringPtr dest, r4 = short listID, r5 = short index. Serve STR#.
      uint32_t dst=r.r[3].u; int16_t lid=(int16_t)r.r[4].u; int idx=(int16_t)r.r[5].u;
      if(dst) mem->write_u8(dst,0);
      int fi=H.find_file(0x53545223 /*STR#*/, lid);
      if(fi>=0 && dst){
        auto res=H.res_files[fi].get_resource(0x53545223, lid); const string& d=res->data;
        if(d.size()>=2){ int n=(uint8_t(d[0])<<8)|uint8_t(d[1]); size_t o=2;
          for(int i=1;i<=n && o<d.size(); i++){ int len=uint8_t(d[o]);
            if(i==idx){ mem->write_u8(dst,len); for(int k=0;k<len && o+1+k<d.size();k++) mem->write_u8(dst+1+k, d[o+1+k]); break; }
            o += 1+len; } }
      }
    } else if(nm=="InterfaceLib:SizeResource"||nm=="InterfaceLib:GetResourceSizeOnDisk"||
              nm=="InterfaceLib:GetMaxResourceSize"){
      auto it=H.handle_res.find(r.r[3].u); uint32_t sz=0;
      if(it!=H.handle_res.end()){ int fi=H.find_file(it->second.first,it->second.second);
        if(fi>=0) sz=(uint32_t)H.res_files[fi].get_resource(it->second.first,it->second.second)->data.size(); }
      ret(sz);
    } else if(nm=="InterfaceLib:UseResFile"){
      if(NOUSERESFIX){ ADSTUB(string("STUB:")+nm); }   // opt-out: no-op
      else {
        H.cur_file = (int)((int16_t)r.r[3].u) - 2;   // refnum 2 -> file 0, 3 -> 1, ...
        if(H.cur_file<0||H.cur_file>=(int)H.res_files.size()) H.cur_file=0;
      }
    } else if(nm=="InterfaceLib:OpenResFile"||nm=="InterfaceLib:FSpOpenResFile"||
              nm=="InterfaceLib:OpenRFPerm"||nm=="InterfaceLib:HOpenResFile"){
      // A named companion file (…After Dark 4.0 Library) maps to file index 1.
      ret(H.res_files.size()>1 ? 3 : 2);
    } else if(nm=="InterfaceLib:CurResFile"||nm=="InterfaceLib:HomeResFile"){
      ret(H.cur_file + 2);
    } else if(nm=="InterfaceLib:LoadResource"||nm=="InterfaceLib:ReleaseResource"||
              nm=="InterfaceLib:DetachResource"||nm=="InterfaceLib:CloseResFile"||
              nm=="InterfaceLib:SetResAttrs"||nm=="InterfaceLib:ChangedResource"||
              nm=="InterfaceLib:GetResAttrs"){
      // no-op (data already resident); GetResAttrs returns 0 attrs
      ADSTUB(string("STUB:")+nm);
      if(nm=="InterfaceLib:GetResAttrs") ret(0);
    } else if(nm=="InterfaceLib:GetResInfo"){
      // r3=Handle, r4=short* id, r5=ResType* type
      auto it=H.handle_res.find(r.r[3].u);
      if(it!=H.handle_res.end()){ if(r.r[4].u) mem->write_u16b(r.r[4].u,(uint16_t)it->second.second);
                                  if(r.r[5].u) mem->write_u32b(r.r[5].u,it->second.first); }
    } else if(nm=="InterfaceLib:Microseconds"){
      // r3 -> UnsignedWide; advance a virtual clock. Default 1ms/call; the FT
      // module spawns toasters on 400-500ms timers, so the per-call step sets
      // the effective animation speed. ADUSPERCALL overrides it.
      static uint64_t us=0;
      static uint64_t step = getenv("ADUSPERCALL")? strtoull(getenv("ADUSPERCALL"),0,0) : 1000;
      if(ad_wallclock_on()) us = (uint64_t)(ad_wall_elapsed()*1.0e6) + (uint64_t)((double)g_adff_ticks*16625.15);   // real elapsed us + idle catch-up (ticks @60.15Hz -> us)
      else us+=step;
      if(getenv("ADTICKLOG")){ static uint32_t nc=0; if((++nc & 0x3F)==0)
        fprintf(stderr,"[uslog] call=%u wall=%.2fs -> virt=%.2fs\n",nc,ad_wall_elapsed(),us/1.0e6); }
      if(r.r[3].u){ mem->write_u32b(r.r[3].u, us>>32); mem->write_u32b(r.r[3].u+4, us&0xFFFFFFFF); }
    } else if(nm=="InterfaceLib:GetDateTime"||nm=="InterfaceLib:ReadDateTime"){
      // GetDateTime(unsigned long* secs): Mac seconds since 1904-01-01, LOCAL time. THIS coarse
      // source (decoded by SecondsToDate) is the one Time Flies draws as its time-of-day — not
      // TickCount/Microseconds. The fixed epoch 0xB0000000 decodes to 19:26:56, and the per-call
      // +1 advances one Mac-second EVERY call, so at the app's ~30 fps a displayed clock runs
      // tens of x fast.
      //   ADREALCLOCK (opt-IN, set in the catalog recipe for clock savers only): serve the REAL
      // current local time so the clock starts == wall clock and ticks at true 1x. It is NOT the
      // default because a real-time (1s-granularity) GetDateTime FREEZES modules whose
      // animation/RNG spins on do{GetDateTime(&t)}while(t==start): Psycho Deli runs that loop
      // EVERY frame, so a value that only changes once per real second drops it to ~1 fps. Those
      // modules keep the per-call virtual epoch below. ADFAKEEPOCH forces a fixed value for
      // deterministic reference renders.
      uint32_t secs;
      if(getenv("ADFAKEEPOCH")){
        secs = (uint32_t)strtoull(getenv("ADFAKEEPOCH"),0,0);
      } else if(getenv("ADREALCLOCK")){
        time_t now=time(NULL); struct tm lt; localtime_r(&now,&lt);
        // Unix(1970 UTC) -> Mac(1904 LOCAL): +2082844800 base epoch, + local UTC offset (incl. DST).
        secs = (uint32_t)((int64_t)now + 2082844800LL + (int64_t)lt.tm_gmtoff);
      } else {
        static uint32_t s = 0xB0000000; s += 1; secs = s;
      }
      if(getenv("ADDATELOG")){ static uint32_t nc=0; if((++nc & 0x3F)==0)
        fprintf(stderr,"[datelog] call=%u secs=%u tod=%02u:%02u:%02u\n",nc,secs,
          (unsigned)((secs%86400)/3600),(unsigned)((secs%86400)%3600/60),(unsigned)(secs%60)); }
      if(r.r[3].u) mem->write_u32b(r.r[3].u, secs);
      ret(0); // noErr for ReadDateTime
    } else if(nm=="InterfaceLib:TickCount"){
      static uint32_t t=0; static uint32_t step=getenv("ADTICKSTEP")?strtoul(getenv("ADTICKSTEP"),0,0):1;
      uint32_t tv = ad_wallclock_on() ? (uint32_t)(ad_wall_elapsed()*60.14985) + g_adff_ticks : (t+=step);  // 60.15 Hz + idle catch-up
      if(getenv("ADTICKLOG")){ static uint32_t nc=0; if((++nc & 0x3F)==0)
        fprintf(stderr,"[ticklog] call=%u tick=%u wall=%.2fs -> virt=%.2fs\n",nc,tv,ad_wall_elapsed(),tv/60.14985); }
      ret(tv);
    } else if(nm=="InterfaceLib:Random"){
      // Toolbox Random(): randSeed = (randSeed*16807) mod (2^31-1); return LoWord as
      // a signed 16-bit. Modules fill a field by rejection-sampling Random (Points of
      // View spins ~4.5M Random/frame building its illusion), so a constant return
      // degenerates the whole field.
      if(getenv("ADNORAND")){ ret(0); }
      else {
        // Seed lives in Host.rnd_seed so LMSetRndSeed can set the sequence (low-mem RndSeed).
        uint64_t t = (uint64_t)H.rnd_seed * 16807u; H.rnd_seed = (uint32_t)(t % 0x7FFFFFFFu);
        if(H.rnd_seed==0) H.rnd_seed=1;
        ret((uint32_t)(int32_t)(int16_t)(H.rnd_seed & 0xFFFF));
      }
    } else if(nm=="InterfaceLib:FreeMem"||nm=="InterfaceLib:MaxMem"||
              nm=="InterfaceLib:CompactMem"||nm=="InterfaceLib:FreeMemSys"||
              nm=="InterfaceLib:MaxBlock"||nm=="InterfaceLib:MaxBlockSys"||
              nm=="InterfaceLib:FreeMemSys"){
      ADSTUB(string("STUB:")+nm);  // constant 32 MB free
      ret(0x02000000); // pretend 32 MB free
    } else if(nm=="InterfaceLib:PurgeSpace"||nm=="InterfaceLib:PurgeSpaceSys"){
      // r3 = long* total, r4 = long* contig
      ADSTUB(string("STUB:")+nm);  // constant 32 MB
      if(r.r[3].u) mem->write_u32b(r.r[3].u, 0x02000000);
      if(r.r[4].u) mem->write_u32b(r.r[4].u, 0x02000000);
      ret(0x02000000);
    } else if(nm=="InterfaceLib:TempMaxMem"){
      // r3 = long* grow -> return biggest free block
      ADSTUB(string("STUB:")+nm);  // constant 32 MB
      if(r.r[3].u) mem->write_u32b(r.r[3].u, 0);
      ret(0x02000000);
    } else if(nm=="InterfaceLib:TempFreeMem"){
      ADSTUB(string("STUB:")+nm);  // constant 32 MB
      ret(0x02000000);
    } else if(nm=="InterfaceLib:TempNewHandle"){
      // r3 = size, r4 = OSErr* -> Handle (real allocation, like NewHandle)
      uint32_t sz=r.r[3].u; uint32_t p = sz? mem->allocate(sz):0;
      uint32_t h=mem->allocate(4); mem->write_u32b(h,p);
      H.block_size[h]=4; if(p) H.block_size[p]=sz;
      if(r.r[4].u) mem->write_u16b(r.r[4].u, 0); // noErr
      if(getenv("ADALLOCLOG")) fprintf(stderr,"      TempNewHandle sz=%08X -> block %08X..%08X\n",sz,p,p+sz);
      ret(h);
    } else if(nm=="InterfaceLib:GetZone"||nm=="InterfaceLib:SystemZone"||
              nm=="InterfaceLib:ApplicationZone"||nm=="InterfaceLib:GetApplLimit"){
      // A single synthetic zone header; contents unused by our path.
      static uint32_t zone=0; if(!zone){ zone=mem->allocate(0x20); mem->memset(zone,0,0x20); }
      ADSTUB(string("STUB:")+nm);  // synthetic zeroed zone header (contents unused)
      ret(zone);
    } else if(nm=="InterfaceLib:SetZone"||nm=="InterfaceLib:SetApplLimit"||
              nm=="InterfaceLib:MoreMasters"||nm=="InterfaceLib:ReserveMem"||
              nm=="InterfaceLib:ReserveMemSys"){
      // no-op
      ADSTUB(string("STUB:")+nm);
    } else if(nm=="InterfaceLib:FSMakeFSSpec"||nm=="InterfaceLib:FSpOpenDF"||
              nm=="InterfaceLib:FSpOpenRF"||nm=="InterfaceLib:HOpen"||
              nm=="InterfaceLib:HOpenRF"||nm=="InterfaceLib:OpenDF"||
              nm=="InterfaceLib:PBGetCatInfoSync"||nm=="InterfaceLib:PBGetCatInfo"||
              nm=="InterfaceLib:PBHGetVInfoSync"||nm=="InterfaceLib:PBGetFInfoSync"){
      // No external files on disk: report file-not-found so the module skips
      // file-backed sounds gracefully (matches a Mac with the files absent).
      // CRITICAL: PBGetCatInfoSync must return an error, else a module's directory
      // scan (e.g. Art Critic hunting painting files) loops forever expecting the
      // File Manager to signal "no more entries" — 1.8M calls -> the max-call cap.
      ADSTUB(string("STUB:")+nm);  // constant fnfErr (no filesystem)
      ret((uint32_t)(int32_t)-43); // fnfErr
    } else if(nm=="InterfaceLib:GetEOF"){
      // r3 = refNum, r4 = long* eof -> write 0 and report error
      ADSTUB(string("STUB:")+nm);  // constant fnfErr, eof=0
      if(r.r[4].u) mem->write_u32b(r.r[4].u, 0);
      ret((uint32_t)(int32_t)-43); // fnfErr
    } else if(nm=="InterfaceLib:FSClose"||nm=="InterfaceLib:SetEOF"||
              nm=="InterfaceLib:SetFPos"||nm=="InterfaceLib:GetFPos"){
      ADSTUB(string("STUB:")+nm);  // constant noErr (no filesystem)
      ret(0); // noErr
    } else if(nm=="InterfaceLib:FSRead"||nm=="InterfaceLib:FSWrite"){
      // No backing files: report EOF/no-data. r4 = long* count -> 0 bytes.
      ADSTUB(string("STUB:")+nm);  // constant eofErr, count=0
      if(r.r[4].u) mem->write_u32b(r.r[4].u,0);
      ret((uint32_t)(int32_t)-39); // eofErr
    } else if(nm=="InterfaceLib:HOpenDF"){
      ADSTUB(string("STUB:")+nm);  // constant fnfErr
      ret((uint32_t)(int32_t)-43); // fnfErr (no external data files)
    } else if(nm=="InterfaceLib:GetVol"||nm=="InterfaceLib:HGetVol"){
      // r4 = short* vRefNum. Report the (synthetic) boot volume, noErr.
      ADSTUB(string("STUB:")+nm);  // constant vRefNum=-1, noErr
      if(r.r[4].u) mem->write_u16b(r.r[4].u,(uint16_t)(int16_t)-1);
      ret(0);
    } else if(nm=="InterfaceLib:FindFolder"){
      // FindFolder(vRefNum, folderType, createFolder, short* foundVRefNum, long* foundDirID)
      // No real filesystem here — report the folder can't be found so modules skip
      // reading/writing prefs/temp files. A silent 0 (== noErr) would send callers on to
      // open a folder that does not exist.
      ADSTUB(string("STUB:")+nm);  // constant fnfErr (no folders)
      if(r.r[6].u) mem->write_u16b(r.r[6].u,(uint16_t)(int16_t)-1);
      if(r.r[7].u) mem->write_u32b(r.r[7].u,0);
      ret((uint32_t)(int32_t)-43); // fnfErr
    } else if(nm=="InterfaceLib:SndNewChannel"){
      // SndNewChannel(SndChannelPtr* chan, short synth, long init, SndCallBackUPP proc)
      // No audio hardware: hand back a small zeroed channel block so the module has a
      // valid non-null pointer to pass to Snd* calls, and return noErr.
      uint32_t outp=r.r[3].u;
      ADSTUB(string("STUB:")+nm);  // zeroed channel block + constant noErr (no audio)
      if(outp){ uint32_t ch=mem->allocate(0x100); mem->memset(ch,0,0x100); mem->write_u32b(outp,ch); }
      ret(0);
    } else if(nm=="InterfaceLib:SndDisposeChannel"||nm=="InterfaceLib:SndDoCommand"||
              nm=="InterfaceLib:SndDoImmediate"||nm=="InterfaceLib:SndPlay"||
              nm=="InterfaceLib:SndPlayDoubleBuffer"||nm=="InterfaceLib:SndControl"){
      // Sound Manager: explicit silent no-ops (noErr). No audio in this host.
      ADSTUB(string("STUB:")+nm);  // constant noErr (no audio)
      ret(0);
    } else if(nm=="InterfaceLib:SndChannelStatus"){
      // SndChannelStatus(chan, short len, SCStatus* status): report the channel as idle
      // (scChannelBusy=false, all counters 0). Modules wait for "channel not busy" before
      // queueing the next sound, so a garbage status stalls them.
      ADSTUB(string("STUB:")+nm);  // reports channel idle (all-zero status)
      uint32_t st=r.r[5].u, len=r.r[4].u; if(st) mem->memset(st,0,len?min<uint32_t>(len,0x40):0x18);
      ret(0);
    } else if(nm=="InterfaceLib:SecondsToDate"||nm=="InterfaceLib:Secs2Date"){
      // SecondsToDate(unsigned long secs, DateTimeRec* d): decode Mac epoch (1904-01-01)
      // seconds into y/m/d/h/m/s + weekday. Clock savers (Time Flies) draw their hands
      // and calendar text from this record.
      uint32_t secs=r.r[3].u, d=r.r[4].u;
      if(d){
        long days=(long)(secs/86400UL); long rem=(long)(secs%86400UL);
        int hh=(int)(rem/3600); int mm=(int)((rem%3600)/60); int ss=(int)(rem%60);
        // Mac dayOfWeek 1=Sunday..7=Saturday; 1904-01-01 (days==0) was a Friday.
        int dow=(int)(((days%7)+5)%7)+1;
        int year=1904; while(true){ bool leap=(year%4==0&&(year%100!=0||year%400==0));
          int yd=leap?366:365; if(days>=yd){days-=yd;year++;} else break; }
        bool leap=(year%4==0&&(year%100!=0||year%400==0));
        static const int mlen[12]={31,28,31,30,31,30,31,31,30,31,30,31};
        int mon=0; for(;mon<12;mon++){ int ml=mlen[mon]+((mon==1&&leap)?1:0);
          if(days>=ml)days-=ml; else break; }
        mem->write_u16b(d+0,(uint16_t)year); mem->write_u16b(d+2,(uint16_t)(mon+1));
        mem->write_u16b(d+4,(uint16_t)(days+1)); mem->write_u16b(d+6,(uint16_t)hh);
        mem->write_u16b(d+8,(uint16_t)mm); mem->write_u16b(d+10,(uint16_t)ss);
        mem->write_u16b(d+12,(uint16_t)dow);
      }
      ret(0);
    } else if(nm=="InterfaceLib:GetString"){
      // GetString(short id) -> StringHandle. Serve a 'STR ' resource from the chain.
      ret(H.handle_for(0x53545220 /*'STR '*/, (int16_t)r.r[3].u));
    } else if(nm=="InterfaceLib:GetMBarHeight"){ ADSTUB(string("STUB:")+nm); ret(20); // standard menu-bar height
    } else if(nm=="InterfaceLib:RecoverHandle"){
      // RecoverHandle(Ptr) -> Handle. A master pointer cannot be reverse-mapped to its
      // handle here; return 0.
      ADSTUB(string("STUB:")+nm);  // constant NULL handle (cannot reverse-map)
      ret(0);
    } else if(nm=="InterfaceLib:TempHLock"||nm=="InterfaceLib:TempHUnlock"||
              nm=="InterfaceLib:TempDisposeHandle"||nm=="InterfaceLib:InitZone"){
      // Temporary-memory / zone hints — no-op (our allocations never move or purge).
      ADSTUB(string("STUB:")+nm);
      if(nm=="InterfaceLib:TempHLock"||nm=="InterfaceLib:TempHUnlock"){
        if(r.r[4].u) mem->write_u16b(r.r[4].u,0); } // OSErr* -> noErr
    } else if(nm=="InterfaceLib:GetIndPattern"){
      // GetIndPattern(Pattern* thePat, short patListID, short index): copy the 8-byte
      // pattern #index (1-based) out of the real 'PAT#' resource patListID in the fork
      // chain. PAT# = count.w then count x 8-byte Pattern. Fall back to 50% gray only if
      // the resource is genuinely absent.
      uint32_t p=r.r[3].u; int16_t listID=(int16_t)r.r[4].u; int idx=(int16_t)r.r[5].u;
      bool served=false;
      if(NOINDPAT){ ADSTUB(string("STUB:")+nm); if(p) for(int k=0;k<8;k++) mem->write_u8(p+k,(k&1)?0x55:0xAA); served=true; }  // opt-out: gray
      int fi=(served?-1:H.find_file(0x50415423 /*'PAT#'*/, listID));
      if(p && fi>=0){
        auto res=H.res_files[fi].get_resource(0x50415423, listID); const string& d=res->data;
        if(d.size()>=2){ int cnt=(uint8_t(d[0])<<8)|uint8_t(d[1]);
          if(idx>=1 && idx<=cnt && (size_t)(2+idx*8) <= d.size()){
            size_t o=2+(size_t)(idx-1)*8; for(int k=0;k<8;k++) mem->write_u8(p+k,(uint8_t)d[o+k]);
            served=true; } }
      }
      if(p && !served) for(int k=0;k<8;k++) mem->write_u8(p+k,(k&1)?0x55:0xAA); // gray fallback
    } else if(nm=="InterfaceLib:GetPattern"){
      // GetPattern(short patID) -> PatHandle. Read the real 'PAT ' resource; fall back to
      // a 50% gray pattern only if it is absent.
      int16_t patID=(int16_t)r.r[3].u;
      uint32_t p=mem->allocate(8); bool served=false;
      int fi=(NOINDPAT?-1:H.find_file(0x50415420 /*'PAT '*/, patID));   // opt-out: gray
      if(fi>=0){ auto res=H.res_files[fi].get_resource(0x50415420, patID); const string& d=res->data;
        if(d.size()>=8){ for(int k=0;k<8;k++) mem->write_u8(p+k,(uint8_t)d[k]); served=true; } }
      if(!served) for(int k=0;k<8;k++) mem->write_u8(p+k,(k&1)?0x55:0xAA);
      uint32_t h=mem->allocate(4); mem->write_u32b(h,p); ret(h);
    } else if(nm.rfind("QuickTimeLib:",0)==0){
      ADSTUB(string("STUB:")+nm);  // QuickTime out of scope: movie=fnfErr, else no-op
      // QuickTime savers are out of scope (no movie decode/audio in this host). Stub
      // the whole library: movie-open calls report an error so the module falls back to
      // its non-movie path; everything else is a benign noErr/no-op.
      if(nm=="QuickTimeLib:OpenMovieFile"||nm=="QuickTimeLib:NewMovieFromFile"||
         nm=="QuickTimeLib:LoadMovieIntoRam")
        ret((uint32_t)(int32_t)-43); // fnfErr / no movie
      else if(nm=="QuickTimeLib:IsMovieDone") ret(1); // "done" -> stop waiting
      else ret(0);
    } else if(nm=="InterfaceLib:GetCTable"){
      // GetCTable(short ctID) -> CTabHandle. Real Toolbox loads the 'clut' resource ctID
      // and returns a fresh handle to its ColorTable {ctSeed(4),ctFlags(2),ctSize(2),
      // (ctSize+1) x ColorSpec{value(2),r(2),g(2),b(2)}}. Points of View calls this once
      // per frame, ROTATES the returned entries, reinstalls them via SetEntries (which the
      // host reflects into g_clut -> the composited frame), then DisposeHandle()s it: a
      // classic palette-cycle animation, so a null return leaves every frame identical.
      // Serve a FRESH handle each call (the module mutates then disposes it). If the
      // module/library ships 'clut' ctID, build from it; else
      // (POV requests ctID 127, a System table we don't have) return a copy of the current
      // 256-entry color environment (g_clut) so the rotate animates the on-screen palette.
      int16_t ctID=(int16_t)r.r[3].u;
      uint32_t body=0, bodysz=0;
      int fi = H.find_file(0x636C7574 /*'clut'*/, ctID);
      if(fi>=0){
        try { auto res=H.res_files[fi].get_resource(0x636C7574, ctID);
          const string& d=res->data; bodysz=(uint32_t)d.size();
          body=mem->allocate(bodysz?bodysz:1);
          if(bodysz) mem->memcpy(body, d.data(), bodysz);
        } catch(...) { body=0; bodysz=0; }
      }
      if(!body){ ADSTUB("STUB:GetCTable_fallback_g_clut_copy"); // requested ctID has no 'clut' resource
        bodysz = 8u+256u*8u; body=mem->allocate(bodysz);
        mem->memcpy(body, H.g_clut, bodysz); }   // copy current color environment
      uint32_t h=mem->allocate(4); mem->write_u32b(h, body);
      H.block_size[h]=4; H.block_size[body]=bodysz;
      ret(h);
    } else if(nm=="InterfaceLib:GetForeColor"){
      // GetForeColor(RGBColor* result): report the current port's foreground color. The
      // host tracks fg as a palette index (qd_fg); reflect it back as RGB from g_clut.
      uint32_t cp=r.r[3].u;
      if(cp){ uint32_t e=H.g_clut+8+(uint32_t)H.qd_fg*8;
        mem->write_u16b(cp+0, mem->read_u16b(e+2));
        mem->write_u16b(cp+2, mem->read_u16b(e+4));
        mem->write_u16b(cp+4, mem->read_u16b(e+6)); }
    } else if(nm=="InterfaceLib:LMSetRndSeed"){
      // LMSetRndSeed(long seed): set the low-mem RndSeed the Toolbox Random() reads. Wire
      // it to Host.rnd_seed so a module that seeds its RNG gets a matching sequence.
      H.rnd_seed = r.r[3].u; if(H.rnd_seed==0) H.rnd_seed=1;
      mem->write_u32b(0x908, r.r[3].u);   // faithful low-mem poke (RndSeed @ $0908)
    } else if(nm=="InterfaceLib:AddSearch"){
      // Color Manager AddSearch(SearchProcPtr): register a color-search proc. The host maps
      // the 8-bit fb through g_clut directly and never invokes module search procs, so this
      // is a faithful no-op registration (Psycho Deli adds/deletes one per frame).
      ADSTUB(string("STUB:")+nm);  // registered but never invoked (host maps via g_clut)
      if(r.r[3].u) H.color_searches.push_back(r.r[3].u);
    } else if(nm=="InterfaceLib:DelSearch"){
      // DelSearch(SearchProcPtr): unregister. Manual find+erase (no <algorithm> dependency).
      ADSTUB(string("STUB:")+nm);
      for(size_t i=0;i<H.color_searches.size();i++)
        if(H.color_searches[i]==r.r[3].u){ H.color_searches.erase(H.color_searches.begin()+i); break; }
    } else if(nm=="InterfaceLib:ExitToShell"){
      // The Metrowerks CFM __start stub calls ExitToShell to "exit the app" (the runtime's
      // "if loaded as an application, quit" path — for a shared-lib fragment the host drives,
      // that code must NOT run). Treat it as a clean stop of the CURRENT emulated call only by
      // throwing terminate_emulation — exactly what the RET_SENT hook does at a normal top-
      // level return, so execute() unwinds and the caller (call_tvec/call_main) returns. The
      // host then continues its own orchestration (init + frame loop + capture). Does NOT
      // terminate the host process. (Redirecting PC to RET_SENT is wrong here: the OS-dispatch
      // sets pc mid-step so the next instruction FETCH targets the unmapped RET_SENT address
      // and faults with out_of_range, which call_tvec rethrows -> abort.)
      throw EmulatorBase::terminate_emulation();
    } else {
      handled=false;
      ADSTUB(string("UNIMPL:")+nm);  // named-but-unimplemented import faked as ret(0)/noErr
    }
    if(H.verbose && (!handled || H.call_count<400 || getenv("ADOSLOG"))){
      fprintf(stderr,"  [%4llu] %-40s r3=%08X r4=%08X r5=%08X lr=%08X%s\n",
              (unsigned long long)H.call_count, nm.c_str(),
              r.r[3].u,r.r[4].u,r.r[5].u,r.lr, handled?"":"  <-- UNIMPLEMENTED");
    }
    if(!handled) ret(0);        // default: return 0/noErr, keep going
    if(redirect_pc){ r.r[2].u = redirect_toc; r.pc = redirect_pc; } // enter dispatched UPP
    else r.pc = r.lr;           // return to caller glue
  };

  // Live snapshot of an 8-bit buffer -> color PPM via the CLUT (call any time,
  // e.g. mid-animation before the module frees its offscreen buffers). One bulk
  // index read (row by row; rowBytes may exceed W) + a 256-entry CLUT LUT + one
  // buffered fwrite.
  auto snap_ppm_to=[&](FILE* f, uint32_t base, int W, int Hh, int rowBytes)->long{
    fprintf(f,"P6\n%d %d\n255\n",W,Hh);
    static std::vector<uint8_t> sidx, srgb; static uint8_t slut[256*3];
    size_t n=(size_t)W*Hh; sidx.resize(n); srgb.resize(n*3);
    for(int y=0;y<Hh;y++){ uint8_t* row=sidx.data()+(size_t)y*W;
      try{ mem->memcpy(row, base+(uint32_t)y*rowBytes, (size_t)W); }
      catch(...){ for(int x=0;x<W;x++){ uint8_t v=0; try{v=mem->read_u8(base+(uint32_t)y*rowBytes+x);}catch(...){} row[x]=v; } } }
    for(int i=0;i<256;i++){ uint32_t e=H.g_clut+8+(uint32_t)i*8;
      slut[i*3]=(uint8_t)(mem->read_u16b(e+2)>>8); slut[i*3+1]=(uint8_t)(mem->read_u16b(e+4)>>8); slut[i*3+2]=(uint8_t)(mem->read_u16b(e+6)>>8); }
    long nz=0;
    for(size_t i=0;i<n;i++){ uint8_t v=sidx[i]; if(v)nz++; const uint8_t* c=&slut[(size_t)v*3];
      srgb[i*3]=c[0]; srgb[i*3+1]=c[1]; srgb[i*3+2]=c[2]; }
    fwrite(srgb.data(),1,n*3,f);
    fflush(f);
    return nz;
  };
  // ADSTREAMIDX=1: private "P8" indexed frame — "P8\n<w> <h>\n" + 768-byte RGB
  // palette (256 CLUT entries) + w*h raw index bytes. Minimal host CPU; the app
  // reconstructs colour via an indexed CGImage with no per-pixel work.
  auto snap_p8_to=[&](FILE* f, uint32_t base, int W, int Hh, int rowBytes){
    fprintf(f,"P8\n%d %d\n",W,Hh);
    static std::vector<uint8_t> pidx; static uint8_t plut[256*3];
    for(int i=0;i<256;i++){ uint32_t e=H.g_clut+8+(uint32_t)i*8;
      plut[i*3]=(uint8_t)(mem->read_u16b(e+2)>>8); plut[i*3+1]=(uint8_t)(mem->read_u16b(e+4)>>8); plut[i*3+2]=(uint8_t)(mem->read_u16b(e+6)>>8); }
    fwrite(plut,1,256*3,f);
    size_t n=(size_t)W*Hh; pidx.resize(n);
    for(int y=0;y<Hh;y++){ uint8_t* row=pidx.data()+(size_t)y*W;
      try{ mem->memcpy(row, base+(uint32_t)y*rowBytes, (size_t)W); }
      catch(...){ for(int x=0;x<W;x++){ uint8_t v=0; try{v=mem->read_u8(base+(uint32_t)y*rowBytes+x);}catch(...){} row[x]=v; } } }
    fwrite(pidx.data(),1,n,f);
    fflush(f);
  };
  auto snap_ppm=[&](const string& path, uint32_t base, int W, int Hh, int rowBytes){
    if(W<=0||Hh<=0||!base) return;
    FILE* f=fopen(path.c_str(),"wb"); if(!f) return;
    long nz=snap_ppm_to(f,base,W,Hh,rowBytes);
    fclose(f);
    fprintf(stderr,"[adhost]   snap %s (%ld nz of %d)\n",path.c_str(),nz,W*Hh);
  };
  // ADSTREAM=1: stream concatenated P6 frames over stdout (no filesystem writes; the
  // modern-app preview reads the pipe). stdout is dup'ed to a private stream and fd 1
  // pointed at /dev/null so stray prints can't corrupt frames. Pipe backpressure
  // throttles rendering to the consumer's pace; a closed pipe kills the host (SIGPIPE)
  // so an orphaned host cleans itself up by construction.
  // Declared here (ahead of the GM loop) so the wait_go lambda below can reference it.
  uint32_t g_ctrl_addr = 0;
  // ---- ZERO-COPY SHARED-MEMORY TRANSPORT (ADSHM + ADSHMNAME) ------------------
  // Strict request-response lockstep (see the 68K host for the full protocol note).
  // The app owns/sizes the shm object [64B header {'ADSM',w,h,seq,format} + 768B
  // palette + w*h index bytes]; we mmap it, block on "GO\n" per frame (SET applied
  // inline while blocked), bulk-write palette+indices, bump seq, and write one 'F'
  // ack byte on a private dup of stdout. No GO => host idles in read() at ~0% CPU.
  uint8_t* g_shm=nullptr; uint8_t* g_shm_pal=nullptr; uint8_t* g_shm_idx=nullptr;
  size_t g_shm_sz=0; FILE* g_ackf=nullptr; std::string g_gobuf;
  const char* g_shmname = getenv("ADSHM") ? getenv("ADSHMNAME") : nullptr;
  if(g_shmname && g_shmname[0] && !isatty(0)){
    size_t idxsz=(size_t)H.fb_w*H.fb_h; g_shm_sz=64+768+idxsz;
    int mfd=shm_open(g_shmname, O_RDWR, 0600);
    if(mfd>=0){
      void* p=mmap(nullptr,g_shm_sz,PROT_READ|PROT_WRITE,MAP_SHARED,mfd,0); close(mfd);
      if(p!=MAP_FAILED){
        g_shm=(uint8_t*)p; g_shm_pal=g_shm+64; g_shm_idx=g_shm+64+768;
        memcpy(g_shm,"ADSM",4);
        *(uint32_t*)(g_shm+4)=(uint32_t)H.fb_w; *(uint32_t*)(g_shm+8)=(uint32_t)H.fb_h;
        *(uint32_t*)(g_shm+12)=0; *(uint32_t*)(g_shm+16)=8;   // seq=0, format=8(indexed)
        // FD_CLOEXEC so a cycle re-exec does not leak this descriptor. The restore below
        // dup2()s it onto fd 1, and dup2 clears CLOEXEC on the copy, so the transport
        // survives execv exactly once — as fd 1 — instead of once per generation.
        int afd=dup(1); if(afd>=0){ fcntl(afd,F_SETFD,FD_CLOEXEC); g_ackf=fdopen(afd,"wb"); freopen("/dev/null","w",stdout); }
      }
    }
    if(!g_shm || !g_ackf) fprintf(stderr,"[adhost] ADSHM setup failed for '%s' (errno=%d %s) — using stdout stream\n",g_shmname,errno,strerror(errno));
  }
  const bool g_shm_on = (g_shm!=nullptr && g_ackf!=nullptr);
  if(g_shm_on) fprintf(stderr,"[adhost] ADSHM active: %s (%dx%d, %zu bytes)\n",g_shmname,H.fb_w,H.fb_h,g_shm_sz);
  FILE* streamf=nullptr;
  if(getenv("ADSTREAM") && !g_shm_on){
    // The frame stream is throttled by the CONSUMER's pipe. Pointed at a REGULAR FILE nothing
    // throttles it — the host writes frames as fast as it renders them (~1.4 GB/s) and fills the
    // disk in about a minute. Refuse that and name the fix; ADSTREAMFORCE=1 overrides.
    // Pipes/sockets/ttys (the app's path — EmulatedHost.swift sets p.standardOutput = pipe) are
    // untouched, and ADSHM never reaches this branch.
    struct stat sst;
    bool isreg = (fstat(1,&sst)==0) && S_ISREG(sst.st_mode);
    if(isreg && !getenv("ADSTREAMFORCE")){
      fprintf(stderr,"[adhost] ADSTREAM REFUSED: stdout is a regular file. This stream has no "
                     "backpressure and writes ~1.4 GB/s (a 65 GB file in a minute). Pipe it to a "
                     "consumer instead, or set ADSTREAMFORCE=1 to write the file anyway.\n");
    } else {
      if(isreg) fprintf(stderr,"[adhost] ADSTREAMFORCE: streaming to a regular file with NO "
                               "backpressure (~1.4 GB/s) — watch your disk.\n");
      int sfd=dup(1); if(sfd>=0){ fcntl(sfd,F_SETFD,FD_CLOEXEC); streamf=fdopen(sfd,"wb"); freopen("/dev/null","w",stdout); }   // see the ack dup above
    }
  }
  bool stream_idx = getenv("ADSTREAMIDX")!=nullptr;
  // Snapshot palette+indices into the shm slot (zero-copy) + one 'F' ack byte.
  uint32_t g_shm_seq=0;
  auto shm_present=[&](){
    for(int i=0;i<256;i++){ uint32_t e=H.g_clut+8+(uint32_t)i*8;
      g_shm_pal[i*3]=(uint8_t)(mem->read_u16b(e+2)>>8); g_shm_pal[i*3+1]=(uint8_t)(mem->read_u16b(e+4)>>8); g_shm_pal[i*3+2]=(uint8_t)(mem->read_u16b(e+6)>>8); }
    if(getenv("ADRAWIDX")){ for(int i=0;i<256;i++){ uint8_t v=(uint8_t)i; g_shm_pal[i*3]=v;g_shm_pal[i*3+1]=v;g_shm_pal[i*3+2]=v; } }
    for(int y=0;y<H.fb_h;y++){ uint8_t* row=g_shm_idx+(size_t)y*H.fb_w;
      try{ mem->memcpy(row, H.g_fb+(uint32_t)y*H.fb_w, (size_t)H.fb_w); }
      catch(...){ for(int x=0;x<H.fb_w;x++){ uint8_t v=0; try{v=mem->read_u8(H.g_fb+(uint32_t)y*H.fb_w+x);}catch(...){} row[x]=v; } } }
    __sync_synchronize();
    *(uint32_t*)(g_shm+12)=++g_shm_seq;
    fputc('F', g_ackf); fflush(g_ackf);
  };
  auto stream_frame=[&](){
    if(g_shm_on){ shm_present(); return; }
    if(!streamf) return;
    if(stream_idx) snap_p8_to(streamf, H.g_fb, H.fb_w, H.fb_h, H.fb_w);
    else           snap_ppm_to(streamf, H.g_fb, H.fb_w, H.fb_h, H.fb_w); };
  // BANNER OVERLAY (present-time): paint the captured banner DIRECTLY onto H.g_fb for the first
  // g_banner_nframes frames (saving the underlying rect), then restore after present so g_fb / the sprite bg
  // are never polluted. Direct blit (not qd_char) so it hits the screen regardless of the module's cur_port.
  // The exact fun-fact duration is an AUTHORED DEFAULT, not RE'd: it is absent from every disk image (it lived
  // in the fullscreen-blanker engine this host replaces), and ~3s matches the please-wait feel. The real clear
  // is DESTRUCTIVE overdraw — the library does no save-under — which is what this overlay model reproduces.
  static int g_banner_nframes = getenv("ADBANNERFRAMES") ? atoi(getenv("ADBANNERFRAMES"))
      : (int)((getenv("ADBANNERSECS")?atof(getenv("ADBANNERSECS")):3.0) * (getenv("ADBANNERFPS")?atof(getenv("ADBANNERFPS")):15.0) + 0.5);
  struct BSave{ bool painted=false; int x0=0,y0=0,x1=0,y1=0; std::vector<uint8_t> px; };
  auto banner_paint=[&](int fN)->BSave{ BSave s;
    if(!g_banner_have || getenv("ADNOBANNEROVL") || fN>=g_banner_nframes) return s;
    int len=(int)g_banner_text.size(); s.x0=g_banner_x-1; s.y0=g_banner_y-8; s.x1=g_banner_x+6*len+2; s.y1=g_banner_y+2;
    if(s.x0<0)s.x0=0; if(s.y0<0)s.y0=0; if(s.x1>H.fb_w)s.x1=H.fb_w; if(s.y1>H.fb_h)s.y1=H.fb_h;
    if(s.x1<=s.x0||s.y1<=s.y0) return s;
    s.px.reserve((size_t)(s.x1-s.x0)*(s.y1-s.y0));
    for(int yy=s.y0;yy<s.y1;yy++)for(int xx=s.x0;xx<s.x1;xx++){ uint8_t v=0; try{v=mem->read_u8(H.g_fb+(uint32_t)(yy*H.fb_w+xx));}catch(...){} s.px.push_back(v); }
    int px=g_banner_x; for(unsigned char c: g_banner_text){ if(c>=0x20&&c<=0x7F){ const uint8_t* g=QDFONT[c-0x20];
      for(int row=0;row<8;row++){ uint8_t bits=g[row]; int yy=g_banner_y-7+row; if(yy<0||yy>=H.fb_h) continue;
        for(int col=0;col<8;col++) if(bits&(1<<col)){ int xx=px+col; if(xx>=0&&xx<H.fb_w) try{ mem->write_u8(H.g_fb+(uint32_t)(yy*H.fb_w+xx), g_banner_fg); }catch(...){} } } }
      px+=6; }
    s.painted=true; return s; };
  auto banner_restore=[&](BSave& s){ if(!s.painted) return; size_t idx=0;
    for(int yy=s.y0;yy<s.y1;yy++)for(int xx=s.x0;xx<s.x1;xx++){ try{ mem->write_u8(H.g_fb+(uint32_t)(yy*H.fb_w+xx), s.px[idx]); }catch(...){} idx++; } };
  // Block until the app sends "GO\n"; apply any SET lines that arrive meanwhile (writes
  // the same GetControlValue slot ADCTRL seeds, once g_ctrl_addr is populated — it stays
  // 0 during the GM path, which therefore takes no live SETs).
  // Returns false on EOF (app gone) so the frame loop exits cleanly.
  auto wait_go=[&]()->bool{
    for(;;){
      size_t nl;
      while((nl=g_gobuf.find('\n'))!=std::string::npos){
        std::string line=g_gobuf.substr(0,nl); g_gobuf.erase(0,nl+1);
        if(line=="GO") return true;
        int idx,val;
        if(sscanf(line.c_str(),"SET %d %d",&idx,&val)==2 && idx>=0 && idx<16 && g_ctrl_addr){
          try{ mem->write_u16b(g_ctrl_addr + (uint32_t)idx*2, (uint16_t)val); }catch(...){}
          if(getenv("ADSETLOG")) fprintf(stderr,"[adhost] live SET ctrl[%d]=%d\n",idx,val);
        } else ad_input_line(line.c_str());   // KEY/CAPS/MOUSE
      }
      char tmp[512]; ssize_t rn=read(0,tmp,sizeof(tmp));
      if(rn<=0) return false;
      g_gobuf.append(tmp,(size_t)rn);
    }
  };
  // Dump g_fb + every live GWorld buffer with a tag.
  auto snap_all=[&](const string& tag){
    if(const char* p=getenv("ADFBDUMP")){
      snap_ppm(string(p)+"."+tag+".fb.ppm", H.g_fb, H.fb_w, H.fb_h, H.fb_w);
      for(size_t i=0;i<H.gworlds.size();i++){
        uint32_t port=H.gworlds[i], pmh=mem->read_u32b(port+2), pm=mem->read_u32b(pmh);
        uint32_t base=mem->read_u32b(pm+0); int rb=mem->read_u16b(pm+4)&0x3FFF;
        int t=(int16_t)mem->read_u16b(pm+6),l=(int16_t)mem->read_u16b(pm+8),
            b=(int16_t)mem->read_u16b(pm+10),rr=(int16_t)mem->read_u16b(pm+12);
        snap_ppm(string(p)+"."+tag+".gw"+to_string(i)+".ppm", base, rr-l, b-t, rb);
      }
    }
  };

  // Watchpoints inside the module to trace the init handshake.
  const uint32_t W_INIT   = BASE_M + 0x121D4;  // one-time init
  const uint32_t W_SETJMP = BASE_M + 0x12A08;  // setjmp
  const uint32_t W_LONGJMP= BASE_M + 0x12A7C;  // longjmp(jmpbuf,val)
  const uint32_t W_VERCHK = BASE_M + 0x12290;  // 'ADdl' version check
  const uint32_t W_ADDL   = BASE_M + 0x12258;  // just before 'ADdl' lookup call
  bool trace_path = getenv("ADTRACE")!=nullptr;
  // Cache every per-instruction debug/trace env ONCE. getenv() is a linear scan of the
  // environment; evaluating ~30 of them PER emulated instruction dominated runtime (slow
  // color-table INITs timed out, esp. at 640x480). When no trace env is set, ANY_TRACE is
  // false and the whole debug region is skipped, leaving only the essential screen-fix +
  // import-sentinel dispatch. This makes emulation several times faster.
  const bool ANY_TRACE =
    trace_path || trace_engine ||
    getenv("ADRET")||getenv("ADWATCHPC")||getenv("ADFRAMEWATCH")||getenv("ADADDLWATCH")||
    getenv("ADERR")||getenv("ADSPAWN")||getenv("ADDRAWTRACE")||getenv("ADVTBL")||getenv("ADDF")||
    getenv("ADDRAWFNS")||getenv("ADB1F8")||getenv("ADWATCHADDR")||getenv("ADWATCH25")||
    getenv("ADDUMPLIST")||getenv("ADR30")||getenv("ADSNAPBLIT")||getenv("ADPCSAMPLE")||
    getenv("ADWILDPC")||getenv("ADFBDUMP")||getenv("ADFORCEFRAME")||getenv("ADTALLY")||
    getenv("ADSNAPRAW")||getenv("ADRAWADDR")||getenv("ADRAWLEN")||getenv("ADSNAPW")||getenv("ADSNAPH");
  // RD UserInput execution-evidence counters. Captured ONCE, not folded into ANY_TRACE
  // (which pays a ~30-getenv/instruction tax), so counting runs at near-full speed.
  const bool RDLOG = getenv("ADRDLOG")!=nullptr;
  // Hoisted like RDLOG/ANY_TRACE: this tracer is read per emulated instruction, and
  // getenv is a linear __findenv scan that dominates execute() (1839/2005 samples on
  // Swirling Magic's 70-pixie swirl). Env is set once at launch — live controls arrive
  // on the stdin SET channel, not env — so a hoisted read is equivalent.
  const bool TFGATELOG = getenv("ADTFGATELOG")!=nullptr;
  // fctiwz correctness lives in the bundled PPC32Emulator, not in a host shim: it carries
  // the saturation fix (upstream PR #95) plus a NaN-saturation fix (static_cast<int32_t>(NaN)
  // is UB and yields 0 on arm64, where real PPC gives 0x80000000). A host-side shim would
  // have to re-fetch the opcode per instruction, ~170ns x every instruction (~32% of Psycho
  // Deli's frame time).
  // ---- CAPS-LOCK live options toggle (PPC) ------------------------------------
  // Every AD4 PPC module imports PortableModule::GetCapsLockChange() from adxpl510
  // and calls it each frame; a returned 1 (caps toggled since last call) drives the
  // module's live option change (Time Flies -> next clock face, Fish World -> fish
  // scatter, etc.). The library's GetCapsLockChange (lib+0xEA8C) reads the live
  // caps state as bit 0x20000 of *(libTOC-0x39D0)[0] (a heap modifier-state object),
  // edge-detects it against a cached prev at libTOC+0x390, and returns the change.
  // Under this host's BLANK/DRAW drive the library's own event pump that would set
  // that bit is never reached (the shell input loop is not run), so the bit is
  // permanently 0 and caps does nothing. So mirror the app-supplied g_caps LEVEL into
  // that bit at the instant the function runs (right before its first read), and the
  // edge-detector fires exactly once per caps transition — the real key-toggle cadence.
  // Default ON; ADNOCAPS opts out. Inert without CAPS input: g_caps stays false, so the
  // mirror only ever clears a bit that is already 0.
  static const uint32_t kCapsFnPC  = BASE_A + 0xEA8C;   // GetCapsLockChange entry
  static const uint32_t kCapsGlOff = 0x39D0;            // libTOC - this = &capsObj ptr
  static const uint32_t kCapsBit   = 0x00020000;        // caps-state bit in capsObj[0]
  const bool CAPS_OFF = getenv("ADNOCAPS")!=nullptr;
  const bool CAPSLOG  = getenv("ADCAPSLOG")!=nullptr;
  emu.set_debug_hook([&](PPC32Emulator& e){
    auto& r = e.registers();
    uint32_t pc = r.pc;
    // ---- serve the caps state into GetCapsLockChange's source object -------------
    // A single compare per instruction. Only acts at the library's GetCapsLockChange
    // entry, and only mutates one bit of one heap word.
    if(!CAPS_OFF && pc==kCapsFnPC){
      // ADTESTCAPSAFTER=N (test lever): deterministically flip g_caps after N reads of
      // GetCapsLockChange — a frame-synced caps edge with no wall-clock/stdin confound,
      // for clean A/B. Default-off; never active in normal runs.
      if(const char* ta=getenv("ADTESTCAPSAFTER")){ static long ec=0; static bool done=false;
        long thr=atol(ta); if(!done && ++ec>=thr){ g_caps=!g_caps; done=true;
          if(CAPSLOG) fprintf(stderr,"    [caps] TESTCAPSAFTER edge at entry %ld -> g_caps=%d\n",ec,g_caps?1:0); } }
      uint32_t tocp = r.r[2].u;               // r2 = libTOC at the fn's own entry
      uint32_t objp = 0;
      try { objp = mem->read_u32b(tocp - kCapsGlOff); } catch(...) { objp = 0; }
      if(objp){
        try {
          uint32_t w = mem->read_u32b(objp);
          uint32_t nw = g_caps ? (w | kCapsBit) : (w & ~kCapsBit);
          if(nw != w) mem->write_u32b(objp, nw);
          if(CAPSLOG){ static std::map<uint32_t,long> callers; callers[r.lr]++;
            static long every=0; if((++every % 200)==1 || nw!=w){
              fprintf(stderr,"    [caps] entry#%ld lr=%08X g_caps=%d word=%08X%s callers=%zu\n",
                      every, r.lr, g_caps?1:0, w, nw!=w?" EDGE":"", callers.size());
              for(auto&kv:callers) fprintf(stderr,"        caller lr=%08X x%ld\n",kv.first,kv.second); } }
        } catch(...) {}
      } else if(CAPSLOG){ static long n=0; if(n++<8)
          fprintf(stderr,"    [caps] GetCapsLockChange: caps obj ptr NULL (toc=%08X)\n", tocp); }
    }
    // ---- Time Flies clock-TYPE-swap gate tracer (ADTFGATELOG) --------------------
    // fnB @module+0x2680 is the caps-driven clock-TYPE swap. It reads the Type
    // control into obj[0x0A]/obj[0x48], then GATES the caps poll on obj[0x16]:
    //   0x26E0  lwz r0,[r28+0x16]; cmpwi 0; bne 0x271C   (obj[0x16]!=0 -> skip caps)
    //   0x26EC  bl GetCapsLockChange; on edge toggle obj[0x48] (0<->1)
    //   0x2738 -> load resset "type 0" ;  0x2754 -> load resset "type 1"
    // obj[0x16] = (GetControlValue(3)!=0) set at module+0x155C. Log the gate + path.
    if(TFGATELOG){
      switch(pc){
        case 0x300026E0: { uint32_t o=r.r[28].u,v=0; try{v=mem->read_u32b(o+0x16);}catch(...){}
          static long n=0; if(n++<6||v==0) fprintf(stderr,"    [tfgate] obj=%08X obj[0x16]=%08X gate=%s\n",
            o,v,v==0?"OPEN(caps-swap enabled)":"CLOSED(caps-swap skipped)"); } break;
        case 0x300026EC: { static long n=0; if(n++<6) fprintf(stderr,"    [tfgate] caps poll REACHED (obj[0x16]==0)\n"); } break;
        case 0x3000270C: { static long n=0; if(n++<20) fprintf(stderr,"    [tfgate] CAPS EDGE -> obj[0x48]=1 (type flip)\n"); } break;
        case 0x30002718: { static long n=0; if(n++<20) fprintf(stderr,"    [tfgate] CAPS EDGE -> obj[0x48]=0 (type flip)\n"); } break;
        case 0x30002738: { static long n=0; if(n++<6) fprintf(stderr,"    [tfgate] resset TYPE0 (r2+532/0x214)\n"); } break;
        case 0x30002754: { static long n=0; if(n++<6) fprintf(stderr,"    [tfgate] resset TYPE1 (r2+468/0x1D4)\n"); } break;
      }
    }
    // ---- UserInput execution evidence (ADRDLOG; NOT under ANY_TRACE) -------------
    // Count entries to the library UserInput methods (fixed addrs at BASE_A) and log
    // the VALUE that GetLastKey/GetNextKey return (store to *r3) and KeyPressed's r3.
    // This proves whether the module reaches the input reads and what they hand back.
    if(RDLOG){
      static long cAdd=0,cLast=0,cNext=0,cPres=0,cDown=0,cDo=0,cKM=0;
      switch(pc){
        case 0x1002D8B0: cAdd++;  break;                    // AddKey entry (module-side call)
        case 0x1002D86C: { cLast++;                          // GetLastKey entry
            static long n=0; if(n++<10) fprintf(stderr,"    [rdcaller] GetLastKey called from lr=%08X (module+%X)\n",
              r.lr, r.lr-0x30000000); } break;
        case 0x1002D814: cNext++; break;                    // GetNextKey entry
        case 0x1002D69C: cPres++; break;                    // KeyPressed entry
        case 0x1002D6D4: cDown++; break;                    // KeyDownNow entry
        case 0x1002D4FC: cDo++;   break;                    // Do entry
        case 0x1002D894:                                    // GetLastKey store (write==0 branch)
        case 0x1002D8A8: { static long n=0; if(n++<40)      // GetLastKey store (write!=0 branch)
            fprintf(stderr,"    [rdread] GetLastKey -> %08X\n", r.r[0].u); } break;
        case 0x1002D864: { static long n=0; if(n++<40)      // GetNextKey: stw r0,0(r3)
            fprintf(stderr,"    [rdread] GetNextKey -> %08X\n", r.r[0].u); } break;
        case 0x1002D6D0: { static long n=0; if(n++<40)      // KeyPressed: blr (r3=result)
            fprintf(stderr,"    [rdread] KeyPressed -> %d\n", (int)r.r[3].u); } break;
        // ---- module-side movement/state reachability (BASE_M=0x30000000) ----
        case 0x300094A0: { cKM++;                            // KeyToMove translator entry
            if(cKM<=20) fprintf(stderr,"    [rdmove] KeyToMove(key=%08X hi=%02X) from lr=%08X (module+%X)\n",
              r.r[3].u, (r.r[3].u>>16)&0xFF, r.lr, r.lr-0x30000000); } break;
        case 0x30006F98: { static long t=0; if(t++<8)       // 0x53 'TEXT' info-screen block
            fprintf(stderr,"    [rdstate] entered 0x53 TEXT block (module+6F98)\n"); } break;
        case 0x30004124: { static long h=0; if(h<12){ h++;  // MoveHandler entry (mode-gated KeyToMove)
            uint32_t flag=0; try{ flag=mem->read_u32b(r.r[2].u+0x3EA4); }catch(...){}
            fprintf(stderr,"    [rdmove] MoveHandler entry, TOC+0x3EA4(demo/player flag)=%08X\n",flag); } } break;
        case 0x30004134: { static long d=0; if(d++<6)       // demo branch (move by stored dir)
            fprintf(stderr,"    [rdmove] MoveHandler DEMO branch (flag==0)\n"); } break;
        case 0x30004148: { static long pl=0; if(pl++<6)     // player branch (reads keys->KeyToMove)
            fprintf(stderr,"    [rdmove] MoveHandler PLAYER branch (flag!=0)\n"); } break;
        case 0x30006F74:                                    // state <- 0x3EC (quit/timeout)
        case 0x3000715C:                                    // state <- 0x3ED (collision-die)
        case 0x3000717C: { static long s=0; if(s++<20)      // state <- 0x3EE (collision-win)
            fprintf(stderr,"    [rdstate] PlayThing state write r0=%X @module+%X\n",
              r.r[0].u, pc-0x30000000); } break;
      }
      static long every=0;
      if((++every % 20000000)==0)
        fprintf(stderr,"    [rdstat] AddKey=%ld GetLast=%ld GetNext=%ld KeyPressed=%ld KeyDownNow=%ld Do=%ld KeyToMove=%ld\n",
                cAdd,cLast,cNext,cPres,cDown,cDo,cKM);
    }
    if(ANY_TRACE){
    if(trace_path){
      if(pc==W_INIT)    fprintf(stderr,"  >> init 0x121D4  r3=%08X\n",r.r[3].u);
      else if(pc==W_ADDL) fprintf(stderr,"  >> pre-ADdl 0x12258 r5=%08X ([r5]=%08X)\n",r.r[5].u, r.r[5].u?mem->read_u32b(r.r[5].u):0);
      else if(pc==W_SETJMP) fprintf(stderr,"  >> setjmp   from lr=%08X jb=%08X\n",r.lr,r.r[3].u);
      else if(pc==W_LONGJMP)fprintf(stderr,"  >> LONGJMP  from lr=%08X jb=%08X val=%d\n",r.lr,r.r[3].u,(int)r.r[4].u);
      else if(pc==W_VERCHK) fprintf(stderr,"  >> verchk 0x12290 r3=%08X ver=%d\n",r.r[3].u, r.r[3].u?(int16_t)mem->read_u16b(r.r[3].u):-1);
    }
    if(trace_engine){
      auto it=ad_fn_name.find(pc);
      if(it!=ad_fn_name.end()){
        // print C-string arg for dprintf/ErrorMessage-ish calls
        string extra;
        if(it->second=="dprintf" && r.r[3].u){ char b[160]; int i=0; for(;i<159;i++){ uint8_t c=mem->read_u8(r.r[3].u+i); if(!c)break; b[i]=(char)c;} b[i]=0; extra=string("  \"")+b+"\""; }
        fprintf(stderr,"    ENGINE @%08X %-42s r3=%08X r4=%08X r5=%08X%s\n",pc,it->second.c_str(),r.r[3].u,r.r[4].u,r.r[5].u,extra.c_str());
      }
    }
    if(getenv("ADRET")){
      static unordered_map<uint32_t,pair<string,uint32_t>>* pend=new unordered_map<uint32_t,pair<string,uint32_t>>();
      auto it=ad_fn_name.find(pc);
      if(it!=ad_fn_name.end() && (it->second.find("CalcMem")!=string::npos ||
          it->second.find("GetDepth")!=string::npos)){
        (*pend)[r.lr]={it->second, pc};
      }
      auto pit=pend->find(pc);
      if(pit!=pend->end()){
        fprintf(stderr,"      RET %-40s -> r3=%08X\n", pit->second.first.c_str(), r.r[3].u);
        pend->erase(pit);
      }
    }
    if(pc==0x10010D38) fprintf(stderr,"    >>> IXHeap called from lr=%08X size=%08X\n", r.lr, r.r[4].u);
    // ADWATCHPC=hexaddr : log r3/r4/lr whenever the module PC hits that address
    // (used to catch longjmp(jb, err) and see who aborted INIT + with what code).
    if(const char* wz=getenv("ADWATCHPC")){
      static uint32_t wpc=(uint32_t)strtoul(wz,0,16);
      if(pc==wpc){ fprintf(stderr,"    >>> WATCH pc=%08X r3=%08X r4=%08X(%d) lr=%08X (mod+%X)\n",
                          pc, r.r[3].u, r.r[4].u, (int)r.r[4].u, r.lr, r.lr-BASE_M);
        if(getenv("ADWATCHREGS")) fprintf(stderr,"        r20=%08X r23=%08X r24=%08X r25=%08X r29=%08X r30=%08X r31=%08X\n",
                          r.r[20].u,r.r[23].u,r.r[24].u,r.r[25].u,r.r[29].u,r.r[30].u,r.r[31].u); }
    }
    // ADADDLWATCH: log every D-form load that reads the 'ADdl' Deluxe blob, so we can
    // see which fields the failing (0x801) modules require beyond the version short.
    if(getenv("ADADDLWATCH") && H.g_addl){
      uint32_t op=mem->read_u32b(pc); uint32_t prim=(op>>26)&0x3F;
      if(prim==32||prim==40||prim==34||prim==42||prim==33||prim==41||prim==35||prim==43){ // lwz/lhz/lbz/lha(+u)
        uint8_t ra=(op>>16)&0x1F; int32_t imm=(int16_t)(op&0xFFFF);
        uint32_t ea=(ra?r.r[ra].u:0)+imm;
        if(ea>=H.g_addl && ea<H.g_addl+0x400){
          const char* nm=(prim&0xFE)==32?"lwz":(prim&0xFE)==40?"lhz":(prim&0xFE)==34?"lbz":"lha";
          fprintf(stderr,"    [ADdl] %s +0x%X = %08X  (pc=%08X module+%X)\n",
                  nm, ea-H.g_addl, mem->read_u32b(ea), pc, pc-BASE_M); }
      }
    }
    // ADFRAMEWATCH: log D-form loads that read the engine "frame"/faceplate blob, so we
    // can see which frame fields gate the draw path (title->animation state machine).
    if(getenv("ADFRAMEWATCH") && H.g_frame){
      uint32_t op=mem->read_u32b(pc); uint32_t prim=(op>>26)&0x3F;
      if(prim==32||prim==40||prim==34||prim==42||prim==33||prim==41||prim==35||prim==43){
        uint8_t ra=(op>>16)&0x1F; int32_t imm=(int16_t)(op&0xFFFF);
        uint32_t ea=(ra?r.r[ra].u:0)+imm;
        if(ea>=H.g_frame && ea<H.g_frame+0x400){
          const char* nm=(prim&0xFE)==32?"lwz":(prim&0xFE)==40?"lhz":(prim&0xFE)==34?"lbz":"lha";
          static int n=0; if(n++<200) fprintf(stderr,"    [frame] %s +0x%X = %08X  (pc=%08X %s+%X)\n",
                  nm, ea-H.g_frame, mem->read_u32b(ea), pc,
                  (pc>=BASE_M&&pc<BASE_M+0x60000)?"module":"engine",
                  (pc>=BASE_M&&pc<BASE_M+0x60000)?pc-BASE_M:pc-BASE_A); }
      }
    }
    // ErrorMessage(code): log the module caller so we can locate 0x801 version checks.
    if(pc==0x1000E8F0 && getenv("ADERR")) fprintf(stderr,"    >>> ErrorMessage(0x%X) from lr=%08X (module+%X)\n",
                               r.r[3].u, r.lr, r.lr-BASE_M);
    // Capture the module's virtual DrawFrame method address (vtable[0x0C]),
    // loaded into r12 by the sel-2 stub just before its trampoline call.
    if(getenv("ADSPAWN")){
      // FT DrawFrame spawn logic @module 0xBF28. Trace the time vs spawn-timer.
      if(pc==0x3000BF8C) fprintf(stderr,"    [spawn] enter BF28 TOC+3CC=%02X TOC+3D0=%08X\n",
                                 mem->read_u8(r.r[2].u+0x3CC), mem->read_u32b(r.r[2].u+0x3D0));
      if(pc==0x3000BF94) fprintf(stderr,"    [spawn] 12D84 ret r3=%08X (extsh)\n",r.r[3].u);
      if(pc==0x30012D88){ uint32_t tv=r.r[12].u,code=mem->read_u32b(tv);
        auto it2=ad_fn_name.find(code);
        fprintf(stderr,"    [spawn] 12D84->TOC+AC tvec=%08X code=%08X %s\n",tv,code,
                it2!=ad_fn_name.end()?it2->second.c_str():"?"); }
      if(pc==0x30012DA0){ static int n=0; if(n++<3){ uint32_t tv=r.r[12].u,code=mem->read_u32b(tv);
        auto it2=ad_fn_name.find(code);
        fprintf(stderr,"    [spawn] 12D9C->TOC+7C code=%08X %s\n",code,
                it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
      if(pc==0x30012D58){ static int n=0; if(n++<3){ uint32_t tv=r.r[12].u,code=mem->read_u32b(tv);
        auto it2=ad_fn_name.find(code);
        fprintf(stderr,"    [spawn] 12D54->TOC+50 code=%08X %s\n",code,
                it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
      if(pc==0x3000BFA4) fprintf(stderr,"    [spawn] 12D54 time r3=%08X  timer[3D0]=%08X (+400=%08X)\n",
                                 r.r[3].u, mem->read_u32b(r.r[2].u+0x3D0), mem->read_u32b(r.r[2].u+0x3D0)+400);
      if(pc==0x3000C000||pc==0x3000C014) fprintf(stderr,"    [spawn] *** SPAWN fired @%08X obj+26=%08X\n",pc,r.r[3].u);
      if(pc==0x3000C0EC) fprintf(stderr,"    [spawn] reached ADD-sprite block: obj+4A=%08X obj+46=%08X\n",mem->read_u32b(r.r[25].u+0x4A),mem->read_u32b(r.r[25].u+0x46));
      if(pc==0x3000C0F4) fprintf(stderr,"    [spawn] >>> calling AddSprite 0xAD38(r3=%08X r4=%08X)\n",r.r[3].u,r.r[4].u);
      if(pc==0x3000C104) fprintf(stderr,"    [spawn] >>> calling AddSprite 0xCE10(r3=%08X r4=%08X)\n",r.r[3].u,r.r[4].u);
      if(pc==0x3000C0AC) fprintf(stderr,"    [spawn] at C0AC r26=%08X (beq skips spawn if r26&FFFF==0)\n",r.r[26].u);
      // IsValidFrame__CompoundSequence entry (r3=seq, r4=frame). Log a few
      // returns via the blr to see whether sprites ever have a valid frame.
      if(pc==0x1001E5BC){ static int n=0; if(n++<6) fprintf(stderr,"    [spawn] IsValidFrame(seq=%08X frame=%d) ...\n",r.r[3].u,(int)r.r[4].u); }
      if(pc==0x3000B31C){ static int n=0; if(n++<4) fprintf(stderr,"    [grp] B2F8 group=%08X calling method[0x70]\n",r.r[3].u); }
      if(pc==0x3000B324){ static int n=0; if(n++<4) fprintf(stderr,"    [grp] method[0x70] returned r31=%08X %s\n",r.r[3].u, r.r[3].u?"(nonzero -> draws)":"(ZERO -> SKIPS draw!)"); }
      if(pc==0x3000B358){ static int n=0; if(n++<2){ uint32_t code=mem->read_u32b(r.r[12].u); auto it2=ad_fn_name.find(code);
        fprintf(stderr,"    [grp] method[0x60] -> %08X %s\n",code,it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
      for(uint32_t sp : {0x30012C98u,0x30012D10u,0x30012D28u}){ if(pc==sp){ static int seen=0; if(seen<3){seen++;
        uint32_t code=mem->read_u32b(r.r[12].u); auto it2=ad_fn_name.find(code);
        fprintf(stderr,"    [spawn] DrawFrame-call @%08X -> %08X %s (r3=%08X)\n",sp,code,it2!=ad_fn_name.end()?it2->second.c_str():"?",r.r[3].u);}}}
      if(pc==0x1000E048){ static int n=0; if(n++<8){
        uint32_t g=mem->read_u32b(r.r[2].u+0x3A8);
        fprintf(stderr,"    [spawn] GetControlValue -> r3=%08X (ctrlArray[*TOC+3A8]=%08X)\n",r.r[3].u,g); } }
    }
    if(getenv("ADDRAWTRACE")){
      // Who calls HideSprite / UpdateSpriteMovement? Find the containing loop fn.
      if(pc==0x10022DB8){ static int n=0; if(n++<4) fprintf(stderr,"    [dt] HideSprite(sprite=%08X) lr=%08X\n",r.r[3].u,r.lr); }
      if(pc==0x100205DC){ static int n=0; if(n++<4) fprintf(stderr,"    [dt] UpdateSpriteMovement(cs=%08X) lr=%08X\n",r.r[3].u,r.lr); }
      // Are the real draw functions ever reached?
      if(pc==0x100025C0){ static int n=0; if(n++<4) fprintf(stderr,"    [dt] *** MaybeDraw__Sprite(sprite=%08X canvas=%08X) lr=%08X\n",r.r[3].u,r.r[4].u,r.lr); }
      if(pc==0x100020F0){ static int n=0; if(n++<6){ uint32_t cv=r.r[6].u, base=0;
        if(cv){ uint32_t px=mem->read_u32b(cv+0x08); if(px) base=mem->read_u32b(px); }
        fprintf(stderr,"    [dt] *** DrawFrame__CompoundSequence(seq=%08X frame=%d canvas=%08X pixBase=%08X) lr=%08X\n",r.r[3].u,(int)(int16_t)r.r[4].u,cv,base,r.lr); } }
      // EXPERIMENT: force a valid frame so the draw gate ([sprite+0x44]>0) passes.
      if(pc==0x100202CC && getenv("ADFORCEFRAME") && (int16_t)r.r[4].u==0)
        r.r[4].u = (uint32_t)atoi(getenv("ADFORCEFRAME"));
      // MoveThroughRelativeFrame entry: tally distinct sprites + valid-frame count.
      if(pc==0x100202CC && getenv("ADTALLY")){ static uint32_t lo=~0u,hi=0; static int valid=0,zero=0,tot=0;
        uint32_t sp2=r.r[3].u; int16_t frame=(int16_t)mem->read_u16b(sp2+0x44);
        if(sp2<lo)lo=sp2; if(sp2>hi)hi=sp2; tot++; if(frame>0)valid++; else zero++;
        if(tot%333==0) fprintf(stderr,"    [tally] MTRF calls=%d spriteRange=%08X..%08X validFrame=%d zeroFrame=%d\n",tot,lo,hi,valid,zero); }
      // At first sel-2 stub entry (post-INIT), dump frames of the sprites that got sequences queued.
      if(pc==0x30012474){ static bool once=false; if(!once){ once=true;
        for(uint32_t sp2 : {0x3F43FCu,0x3F447Au,0x3F44F8u,0x69275Cu}){
          fprintf(stderr,"    [post-init] sprite=%08X seq[+40]=%08X frame[+44]=%d sub[+4E]=%d q[+50]=%d q[+54]=%d q[+56]=%d\n",
            sp2,mem->read_u32b(sp2+0x40),(int16_t)mem->read_u16b(sp2+0x44),(int16_t)mem->read_u16b(sp2+0x4E),
            (int16_t)mem->read_u16b(sp2+0x50),(int16_t)mem->read_u16b(sp2+0x54),(int16_t)mem->read_u16b(sp2+0x56)); } } }
      if(pc==0x1001A190){ static int n=0; if(n++<4){ uint32_t sp2=r.r[3].u, seq=mem->read_u32b(sp2+0x40);
        uint32_t svt=seq?mem->read_u32b(seq):0, m20=svt?mem->read_u32b(mem->read_u32b(svt+0x20)):0;
        auto it2=ad_fn_name.find(m20);
        fprintf(stderr,"    [DRAW] MoveSpriteAndFrame(sprite=%08X frame=%d) seq[+40]=%08X seq.vt+20=%08X %s\n",
          sp2,(int)(int16_t)r.r[4].u,seq,m20,it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
      if(pc==0x1001A970){ static int n=0; if(n++<4){ uint32_t bg=r.r[3].u;
        fprintf(stderr,"    [mix] MixToSaveBackCanvas bg=%08X [+0x16]=%08X [+0x12]=%08X %s\n",bg,
          mem->read_u32b(bg+0x16),mem->read_u32b(bg+0x12),
          (mem->read_u32b(bg+0x16)&&mem->read_u32b(bg+0x12))?"-> DRAWS":"-> SKIPS"); } }
      if(pc==0x100022C0){ static int n=0; if(n++<6) fprintf(stderr,"    [Q] QueueSequence(sprite=%08X seq=%d) lr=%08X\n",r.r[3].u,(int)(int16_t)r.r[4].u,r.lr); }
      if(pc==0x10002120||pc==0x10002128){ static int n=0; if(n++<6) fprintf(stderr,"    [Q] AddSequencesToQueue(sprite=%08X) @%08X lr=%08X\n",r.r[3].u,pc,r.lr); }
      if(pc==0x10002130){ static int n=0; if(n++<6) fprintf(stderr,"    [Q] AddSequenceToQueue(sprite=%08X seq=%d) lr=%08X\n",r.r[3].u,(int)(int16_t)r.r[4].u,r.lr); }
      if(pc==0x1001F174){ static int n=0; if(n++<4) fprintf(stderr,"    [nss] NextSubSequence(sprite=%08X r4=%d) lr=%08X\n",r.r[3].u,(int)(int16_t)r.r[4].u,r.lr); }
      if(pc==0x1001EE34){ int16_t a=(int16_t)r.r[4].u; if(a!=0){ static int n=0; if(n++<8) fprintf(stderr,"    [ns] NextSequence(sprite=%08X ARG=%d) lr=%08X\n",r.r[3].u,a,r.lr);} }
      // AddSprite__Background(bg, newSprite=r4, after, rect): dump sprite anim state + FT caller.
      if(pc==0x10002018){ static int n=0; if(n++<8){ uint32_t sp2=r.r[4].u;
        fprintf(stderr,"    [add] AddSprite newSprite=%08X seq[+40]=%08X frame[+44]=%d sub[+4E]=%d q[+50]=%d lr=%08X\n",
          sp2, sp2?mem->read_u32b(sp2+0x40):0, sp2?(int16_t)mem->read_u16b(sp2+0x44):0,
          sp2?(int16_t)mem->read_u16b(sp2+0x4E):0, sp2?(int16_t)mem->read_u16b(sp2+0x50):0, r.lr); } }
      // UpdateSpriteMovement (0x205DC): resolve the draw vtable slots 0x74/0x160/0x34.
      if(pc==0x100205DC){ static bool once=false; if(!once){ once=true; uint32_t vt=mem->read_u32b(r.r[3].u);
        fprintf(stderr,"    [usm] sprite=%08X frame[+0x44]=%d\n",r.r[3].u,(int16_t)mem->read_u16b(r.r[3].u+0x44));
        for(uint32_t off : {0x34u,0x74u,0x160u}){ uint32_t tv=mem->read_u32b(vt+off),code=tv?mem->read_u32b(tv):0;
          auto it2=ad_fn_name.find(code); fprintf(stderr,"    [usm]   vtable+0x%03X -> %08X %s\n",off,code,it2!=ad_fn_name.end()?it2->second.c_str():"?"); } } }
      // GetSequenceFirstFrame entry (0x20A5C): dump sub-sequence state + resolve seq.vtable+0x94.
      if(pc==0x10020A5C){ static int n=0; if(n++<6){ uint32_t sp2=r.r[3].u;
        int16_t subseq=(int16_t)mem->read_u16b(sp2+0x4E); uint32_t seq=mem->read_u32b(sp2+0x40);
        uint32_t svt=seq?mem->read_u32b(seq):0, m94=svt?mem->read_u32b(mem->read_u32b(svt+0x94)):0;
        auto it2=ad_fn_name.find(m94);
        fprintf(stderr,"    [dt] GetSeqFirstFrame sprite=%08X arg r4=%d sub[+0x4E]=%d seq=%08X seq.vt+94=%s\n",
                sp2,(int)(int16_t)r.r[4].u,subseq,seq,it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
      // vtable trampoline: resolve the method dispatched from the sprite-loop fn.
      if(pc==0x1003EB70 && r.lr>=0x10020300 && r.lr<=0x10020660){
        static int n=0; if(n++<40){ uint32_t code=mem->read_u32b(r.r[12].u); auto it2=ad_fn_name.find(code);
          fprintf(stderr,"    [dt] loop vcall lr=%08X -> %08X %s\n",r.lr,code,it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
    }
    if(getenv("ADVTBL") && pc==0x30012480){
      uint32_t tv=r.r[12].u, code=mem->read_u32b(tv), toc=mem->read_u32b(tv+4);
      fprintf(stderr,"    >>> sel2 DrawFrame TVector=%08X code=%08X toc=%08X (code off=%X)\n",
              tv, code, toc, code-BASE_M);
    }
    // DrawFrame structure probe: log module fields that gate the sub-draws, and
    // count entries to the candidate composite fns fn0xA890 / fn0xCFF8 / fn0xB2F8.
    if(getenv("ADDF")){
      if(pc==0x3000BE40){ uint32_t m=r.r[3].u; static int n=0; if(n++<6)
        fprintf(stderr,"  [DF] enter mod=%08X +1E=%08X +22=%08X +26=%08X +2A=%08X\n",
          m,mem->read_u32b(m+0x1E),mem->read_u32b(m+0x22),mem->read_u32b(m+0x26),mem->read_u32b(m+0x2A)); }
      if(pc==0x3000A890){ static int n=0; if(n++<8) fprintf(stderr,"  [DF] -> fn0xA890(obj=%08X) lr=%08X\n",r.r[3].u,r.lr); }
      if(pc==0x3000CFF8){ static int n=0; if(n++<8) fprintf(stderr,"  [DF] -> fn0xCFF8(obj=%08X) lr=%08X\n",r.r[3].u,r.lr); }
      if(pc==0x3000B2F8){ static int n=0; if(n++<8) fprintf(stderr,"  [DF] -> fn0xB2F8(grp=%08X) lr=%08X\n",r.r[3].u,r.lr); }
      if(pc==0x3000CE10){ static int n=0; if(n++<8) fprintf(stderr,"  [DF] -> fn0xCE10(obj=%08X r4=%08X) lr=%08X\n",r.r[3].u,r.r[4].u,r.lr); }
      // fn0xCFF8 vcalls: 0xD028 = [vt+0x84], 0xD104 = [vt+0x20]
      if(pc==0x3000D028||pc==0x3000D104){ static int n=0; if(n++<16){
        uint32_t tv=r.r[12].u, code=tv?mem->read_u32b(tv):0; auto it2=ad_fn_name.find(code);
        fprintf(stderr,"    [CFF8] @%05X vcall obj=%08X r4=%08X -> %08X %s\n",
          pc-BASE_M,r.r[3].u,r.r[4].u,code,it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
    }
    // Trace entry to any engine draw/sprite fn by name (via ad_fn_name code map).
    if(getenv("ADDRAWFNS")){
      auto it=ad_fn_name.find(pc);
      if(it!=ad_fn_name.end()){
        const string& nm=it->second;
        if(nm.find("Draw")!=string::npos||nm.find("AddSprite")!=string::npos||
           nm.find("MixTo")!=string::npos||nm.find("Blit")!=string::npos||
           nm.find("MaybeDraw")!=string::npos){
          static int n=0; if(n++<100)
            fprintf(stderr,"  [drawfn] %-42s r3=%08X r4=%08X r5=%08X r6=%08X lr=%08X\n",
              nm.c_str(),r.r[3].u,r.r[4].u,r.r[5].u,r.r[6].u,r.lr);
        }
      }
    }
    // Trace the per-sprite draw fn0xB1F8 (module 0x3000B1F8): entry count + the
    // polymorphic draw vcalls it dispatches ([vt+0x1C]@B288, [vt+0x74]@B2A4,
    // [vt+0x5C]@B2D0). At each, r12 = method TVector -> resolve engine fn + args.
    if(getenv("ADB1F8")){
      if(pc==0x3000B1F8){ static int n=0; if(n++<12)
        fprintf(stderr,"  [B1F8] enter obj=%08X r4=%08X r5=%08X lr=%08X\n",r.r[3].u,r.r[4].u,r.r[5].u,r.lr); }
      if(pc==0x3000B288||pc==0x3000B2A4||pc==0x3000B2D0){
        static int n=0; if(n++<40){ uint32_t tv=r.r[12].u, code=tv?mem->read_u32b(tv):0;
          auto it2=ad_fn_name.find(code);
          fprintf(stderr,"    [B1F8] @%05X vcall obj=%08X r4=%08X -> %08X %s\n",
            pc-BASE_M,r.r[3].u,r.r[4].u,code,it2!=ad_fn_name.end()?it2->second.c_str():"?"); } }
    }
    } // end ANY_TRACE (debug region 1)
    // IMacScreenPixels(GrafPort*, const XR* bounds, XCanvas*) copies the bounds
    // rect (r5) into the screen-pixels object. Our host never wires the real
    // screen size into that rect, so force it to the framebuffer bounds so the
    // whole canvas/flying-area sizing downstream is correct. ESSENTIAL — always runs.
    // Substitute the seed the module hands adxpl510's SRand. The module reads it from
    // XTimer::GetMilliseconds(), which our virtual clock pins to a constant at INIT;
    // this hands it the value a real machine's uptime would have given. Inert unless
    // ADLIBRNGSEED is set.
    if(g_librng_armed && pc==g_librng_srand_pc){
      static long n=0;
      fprintf(stderr,"[rngseed] SRand entry #%ld: seed %08X -> %08X\n",++n,r.r[3].u,g_librng_seed);
      r.r[3].u = g_librng_seed;
    }
    if(pc==0x1000E2E4 && !getenv("ADNOSCREENFIX")){
      // GetMonitorInfo__PortableModule convergence point: r29 = the output XR
      // pointer, bounds fully computed. Our emulated Mac has no real GrayRgn /
      // GDevice list, so the engine computes a degenerate 0x0 monitor rect.
      // Overwrite it with the true framebuffer extent at the source, so all
      // downstream canvas sizing (IScreenCanvas, IMacScreenPixels, offscreen
      // GWorlds) is correct. XR layout is {left, top, right, bottom} (16-bit).
      uint32_t rp=r.r[29].u;
      if(rp){
        mem->write_u16b(rp+0,0); mem->write_u16b(rp+2,0);
        mem->write_u16b(rp+4,(uint16_t)H.fb_w); mem->write_u16b(rp+6,(uint16_t)H.fb_h);
        if(getenv("ADENGINE")) fprintf(stderr,"    [host] GetMonitorInfo XR -> l0,t0,r%d,b%d\n",H.fb_w,H.fb_h);
      }
    }
    if(ANY_TRACE){ // debug region 2
    if(const char* wa=getenv("ADWATCHADDR")){
      static uint32_t addr=strtoul(wa,0,0); static uint32_t last=0xDEADBEEF;
      uint32_t v=0; try{v=mem->read_u32b(addr);}catch(...){}
      if(v!=last){ fprintf(stderr,"    [%08X] %08X -> %08X at pc=%08X(%s+%05X)\n",addr,last,v,pc,
                   (pc>=BASE_A&&pc<BASE_A+0x50000)?"adx":(pc>=BASE_M?"mod":"?"),
                   (pc>=BASE_A&&pc<BASE_A+0x50000)?pc-BASE_A:(pc>=BASE_M?pc-BASE_M:pc)); last=v; }
    }
    if(getenv("ADWATCH25")){
      static uint32_t last=0xFFFFFFFF;
      static bool inwin=false;
      if(pc==0x100085B8) inwin=true;
      if(pc==0x100086B8) inwin=false; // LoadNoiseIDs blr
      if(inwin && r.r[25].u!=last){
        fprintf(stderr,"    r25 %08X -> %08X at pc=%08X(%s+%05X)\n",last,r.r[25].u,pc,
                (pc>=BASE_A&&pc<BASE_A+0x50000)?"adx":(pc>=BASE_M?"mod":"?"),
                (pc>=BASE_A&&pc<BASE_A+0x50000)?pc-BASE_A:(pc>=BASE_M?pc-BASE_M:pc));
        last=r.r[25].u;
      }
    }
    if(getenv("ADDUMPLIST") && pc==0x10008640){ // lhax r4,[r25+r31] in LoadNoiseIDs loop
      fprintf(stderr,"    >>> @8640 r25=%08X r31=%08X r27=%08X read=%04X\n",
              r.r[25].u, r.r[31].u, r.r[27].u, mem->read_u16b(r.r[25].u + r.r[31].u));
    }
    if(getenv("ADDUMPLIST") && pc==0x100085B8){ // LoadNoiseIDs entry: dump list r4, count r5
      uint32_t lp=r.r[4].u, cnt=r.r[5].u; fprintf(stderr,"    >>> LoadNoiseIDs list=%08X cnt=%u : ",lp,cnt);
      for(uint32_t i=0;i<cnt+3 && i<20;i++) fprintf(stderr,"%04X ", mem->read_u16b(lp+i*2));
      fprintf(stderr,"\n");
    }
    if(pc==0x10007FDC) fprintf(stderr,"    >>> @7FDC r25=%08X [r25]=%08X r26=%08X r30=%08X\n",
        r.r[25].u, r.r[25].u?mem->read_u32b(r.r[25].u):0, r.r[26].u, r.r[30].u);
    if(getenv("ADR30")){
      static uint32_t last_r30=0;
      if(r.r[30].u != last_r30){
        fprintf(stderr,"    r30: %08X -> %08X  at pc=%08X (%s+%06X)\n",last_r30,r.r[30].u,pc,
                (pc>=BASE_A&&pc<BASE_A+0x50000)?"adxpl510":(pc>=BASE_M?"module":"?"),
                (pc>=BASE_A&&pc<BASE_A+0x50000)?pc-BASE_A:(pc>=BASE_M?pc-BASE_M:pc));
        last_r30=r.r[30].u;
      }
    }
    if(pc==0x10000004 && getenv("ADSNAPBLIT")){ // rleNewBlit: snapshot every Nth
      static long n=0; long every=atol(getenv("ADSNAPBLIT"));
      if(every>0 && (n % every)==0){
        snap_all("blit"+to_string(n));
        // rleNewBlit(r3=RLE, r4=dstBuf, r5=...): dump the dest buffer raw.
        if(getenv("ADSNAPRAW")){ int W=atoi(getenv("ADSNAPW")?:"512"), Hh=atoi(getenv("ADSNAPH")?:"384");
          snap_ppm(string(getenv("ADFBDUMP"))+".blit"+to_string(n)+".dst.ppm", r.r[4].u, W, Hh, W); }
        fprintf(stderr,"    rleNewBlit r3=%08X r4=%08X r5=%08X\n",r.r[3].u,r.r[4].u,r.r[5].u);
        fprintf(stderr,"      r4[0..48]:");
        for(int k=0;k<48;k+=4) fprintf(stderr," %08X",mem->read_u32b(r.r[4].u+k));
        if(const char* rd=getenv("ADRAWADDR")){
          uint32_t a=strtoul(rd,0,0), len=strtoul(getenv("ADRAWLEN")?:"0x80000",0,0);
          FILE* rf=fopen((string(getenv("ADFBDUMP"))+".raw").c_str(),"wb");
          if(rf){ for(uint32_t k=0;k<len;k++){ uint8_t b=0; try{b=mem->read_u8(a+k);}catch(...){b=0;} fputc(b,rf);} fclose(rf);
                  fprintf(stderr,"      raw dump %08X..+%X -> .raw\n",a,len); }
        }
        fprintf(stderr,"\n      screenPixels@30013F14[0..60]:");
        for(int k=0;k<0x40;k+=4) fprintf(stderr," %08X",mem->read_u32b(0x30013F14+k));
        fprintf(stderr,"\n      screenPort@30017BBC pixmap:");
        { uint32_t pmh=mem->read_u32b(0x30017BBC+2), pm=mem->read_u32b(pmh);
          fprintf(stderr," pmh=%08X pm=%08X base=%08X rb=%04X",pmh,pm,mem->read_u32b(pm),mem->read_u16b(pm+4)); }
        fprintf(stderr,"\n");
        // Dump every live GWorld's pixmap by reading its port directly.
        for(size_t i=0;i<H.gworlds.size();i++){
          uint32_t port=H.gworlds[i], pmh=mem->read_u32b(port+2), pm=pmh?mem->read_u32b(pmh):0;
          if(!pm) continue;
          uint32_t base=mem->read_u32b(pm+0); int rb=mem->read_u16b(pm+4)&0x3FFF;
          int t=(int16_t)mem->read_u16b(pm+6),l=(int16_t)mem->read_u16b(pm+8),
              b=(int16_t)mem->read_u16b(pm+10),rr=(int16_t)mem->read_u16b(pm+12);
          fprintf(stderr,"      gw%zu port=%08X base=%08X rb=%d bounds=%d,%d,%d,%d\n",
                  i,port,base,rb,t,l,b,rr);
          snap_ppm(string(getenv("ADFBDUMP"))+".blit"+to_string(n)+".gw"+to_string(i)+".ppm",
                   base, rr-l, b-t, rb);
        }
      }
      n++;
    }
    if(getenv("ADPCSAMPLE")){
      static uint64_t n=0; static uint32_t lo=0xFFFFFFFF, hi=0;
      if(pc<lo)lo=pc; if(pc>hi)hi=pc;
      if(++n % 2000000ULL == 0){ fprintf(stderr,"    [PCSAMPLE] n=%lluM pc=%08X lr=%08X range=[%08X,%08X]\n",(unsigned long long)(n/1000000),pc,r.lr,lo,hi); lo=0xFFFFFFFF; hi=0; }
    }
    if(getenv("ADWILDPC")){
      // Log any PC outside known code regions (engine, module, sentinels, RET_SENT).
      bool ok = (pc>=BASE_A && pc<BASE_A+0x40000) || (pc>=BASE_M && pc<BASE_M+0x60000)
             || (pc>=SENT_BASE && pc<SENT_BASE+0x100000) || pc==RET_SENT;
      if(!ok){ static int n=0; if(n++<8) fprintf(stderr,"    [WILDPC] pc=%08X lr=%08X r2=%08X r12=%08X\n",pc,r.lr,r.r[2].u,r.r[12].u); }
    }
    } // end ANY_TRACE (debug region 2)
    // ESSENTIAL — always runs: import-sentinel dispatch + emulation termination.
    if(pc==RET_SENT) throw EmulatorBase::terminate_emulation();
    if(pc>=SENT_BASE && pc < SENT_BASE + 0x100000){
      uint32_t idx=(pc-SENT_BASE)/4;
      H.call_count++;
      do_import(e, idx);
      if(H.call_count>H.max_calls){ fprintf(stderr,"[adhost] max calls reached\n"); throw EmulatorBase::terminate_emulation(); }
    }
  });

  // ---- 6. call helper: invoke a TVector with up to 6 gpr args --------------
  auto call_tvec = [&](uint32_t tvec, std::initializer_list<uint32_t> args, const char* label)->uint32_t{
    uint32_t code = mem->read_u32b(tvec), toc = mem->read_u32b(tvec+4);
    auto& rr = emu.registers();
    rr = PPC32Emulator::Regs();
    rr.r[1].u = stack; rr.r[2].u = toc; rr.lr = RET_SENT; rr.pc = code;
    int gi=3; for(uint32_t a: args){ if(gi<11) rr.r[gi].u=a; gi++; }
    fprintf(stderr,"[adhost] call %s (code=%08X toc=%08X)\n",label,code,toc);
    try { emu.execute(); return emu.registers().r[3].u; }
    catch(const exception& ex){
      auto& e=emu.registers();
      fprintf(stderr,"[adhost] %s stopped: %s  pc=%08X lr=%08X faultAddr=%08X r3=%08X r4=%08X\n",
              label,ex.what(),e.pc,e.lr,e.debug.addr,e.r[3].u,e.r[4].u);
      const char* frag=(e.pc>=BASE_A&&e.pc<BASE_A+0x50000)?"adxpl510":(e.pc>=BASE_M&&e.pc<BASE_M+0x20000)?"module":"?";
      uint32_t off=(e.pc>=BASE_A)?e.pc-BASE_A:(e.pc>=BASE_M?e.pc-BASE_M:e.pc);
      fprintf(stderr,"         pc in %s + %08X\n",frag,off);
      throw;
    }
  };

  // call an engine function by raw code address + explicit TOC (for engine C++
  // methods that aren't exported as TVectors, e.g. Validate__10Background).
  auto call_code = [&](uint32_t code, uint32_t toc, std::initializer_list<uint32_t> args, const char* label)->uint32_t{
    auto& rr = emu.registers();
    rr = PPC32Emulator::Regs();
    rr.r[1].u = stack; rr.r[2].u = toc; rr.lr = RET_SENT; rr.pc = code;
    int gi=3; for(uint32_t a: args){ if(gi<11) rr.r[gi].u=a; gi++; }
    fprintf(stderr,"[adhost] call %s (code=%08X toc=%08X args:",label,code,toc);
    for(uint32_t a: args) fprintf(stderr," %08X",a); fprintf(stderr,")\n");
    try { emu.execute(); return emu.registers().r[3].u; }
    catch(const exception& ex){ auto& e=emu.registers();
      fprintf(stderr,"[adhost] %s stopped: %s pc=%08X lr=%08X faultAddr=%08X\n",label,ex.what(),e.pc,e.lr,e.debug.addr);
      return 0xDEADBEEF; }
  };

  // ---- 6a. run adxpl510 static initialization ------------------------------
  // CFM Prepare normally runs the fragment's __start (which runs the C++ static
  // constructors that populate engine global objects — e.g. the sprite-draw
  // callback table at [engineTOC+0x3C8]+0x22/+0x26 that Magic Turtle's per-turtle
  // engine callback loop dispatches through). ADENGSTART calls __start before
  // __initialize so those ctors run; env-gated.
  if(getenv("ADENGSTART")){
    try { uint32_t st = mem->get_symbol_addr("adxpl510:__start");
      call_tvec(st, {}, "adxpl510:__start");
      fprintf(stderr,"[adhost] adxpl510 __start OK (%llu OS calls)\n",(unsigned long long)H.call_count);
    } catch(const exception& ex){ fprintf(stderr,"[adhost] __start failed: %s\n",ex.what()); }
  }
  // Run the engine's C++ static-constructor list DIRECTLY (bypassing __start, whose
  // Metrowerks app-entry path calls ExitToShell and never runs the ctors for a
  // shared-lib fragment). The ctor runner at adxpl510 0x1002FB84 loops i=0..33
  // calling [engineTOC+0x9C4 + i*4], guarded by [engineTOC+0x9C0]. These ctors
  // construct the engine's global objects incl. the sprite-draw callback table at
  // [engineTOC+0x3C8]+0x22/+0x26 that Magic Turtle's per-turtle draw dispatches
  // through. ADENGCTOR=code (default 0x1002FB84) overrides the address.
  if(getenv("ADENGCTOR")){
    uint32_t engtoc = mem->read_u32b(mem->get_symbol_addr("adxpl510:__initialize")+4);
    uint32_t caddr = getenv("ADENGCTORADDR")? (uint32_t)strtoul(getenv("ADENGCTORADDR"),0,16) : 0x1002FB84;
    uint32_t guard = engtoc + 0x9C0;
    uint32_t cbobj = mem->read_u32b(engtoc + 0x3C8);
    fprintf(stderr,"[adhost] pre-ctor: guard[%08X]=%08X cbobj=%08X cb22=%08X\n",
      guard, mem->read_u32b(guard), cbobj, cbobj?mem->read_u32b(cbobj+0x22):0);
    if(getenv("ADENGCTORFORCE")) mem->write_u32b(guard, 0);   // clear "already-ran" guard
    try { call_code(caddr, engtoc, {0}, "adxpl510 static ctors");
      uint32_t cb2 = mem->read_u32b(engtoc + 0x3C8);
      fprintf(stderr,"[adhost] adxpl510 ctors OK (%llu OS calls) post cbobj=%08X cb22=%08X\n",
        (unsigned long long)H.call_count, cb2, cb2?mem->read_u32b(cb2+0x22):0);
    } catch(const exception& ex){ fprintf(stderr,"[adhost] ctors failed: %s\n",ex.what()); }
  }
  try {
    uint32_t init_tvec = mem->get_symbol_addr("adxpl510:__initialize");
    call_tvec(init_tvec, {scratch}, "adxpl510:__initialize");
    fprintf(stderr,"[adhost] adxpl510 __initialize OK (%llu OS calls)\n",(unsigned long long)H.call_count);
  } catch(const exception& ex){ fprintf(stderr,"[adhost] __initialize failed: %s\n",ex.what()); }

  // ---- 6a2. build a real screen canvas: framebuffer + PixMap + CGrafPort ---
  {
    int W=H.fb_w, Hh=H.fb_h;
    H.g_fb   = mem->allocate(W*Hh); mem->memset(H.g_fb,0,W*Hh);
    // clut: 256 CTabEntries. header: ctSeed(4) ctFlags(2) ctSize(2)=255, then
    // entries {value(2),r(2),g(2),b(2)}. Start with a grayscale ramp...
    H.g_clut = mem->allocate(8+256*8); mem->memset(H.g_clut,0,8+256*8);
    mem->write_u16b(H.g_clut+6, 255);
    // Seed the AUTHENTIC Macintosh default 8-bit System palette. Structure = 6x6x6 colour
    // cube (0-214, black excluded) + red/green/blue/gray 10-step ramps (215-254) + black@255;
    // matches resource_dasm default_icon_color_table_8bit (md5 of 768B RGB8 = 7bb7a37a).
    // idx0=WHITE, idx255=BLACK. Module 'clut'/PICT palettes still overlay below, so a module
    // that ships neither keeps this table.
    {
      static const int PV[6]={0xFF,0xCC,0x99,0x66,0x33,0x00};
      static const int PRV[10]={0xEE,0xDD,0xBB,0xAA,0x88,0x77,0x55,0x44,0x22,0x11};
      uint8_t pal[256][3]; int n=0;
      for(int r=0;r<6;r++)for(int g=0;g<6;g++)for(int b=0;b<6;b++){
        if(PV[r]==0&&PV[g]==0&&PV[b]==0) continue;      // black -> index 255
        pal[n][0]=(uint8_t)PV[r]; pal[n][1]=(uint8_t)PV[g]; pal[n][2]=(uint8_t)PV[b]; n++; }
      for(int k=0;k<10;k++){ pal[215+k][0]=(uint8_t)PRV[k]; pal[215+k][1]=0; pal[215+k][2]=0; }
      for(int k=0;k<10;k++){ pal[225+k][0]=0; pal[225+k][1]=(uint8_t)PRV[k]; pal[225+k][2]=0; }
      for(int k=0;k<10;k++){ pal[235+k][0]=0; pal[235+k][1]=0; pal[235+k][2]=(uint8_t)PRV[k]; }
      for(int k=0;k<10;k++){ pal[245+k][0]=(uint8_t)PRV[k]; pal[245+k][1]=(uint8_t)PRV[k]; pal[245+k][2]=(uint8_t)PRV[k]; }
      pal[255][0]=0; pal[255][1]=0; pal[255][2]=0;
      for(int i=0;i<256;i++){ uint32_t e=H.g_clut+8+i*8;
        mem->write_u16b(e,(uint16_t)i);
        mem->write_u16b(e+2,(uint16_t)(pal[i][0]*257));
        mem->write_u16b(e+4,(uint16_t)(pal[i][1]*257));
        mem->write_u16b(e+6,(uint16_t)(pal[i][2]*257)); }
    }
    // ...then overlay the module's real palette from its 'clut' resource. The art
    // is authored against this table; without it every render is grayscale. Each
    // ColorSpec is {value(2),r(2),g(2),b(2)} 16-bit/chan; value = the palette slot.
    if(!H.res_files.empty()){
      const uint32_t CLUT='clut'; // 0x636C7574
      auto ids=H.res_files[0].all_resources_of_type(CLUT);
      if(!ids.empty()){
        int want = getenv("ADCLUTID")? (int)strtol(getenv("ADCLUTID"),0,0) : ids[0];
        bool have=false; for(int id:ids) if(id==want){have=true;break;}
        if(!have) want=ids[0];
        try {
          auto res=H.res_files[0].get_resource(CLUT,want);
          const string& d=res->data;
          if(d.size()>=8){
            uint16_t size=((uint8_t)d[6]<<8)|(uint8_t)d[7]; int n=size+1;
            for(int k=0;k<n;k++){ size_t o=8+k*8; if(o+8>d.size()) break;
              uint16_t val=((uint8_t)d[o]<<8)|(uint8_t)d[o+1];
              if(val>255) continue;
              uint32_t e=H.g_clut+8+val*8;
              mem->write_u16b(e,val);
              mem->write_u16b(e+2, ((uint8_t)d[o+2]<<8)|(uint8_t)d[o+3]);
              mem->write_u16b(e+4, ((uint8_t)d[o+4]<<8)|(uint8_t)d[o+5]);
              mem->write_u16b(e+6, ((uint8_t)d[o+6]<<8)|(uint8_t)d[o+7]); }
            // After Dark blanks the screen to black and treats palette index 0 as
            // the transparent backdrop key for its sprite art (the framebuffer is
            // left at 0 wherever nothing is drawn). The resource often lists slot 0
            // as white; force it black so untouched background renders black like
            // the real screensaver. Override with ADKEEP0=1 for modules that paint
            // a real color into index 0.
            if(!getenv("ADKEEP0")){ uint32_t e=H.g_clut+8;
              mem->write_u16b(e+2,0); mem->write_u16b(e+4,0); mem->write_u16b(e+6,0); }
            fprintf(stderr,"[adhost] loaded module clut id=%d (%d entries) into g_clut\n",want,n);
          }
        } catch(...) { fprintf(stderr,"[adhost] clut id=%d load failed; grayscale\n",want); }
      } else {
        // No 'clut' resource (e.g. Rainforest): the art palette is embedded in the
        // module's indexed 'PICT' resources as a QuickDraw ColorTable. Scan each PICT
        // for a ColorTable — {ctSeed(4),ctFlags(2),ctSize(2), (size+1)x ColorSpec} —
        // identified by its sequential value fields (entry k has value==k). Load the
        // largest one found into g_clut so the composited frame shows real color
        // instead of the grayscale ramp. Gated on ids.empty() -> zero effect on the
        // modules that ship their own 'clut'.
        const uint32_t PICTT='PICT';
        auto pids=H.res_files[0].all_resources_of_type(PICTT);
        int bestN=0, bestOff=-1; string bestData;
        for(int pid : pids){
          try {
            auto res=H.res_files[0].get_resource(PICTT,pid);
            const string& d=res->data; size_t L=d.size();
            auto s16=[&](size_t o)->int{ return (int)(int16_t)(((uint8_t)d[o]<<8)|(uint8_t)d[o+1]); };
            auto u16=[&](size_t o)->int{ return ((uint8_t)d[o]<<8)|(uint8_t)d[o+1]; };
            for(size_t o=0; o+16<=L; o++){
              int size=s16(o+6);
              if(size<8 || size>255) continue;
              if(o+8+(size_t)(size+1)*8 > L) continue;
              bool ok=true;
              for(int k=0;k<=size;k++){ if(u16(o+8+(size_t)k*8)!=k){ ok=false; break; } }
              if(ok && size+1>bestN){ bestN=size+1; bestOff=(int)o; bestData=d; }
            }
          } catch(...) {}
        }
        if(bestOff>=0){
          const string& d=bestData;
          for(int k=0;k<bestN;k++){ size_t o=(size_t)bestOff+8+(size_t)k*8;
            uint32_t e=H.g_clut+8+(uint32_t)k*8;
            mem->write_u16b(e,(uint16_t)k);
            mem->write_u16b(e+2, ((uint8_t)d[o+2]<<8)|(uint8_t)d[o+3]);
            mem->write_u16b(e+4, ((uint8_t)d[o+4]<<8)|(uint8_t)d[o+5]);
            mem->write_u16b(e+6, ((uint8_t)d[o+6]<<8)|(uint8_t)d[o+7]); }
          if(!getenv("ADKEEP0")){ uint32_t e=H.g_clut+8;
            mem->write_u16b(e+2,0); mem->write_u16b(e+4,0); mem->write_u16b(e+6,0); }
          fprintf(stderr,"[adhost] no 'clut'; loaded %d-entry palette from PICT ColorTable into g_clut\n",bestN);
        }
      }
    }
    uint32_t cth=mem->allocate(4); mem->write_u32b(cth,H.g_clut);   // CTabHandle
    // PixMap (50 bytes)
    H.g_pm = mem->allocate(0x32); mem->memset(H.g_pm,0,0x32);
    mem->write_u32b(H.g_pm+0x00, H.g_fb);              // baseAddr
    mem->write_u16b(H.g_pm+0x04, 0x8000 | (uint16_t)W);// rowBytes | pixmap flag
    mem->write_u16b(H.g_pm+0x06, 0); mem->write_u16b(H.g_pm+0x08, 0);   // bounds top,left
    mem->write_u16b(H.g_pm+0x0A, Hh);mem->write_u16b(H.g_pm+0x0C, W);   // bounds bottom,right
    mem->write_u32b(H.g_pm+0x16, 0x00480000);          // hRes 72dpi
    mem->write_u32b(H.g_pm+0x1A, 0x00480000);          // vRes 72dpi
    mem->write_u16b(H.g_pm+0x1E, 0);                   // pixelType chunky
    mem->write_u16b(H.g_pm+0x20, 8);                   // pixelSize
    mem->write_u16b(H.g_pm+0x22, 1);                   // cmpCount
    mem->write_u16b(H.g_pm+0x24, 8);                   // cmpSize
    mem->write_u32b(H.g_pm+0x2A, cth);                 // pmTable
    H.g_pmh = mem->allocate(4); mem->write_u32b(H.g_pmh, H.g_pm);  // PixMapHandle
    // A region {size=10, bbox=0,0,H,W} for visRgn/clipRgn
    auto mkrgn=[&](){ uint32_t r=mem->allocate(10); mem->write_u16b(r,10);
      mem->write_u16b(r+2,0);mem->write_u16b(r+4,0);mem->write_u16b(r+6,Hh);mem->write_u16b(r+8,W);
      uint32_t h=mem->allocate(4); mem->write_u32b(h,r); return h; };
    H.g_visrgn=mkrgn();
    // CGrafPort (256 bytes, generous)
    H.g_port = mem->allocate(0x100); mem->memset(H.g_port,0,0x100);
    mem->write_u32b(H.g_port+0x02, H.g_pmh);           // portPixMap
    mem->write_u16b(H.g_port+0x06, 0xC000);            // portVersion: color port
    mem->write_u16b(H.g_port+0x10, 0); mem->write_u16b(H.g_port+0x12, 0);
    mem->write_u16b(H.g_port+0x14, Hh);mem->write_u16b(H.g_port+0x16, W); // portRect
    mem->write_u32b(H.g_port+0x18, H.g_visrgn);        // visRgn
    mem->write_u32b(H.g_port+0x1C, mkrgn());           // clipRgn
    // GDevice (60 bytes) + GDHandle
    H.g_gdev = mem->allocate(0x3C); mem->memset(H.g_gdev,0,0x3C);
    mem->write_u16b(H.g_gdev+0x04, 0);                 // gdType
    // gdFlags @0x14: the real attribute bits TestDeviceAttribute reads.
    // gdDevType(0)=color, mainScreen(11), screenDevice(13), screenActive(15) set;
    // ramInit(10)/allInit(12)/noDriver(14) clear -> = 0xA801.
    mem->write_u16b(H.g_gdev+0x14, 0xA801);            // gdFlags
    mem->write_u32b(H.g_gdev+0x16, H.g_pmh);           // gdPMap
    mem->write_u16b(H.g_gdev+0x22, 0); mem->write_u16b(H.g_gdev+0x24, 0);
    mem->write_u16b(H.g_gdev+0x26, Hh);mem->write_u16b(H.g_gdev+0x28, W); // gdRect
    H.g_gdh = mem->allocate(4); mem->write_u32b(H.g_gdh, H.g_gdev);
    fprintf(stderr,"[adhost] canvas %dx%d fb=%08X port=%08X pm=%08X gdev=%08X\n",
            W,Hh,H.g_fb,H.g_port,H.g_pm,H.g_gdev);
  }

  // ---- 6b. build a minimal valid engine "frame" object ---------------------
  // Init's tagged-data lookup (adxpl510 0x10EC0) needs: frame[+0x0E] bit 0x4000
  // set, and frame[+0x28] -> table { u16 count; count x {u32 fourcc,u32 value} }.
  // The module requires an 'ADdl' entry whose blob starts with a version short
  // >= 768 (else it longjmps with error 1284). We synthesize a minimal blob and
  // discover the rest of its layout by tracing what init reads.
  // Same overlap guard as the stack: the frame MUST NOT land inside a module image
  // (Magic Turtle's frame landed at 0x3000B43C inside sec0, so the module's own data
  // writes collided with theFrame fields). Relocate clear if needed.
  uint32_t frame = hi_alloc(0x400, 0x50200000); mem->memset(frame,0,0x400);
  uint32_t addl  = hi_alloc(0x400, 0x50300000); mem->memset(addl,0,0x400);
  H.g_addl = addl;   // expose to the debug-hook read-watch (ADADDLWATCH)
  uint16_t addlver = getenv("ADDLVER")? (uint16_t)strtoul(getenv("ADDLVER"),0,0) : 0x0300;
  mem->write_u16b(addl, addlver);                 // 'ADdl' version (default 768)
  // Faceplate-provided per-sprite draw callback UPPs. The engine dispatches each
  // turtle/sprite through [ [engineTOC+0x3C8] + 0x22 ] and [+0x26] (procInfo 0xC0/0x30),
  // where [engineTOC+0x3C8] == this ADdl blob. The real After Dark faceplate pre-fills
  // these with engine draw-proc TVectors; ours are null unless installed, so a module
  // whose drawing goes through them (Magic Turtle) draws nothing. ADDLCB=EngineSymbol
  // installs that export's TVector into both slots (needs ADCUP so CallUniversalProc
  // actually dispatches). ADDLCB22 / ADDLCB26 set them separately.
  {
    auto instcb=[&](uint32_t off,const char* sym){ if(!sym||!*sym) return;
      try{ uint32_t tv=mem->get_symbol_addr((string("adxpl510:")+sym).c_str());
        mem->write_u32b(addl+off, tv);
        fprintf(stderr,"[adhost] ADdl+%02X <- adxpl510:%s tvec=%08X\n",off,sym,tv);
      }catch(const exception& ex){ fprintf(stderr,"[adhost] ADDLCB %s not found: %s\n",sym,ex.what()); } };
    if(getenv("ADDLCB")){ instcb(0x22,getenv("ADDLCB")); instcb(0x26,getenv("ADDLCB")); }
    if(getenv("ADDLCB22")) instcb(0x22,getenv("ADDLCB22"));
    if(getenv("ADDLCB26")) instcb(0x26,getenv("ADDLCB26"));
  }
  uint32_t tags  = hi_alloc(0x100, 0x50400000); mem->memset(tags,0,0x100);
  // Faceplate scratch string buffer that the module expects theFrame+0x1E to point
  // at (Magic Turtle strcpy's a sprite/turtle name into it). Real After Dark's
  // faceplate provides this; our synthetic frame must too.
  uint32_t fscratch = hi_alloc(0x1000, 0x50500000); mem->memset(fscratch,0,0x1000);
  mem->write_u16b(tags, 1);                        // 1 tagged entry
  mem->write_u32b(tags+2, 0x4144646C);             // 'ADdl'
  mem->write_u32b(tags+6, addl);                   // -> blob
  mem->write_u16b(frame+0x0E, 0x4000);             // flags: enable tag lookup
  mem->write_u32b(frame+0x1E, fscratch);           // faceplate scratch string buffer
  mem->write_u32b(frame+0x28, tags);               // -> tag table
  // Controls array at frame+0x08 (same layout the GM path builds at gmpb+0x08).
  // Modules driven through the myModulePPC dispatcher (Points of View, via ADMAINCODE)
  // receive this frame AS the GMParamBlock, and their DoInitialize searches the
  // controls array for a record whose signed type-tag byte at [ctrls + i*10 + 11]
  // is >= 8 (else GetIndString the "use colors" caption + return -1). The record's
  // Rect data (rec+2..rec+9) supplies the offscreen/screen bounds. Modules that ignore
  // +0x08 are unaffected.
  if(!getenv("ADNOFCTRL")) {
    uint32_t fctrls = hi_alloc(0x100, 0x50600000); mem->memset(fctrls,0,0x100);
    int fctrlN = getenv("ADFCTRLN")? atoi(getenv("ADFCTRLN")) : 8;
    mem->write_u16b(fctrls, (uint16_t)fctrlN);       // count
    for(int i=0;i<fctrlN;i++){ uint32_t rec=fctrls + i*10;
      mem->write_u16b(rec+2, 0);                     // top
      mem->write_u16b(rec+4, 0);                     // left
      mem->write_u16b(rec+6, (uint16_t)H.fb_h);      // bottom
      mem->write_u16b(rec+8, (uint16_t)H.fb_w);      // right
      mem->write_u8(rec+11, 8); }                    // type tag = 8
    mem->write_u32b(frame+0x08, fctrls);
  }
  try {
    uint32_t g = mem->get_symbol_addr("adxpl510:theFrame");
    mem->write_u32b(g, frame);
    H.g_frame = frame;
    fprintf(stderr,"[adhost] frame=%08X addl=%08X tags=%08X, theFrame@%08X set\n",frame,addl,tags,g);
  } catch(const exception& ex){ fprintf(stderr,"[adhost] no theFrame symbol: %s\n",ex.what()); }

  // ---- 7. call module main --------------------------------------------------
  // Guess the PortableModule factory convention; the trace tells us the truth.
  uint32_t params = mem->allocate(0x200); mem->memset(params,0,0x200);
  uint32_t evt = mem->allocate(0x100); mem->memset(evt,0,0x100);
  (void)evt;
  // Invoke module main(arg0=params, arg1=0, selector, arg3=frame). INIT (sel 0)
  // constructs the module and stores it in a global; later selectors drive the
  // module's vtable (sel 1=+0x08, 2=+0x0C, 3=+0x10 -> per-frame Draw).
  auto call_main=[&](int selector, const char* label)->uint32_t{
    auto& r = emu.registers();
    r = PPC32Emulator::Regs();
    r.r[1].u = stack; r.r[2].u = main_toc;
    r.r[3].u = params; r.r[4].u = 0; r.r[5].u = (uint32_t)selector; r.r[6].u = frame;
    r.lr = RET_SENT; r.pc = main_code;
    fprintf(stderr,"[adhost] calling module main selector=%d (%s)...\n",selector,label);
    try { emu.execute(); }
    catch(const exception& ex){
      auto& rr = emu.registers();
      fprintf(stderr,"[adhost] stopped: %s  pc=%08X lr=%08X faultAddr=%08X\n",
              ex.what(),rr.pc,rr.lr,rr.debug.addr);
      const char* frag=(rr.pc>=BASE_A&&rr.pc<BASE_A+0x50000)?"adxpl510":
                       (rr.pc>=BASE_M&&rr.pc<BASE_M+0x20000)?"module":"?";
      uint32_t off=(rr.pc>=BASE_A&&rr.pc<BASE_A+0x50000)?rr.pc-BASE_A:
                   (rr.pc>=BASE_M&&rr.pc<BASE_M+0x20000)?rr.pc-BASE_M:rr.pc;
      fprintf(stderr,"         pc in %s + %08X\n",frag,off);
    }
    uint32_t rv=emu.registers().r[3].u;
    fprintf(stderr,"[adhost] main(sel=%d) returned r3=%08X (%llu OS calls total)\n",
            selector,rv,(unsigned long long)H.call_count);
    return rv;
  };
  // ---- control-value seeding, factored so it can run BEFORE the module latches its
  // controls. A module that reads GetControlValue DURING its INIT must see the user's
  // value, not 0. The library control array lives at *(adxplTOC+0x3A8) and is allocated
  // by adxpl510's __initialize (already called at load, well above here), so the pointer
  // is valid before the module's INIT runs. Seeding is idempotent, so the early seed and
  // the retained post-INIT re-seed can both run. ADNOEARLYSEED reverts to post-INIT-only
  // ordering, under which modules that latch at INIT latch 0.
  // Compute the module's REAL control slots (from its sVal/mVal/xVal/bVal fork
  // resources, IDs 1000.., slot = ID-1000 — the same map the app uses) and the
  // desired per-slot values (ADCTRL overrides). Shared by both the pre-INIT frame
  // seed and the post-INIT array (re-)seed below.
  //
  // Those SAME resources carry each control's SHIPPED FACTORY VALUE — one big-endian
  // short in the resource body (sVal=slider 0..100, mVal=popup 1-based, xVal=checkbox
  // 0/1, bVal=radio/palette enum). Default each REAL slot to its OWN factory value;
  // slots with NO *Val resource keep the filler 50. A blanket 50 is a value no faceplate
  // can produce for a popup or a checkbox, so it silently mis-drives those modules.
  // Cross-checked against catalog.json corpus-wide: 230/230 controls agree.
  //
  // FIRST-WINS per slot, in sVal→mVal→xVal→bVal order, for the one module in the
  // corpus whose slot carries two types (Swirling Magic: mVal 1002=9 popup "Palette"
  // AND xVal 1002=1 checkbox "Magnify Pixies", both real, an inherent limit of the
  // shared resource ID). This is the same tiling the 68K host's cvseed and
  // EmulatedHost.swift's ADCTRL builder use, so all three agree on 9.
  //
  // Explicit ADCTRL overrides only the entries actually GIVEN: a short ADCTRL leaves its
  // unspecified tail at the factory values rather than at 50. ADNOVALDEFAULTS=1 restores
  // the filler-50 baseline for every slot.
  int ctrl_defs[16]; for(int i=0;i<16;i++) ctrl_defs[i]=50;
  bool ctrl_valid[16]={false}; int ctrl_found=0;
  const bool no_val_defaults = getenv("ADNOVALDEFAULTS")!=nullptr;
  int val_defaulted=0;
  if(!H.res_files.empty()){
    static const uint32_t kValTypes[4] = {0x7356616C,0x6D56616C,0x7856616C,0x6256616C}; // sVal mVal xVal bVal
    for(uint32_t t : kValTypes){
      try { for(int16_t id : H.res_files[0].all_resources_of_type(t)){
              int slot=(int)id-1000; if(slot<0 || slot>=16) continue;
              const bool first = !ctrl_valid[slot];
              ctrl_valid[slot]=true; ctrl_found++;
              if(!first || no_val_defaults) continue;
              try { const std::string& d = H.res_files[0].get_resource(t,id)->data;
                    if(d.size()>=2){
                      ctrl_defs[slot]=(int)(int16_t)(((uint16_t)(uint8_t)d[0]<<8)|(uint8_t)d[1]);
                      val_defaulted++; } }
              catch(...) {}
            } }
      catch(...) {}
    }
  }
  if(val_defaulted){
    fprintf(stderr,"[adhost] %d control slot(s) defaulted to the module's own factory value:",val_defaulted);
    for(int i=0;i<16;i++) if(ctrl_valid[i]) fprintf(stderr," [%d]=%d",i,ctrl_defs[i]);
    fprintf(stderr,"\n");
  }
  if(const char* cs=getenv("ADCTRL")){ int i=0; const char* p=cs;
    while(*p && i<16){ ctrl_defs[i++]=(int)strtol(p,nullptr,0); const char* c=strchr(p,','); if(!c)break; p=c+1; } }
  auto seed_controls = [&](const char* phase, bool pre_init){
    uint32_t adxpl_tv = mem->get_symbol_addr("adxpl510:__initialize");
    uint32_t adxpl_toc = adxpl_tv ? mem->read_u32b(adxpl_tv + 4) : 0;
    uint32_t ctrl = adxpl_toc ? mem->read_u32b(adxpl_toc + 0x3A8) : 0;
    if(!ctrl){
      // PRE-INIT PATH: the library control array *(adxplTOC+0x3A8) does not exist yet — a
      // STANDARD module allocates it during its OWN INIT and points it at the faceplate
      // "frame" it was handed (the store at module+0x3C writes theFrame's address into
      // adxplTOC+0x3A8; GetControlValue(idx) then reads *(frame + idx*2)). theFrame is
      // built by the host BEFORE INIT, so to make a control that a standard module LATCHES
      // during INIT read the user's value, write the value into theFrame's control shorts
      // here (real control slots only).
      //
      // This frame-overlay is ONLY valid for standard modules. A module driven through the
      // myModulePPC dispatcher (ADMAINCODE, e.g. Points of View) NEVER stores theFrame into
      // adxplTOC+0x3A8, so its `ctrl` stays NULL at EVERY phase — and it reads its controls
      // from the fctrls RECORDS at frame+0x08, using theFrame's low bytes (frame+0..+7) as
      // OTHER header fields, not as a control array. Writing control shorts into frame+0..+6
      // for such a module corrupts that header (Points of View then renders a WHITE
      // background instead of black). So: (a) only ever do the frame-overlay in the PRE-INIT
      // phase, and (b) never for an ADMAINCODE module. ADNOFRAMESEED disables it too.
      bool do_frame_seed = pre_init && H.g_frame && ctrl_found>0
                           && !getenv("ADMAINCODE") && !getenv("ADNOFRAMESEED");
      if(do_frame_seed){
        int wrote=0;
        for(int i=0;i<16;i++) if(ctrl_valid[i]){ mem->write_u16b(H.g_frame + (uint32_t)i*2, (uint16_t)ctrl_defs[i]); wrote++; }
        g_ctrl_addr = H.g_frame;   // GetControlValue will read from here once INIT installs the ptr
        fprintf(stderr,"[adhost] seed_controls(%s): array not yet allocated; wrote %d real control "
                "slot(s) into theFrame@%08X so INIT-time GetControlValue reads user values\n",
                phase,wrote,H.g_frame);
      } else {
        fprintf(stderr,"[adhost] seed_controls(%s): control array ptr @adxplTOC+0x3A8 is NULL, "
                "no theFrame control-overlay (pre_init=%d admaincode=%d) — matches legacy no-op\n",
                phase,(int)pre_init,getenv("ADMAINCODE")?1:0);
      }
      return;
    }
    g_ctrl_addr = ctrl;
    for(int i=0;i<16;i++) mem->write_u16b(ctrl + i*2, (uint16_t)ctrl_defs[i]);
    fprintf(stderr,"[adhost] seeded control values @%08X (%s; defaults=module factory *Val, 50 where "
            "no *Val resource, ADCTRL to override)\n",ctrl,phase);
    // ---- authentic control-array size clamp --------------------------------
    // GetControlValue(idx) is an UNCHECKED flat read of this 16-short array. Real
    // After Dark sized the array to the module's ACTUAL controls and left the rest
    // zero; this host over-provisions all 16 slots (=50 or the app's padded ADCTRL),
    // so a module that reads a control index BEYOND its own controls would get 50 where
    // the engine returns 0 (Time Flies reads GetControlValue(3) to gate its caps-lock
    // clock-type swap). Zero every array slot with no backing control. Opt-out
    // ADNOCTRLZERO.
    if(!getenv("ADNOCTRLZERO") && ctrl_found>0){
      int zeroed=0;
      for(int i=0;i<16;i++) if(!ctrl_valid[i]){ mem->write_u16b(ctrl + i*2, 0); if(ctrl_defs[i]!=0) zeroed++; }
      fprintf(stderr,"[adhost] control-array clamp: %d real control slot(s), "
              "zeroed %d non-control slot(s) (authentic GetControlValue out-of-range=0)\n",ctrl_found,zeroed);
    }
  };
  const bool g_early_seed = !getenv("ADNOEARLYSEED");
  if(getenv("ADFBFILL")){ uint8_t fb=(uint8_t)strtoul(getenv("ADFBFILL"),0,0);
    mem->memset(H.g_fb, fb, H.fb_w*H.fb_h);
    fprintf(stderr,"[adhost] prefilled g_fb with 0x%02X\n",fb); }
  // ---- 7-GM. GraphicsModule ABI (mainSection<0 modules, e.g. Psycho Deli) --------
  // These export DoInitialize/DoBlank taking a GMParamBlock instead of main(selector).
  // Gate on ADGM; TVector offsets in sec1 via ADGMINIT/ADGMBLANK (defaults = Psycho
  // Deli's DoInitialize@sec1+0x304, DoBlank@sec1+0x2F4). See project memory for the
  // GMParamBlock layout RE (bounds Rect@0..6, controls@8, flags@0xE, ctx ptr@0x1E).
  if(getenv("ADGM")){
    // Apply the After-Dark blanked-backdrop convention (CLUT[0]->black) that the
    // NON-GM path already does (§6a2 clut/PICT block, skipped when ADGM): a real Mac
    // blanks the screen to black before a saver runs, so an unpainted index-0 pixel
    // must display black. Psycho Deli's own SetEntries starts at index 1 (never idx0),
    // so its palette is untouched. Opt-out ADKEEP0.
    if(!getenv("ADKEEP0") && H.g_clut){ uint32_t e0=H.g_clut+8;
      mem->write_u16b(e0+2,0); mem->write_u16b(e0+4,0); mem->write_u16b(e0+6,0);
      fprintf(stderr,"[adhost] GM black-blank: forced CLUT index 0 -> black\n"); }
    uint32_t initOff = getenv("ADGMINIT")? (uint32_t)strtoul(getenv("ADGMINIT"),0,16) : 0x304;
    uint32_t blankOff= getenv("ADGMBLANK")?(uint32_t)strtoul(getenv("ADGMBLANK"),0,16): 0x2F4;
    uint32_t startOff= getenv("ADGMSTART")?(uint32_t)strtoul(getenv("ADGMSTART"),0,16): 0x4AC; // __start
    uint32_t frameOff= getenv("ADGMFRAME")?(uint32_t)strtoul(getenv("ADGMFRAME"),0,16): 0x2EC; // DoDrawFrame
    uint32_t initTV = m_sec1 + initOff, blankTV = m_sec1 + blankOff, startTV = m_sec1 + startOff;
    uint32_t frameTV = m_sec1 + frameOff;
    if(!getenv("ADGMNOSTART")) call_tvec(startTV, {}, "module __start");
    // Metrowerks CFM shared-library fragment init routine: runs the C++ static
    // constructors (which set the module's global-object vtables, e.g. the draw-
    // context @0x3000864C). CFM normally calls this on Prepare; our host must call
    // it explicitly (initSection=-1 -> no auto-init). Arg = a dummy InitBlockPtr.
    if(!getenv("ADGMNOINIT")){
      uint32_t initOff2 = getenv("ADGMINITZ")?(uint32_t)strtoul(getenv("ADGMINITZ"),0,16):0x4A4; // __initialize
      uint32_t ib = mem->allocate(0x40); mem->memset(ib,0,0x40);
      call_tvec(m_sec1 + initOff2, {ib}, "module __initialize");
    }
    // C++ static-constructor array initializer (__init_arr export). CFM Prepare
    // normally runs the module's global ctors; call the module's own __init_arr to
    // construct static objects (e.g. Psycho Deli's draw-context whose vtable the
    // tiling code dispatches through). ADGMINITARR=off overrides the sec1 offset.
    if(getenv("ADGMINITARR")){
      uint32_t iao=(uint32_t)strtoul(getenv("ADGMINITARR"),0,16);
      call_tvec(m_sec1 + iao, {}, "module __init_arr");
    }
    uint32_t gmpb = mem->allocate(0x80); mem->memset(gmpb,0,0x80);
    mem->write_u16b(gmpb+0, 0); mem->write_u16b(gmpb+2, 0);
    mem->write_u16b(gmpb+4, (uint16_t)H.fb_h); mem->write_u16b(gmpb+6, (uint16_t)H.fb_w); // bounds Rect
    // Controls array: DoInit searches for a record whose type-tag byte at
    // [ctrls + i*10 + 11] == 8 (else it shows the "use colors" caption + returns -1).
    // Build ctrlCount records (10 bytes each from ctrls+2); tag each with 8 and seed
    // the value from ADCTRL. ADGMCTRLN sets how many (default 8).
    uint32_t ctrls = mem->allocate(0x100); mem->memset(ctrls,0,0x100);
    int ctrlN = getenv("ADGMCTRLN")? atoi(getenv("ADGMCTRLN")) : 8;
    mem->write_u16b(ctrls, (uint16_t)ctrlN);      // count
    // Each control record: data (a Rect {top,left,bottom,right}) at rec+2, type tag
    // at rec+11. The tag-8 control DoInit selects supplies the OFFSCREEN/screen bounds
    // (the module reads [rec+2] and [rec+6] as the two rect words), so write the full
    // screen Rect {0,0,fb_h,fb_w} — NOT a scalar (a scalar made a degenerate 1x1 GWorld).
    for(int i=0;i<ctrlN;i++){ uint32_t rec=ctrls + i*10;
      mem->write_u16b(rec+2, 0);                  // top
      mem->write_u16b(rec+4, 0);                  // left
      mem->write_u16b(rec+6, (uint16_t)H.fb_h);   // bottom
      mem->write_u16b(rec+8, (uint16_t)H.fb_w);   // right
      mem->write_u8(rec+11, 8); }                 // type tag = 8
    mem->write_u32b(gmpb+0x08, ctrls);
    mem->write_u16b(gmpb+0x0E, 0);                // flags clear (bit 0x400 must be 0)
    uint32_t gmctx = mem->allocate(0x400); mem->memset(gmctx,0,0x400);
    mem->write_u32b(gmpb+0x1E, gmctx);            // the +0x1E ptr (client/QD ctx - provisional)
    // AUTHENTIC GMParamBlock layout (Greg Parker Fringe Player SDK; see
    // reference_afterdark_sdk_structs): controlValues[4]@0x00, monitors@0x08,
    // colorQDAvail@0x0C, qdGlobals@0x10, demoRect@0x16 (drawing bounds), errorMessage@0x1E,
    // adVersion@0x26. Note gmpb+0..+6 is controlValues[4], NOT a bounds Rect: putting the
    // bounds there makes an ADGM module read controlValues[0] = the bounds top for every
    // option (Psycho Deli's 12 Pictures then all render identically). Rebuild the block in
    // that layout and seed controlValues from ADCTRL (slot i -> gmpb+i*2). ADNOAUTHPB
    // reverts to the legacy layout for A/B.
    if(!getenv("ADNOAUTHPB")){
      mem->memset(gmpb,0,0x80);
      for(int i=0;i<4;i++) mem->write_u16b(gmpb + (uint32_t)i*2, (uint16_t)ctrl_defs[i]); // controlValues[4]
      uint32_t minfo = mem->allocate(0x20); mem->memset(minfo,0,0x20);
      mem->write_u16b(minfo+0x00, 1);                    // monitorCount
      mem->write_u16b(minfo+0x02, 0);                    // MonitorData.bounds top
      mem->write_u16b(minfo+0x04, 0);                    //                    left
      mem->write_u16b(minfo+0x06, (uint16_t)H.fb_h);     //                    bottom
      mem->write_u16b(minfo+0x08, (uint16_t)H.fb_w);     //                    right
      mem->write_u8  (minfo+0x0A, 0);                    // synchFlag
      mem->write_u8  (minfo+0x0B, 8);                    // curDepth (8bpp)
      mem->write_u32b(gmpb+0x08, minfo);                 // monitors ^MonitorsInfo
      mem->write_u8  (gmpb+0x0C, 1);                     // colorQDAvail
      // demoRect: EMPTY is the authentic full-screen value, matching the 68K host.
      // demoRect is the control-panel preview box. A saver running full-screen is not
      // in a preview box, so the field must be an EMPTY Rect; a non-empty one means
      // "you are previewing inside this box". Modules read it two ways and BOTH agree
      // that empty == full-screen: (a) as an EmptyRect() boolean picking the
      // full-screen branch (68K RE of Boris/Strange Attractors/Spotlight; full-screen
      // bounds there collapse Boris d23->1 and Strange Attractors d233->1), and (b) as
      // an EXCLUSION rect — Psycho Deli's DoDrawFrame passes demoRect by value
      // (gmpb+0x16/+0x1A -> r4:r5, module code 30001DF8-30001E0C) into a per-element
      // loop that calls PtInRect(point, demoRect) and SKIPS the per-style element draw
      // when the point is INSIDE. Publishing the screen Rect here excluded the whole
      // screen: measured 5056/5056 points inside, style-draw callback invoked 0 times,
      // so PD's 11 Picture options all rendered byte-identically (the style setup ran
      // and installed the right per-style callback — it was simply never called).
      // Empty means "exclude nothing / draw everywhere".
      const bool demo_full = getenv("ADGMDEMOFULL")!=nullptr;   // lever: legacy screen-Rect
      mem->write_u16b(gmpb+0x16, 0); mem->write_u16b(gmpb+0x18, 0);            // demoRect topLeft
      mem->write_u16b(gmpb+0x1A, demo_full?(uint16_t)H.fb_h:0);                //          bottom
      mem->write_u16b(gmpb+0x1C, demo_full?(uint16_t)H.fb_w:0);                //          right
      mem->write_u32b(gmpb+0x1E, gmctx);                 // errorMessage scratch
      mem->write_u16b(gmpb+0x26, 0x0200);                // adVersion 2.0
      fprintf(stderr,"[adhost] authentic GMParamBlock: controlValues=[%d,%d,%d,%d] "
              "monitors@%08X demoRect=(0,0,%d,%d)%s\n",
              ctrl_defs[0],ctrl_defs[1],ctrl_defs[2],ctrl_defs[3],minfo,
              demo_full?H.fb_h:0, demo_full?H.fb_w:0, demo_full?" [ADGMDEMOFULL legacy]":" empty=full-screen");
    }
    uint32_t storeSlot = mem->allocate(4); mem->write_u32b(storeSlot, 0);   // char*** clientStorage
    uint32_t rgn = mem->allocate(0x10); mem->memset(rgn,0,0x10);
    mem->write_u16b(rgn,10); mem->write_u16b(rgn+2,0); mem->write_u16b(rgn+4,0);
    mem->write_u16b(rgn+6,(uint16_t)H.fb_h); mem->write_u16b(rgn+8,(uint16_t)H.fb_w);
    uint32_t rgnH = mem->allocate(4); mem->write_u32b(rgnH, rgn);
    fprintf(stderr,"[adhost] GM: DoInit TVec@%08X DoBlank TVec@%08X gmpb=%08X\n",initTV,blankTV,gmpb);
    // The ADGM path returns before the main control-seeding block, so seed the library
    // control array here, before DoInitialize, and a module that reads controls through
    // the shared library during init latches the user's values. The GMParamBlock.controls
    // records built below carry only the screen-Rect/offscreen bounds; if an ADGM module
    // turns out to read its user settings SOLELY through those records, that record format
    // still needs RE.
    if(g_early_seed) seed_controls("GM pre-DoInitialize", true);
    call_tvec(initTV, {storeSlot, rgnH, gmpb}, "DoInitialize");
    H.cur_port = H.g_port;   // make our screen the current port before DoBlank draws
    uint32_t storeH = mem->read_u32b(storeSlot);
    // msg-1 handler (dispatcher 0x30000004 case 1 -> 0x300024A4): possibly the setup
    // step that constructs the draw-context (SelectParams-like). Call it after init.
    if(getenv("ADGMMSG1")){
      uint32_t m1 = m_sec1 + (getenv("ADGMMSG1OFF")?(uint32_t)strtoul(getenv("ADGMMSG1OFF"),0,16):0x2C4);
      call_tvec(m1, {storeH, rgnH, gmpb}, "msg1/SelectParams");
    }
    fprintf(stderr,"[adhost] GM: clientStorage handle=%08X\n",storeH);
    int gmframes = getenv("ADFRAMES")? atoi(getenv("ADFRAMES")) : 10;
    // STRUCTURAL arm, not a module-name gate: Psycho Deli's psychedelic motion is produced
    // by its OWN DoDrawFrame step, which advances the module's float palette and
    // re-installs it via SetEntries every frame. That step draws each pattern element
    // through a per-style callback held at the module's DoDrawFrame TOC+0x64c; the callback
    // is installed by the module's StyleDispatch (code 0x30002584, a 12-case style table),
    // reached only via a setup message the direct DoBlank/DoDrawFrame GM drive never sends —
    // so under this drive that slot is NULL while the seed TVector at TOC+0xf0 is present.
    // (Driving DoDrawFrame with the callback still NULL is what crashes the module.) That
    // null-callback-with-seed signature is produced by no other state, so arming keys on it.
    // Reproduce exactly the store StyleDispatch's first case performs (*(TOC+0x64c) =
    // *(TOC+0xf0), an authentic module TVector); DoDrawFrame then runs and the module
    // re-randomises styles itself thereafter (StyleDispatch is re-called from inside
    // DoDrawFrame). The seed range-check (a seed pointing into the module image
    // [BASE_M,+0x100000)) self-validates, so a GM module lacking the signature simply runs
    // DoBlank-only rather than getting fabricated colours.
    bool pd_frame = false; uint32_t pd_toc = 0;
    {
      pd_toc = mem->read_u32b(frameTV+4);            // module RTOC (DoDrawFrame's toc)
      uint32_t cb   = mem->read_u32b(pd_toc+0x64c);  // per-style element-draw callback (NULL until StyleDispatch)
      uint32_t seed = mem->read_u32b(pd_toc+0xf0);   // style-0 element-draw TVector
      // The Picture popup selects one of 12 StyleDispatch cases, but the module
      // RE-RANDOMISES the style inside DoDrawFrame, so pre-installing a per-Picture seed
      // here does not stick (seed index 0..11 all render identically). Locking the style to
      // the Picture control requires the StyleDispatch SETUP call (code 0x30002584) that
      // reads controlValues[0]; calling it directly with these GM args faults (address
      // 2FFEEFE4, wrong arg/context), so Picture selection is a known gap pending a PPC
      // disassembly of StyleDispatch's signature. The authentic GMParamBlock layout above
      // DOES deliver Psycho Deli's Speed + Knots sliders.
      bool seed_in_module = (seed>=BASE_M && seed<BASE_M+0x100000);
      if(cb==0 && seed_in_module){ mem->write_u32b(pd_toc+0x64c, seed); cb=seed; }
      if(cb){ pd_frame = true;
        fprintf(stderr,"[adhost] GM real-palette armed (structural null-callback signal): TOC=%08X drawCB@+0x64c=%08X seed@+0xf0=%08X\n",pd_toc,cb,seed); }
    }
    // DoBlank is a ONE-SHOT, not a per-frame call. The ADgm contract is Initialize ->
    // BlankScreen (once, paints the initial field) -> DrawFrame (repeatedly, animates it).
    // Blanking every frame resets the module to frame 0: Psycho Deli's DoBlank (code
    // 30000DD4) zeroes its animation cursor at state+0x2420/+0x2422 and repaints the
    // initial field, so only one DrawFrame pass ever shows — a flat repeating chevron field
    // in which all 12 Picture styles look near-identical. Blanking once yields the module's
    // real artwork (spiral/fractal fields) and 12 visibly distinct Pictures.
    // ADGMBLANKEACH restores the per-frame blank for A/B.
    const bool blank_each = getenv("ADGMBLANKEACH")!=nullptr;
    if(!getenv("ADGMNOBLANK") && !blank_each)
      call_tvec(blankTV, {storeH, rgnH, gmpb}, "DoBlank (once, pre-frame-loop)");
    for(int fN=0; fN<gmframes; fN++){
      if(g_shm_on){ if(!wait_go()) break; }   // block for the app's GO
      if(!getenv("ADGMNOBLANK") && blank_each) call_tvec(blankTV, {storeH, rgnH, gmpb}, "DoBlank");
      if(pd_frame){
        // The module's real per-frame animation step. Measured robust (0 faults / 300
        // frames), so this try/catch is defensive only: if DoDrawFrame ever faults we
        // stop driving it and leave the last real palette in place (an honest freeze) —
        // we do NOT fabricate colours (the HSV synth is gone). A fault here would be a
        // real-path bug to RE and fix, not to paper over.
        try { call_tvec(frameTV, {storeH, rgnH, gmpb}, "DoDrawFrame"); }
        catch(const exception& ex){
          fprintf(stderr,"[adhost] GM DoDrawFrame faulted (%s) — stopping frame-drive, last palette frozen\n",ex.what());
          pd_frame=false;
        }
      }
      else if(getenv("ADGMFRAME")) call_tvec(frameTV, {storeH, rgnH, gmpb}, "DoDrawFrame"); // opt-in (crashes standalone)
      // No host auto-present of a module-private GWorld.
      //
      // AUTHENTIC ADgm PRESENT CONTRACT (RE, scratchpad/gworld_evidence.md): a
      // classic GraphicsModule draws each frame to the CURRENT port, which the
      // engine sets to the screen (our host sets H.cur_port=H.g_port before
      // DoBlank, so DoBlank/DoDrawFrame draw straight into g_fb). If a module
      // wants to double-buffer, IT allocates the GWorld, SetGWorld's to it,
      // draws, and CopyBits it back to the screen port itself — the engine never
      // knows about, nor auto-presents, that private offscreen. So presentation
      // is ALWAYS one of: (a) direct draw to the screen port (g_fb), or (b) the
      // module's own CopyBits to the screen port, which the CopyBits trap already
      // honors (it writes to whatever the dst pixmap's baseAddr is, = g_fb for the
      // screen port). There is no third "engine blits the module's GWorld" path.
      //
      // Ground truth for the sole PPC GM module, Psycho Deli: 0 CopyBits, and its
      // 640x480 GWorld is a scratch/backdrop buffer DoBlank fills but never blits;
      // its animated palette pattern is drawn directly into g_fb. Presenting that
      // scratch buffer blits a blank frame over the real pattern. Honoring the contract
      // structurally — only present what the module drew to the screen port — makes it
      // correct independent of any pixel-value coincidence.
      if(getenv("ADGMGWLOG")) for(size_t i=0;i<H.gworlds.size();i++){
        uint32_t port=H.gworlds[i], pmh=mem->read_u32b(port+0x02), pm=pmh?mem->read_u32b(pmh):0;
        if(!pm) continue; uint32_t base=mem->read_u32b(pm+0); int rb=mem->read_u16b(pm+4)&0x3FFF;
        int t=(int16_t)mem->read_u16b(pm+6),l=(int16_t)mem->read_u16b(pm+8),
            b=(int16_t)mem->read_u16b(pm+10),rr=(int16_t)mem->read_u16b(pm+12);
        fprintf(stderr,"[adhost] GM gw%zu base=%08X %dx%d rb=%d (no auto-present)\n",i,base,rr-l,b-t,rb);
      }
      if(getenv("ADGMOFF")){ uint32_t off=strtoul(getenv("ADGMOFF"),0,0);
        for(int y=0;y<H.fb_h;y++)for(int x=0;x<H.fb_w;x++)
          mem->write_u8(H.g_fb+(uint32_t)y*H.fb_w+x, mem->read_u8(off+(uint32_t)y*H.fb_w+x)); }
      BSave _bs=banner_paint(fN);   // composite the timed banner onto this presented frame
      if(const char* fbp=getenv("ADFRAMEDIR"))
        snap_ppm(string(fbp)+"/frame"+to_string(fN)+".ppm", H.g_fb, H.fb_w, H.fb_h, H.fb_w);
      stream_frame();
      banner_restore(_bs);          // restore g_fb — the banner never persists in module state
    }
    fprintf(stderr,"[adhost] GM: done (%llu OS calls, %llu CopyBits)\n",
            (unsigned long long)H.call_count,(unsigned long long)H.copybits_count);
    return 0;
  }
  // ---- module CFM-init: run the module's PEF init routine (its C++ _STI static
  // constructors) BEFORE main(sel=0). CodeWarrior C++ (CP-app) modules must run their
  // _STI ctor list before the first frame (see LUNACY_LINKER.md) — that is what
  // constructs the Sprite/XCanvas object graph; without it sprite modules run with empty
  // sprite objects ([sprite+4]==0). The GM path does this via module __start/__initialize;
  // the call_main path runs the module's real CFM init symbol here. ADNOMODCTOR opts out.
  if(getenv("ADMODCTOR")){
    const auto& isym = mod.init();
    fprintf(stderr,"[adhost] module init symbol: section=%d value=%08X (m_sec0=%08X m_sec1=%08X)\n",
      isym.section_index, isym.value, m_sec0, m_sec1);
    if(isym.section_index >= 0){
      uint32_t itv = (isym.section_index==0? m_sec0 : m_sec1) + isym.value;
      uint32_t icode=0,itoc=0; try{ icode=mem->read_u32b(itv); itoc=mem->read_u32b(itv+4);}catch(...){}
      fprintf(stderr,"[adhost] module init TVector @%08X -> code=%08X toc=%08X\n",itv,icode,itoc);
      uint32_t ib = mem->allocate(0x40); mem->memset(ib,0,0x40);
      try { call_tvec(itv, {ib}, "module CFM-init (_STI ctors)"); }
      catch(const exception& ex){ fprintf(stderr,"[adhost] module CFM-init failed: %s\n",ex.what()); }
    }
  }
  // Seed the control array BEFORE the module's INIT, so a module that LATCHES a control
  // during INIT (Rainforest/Swirling Magic/Life & All/Points of View …) reads the user's
  // value, not 0. ADNOEARLYSEED reverts to post-INIT-only ordering; the post-INIT re-seed
  // below covers re-reading modules and that revert path.
  if(g_early_seed) seed_controls("pre-INIT", true);
  // ---- arm the opt-in library-RNG seeding (see the note at the top of the file for the
  // full mechanism). Unset => nothing here runs.
  if(const char* rs = getenv("ADLIBRNGSEED")){
    uint32_t engtoc = 0;
    try { engtoc = mem->read_u32b(mem->get_symbol_addr("adxpl510:__initialize")+4); }
    catch(const exception&){ engtoc = 0; }
    auto dump_rng = [&](const char* when){
      if(!engtoc) return;
      fprintf(stderr,"[rngseed] %-6s i=%d j=%d lcg=%08X tbl[0..3]=%08X %08X %08X %08X\n", when,
        (int)(int16_t)mem->read_u16b(engtoc-0x2394), (int)(int16_t)mem->read_u16b(engtoc+0x0442),
        mem->read_u32b(engtoc-0x056C),
        mem->read_u32b(engtoc-0x254C), mem->read_u32b(engtoc-0x2548),
        mem->read_u32b(engtoc-0x2544), mem->read_u32b(engtoc-0x2540));
    };
    dump_rng("baked");
    if(!strcasecmp(rs,"probe")){
      fprintf(stderr,"[adhost] ADLIBRNGSEED=probe: state reported, NOT seeded\n");
    } else {
      g_librng_seed  = !strcasecmp(rs,"random") ? (uint32_t)arc4random()
                                                : (uint32_t)strtoul(rs, nullptr, 16);
      try {
        uint32_t tv = mem->get_symbol_addr("adxpl510:SRand__FUi");
        g_librng_srand_pc = mem->read_u32b(tv);      // TVector -> code entry (0x10029D74)
        g_librng_armed = true;                        // arm BEFORE the call so the
        call_tvec(tv, {g_librng_seed}, "adxpl510:SRand__FUi");  // override logs uniformly
        fprintf(stderr,"[adhost] ADLIBRNGSEED: adxpl510 SRand(%08X) [%s], SRand entry %08X armed\n",
                g_librng_seed, !strcasecmp(rs,"random")?"random":"fixed", g_librng_srand_pc);
      } catch(const exception& ex){
        g_librng_armed = false;
        fprintf(stderr,"[adhost] ADLIBRNGSEED failed: %s — library RNG left at its baked state\n",
                ex.what());
      }
      dump_rng("seeded");
    }
  }
  call_main(0, "INIT");
  // ADSCANCB: after INIT, scan the whole faceplate region (frame/ADdl/tags) + the engine
  // TOC for any u32 that points into the MODULE code region [BASE_M, BASE_M+0x40000).
  // Decisive test for whether Magic Turtle's draw callback is MODULE-provided (routable to
  // the engine's ADdl callback slots) or purely faceplate-app data (unreachable here).
  if(getenv("ADSCANCB")){
    auto scan=[&](uint32_t lo,uint32_t hi,const char* tag){
      for(uint32_t a=lo;a+4<=hi;a+=4){ uint32_t v=0; try{v=mem->read_u32b(a);}catch(...){continue;}
        if(v>=BASE_M && v<BASE_M+0x40000)
          fprintf(stderr,"[scancb] %s[%08X] -> module+%05X\n",tag,a,v-BASE_M); } };
    if(H.g_frame) scan(H.g_frame, H.g_frame+0x600, "frame");
    if(H.g_addl)  scan(H.g_addl,  H.g_addl+0x400,  "addl");
    uint32_t et = mem->read_u32b(mem->get_symbol_addr("adxpl510:__initialize")+4);
    scan(et+0x300, et+0x400, "engTOC");   // around the 0x3C8 callback-object slot
    fprintf(stderr,"[scancb] done\n");
  }
  // Seed (re-seed) the module's control-panel values. Every module reads its slider
  // settings via GetControlValue(index) == (*(adxplTOC+0x3A8))[index] (16-bit shorts).
  // With g_early_seed this has ALREADY run once BEFORE call_main(0,"INIT") above; this
  // post-INIT call keeps re-reading modules working, is the sole seed under
  // ADNOEARLYSEED, and is idempotent when the early seed ran. The full mechanism (0x3A8
  // array, size clamp, Time Flies GetControlValue(3) caps-lock case) is on the lambda above.
  seed_controls(g_early_seed ? "post-INIT re-seed" : "post-INIT (legacy order)", false);
  // ---- resolve the UserInput key ring (RD keypump) -------------------------
  // The ring statics live at the adxpl510 TOC + fixed offsets (see ad_input_line
  // note). Resolve via the AddKey export's TVector TOC (== the r2 the methods use).
  {
    uint32_t addkey_tv = mem->get_symbol_addr("adxpl510:AddKey__9UserInputF7KeyType");
    uint32_t ui_toc = addkey_tv ? mem->read_u32b(addkey_tv + 4) : 0;
    if(ui_toc){
      g_ui_pressed = ui_toc + 0x448; g_ui_write = ui_toc + 0x44A;
      g_ui_read    = ui_toc + 0x44C; g_ui_size  = ui_toc + 0x44E;
      g_ui_buffer  = ui_toc + 0x450;
      // The UserInput ring is shared-library infrastructure, not one module's: ADShared40 exports
      // AddKey, so ui_toc resolves for EVERY module (@TOC=10048000 for Rodger Dodger, Flying
      // Toasters, Fish World, Marbles). The keypump feed is inert without live key input
      // (g_rd_dir==0 => no ring write, no demo/player poke; see the feed block below), so it is
      // enabled whenever the ring resolved (this enclosing `if`) and !ADNOKEYPUMP rather than
      // gated on a module name. ADKEYPUMPALL is a no-op alias.
      g_keypump_on = !getenv("ADNOKEYPUMP");
      fprintf(stderr,"[adhost] UserInput ring @TOC=%08X buffer=%08X size@=%08X "
              "keypump=%s (module=%s)\n", ui_toc, g_ui_buffer, g_ui_size,
              g_keypump_on?"ON":"off", (argc>2?argv[2]:"?"));
    } else {
      fprintf(stderr,"[adhost] UserInput ring NOT resolved (module does not import "
              "AddKey) — keypump inert\n");
    }
  }
  // ---- LIVE CONTROL CHANNEL ------------------------------------------------
  // In ADSTREAM (app) mode, poll stdin non-blocking once per frame for lines
  // "SET <idx> <val>" (idx 0..15) and apply them to the SAME control-value array
  // that ADCTRL seeds — GetControlValue(idx) reads (*(adxplTOC+0x3A8))[idx] every
  // frame, so the module picks the new value up live with NO process respawn.
  // Untouched outside stream mode and when stdin is a tty (census safety); the
  // non-blocking flag is only ever set on a pipe/fifo stdin. Disk-free, synchronous.
  std::string g_setbuf; int g_nbstate = 0;   // 0=uninit 1=poll 2=disabled(tty)
  auto poll_set = [&](){
    if(!streamf) return;
    if(g_nbstate==0){
      if(isatty(0)) g_nbstate=2;
      else { int fl=fcntl(0,F_GETFL,0); if(fl!=-1) fcntl(0,F_SETFL, fl|O_NONBLOCK); g_nbstate=1; }
    }
    if(g_nbstate!=1) return;
    char tmp[512]; ssize_t rn;
    while((rn=read(0,tmp,sizeof(tmp)))>0) g_setbuf.append(tmp,(size_t)rn);
    size_t nl;
    while((nl=g_setbuf.find('\n'))!=std::string::npos){
      std::string line=g_setbuf.substr(0,nl); g_setbuf.erase(0,nl+1);
      int idx,val;
      if(sscanf(line.c_str(),"SET %d %d",&idx,&val)==2 && idx>=0 && idx<16 && g_ctrl_addr){
        try{ mem->write_u16b(g_ctrl_addr + (uint32_t)idx*2, (uint16_t)val); }catch(...){}
        if(getenv("ADSETLOG")) fprintf(stderr,"[adhost] live SET ctrl[%d]=%d\n",idx,val);
      } else ad_input_line(line.c_str());   // KEY/CAPS/MOUSE
    }
  };
  // Headless test lever (no app) so caps/keys can be A/B'd from a decoded stream.
  // Applied once before the loop; default-off.
  if(getenv("ADTESTCAPS")){ g_caps=true; ad_km_set(kCapsKeycode,true); g_input_seen=true; }
  if(const char* tk=getenv("ADTESTKEY")){ ad_km_set(atoi(tk),true); g_input_seen=true; }
  auto fbchanged=[&](uint8_t fb)->long{ long c=0; for(int i=0;i<H.fb_w*H.fb_h;i++) if(mem->read_u8(H.g_fb+i)!=fb) c++; return c; };
  if(getenv("ADFBFILL")){ uint8_t fb=(uint8_t)strtoul(getenv("ADFBFILL"),0,0);
    fprintf(stderr,"[adhost] g_fb bytes changed from fill: %ld / %d\n",fbchanged(fb),H.fb_w*H.fb_h); }
  // Probe mode: after INIT, call each selector 1..7 once and report deltas.
  if(getenv("ADPROBE")){
    // Probe draw candidates before the Close selector (sel 1) tears the module down.
    for(int sel : {2,3,4,5,6,7,1}){
      mem->memset(H.g_fb, 0x55, H.fb_w*H.fb_h);
      uint64_t cb0=H.copybits_count; uint64_t oc0=H.call_count;
      call_main(sel, "PROBE");
      fprintf(stderr,"[adhost] PROBE sel=%d: %llu OS calls, %llu CopyBits, g_fb changed=%ld\n",
              sel,(unsigned long long)(H.call_count-oc0),(unsigned long long)(H.copybits_count-cb0),fbchanged(0x55));
    }
  }
  // Drive animation frames via the requested selector (default 1 = DrawFrame).
  int draw_sel = getenv("ADDRAWSEL")? atoi(getenv("ADDRAWSEL")) : 1;
  int nframes  = getenv("ADFRAMES")? atoi(getenv("ADFRAMES")) : 0;
  // AUTHENTIC PER-MODULE DRAW PACING (stream/lockstep). Under ADSHM/ADSTREAM the app sends a GO (or
  // drains the pipe) at up to 60fps; advancing the module once per GO runs the animation at DISPLAY
  // fps, which is far too fast for most modules. Decouple: advance the module only every
  // pace-interval wall-milliseconds; a GO that arrives early RE-SENDS the previous frame — skip the
  // module DRAW (and its offscreen composite), leaving g_fb unchanged, so stream_frame() re-emits it.
  // The authentic per-module interval is already encoded as ADUSPERCALL (the µs of simulated time the
  // catalog authored per DrawFrame — 100ms/10fps for most modules, 12ms for Magic Turtle); ADPACEMS /
  // ADPACEHZ override. Time sources (Microseconds/TickCount) track real wall-clock in stream mode, so
  // a skipped call costs no time drift. Gated on ADSTREAM; ADNOPACE opts out. Headless (no ADSTREAM)
  // always advances, so the census is unaffected.
  const bool g_pace_on = getenv("ADSTREAM") && !getenv("ADNOPACE");
  double g_pace_ms = getenv("ADUSPERCALL") ? atof(getenv("ADUSPERCALL"))/1000.0 : 100.0;
  if(getenv("ADPACEMS")) g_pace_ms = atof(getenv("ADPACEMS"));
  else if(getenv("ADPACEHZ")){ double hz=atof(getenv("ADPACEHZ")); if(hz>0) g_pace_ms=1000.0/hz; }
  if(g_pace_ms < 0.0) g_pace_ms = 0.0;
  double g_pace_last=-1e18;
  auto pace_now=[&]()->double{ return ad_wall_elapsed()*1000.0; };
  // INIT EXEMPTION: do not pace until the module's DRAW has actually CHANGED the framebuffer vs a
  // pre-first-draw baseline. Slow-init modules (Art Critic does ~900 DRAW calls of init work before
  // the first visible paint) must run init at full call rate; pacing throttles that to 10 calls/s so
  // init never finishes in a 62s window. The test is baseline-CHANGE rather than fb-UNIFORMITY, so it
  // is robust to whatever the blank left on screen. ADNOINITEXEMPT disables.
  bool g_first_drawn = false;
  uint64_t g_pace_basehash = 0; bool g_pace_base_init = false;
  const bool g_initexempt = !getenv("ADNOINITEXEMPT");
  // idle-FF accrual state (see g_adff_ticks). Gated to paced stream wall-clock mode; ADGM (Psycho
  // Deli's palette-cycler, whose fb indices can idle while its palette animates) and ADNOIDLEFF opt
  // out. ADIDLEFFTICKS/ADIDLEFFAFTER/ADIDLEFFLOG as on the 68K host.
  const bool g_idleff_on = g_pace_on && ad_wallclock_on() && !getenv("ADNOIDLEFF") && !getenv("ADGM");
  const uint32_t g_idleff_base = getenv("ADIDLEFFTICKS") ? (uint32_t)strtoul(getenv("ADIDLEFFTICKS"),0,0) : 120;
  const int g_idleff_after = getenv("ADIDLEFFAFTER") ? atoi(getenv("ADIDLEFFAFTER")) : 15;
  uint64_t g_idleff_lasthash = 0; int g_idleff_streak = 0; bool g_idleff_init = false;
  if(g_pace_on) fprintf(stderr,"[adhost] pacing module DRAW to %.1f ms (%.1f fps)\n",g_pace_ms, g_pace_ms>0?1000.0/g_pace_ms:0.0);
  // The AD4 SDK module shim (main's 8-entry selector table) is identical boilerplate in every
  // PPC module: main(&result, ?, message, frame) with
  //   0 Initialize (operator new + ctor, *result = the module object)
  //   1 Close      (virtual dtor, vtable+0x08, delete-flag 1)
  //   2 **BlankScreen**  -> the object's vtable+0x0C
  //   3 DrawFrame        -> the object's vtable+0x10
  //   7 event state machine (frame+0x36 subcode -> vtable+0x14/+0x18/+0x1C/+0x20, else +0x34)
  // The vtable slot names are READ OFF adxpl510's own AfterDarkModule vtable (library data
  // @10044ED8, found by anchoring on the DoLButtonDown/Held/Up/UpdateRgn TVector run):
  //   +0x08 __dt  +0x0C DoBlankScreen  +0x10 DoDrawFrame  +0x14 DoLButtonDown  +0x18 DoLButtonHeld
  //   +0x1C DoLButtonUp  +0x20 DoUpdateRgn  +0x24 DoMouseMove  +0x28 DoDisableAnimation
  //   +0x2C DoEnableAnimation  +0x30 DoButton(short)  +0x34 DoUserMessage(short)
  // (Do not extrapolate TVector addresses down the slot list to name these — the run breaks at
  // +0x30. The base-class vtable dump is the direct evidence.)
  // Message 2 matters: a module's DoBlankScreen override is the ONLY place a module paints its
  // background. Driving only messages 0 and 3 leaves Rainforest a BLACK SCREEN (0.2% non-black)
  // with its 10 Background options collapsed to 2 distinct renders, because nothing ever paints
  // the jungle backdrop or the pattern texture. Sending message 2 once after Initialize is what
  // the real After Dark app does before the first DrawFrame; 16/19 PPC modules render
  // identically either way and the other two GAIN authored content — Flying Toasters! plays its
  // "toaster evolution" era-card intro before resuming, Rock Paper Scissors re-seeds its sprite
  // layout. ADNOBLANKMSG=1 reverts to the 0/3-only drive; ADBLANKSEL=<n> overrides which message
  // is sent.
  if(nframes>0 && !getenv("ADNOBLANKMSG")){
    int blank_sel = getenv("ADBLANKSEL") ? atoi(getenv("ADBLANKSEL")) : 2;
    call_main(blank_sel, "BLANK");
  }
  for(int fN=0; fN<nframes; fN++){
    if(g_shm_on){ if(!wait_go()) break; }   // block for the app's GO (SET applied inline)
    else poll_set();               // apply any live "SET idx val" before the draw
    ad_poll_real_caps();           // ADREALCAPS=1 -> fold real caps state in (inert unless armed)
    // pacing decision — should the module advance this display frame? (else re-send prev frame)
    bool g_advance = !g_pace_on;
    if(!g_advance && g_initexempt && !g_first_drawn){   // init exemption (first change vs baseline)
      uint64_t h=1469598103934665603ULL;
      for(int i=0;i<H.fb_w*H.fb_h;i+=64){ h=(h ^ mem->read_u8(H.g_fb+(uint32_t)i))*1099511628211ULL; }
      if(!g_pace_base_init){ g_pace_base_init=true; g_pace_basehash=h; }
      if(h!=g_pace_basehash) g_first_drawn=true; else g_advance=true;
    }
    if(!g_advance){ double n=pace_now(); if((n-g_pace_last)>=g_pace_ms){ g_pace_last=n; g_advance=true; } }
    // RD keypump — feed one KeyType into the UserInput ring per advanced frame (the
    // shell's per-keyDown AddKey, reproduced). Inert when no key is active (feed==0 -> no
    // ring write, no flag set). ADRDFEED=<hex> forces a constant KeyType.
    // ADRDMODE=<hex> pokes RD's demo/player mode flag at module TOC+0x3EA4 each frame
    // (0=demo/attract, !=0=interactive player). MoveHandler (module+0x4124) and PlayThing
    // (0x6F50) both gate the keyboard path on this flag.
    if(g_advance && g_keypump_on){
      // Auto-trigger: the first MAPPED key press (arrows/WASD via the KEY protocol) flips
      // RD's own demo/player flag, then keeps it poked (sticky, same per-frame semantics as
      // ADRDMODE, in case a state transition resets it). Idle stays in pure demo/attract
      // mode; an arrow press enters interactive player mode (the KeyToMove path). Default
      // ON; ADNORDPLAY opts out; explicit ADRDMODE=<hex> overrides.
      static bool g_rd_play = false;
      if(!g_rd_play && g_rd_dir && !getenv("ADNORDPLAY")) g_rd_play = true;
      const char* rdm = getenv("ADRDMODE");
      if(rdm || g_rd_play){
        uint32_t v = rdm ? (uint32_t)strtoul(rdm,0,16) : 1u;
        try{ mem->write_u32b(main_toc + 0x3EA4, v); }catch(...){}
      }
    }
    if(g_advance && g_keypump_on && g_ui_size){
      // ADRDFEEDSEQ="key:frames,key:frames,..." — advance one segment per DRAWN frame;
      // after the last segment the final key is HELD (prefix-then-hold for the control proof).
      static std::vector<std::pair<int,int>> g_rdseq; static bool g_rdseq_init=false; static long g_rdseq_f=0;
      if(!g_rdseq_init){ g_rdseq_init=true;
        if(const char* s=getenv("ADRDFEEDSEQ")){ const char* p=s;
          while(*p){ int k=(int)strtoul(p,nullptr,16); const char* c=strchr(p,':'); int fr=1;
            if(c){ fr=atoi(c+1); } g_rdseq.push_back({k,fr});
            const char* comma=strchr(p,','); if(!comma)break; p=comma+1; }
          if(getenv("ADRDLOG")){ fprintf(stderr,"    [rdseq] %zu segments:",g_rdseq.size());
            for(auto&pr:g_rdseq) fprintf(stderr," %02X:%d",pr.first,pr.second); fprintf(stderr,"\n"); } }
      }
      int seqfeed=-1;
      if(!g_rdseq.empty()){ long acc=0,idx=g_rdseq_f; size_t i=0;
        for(; i<g_rdseq.size(); i++){ if(idx < acc+g_rdseq[i].second){ seqfeed=g_rdseq[i].first; break; } acc+=g_rdseq[i].second; }
        if(seqfeed<0) seqfeed=g_rdseq.back().first;   // past the end -> hold last
        g_rdseq_f++;
      }
      int feed = seqfeed>=0 ? seqfeed
               : getenv("ADRDFEED") ? (int)strtoul(getenv("ADRDFEED"),0,16)
                                    : (g_input_seen ? g_rd_dir : 0);
      if(feed){
        try{
          int16_t sz = (int16_t)mem->read_u16b(g_ui_size);
          if(sz>0){
            uint16_t w = mem->read_u16b(g_ui_write);
            mem->write_u32b(g_ui_buffer + (uint32_t)w*4u, (uint32_t)feed);   // buffer[w*4]=key
            w = (uint16_t)(((int16_t)w + 1) % sz);                            // write=(write+1)%size
            mem->write_u16b(g_ui_write, w);
            uint16_t rd = mem->read_u16b(g_ui_read);
            if((int16_t)rd == (int16_t)w){ rd = (uint16_t)(((int16_t)rd + 1) % sz); mem->write_u16b(g_ui_read, rd); }
            mem->write_u16b(g_ui_pressed, 1);                                 // pressed=1
            if(getenv("ADRDLOG") && g_rd_feedlog++<50)
              fprintf(stderr,"    [rdpump] fed KeyType %02X -> buf[write->%d] flag=1 (read=%d size=%d)\n",
                      feed, w, rd, sz);
          }
        }catch(...){}
      }
    }
    if(g_advance)
    call_main(draw_sel, "DRAW");
    // Idle detection for the tick catch-up — sampled fb hash (stride 4) after each ADVANCE draw.
    // An unchanged fb means the module idled on virtual time (the only input it waits on), so after
    // g_idleff_after consecutive idle advances add an escalating boost (base<<streak, x64 cap) to
    // the monotonic g_adff_ticks offset TickCount/Microseconds serve. Any fb change resets the
    // streak, so actively-drawing modules never see boosted time and the pacing rates hold.
    if(g_idleff_on && g_advance){
      uint64_t ih=1469598103934665603ULL;
      for(int i=0;i<H.fb_w*H.fb_h;i+=4){ ih=(ih ^ mem->read_u8(H.g_fb+(uint32_t)i))*1099511628211ULL; }
      if(g_idleff_init && ih==g_idleff_lasthash){
        if(++g_idleff_streak>=g_idleff_after){
          int e=g_idleff_streak-g_idleff_after; if(e>6)e=6;
          g_adff_ticks += (g_idleff_base<<e);
          if(getenv("ADIDLEFFLOG")) fprintf(stderr,"[idleff f%d] streak=%d +%u accum=%u\n",fN,g_idleff_streak,(unsigned)(g_idleff_base<<e),(unsigned)g_adff_ticks);
        }
      } else g_idleff_streak=0;
      g_idleff_lasthash=ih; g_idleff_init=true;
    }
    // ADBLITGW: modules that draw via QuickDraw primitives into an offscreen GWorld
    // and rely on the After Dark engine run-loop (not the module) to composite that
    // work canvas to the screen — e.g. Magic Turtle draws its cycling-color pen
    // graphics into a persistent 480x332 GWorld and never CopyBits it to the screen.
    // The host is the engine here, so pick the registered GWorld with the most
    // non-zero pixels (the accumulated drawing) and blit it into g_fb, centered.
    // Uses the real NewGWorld registry (not a memory scan), so it can't grab engine
    // code as a false "image". Opt-in: the 15 sprite modules CopyBits to the screen
    // themselves and must NOT have an offscreen blitted over their correct output.
    if(g_advance && getenv("ADBLITGW")){   // skip the offscreen re-composite on re-send frames
      uint32_t bestBase=0; int bestW=0,bestH=0,bestRB=0; long bestNZ=0;
      for(uint32_t port : H.gworlds){
        uint32_t pmh=0,pm=0; try{ pmh=mem->read_u32b(port+0x02); pm=pmh?mem->read_u32b(pmh):0; }catch(...){continue;}
        if(!pm) continue;
        uint32_t base=0; int rb=0,t=0,l=0,b=0,rr=0;
        try{ base=mem->read_u32b(pm+0); rb=mem->read_u16b(pm+4)&0x3FFF;
             t=(int16_t)mem->read_u16b(pm+6); l=(int16_t)mem->read_u16b(pm+8);
             b=(int16_t)mem->read_u16b(pm+10); rr=(int16_t)mem->read_u16b(pm+12); }catch(...){continue;}
        int w=rr-l,h=b-t; if(!base||w<64||h<64||rb<w) continue;
        if(base==H.g_fb) continue;                 // skip the screen canvas itself
        long nz=0; bool ok=true;
        for(int y=0;y<h && ok;y++) for(int x=0;x<w;x+=4){ uint8_t v=0; try{v=mem->read_u8(base+(uint32_t)y*rb+x);}catch(...){ok=false;break;} if(v)nz++; }
        if(!ok) continue; nz*=4;
        if(nz>bestNZ){ bestNZ=nz; bestBase=base; bestW=w; bestH=h; bestRB=rb; }
      }
      if(bestBase && bestNZ > (long)(H.fb_w*H.fb_h)/200){
        if(fN==0) fprintf(stderr,"[blitgw] work canvas base=%08X %dx%d rb=%d nz~%ld -> screen\n",bestBase,bestW,bestH,bestRB,bestNZ);
        int ox=(H.fb_w-bestW)/2; if(ox<0)ox=0; int oy=(H.fb_h-bestH)/2; if(oy<0)oy=0;
        int cw=min(bestW,H.fb_w), ch=min(bestH,H.fb_h);
        for(int y=0;y<ch;y++) for(int x=0;x<cw;x++){ uint8_t v=0;
          try{v=mem->read_u8(bestBase+(uint32_t)y*bestRB+x);}catch(...){}
          mem->write_u8(H.g_fb+(uint32_t)(y+oy)*H.fb_w+(x+ox), v); }
      }
    }
    // ADSCANIMG: modules that plot into an engine OFFSCREEN canvas (Points of View —
    // no GWorld, no CopyBits) leave their frame in a buffer the host never flips to
    // g_fb. Scan mapped memory in fb_w*fb_h windows for the densest non-zero block
    // (the plotted illusion) and, if it beats g_fb, blit it in so the frame shows.
    if(g_advance && getenv("ADSCANIMG")){   // skip the offscreen scan/blit on re-send frames
      uint32_t span=(uint32_t)H.fb_w*H.fb_h; uint32_t best=0,bestden=0;
      long cur=0; for(uint32_t i=0;i<span;i++){ uint8_t b=0; try{b=mem->read_u8(H.g_fb+i);}catch(...){} if(b)cur++; }
      bestden=(uint32_t)cur; // baseline: g_fb itself
      // Candidate bases across engine, module, host-alloc, and hi-alloc regions.
      uint32_t rngs[][2] = {{0x10000000,0x10400000},{0x30000000,0x30040000},
                            {0x00100000,0x02000000},{0x50000000,0x50800000},{0x60000000,0x60100000}};
      for(auto& R : rngs) for(uint32_t base=R[0]; base<R[1]; base+=0x800){
        long nz=0; bool ok=true;
        for(uint32_t i=0;i<span;i+=64){ uint8_t b=0; try{b=mem->read_u8(base+i);}catch(...){ok=false;break;} if(b)nz++; }
        if(!ok) continue; nz*=64; // extrapolate sampled density
        if((uint32_t)nz>bestden && (uint32_t)nz>span/50){ bestden=(uint32_t)nz; best=base; }
      }
      if(best && getenv("ADSCANIMG")){
        fprintf(stderr,"[scanimg] frame%d densest offscreen @%08X density~%u (g_fb had %ld)\n",fN,best,bestden,cur);
        if(!getenv("ADSCANNOBLIT")) for(uint32_t i=0;i<span;i++){ uint8_t b=0; try{b=mem->read_u8(best+i);}catch(...){} mem->write_u8(H.g_fb+i,b); }
      }
    }
    BSave _bs=banner_paint(fN);   // composite the timed banner onto this presented frame
    if(const char* fbp=getenv("ADFRAMEDIR"))
      snap_ppm(string(fbp)+"/frame"+to_string(fN)+".ppm", H.g_fb, H.fb_w, H.fb_h, H.fb_w);
    stream_frame();
    banner_restore(_bs);          // restore g_fb — the banner never persists in module state
    // TIMED re-init CYCLE (engine-authentic saver cycling). The real After Dark control panel ran
    // each saver for a while then re-inited on a TIMER — never a stall watcher, and never a
    // freshness gate. Plain ADCYCLE is that model: UNCONDITIONAL re-init at ADCYCLESECS (default
    // 60s = sVal 503), no matter what the module is doing on screen, exactly as the fullscreen
    // blanker tore down whatever was mid-animation. ADCYCLEFRESH=1 selects a non-authentic
    // freshness/grace gate that defers the boundary while the module is still animating (its
    // period defaults to 45s). ADCYCLESECS=n overrides the period either way; ADNOCYCLE disables;
    // ADSTALLSECS=n arms the off-by-default sampled safety net. Inert when none of these are set.
    // The idle-FF above is an orthogonal virtual-TIME mechanism and is independent of this.
    {
      static const bool g_cycle_fresh = getenv("ADCYCLEFRESH");      // non-authentic freshness gate
      static const bool g_cycle_on = (getenv("ADAUTORESTART") || getenv("ADCYCLE")) && !getenv("ADNOCYCLE");
      if(g_cycle_on){
        double now = ad_wall_elapsed();
        static double fr_last_sample=-1e18; static uint64_t fr_last_hash=0; static double fr_last_change=0; static bool fr_init=false; static bool fr_seen_change=false;
        static bool cyc_armed=false; static double cyc_start=0;
        double samp_ms = getenv("ADFRESHMS") ? atof(getenv("ADFRESHMS")) : 500.0;
        if((now-fr_last_sample)*1000.0 >= samp_ms){
          uint64_t sh=1469598103934665603ULL;
          for(int i=0;i<H.fb_w*H.fb_h;i+=64){ sh^=mem->read_u8(H.g_fb+(uint32_t)i); sh*=1099511628211ULL; }
          if(!fr_init){ fr_init=true; fr_last_hash=sh; fr_last_change=now; }
          else if(sh!=fr_last_hash){ fr_last_hash=sh; fr_last_change=now; fr_seen_change=true; }
          fr_last_sample=now;
        }
        if(!cyc_armed && (g_first_drawn || fr_seen_change)){ cyc_armed=true; cyc_start=now; }   // anchor at first draw
        // Authentic default Duration = 60s (sVal 503). The freshness path defaults to 45s.
        double cyc_secs  = getenv("ADCYCLESECS")  ? atof(getenv("ADCYCLESECS"))  : (g_cycle_fresh ? 45.0 : 60.0);
        double cyc_grace = getenv("ADCYCLEGRACE") ? atof(getenv("ADCYCLEGRACE")) : 8.0;
        if(cyc_armed && cyc_secs>0 && (now-cyc_start)>=cyc_secs){
          // Freshness gate (ADCYCLEFRESH): defer the boundary if the module is still animating. The
          // authentic engine has no such gate — the DEFAULT path re-inits UNCONDITIONALLY at the
          // Duration boundary, as the fullscreen blanker tore down whatever was on screen.
          if(g_cycle_fresh && (now-fr_last_change) < cyc_grace){
            if(getenv("ADCYCLELOG")) fprintf(stderr,"[ADCYCLE] (legacy fresh) boundary at %.1fs but module still animating (last fb change %.1fs ago < grace %.1fs) -> defer one period\n", now-cyc_start, now-fr_last_change, cyc_grace);
            cyc_start = now;
          } else {
            const char* g=getenv("ADAR_GEN"); int gen=g?atoi(g):0;
            fprintf(stderr,"[ADCYCLE] cycle-timer fired at frame %d gen %d (%.1fs elapsed since first draw) -> re-exec module init\n",
              fN, gen, now-cyc_start);
            // RESTORE THE REAL STDOUT ONTO fd 1 BEFORE execv.
            // Both transports move the app-facing stdout to a PRIVATE dup and then point
            // fd 1 at /dev/null (see the two `dup(1)` sites above), and the re-exec'd
            // process rebuilds its transport from `dup(1)`. If fd 1 is still /dev/null at
            // execv, the new generation dups /dev/null and writes every frame/ack into it
            // while the app's real pipe survives only as an anonymous inherited descriptor
            // nobody looks at. Under ADSHM that is a DEADLOCK, not just a lost frame: the
            // app blocks waiting for the per-frame ack byte and the host blocks in wait_go()
            // waiting for the next GO, so the screensaver freezes at the first cycle
            // boundary. Both transports must be restored, ADSHM (g_ackf) included.
            // stdout is flushed FIRST, while fd 1 is still /dev/null, so that nothing
            // buffered for the bit bucket can land in the transport we are about to restore.
            fflush(stdout);
            FILE* outf = streamf ? streamf : g_ackf;
            if(outf){ fflush(outf); dup2(fileno(outf), 1); }
            fflush(stderr);
            char nb[32]; snprintf(nb,sizeof(nb),"%d",gen+1); setenv("ADAR_GEN",nb,1);
            char exe[4096]; uint32_t esz=(uint32_t)sizeof(exe); const char* path=argv[0];
            if(_NSGetExecutablePath(exe,&esz)==0) path=exe;
            execv(path, argv);
            fprintf(stderr,"[ADCYCLE] execv(%s) failed: %s — continuing without restart\n", path, strerror(errno));
          }
        }
      }
    }
  }
  // Dump g_fb + every live GWorld after the animation. The module composites
  // into its own GWorlds (screen canvas baseAddr != host g_fb), so the visible
  // toasters land in one of these buffers, not necessarily H.g_fb.
  if(nframes>0) snap_all("final");
  // ADDUMPTOC="0x32C,0x274,..." : dump [main_toc+off] and 32 bytes at the target,
  // to inspect module-global objects (e.g. the sel-dispatch object whose vtable is bad).
  if(const char* dt=getenv("ADDUMPTOC")){
    const char* q=dt;
    while(*q){ uint32_t off=strtoul(q,nullptr,0); uint32_t slot=main_toc+off;
      uint32_t ptr=mem->read_u32b(slot);
      fprintf(stderr,"[dumptoc] toc+%04X @%08X = %08X",off,slot,ptr);
      if(ptr){ fprintf(stderr,"  ->");
        for(int k=0;k<32;k+=4) fprintf(stderr," %08X",mem->read_u32b(ptr+k));
        // if [ptr] is a vtable ptr, dump it too
        uint32_t vt=mem->read_u32b(ptr);
        fprintf(stderr,"\n           [obj]=vt=%08X",vt);
        if(vt) for(int k=0;k<20;k+=4) fprintf(stderr," m%d=%08X",k,mem->read_u32b(vt+k));
      }
      fprintf(stderr,"\n");
      const char* c=strchr(q,','); if(!c) break; q=c+1; }
  }
  // Also dump arbitrary explicit addresses (ADDUMPADDRS="0x3A8000,0x378000").
  if(const char* p=getenv("ADFBDUMP")){
    if(const char* das=getenv("ADDUMPADDRS")){
      int W=atoi(getenv("ADDUMPW")?:"512"), Hh=atoi(getenv("ADDUMPH")?:"384");
      const char* q=das; int i=0;
      while(*q){ uint32_t a=strtoul(q,nullptr,0);
        snap_ppm(string(p)+".addr"+to_string(i++)+"_"+to_string(a>>12)+".ppm", a, W, Hh, W);
        const char* c=strchr(q,','); if(!c) break; q=c+1; }
    }
    // Row-table pixmap dump: ADROWDESC=<descAddr> is a MacOffScreenPixels descriptor
    // whose +0x0A is a ptr to a per-scanline address table (NOT linear pixels). Bounds
    // at +0x0E..+0x14 (top,left,bottom,right). Optional ADPAL=<XPalette*> for correct
    // colors (else screen g_clut); also writes a grayscale (index=brightness) version.
    if(const char* rd=getenv("ADROWDESC")){
      uint32_t desc=strtoul(rd,0,0);
      uint32_t rowTab=mem->read_u32b(desc+0x0A);
      int t=(int16_t)mem->read_u16b(desc+0x0E), l=(int16_t)mem->read_u16b(desc+0x10),
          b=(int16_t)mem->read_u16b(desc+0x12), rr=(int16_t)mem->read_u16b(desc+0x14);
      int W=rr-l, Hh=b-t; if(getenv("ADDUMPW")) W=atoi(getenv("ADDUMPW")); if(getenv("ADDUMPH")) Hh=atoi(getenv("ADDUMPH"));
      uint32_t xpal = getenv("ADPAL")? strtoul(getenv("ADPAL"),0,0) : 0;
      uint32_t ct = xpal? mem->read_u32b(xpal+4) : 0;
      fprintf(stderr,"[adhost] ROWDESC desc=%08X rowTab=%08X bounds=%d,%d,%d,%d W=%d H=%d xpal=%08X ct=%08X\n",
              desc,rowTab,t,l,b,rr,W,Hh,xpal,ct);
      FILE* fc=fopen((string(p)+".rowtab.ppm").c_str(),"wb");
      FILE* fg=fopen((string(p)+".rowtab.gray.ppm").c_str(),"wb");
      if(fc) fprintf(fc,"P6\n%d %d\n255\n",W,Hh);
      if(fg) fprintf(fg,"P6\n%d %d\n255\n",W,Hh);
      long nz=0;
      for(int y=0;y<Hh;y++){ uint32_t row=0; try{row=mem->read_u32b(rowTab+(uint32_t)y*4);}catch(...){row=0;}
        for(int x=0;x<W;x++){ uint8_t idx=0; if(row){ try{idx=mem->read_u8(row+x);}catch(...){idx=0;} } if(idx)nz++;
          if(fg){ fputc(idx,fg);fputc(idx,fg);fputc(idx,fg); }
          uint8_t R,G,B;
          if(ct){ R=mem->read_u16b(ct+8+idx*8+2)>>8; G=mem->read_u16b(ct+8+idx*8+4)>>8; B=mem->read_u16b(ct+8+idx*8+6)>>8; }
          else { uint32_t e=H.g_clut+8+idx*8; R=mem->read_u16b(e+2)>>8; G=mem->read_u16b(e+4)>>8; B=mem->read_u16b(e+6)>>8; }
          if(fc){ fputc(R,fc);fputc(G,fc);fputc(B,fc); } } }
      if(fc)fclose(fc); if(fg)fclose(fg);
      fprintf(stderr,"[adhost] ROWDESC wrote %s.rowtab{,.gray}.ppm (%ld nz of %d)\n",p,nz,W*Hh);
    }
  }

  // ---- 8. dump the screen framebuffer as a color PPM (via the CLUT) ---------
  fprintf(stderr,"[adhost] CopyBits calls: %llu, gworlds: %zu\n",
          (unsigned long long)H.copybits_count, H.gworlds.size());
  // ---- Replicate the After Dark engine run-loop composite: call the engine's
  // Background composite chain (FillMixBuffer -> Validate -> MixToSaveBackCanvas)
  // that the real host invokes each frame but the module's DrawFrame does not.
  // ADVALIDATE="0x3758E0,0x375B20" = Background group addrs. Diffs memory to find
  // which buffer receives sprite pixels.
  if(const char* vg=getenv("ADVALIDATE")){
    uint32_t etv = mem->get_symbol_addr("adxpl510:__initialize");
    uint32_t etoc = etv? mem->read_u32b(etv+4) : 0;
    vector<uint32_t> grps; { const char* q=vg; while(*q){ grps.push_back(strtoul(q,0,0)); const char* c=strchr(q,','); if(!c)break; q=c+1; } }
    // Diagnostic: report each group's sprite array [Bg+0x3C], count [Bg+0x40].
    for(uint32_t g:grps){
      uint32_t arr=mem->read_u32b(g+0x3C); int32_t cnt=(int32_t)mem->read_u32b(g+0x40);
      uint32_t mix=mem->read_u32b(g+0x12), sbc=mem->read_u32b(g+0x16);
      fprintf(stderr,"[adhost] VAL group %08X: spriteArr=%08X count=%d mixCanvas=%08X saveBack=%08X\n",g,arr,cnt,mix,sbc);
    }
    // Optional: force one or more sprites drawable (validation only).
    if(const char* ps=getenv("ADPOKESPRITE")){ const char* q=ps;
      while(*q){ uint32_t sp=strtoul(q,0,0);
        mem->write_u16b(sp+0x44,1); mem->write_u16b(sp+0x30,1); mem->write_u16b(sp+0x32,0);
        mem->write_u16b(sp+0x38,1); mem->write_u16b(sp+0x36,1);
        if(const char* pr=getenv("ADPOKERECT")){ int t=0,l=0,b=48,r=64; sscanf(pr,"%d,%d,%d,%d",&t,&l,&b,&r);
          mem->write_u16b(sp+0x18,(uint16_t)t); mem->write_u16b(sp+0x1A,(uint16_t)l);
          mem->write_u16b(sp+0x1C,(uint16_t)b); mem->write_u16b(sp+0x1E,(uint16_t)r); }
        fprintf(stderr,"[adhost] poked sprite %08X (+0x44=1 +0x30=1 +0x38=1)\n",sp);
        const char* c=strchr(q,','); if(!c)break; q=c+1; } }
    auto blocks = mem->allocated_blocks();
    auto csum=[&](uint32_t a,uint32_t sz)->uint32_t{ uint32_t s=0; for(uint32_t i=0;i<sz;i+=16){ uint8_t v=0; try{v=mem->read_u8(a+i);}catch(...){} s=s*131+v; } return s; };
    vector<uint32_t> before; for(auto&b:blocks) before.push_back(csum(b.first,b.second));
    uint32_t GetArr =strtoul(getenv("ADGETARR")?:"0x1001B540",0,0);
    uint32_t FillMix=strtoul(getenv("ADFILLMIX")?:"0x1001B774",0,0);
    uint32_t Valid  =strtoul(getenv("ADVALFN")?:"0x1001B8C0",0,0);
    uint32_t MixSBC =strtoul(getenv("ADMIXFN")?:"0x1001A970",0,0);
    for(uint32_t g:grps){
      // 1. Build the flat draw-array [Bg+0x3C] from the sprite linked list.
      call_code(GetArr, etoc, {g}, "GetSpriteArray");
      uint32_t arr=mem->read_u32b(g+0x3C); int cnt=(int16_t)mem->read_u16b(g+0x40);
      fprintf(stderr,"[adhost] VAL after GetSpriteArray: [Bg+0x3C]=%08X count=%d\n",arr,cnt);
      if(arr && cnt>0){
        // Scan for sprites with a non-null vtable (genuinely initialized/active).
        int active=0, firstActive=-1;
        for(int k=0;k<cnt;k++){ uint32_t sp=mem->read_u32b(arr+k*4);
          if(sp && mem->read_u32b(sp+0x00)){ active++; if(firstActive<0) firstActive=k; } }
        fprintf(stderr,"[adhost] VAL array: %d/%d sprites have non-null vtable (firstActive=%d)\n",active,cnt,firstActive);
        for(int k=0;k<4 && k<cnt;k++){ uint32_t sp=mem->read_u32b(arr+k*4);
          fprintf(stderr,"    sprite[%d]=%08X vt=%08X bounds=%d,%d,%d,%d +0x30(vis)=%d +0x38(dirty)=%d +0x40(src)=%08X +0x44(frame)=%d\n",
            k,sp,mem->read_u32b(sp),(int16_t)mem->read_u16b(sp+0x18),(int16_t)mem->read_u16b(sp+0x1A),
            (int16_t)mem->read_u16b(sp+0x1C),(int16_t)mem->read_u16b(sp+0x1E),
            (int16_t)mem->read_u16b(sp+0x30),(int16_t)mem->read_u16b(sp+0x38),mem->read_u32b(sp+0x40),(int16_t)mem->read_u16b(sp+0x44)); }
        if(firstActive>=0){ uint32_t sp=mem->read_u32b(arr+firstActive*4);
          fprintf(stderr,"    firstActive sprite[%d]=%08X vt=%08X bounds=%d,%d,%d,%d vis=%d dirty=%d src=%08X frame=%d\n",
            firstActive,sp,mem->read_u32b(sp),(int16_t)mem->read_u16b(sp+0x18),(int16_t)mem->read_u16b(sp+0x1A),
            (int16_t)mem->read_u16b(sp+0x1C),(int16_t)mem->read_u16b(sp+0x1E),
            (int16_t)mem->read_u16b(sp+0x30),(int16_t)mem->read_u16b(sp+0x38),mem->read_u32b(sp+0x40),(int16_t)mem->read_u16b(sp+0x44)); }
        // Optional: force the first N sprites visible+dirty+framed and lay them out
        // in a grid across the canvas (they have no position: bounds=0,0,0,0), so
        // Validate actually composites them. ADPOKEALL=N (count), ADPOKEFRAME=f.
        if(getenv("ADPOKEALL")){ int fr=atoi(getenv("ADPOKEFRAME")?:"1");
          int N=atoi(getenv("ADPOKEALL")); if(N<=0||N>cnt) N=cnt;
          int sw=atoi(getenv("ADPOKESW")?:"48"), sh=atoi(getenv("ADPOKESH")?:"48");
          int cols=512/(sw+8); if(cols<1)cols=1;
          for(int k=0;k<N;k++){ uint32_t sp=mem->read_u32b(arr+k*4); if(!sp) continue;
            int col=k%cols, rowi=k/cols;
            int x=8+col*(sw+8), y=8+rowi*(sh+8);
            if(y+sh>384){ break; }
            mem->write_u16b(sp+0x18,(uint16_t)y);  mem->write_u16b(sp+0x1A,(uint16_t)x);       // top,left
            mem->write_u16b(sp+0x1C,(uint16_t)(y+sh)); mem->write_u16b(sp+0x1E,(uint16_t)(x+sw)); // bottom,right
            mem->write_u16b(sp+0x30,1); mem->write_u16b(sp+0x32,0);
            mem->write_u16b(sp+0x38,1); mem->write_u16b(sp+0x36,1);
            mem->write_u16b(sp+0x44,(uint16_t)fr); }
          fprintf(stderr,"[adhost] VAL poked first %d sprites into grid (%dx%d each) frame=%d\n",N,sw,sh,fr); }
      }
      // Proper engine-based activation (crash-free): for the first N pool sprites,
      // set the XPt anchor then call MoveIntoNextSequence(sp, seqIdx) so the frame's
      // blit descriptor + bounds are set consistently, then mark visible+dirty.
      if(arr && cnt>0 && getenv("ADACTIVATE")){
        int N=atoi(getenv("ADACTIVATE")); if(N<=0||N>cnt)N=cnt;
        uint32_t GetFF=strtoul(getenv("ADGETFF")?:"0x10020A5C",0,0);
        uint32_t MoveInto=strtoul(getenv("ADMOVEINTO")?:"0x100203A8",0,0);
        uint32_t sp0=mem->read_u32b(arr);
        int goodn=-1; int probes[]={1,2,3,4,5,0x4F,0x51,6,7,8};
        for(int n:probes){ uint32_t r=call_code(GetFF, etoc, {sp0,(uint32_t)n}, "GetSeqFirstFrame");
          fprintf(stderr,"    probe GetSeqFirstFrame(%08X,%d)=%08X\n",sp0,n,r);
          if(r && r!=0xDEADBEEF){ goodn=n; break; } }
        if(goodn<0){ goodn=atoi(getenv("ADSEQIDX")?:"1"); }
        if(getenv("ADSEQIDX")) goodn=atoi(getenv("ADSEQIDX"));
        fprintf(stderr,"[adhost] activating %d sprites with seqIdx=%d\n",N,goodn);
        int sw=48,sh=48,cols=512/(sw+8); if(cols<1)cols=1;
        for(int k=0;k<N;k++){ uint32_t sp=mem->read_u32b(arr+k*4); if(!sp) continue;
          int col=k%cols,rowi=k/cols; int x=32+col*(sw+8), y=32+rowi*(sh+8); if(y+sh>360) break;
          mem->write_u16b(sp+0x4A,(uint16_t)x); mem->write_u16b(sp+0x4C,(uint16_t)y);
          call_code(MoveInto, etoc, {sp,(uint32_t)goodn}, k<2?"MoveIntoNextSequence":nullptr);
          mem->write_u16b(sp+0x30,1); mem->write_u16b(sp+0x32,0); mem->write_u16b(sp+0x38,1);
          if(k<3) fprintf(stderr,"    activated sprite[%d]=%08X @(%d,%d) frame=%d bounds=%d,%d,%d,%d\n",
            k,sp,x,y,(int16_t)mem->read_u16b(sp+0x44),(int16_t)mem->read_u16b(sp+0x18),
            (int16_t)mem->read_u16b(sp+0x1A),(int16_t)mem->read_u16b(sp+0x1C),(int16_t)mem->read_u16b(sp+0x1E)); }
      }
      if(!getenv("ADNOFILL")) call_code(FillMix, etoc, {g}, "FillMixBuffer");
      call_code(Valid, etoc, {g}, "Validate");
      if(!getenv("ADNOMIX")) call_code(MixSBC, etoc, {g}, "MixToSaveBackCanvas");
    }
    // Dump the composited canvases AFTER the Validate composite (linear 8-bit, rb=512).
    if(const char* p=getenv("ADFBDUMP")){
      snap_ppm(string(p)+".val.fb.ppm",  H.g_fb, H.fb_w, H.fb_h, H.fb_w);
      snap_ppm(string(p)+".val.378.ppm", 0x378000, 512, 384, 512);
      snap_ppm(string(p)+".val.3a8.ppm", 0x3A8000, 512, 384, 512);
      // Colorized dump using the module's own XPalette (from the mix canvas), not g_clut.
      // GetPalette: xpal=[canvas+0x5A] if [canvas+0x54]!=0 else walk parent [canvas+0x56].
      uint32_t g0=grps.empty()?0:grps[0];
      uint32_t canvas=g0?mem->read_u32b(g0+0x12):0;
      uint32_t xpal=0;
      for(int hop=0; canvas && hop<4; hop++){
        if(mem->read_u16b(canvas+0x54)){ xpal=mem->read_u32b(canvas+0x5A); break; }
        canvas=mem->read_u32b(canvas+0x56);
      }
      if(getenv("ADPAL")) xpal=strtoul(getenv("ADPAL"),0,0);
      uint32_t ct = xpal? mem->read_u32b(xpal+4) : 0;
      fprintf(stderr,"[adhost] VAL palette: mixCanvas=%08X xpal=%08X colorTable=%08X count=%d\n",
              mem->read_u32b(g0+0x12),xpal,ct,ct?mem->read_u16b(xpal+8):0);
      if(ct){
        FILE* f=fopen((string(p)+".val.color.ppm").c_str(),"wb");
        if(f){ fprintf(f,"P6\n512 384\n255\n");
          for(int y=0;y<384;y++) for(int x=0;x<512;x++){ uint8_t idx=mem->read_u8(0x378000+(uint32_t)y*512+x);
            uint32_t e=ct+8+idx*8; fputc(mem->read_u16b(e+2)>>8,f); fputc(mem->read_u16b(e+4)>>8,f); fputc(mem->read_u16b(e+6)>>8,f); }
          fclose(f); fprintf(stderr,"[adhost] wrote %s.val.color.ppm\n",p); }
      }
    }
    size_t i=0; for(auto&b:blocks){ uint32_t now=csum(b.first,b.second);
      if(now!=before[i]){ long nz=0; for(uint32_t k=0;k<b.second;k++){ try{if(mem->read_u8(b.first+k))nz++;}catch(...){}}
        fprintf(stderr,"[adhost] VAL CHANGED BLOCK %08X size=%u (%ld nz, %.0f%%)\n",b.first,b.second,nz,100.0*nz/b.second); }
      i++; }
  }
  if(getenv("ADSCANMEM")){
    // Report allocated blocks that hold substantial non-zero content — this
    // locates where the module actually composited pixels (offscreen buffers).
    for(auto& blk : mem->allocated_blocks()){
      uint32_t a=blk.first, sz=blk.second; if(sz<4096) continue;
      long nz=0; for(uint32_t i=0;i<sz;i++){ if(mem->read_u8(a+i)) nz++; }
      if(nz*20 > (long)sz) // >5% non-zero
        fprintf(stderr,"[adhost]   BLOCK %08X size=%u (%ld non-zero, %.0f%%)\n",
                a,sz,nz,100.0*nz/sz);
    }
  }
  if(const char* fbpath=getenv("ADFBDUMP")){
    // Helper: dump an 8-bit pixmap-backed buffer as a color PPM via the CLUT.
    auto dumpbuf=[&](const string& path, uint32_t base, int W, int Hh, int rowBytes){
      FILE* f=fopen(path.c_str(),"wb"); if(!f) return;
      fprintf(f,"P6\n%d %d\n255\n",W,Hh);
      long nonzero=0;
      for(int y=0;y<Hh;y++) for(int x=0;x<W;x++){
        uint8_t idx=mem->read_u8(base + (uint32_t)y*rowBytes + x);
        if(idx) nonzero++;
        uint32_t e=H.g_clut+8+idx*8;
        uint8_t rr=mem->read_u16b(e+2)>>8, gg=mem->read_u16b(e+4)>>8, bb=mem->read_u16b(e+6)>>8;
        fputc(rr,f); fputc(gg,f); fputc(bb,f);
      }
      fclose(f);
      fprintf(stderr,"[adhost]   %s (%ld non-zero px of %d)\n",path.c_str(),nonzero,W*Hh);
    };
    dumpbuf(fbpath, H.g_fb, H.fb_w, H.fb_h, H.fb_w);
    // Also dump each offscreen GWorld (the sprite/background buffers).
    for(size_t i=0;i<H.gworlds.size();i++){
      uint32_t port=H.gworlds[i];
      uint32_t pmh=mem->read_u32b(port+2), pm=mem->read_u32b(pmh);
      uint32_t base=mem->read_u32b(pm+0); int rb=mem->read_u16b(pm+4)&0x3FFF;
      int t=(int16_t)mem->read_u16b(pm+6),l=(int16_t)mem->read_u16b(pm+8),
          b=(int16_t)mem->read_u16b(pm+10),rr=(int16_t)mem->read_u16b(pm+12);
      dumpbuf(string(fbpath)+".gw"+to_string(i)+".ppm", base, rr-l, b-t, rb);
    }
  }
  return 0;
}
