CXX      ?= c++
CXXFLAGS ?= -O2 -std=c++17 -Wall -Wextra

# macOS workaround: some Command Line Tools installs are missing headers in
# /Library/Developer/CommandLineTools/usr/include/c++/v1. Fall back to the
# SDK's c++/v1 only when the compiler cannot find <cstdint> on its own (a
# Homebrew LLVM compiler has its own headers and breaks if given the SDK's).
# No-op on Linux.
UNAME_S := $(shell uname -s)
ifeq ($(UNAME_S),Darwin)
    CXX_HEADERS_OK := $(shell printf 'int main(){return 0;}' | $(CXX) -x c++ -std=c++17 -include cstdint -fsyntax-only - >/dev/null 2>&1 && echo yes)
    ifneq ($(CXX_HEADERS_OK),yes)
        SDK_CXX_INC := $(shell xcrun --sdk macosx --show-sdk-path 2>/dev/null)/usr/include/c++/v1
        ifneq ($(wildcard $(SDK_CXX_INC)/cstdint),)
            CXXFLAGS += -isystem $(SDK_CXX_INC)
        endif
    endif
endif

.PHONY: all foundation solver tools instances test dataset-check check-data clean

all: foundation

# The starting point: a random cut plus greedy local improvement.
foundation: maxcut_foundation.cpp
	$(CXX) $(CXXFLAGS) -o foundation maxcut_foundation.cpp

# Your solver. Start with:  cp maxcut_foundation.cpp solver.cpp
solver: solver.cpp
	$(CXX) $(CXXFLAGS) -o solver solver.cpp

# --- instances ---------------------------------------------------------------

# The six scored graphs (25 MB) are fetched from the GitHub Release and
# checked against checksums/scored.sha256. The dev graphs are in the repo.
instances:
	scripts/download.sh

# --- instructor / tooling targets ------------------------------------------

# The generator and the SDP upper-bound tool. Needed to make your own
# practice instances with a bound.
tools: tools/gen/gen tools/bound/bound
tools/gen/gen: tools/gen/gen.cpp
	$(CXX) $(CXXFLAGS) -o tools/gen/gen tools/gen/gen.cpp
tools/bound/bound: tools/bound/bound.cpp
	$(CXX) $(CXXFLAGS) -o tools/bound/bound tools/bound/bound.cpp

# The reference solver (instructor only; the directory is gitignored).
tools/ref/solvers/refsolve: tools/ref/solvers/refsolve.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

# Regression tests for grade.py (cut verification, the failure accounting,
# the enforced time/memory/thread limits).
test: foundation
	python3 tools/test_grade.py

# Three quick dataset checks: the six v1.2 graphs regenerate exactly,
# scripts/download.sh recovers and aborts correctly (offline), and the six
# scored graphs agree across instances.txt, metas, checksums and download.sh.
dataset-check: tools/gen/gen
	python3 tools/test_dataset.py

# Verify every instance against its meta file (digest, n/m, the stored bound
# is not below the reference cut, the bound tool reproduces it) -- slow.
check-data: tools
	python3 tools/pipeline.py check

clean:
	rm -f foundation solver tools/gen/gen tools/bound/bound tools/ref/solvers/refsolve
