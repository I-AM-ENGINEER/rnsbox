# Host tests for the fast terminal path.
#
#   make            build the unit tests, the fake TUI and the broker
#   make test       run the C++ tests and the Python end-to-end suite
#
# Everything runs on the host (x86_64 Linux / WSL); no Buildroot needed.
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra
INC      := -I../../src/cgi
TERM_SRC := ../../src/cgi/term/screen.cpp ../../src/cgi/term/websocket.cpp ../../src/cgi/term/token.cpp
BROKER_SRC := ../../src/cgi/console_broker.cpp $(TERM_SRC)
BUILD    := build
NATIVE   := /tmp/rnsbox-term-bin

all: $(BUILD)/test_screen $(BUILD)/test_token $(BUILD)/test_ws $(BUILD)/fake_tui $(BUILD)/termd

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/test_screen: test_screen.cpp ../../src/cgi/term/screen.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $^ -o $@

$(BUILD)/test_token: test_token.cpp ../../src/cgi/term/token.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $^ -lcrypto -o $@

$(BUILD)/test_ws: test_ws.cpp ../../src/cgi/term/websocket.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $^ -lcrypto -o $@

$(BUILD)/fake_tui: fake_tui.cpp | $(BUILD)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BUILD)/termd: termd_main.cpp $(BROKER_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(INC) $^ -lcrypto -o $@

# The production build uses these flags; keep the broker compiling with them.
strict: | $(BUILD)
	$(CXX) -std=c++17 -Os -Wall -Wextra -fno-exceptions -fno-rtti $(INC) \
		termd_main.cpp $(BROKER_SRC) -lcrypto -o $(BUILD)/termd-strict

test: all strict
	./$(BUILD)/test_screen
	./$(BUILD)/test_token
	./$(BUILD)/test_ws
	# exec() from /mnt/c (9p) can take seconds; run the broker and the TUI
	# from a native directory so the timing tests are meaningful.
	mkdir -p $(NATIVE)
	cp $(BUILD)/termd $(BUILD)/fake_tui $(NATIVE)/
	TERMD_BIN=$(NATIVE)/termd FAKE_TUI=$(NATIVE)/fake_tui \
		python3 -m pytest -q test_e2e.py

bench: all
	mkdir -p $(NATIVE)
	cp $(BUILD)/termd $(BUILD)/fake_tui $(NATIVE)/
	TERMD_BIN=$(NATIVE)/termd FAKE_TUI=$(NATIVE)/fake_tui python3 bench.py

clean:
	rm -rf $(BUILD) .pytest_cache __pycache__

.PHONY: all test strict bench clean
