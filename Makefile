CXX      ?= c++
CXXFLAGS ?= -std=c++17 -O3 -march=native -Wall -Wextra
BIN      := bin

all: $(BIN)/kcs $(BIN)/hier_index $(BIN)/stream_tool $(BIN)/kcs_pack $(BIN)/kcs_verify

$(BIN):
	mkdir -p $(BIN)

$(BIN)/kcs: src/kcs.cpp src/kcs_stream_format.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -iquote src -o $@ src/kcs.cpp

$(BIN)/hier_index: src/hier_index.cpp | $(BIN)
	$(CXX) $(CXXFLAGS) -o $@ src/hier_index.cpp

$(BIN)/stream_tool: src/stream_tool.cpp src/kcs_stream_format.hpp \
                    src/kcs_index_format.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -iquote src -o $@ src/stream_tool.cpp

$(BIN)/kcs_pack: src/kcs_pack.cpp src/kcs_format.hpp src/kcs_reader.hpp \
                 src/kcs_index_format.hpp src/kcs_stream_format.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -iquote src -o $@ src/kcs_pack.cpp

$(BIN)/kcs_verify: src/kcs_verify.cpp src/kcs_format.hpp src/kcs_reader.hpp | $(BIN)
	$(CXX) $(CXXFLAGS) -iquote src -o $@ src/kcs_verify.cpp

clean:
	rm -rf $(BIN)

.PHONY: all clean
