CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O3 -Wall -Wextra
BIN      := bin

all: $(BIN)/kcs $(BIN)/stream_tool $(BIN)/qtime $(BIN)/community_build $(BIN)/hier_index

$(BIN):
	mkdir -p $(BIN)

# edge core index: Sweep (--nofilter), Sweep+ (--hist), Sweep* (default)
$(BIN)/kcs: src/kcs.cpp src/kcs_stream_format.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -iquote src -o $@ src/kcs.cpp

# turns the construction stream into the edge-major query layout
$(BIN)/stream_tool: src/stream_tool.cpp src/kcs_stream_format.hpp src/kcs_index_format.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -iquote src -o $@ src/stream_tool.cpp

# point queries, onset queries, and every edge's core number at one Delta
$(BIN)/qtime: src/qtime.cpp src/kcs_index_format.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -iquote src -o $@ src/qtime.cpp

# community index: the merge pairs U
$(BIN)/community_build: src/community_build.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ src/community_build.cpp

# community queries over the merge pairs, against a traversal of the graph
$(BIN)/hier_index: src/hier_index.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ src/hier_index.cpp

clean:
	rm -rf $(BIN)

.PHONY: all clean
