# Depends on resource_dasm + phosg checked out under third_party/ (NOT a temp dir —
# the build links their static libraries in place). To build them:
#   cd ../../third_party/phosg && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=../local && cmake --build build -j8 && cmake --install build
#   cd ../resource_dasm && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=../local && cmake --build build --target resource_file m68kdasm resource_dasm -j8
# CMAKE_BUILD_TYPE=Release is LOAD-BEARING: cmake's default is empty = -O0, and an
# unoptimized emulator runs every module ~5x slower (18.7s -> 3.8s Flying Toasters
# init; 541ms -> ~100ms Swirling Magic frames). Output is byte-identical either way.
TP=$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../third_party)
RD=$(TP)/resource_dasm
PH=$(TP)/local
CXX=clang++
CXXFLAGS=-std=c++20 -O2 -I$(RD)/src -I$(PH)/include
# ApplicationServices provides CGEventSourceFlagsState (real Caps Lock state for modules that poll KeyMap).
LIBS=$(RD)/build/libresource_file.a $(PH)/lib/libphosg.a -lz -framework ApplicationServices

EMULIB=$(RD)/build/libresource_file.a

adhost: adhost.cc $(EMULIB)
	$(CXX) $(CXXFLAGS) adhost.cc $(LIBS) -o adhost

adhost68k: adhost68k.cc ad_toolbox.cc ad_toolbox.hh $(EMULIB)
	$(CXX) $(CXXFLAGS) adhost68k.cc ad_toolbox.cc $(LIBS) -o adhost68k
