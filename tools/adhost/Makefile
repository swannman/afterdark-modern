# Dependencies live in the repo (third_party/), NOT /private/tmp: the temp scratchpad gets purged, which
# silently broke the build (libresource_file.a + libphosg.a vanished). Rebuild deps with:
#   cd ../../third_party/phosg && cmake -S . -B build -DCMAKE_INSTALL_PREFIX=../local && cmake --build build -j8 && cmake --install build
#   cd ../resource_dasm && cmake -S . -B build -DCMAKE_PREFIX_PATH=../local && cmake --build build --target resource_file m68kdasm resource_dasm -j8
# NOTE (addm 610, 2026-08-06): the former LOCAL emulator patches are UPSTREAM as of PR #95 (merged;
# resource_dasm master >= 195d7fc carries fctiwz saturation, ADDQ/SUBQ-An, 68020 full extension words
# incl. memory-indirect, OR/NOT Z-masking, and roxl/roxr — partly reworked in maintainer style).
# NO local patches are needed after a re-clone anymore: plain `git clone` + the cmake line above suffices.
# Historical diffs kept for reference: tools/adhost/scratchpad/m68k_68020_full.patch, m68k_roxl_roxr.patch,
# pre610_local_emulator_patches.diff (the exact working-tree state dropped at the addm-610 swap).
# The host-side fctiwz shim in adhost.cc is RETAINED despite the emulator fix: shim-off changes Swirling
# Magic's output (d438 shim vs d446 emulator, reproducible) — the shim is the field-validated behavior;
# reconcile which path is hardware-correct before dropping (ADNOFCTIWZFIX A/Bs it).
# NOTE (addm 413): the 68020 long mulu.l/muls.l/divu.l/divs.l forms (32- and 64-bit, incl. immediate/
# register-direct sources) are ALREADY correct in M68KEmulator.cc (~line 1735) — do NOT re-chase them.
# Time Flies' "frozen 68020 divl clock" was a HOST bug, not the emulator: _Microseconds (A193) returns the
# 64-bit count in REGISTERS A0(high):D0(low); the Pascal VAR glue STORES A0/D0 into the caller's buffer.
# The handler only wrote the buffer (leaving A0/D0 stale garbage), so the glue overwrote the correct value.
# Fix is host-only (adhost68k.cc A193 handler now sets A0/D0). NO emulator/lib change. Flipped Time Flies +
# Shadow Agents (both genuine clock-driven); the canonical m68k_68020_full.patch is UNCHANGED by addm 413.
TP=$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../third_party)
RD=$(TP)/resource_dasm
PH=$(TP)/local
CXX=clang++
CXXFLAGS=-std=c++20 -O2 -I$(RD)/src -I$(PH)/include
# addm 797: -framework ApplicationServices for CGEventSourceFlagsState (ADREALCAPS)
LIBS=$(RD)/build/libresource_file.a $(PH)/lib/libphosg.a -lz -framework ApplicationServices
adhost: adhost.cc
	$(CXX) $(CXXFLAGS) adhost.cc $(LIBS) -o adhost

adhost68k: adhost68k.cc ad_toolbox.cc ad_toolbox.hh
	$(CXX) $(CXXFLAGS) adhost68k.cc ad_toolbox.cc $(LIBS) -o adhost68k

# Lane-D working copy (addm 702: Modern Art Rothko _Pack12 autoPop HSV2RGB).
adhost68k_fd: adhost68k_fd.cc ad_toolbox.cc ad_toolbox.hh
	$(CXX) $(CXXFLAGS) adhost68k_fd.cc ad_toolbox.cc $(LIBS) -o adhost68k_fd

# Lane working copy (addm 756: Rodger Dodger play field).
adhost68k_rd2: adhost68k_rd2.cc ad_toolbox.cc ad_toolbox.hh
	$(CXX) $(CXXFLAGS) adhost68k_rd2.cc ad_toolbox.cc $(LIBS) -o adhost68k_rd2

adhost68k_rd3: adhost68k_rd3.cc ad_toolbox.cc ad_toolbox.hh
	$(CXX) $(CXXFLAGS) adhost68k_rd3.cc ad_toolbox.cc $(LIBS) -o adhost68k_rd3
