# Dependencies live in the repo (third_party/), NOT /private/tmp: the temp scratchpad
# gets purged, which silently broke this build. Rebuild deps with the lines in
# ../adhost/Makefile.
TP=$(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../third_party)
RD=$(TP)/resource_dasm
PH=$(TP)/local
CXX=clang++
CXXFLAGS=-std=c++20 -O2 -I$(RD)/src -I$(PH)/include
LIBS=$(RD)/build/libresource_file.a $(PH)/lib/libphosg.a -lz
adblit: adblit.cc
	$(CXX) $(CXXFLAGS) adblit.cc $(LIBS) -o adblit
