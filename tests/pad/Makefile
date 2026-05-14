CXX      := g++
REPO     := $(realpath ../..)
CPP      := $(REPO)/app/src/main/cpp
STUBS    := $(realpath .)stubs
GTEST    := $(CPP)/3rdparty/googletest/googletest
FMT      := $(CPP)/3rdparty/fmt/include

CXXFLAGS := -std=c++20 -O0 -g \
            -DFMT_HEADER_ONLY \
            -I$(realpath .)/stubs \
            -I$(CPP)/pcsx2 \
            -I$(CPP) \
            -I$(FMT) \
            -I$(GTEST)/include \
            -Wall -Wno-unused-function -Wno-unused-variable

SRCS := \
  $(CPP)/pcsx2/SIO/Pad/PadDualshock2.cpp \
  $(CPP)/pcsx2/SIO/Pad/PadBase.cpp \
  $(GTEST)/src/gtest-all.cc \
  $(GTEST)/src/gtest_main.cc \
  stubs.cpp \
  PressureModifierTests.cpp

OBJS := $(patsubst %.cpp,%.o,$(patsubst %.cc,%.o,$(SRCS)))

.PHONY: all run clean

all: pad_tests

pad_tests: $(OBJS)
	$(CXX) $(CXXFLAGS) -I$(GTEST) $^ -o $@ -lpthread

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

%.o: %.cc
	$(CXX) $(CXXFLAGS) -I$(GTEST) -c $< -o $@

run: pad_tests
	./pad_tests

clean:
	rm -f pad_tests $(OBJS)
