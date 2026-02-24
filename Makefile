# Convenience wrapper around CMake.
# The real build system is CMakeLists.txt -- this just provides short targets.
#
# Usage:
#   make              - incremental build (default/last configured type)
#   make debug        - configure Debug + build
#   make release      - configure Release + build
#   make sanitize     - configure Debug + sanitizers + build
#   make reldbg       - configure RelWithDebInfo + build
#   make clean        - clean build artifacts
#   make rebuild      - clean + build (equivalent of make clean all)
#   make test         - run pytest suite
#   make docs         - generate Sphinx + Doxygen documentation
#   make doxygen      - generate Doxygen documentation only
#   make cppcheck     - run cppcheck static analysis
#   make cppcheck-deep - run exhaustive cppcheck analysis
#   make venv         - create Python venv and install pytest
#   make deps         - print required/optional dependencies
#   make help         - show this list

BUILD_DIR := build

.PHONY: all debug release sanitize reldbg clean rebuild test docs doxygen cppcheck cppcheck-deep venv deps help

all:
	@cmake -B $(BUILD_DIR)
	@cmake --build $(BUILD_DIR)

debug:
	@cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=OFF
	@cmake --build $(BUILD_DIR)

release:
	@cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release -DENABLE_SANITIZERS=OFF
	@cmake --build $(BUILD_DIR)

sanitize:
	@cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DENABLE_SANITIZERS=ON
	@cmake --build $(BUILD_DIR)

reldbg:
	@cmake -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_SANITIZERS=OFF
	@cmake --build $(BUILD_DIR)

clean:
	@cmake --build $(BUILD_DIR) --target clean 2>/dev/null || true

rebuild:
	@cmake --build $(BUILD_DIR) --clean-first

test:
	@python3 -m venv .venv
	@.venv/bin/pip install -q pytest
	@cmake -B $(BUILD_DIR)
	@cmake --build $(BUILD_DIR) --target test

docs:
	@python3 -m venv .venv
	@.venv/bin/pip install -q sphinx breathe myst-parser
	@cmake -B $(BUILD_DIR)
	@cmake --build $(BUILD_DIR) --target docs-sphinx

doxygen:
	@cmake --build $(BUILD_DIR) --target docs-doxygen

cppcheck:
	@cmake --build $(BUILD_DIR) --target cppcheck

cppcheck-deep:
	@cmake --build $(BUILD_DIR) --target cppcheck-deep

venv:
	@python3 -m venv .venv
	@.venv/bin/pip install pytest sphinx breathe myst-parser
	@echo "Venv created. Reconfigure: cmake -B $(BUILD_DIR)"

deps:
	@echo "Project dependency guide"
	@echo ""
	@echo "Required (build + run + tests):"
	@echo "  - C++ compiler with C++17 support (g++)"
	@echo "  - CMake >= 3.14"
	@echo "  - POSIX runtime libs (pthread, rt) [usually provided by libc/dev toolchain]"
	@echo "  - Python 3.10+ (tested on 3.10-3.14)"
	@echo "  - python3-venv"
	@echo "  - pip package: pytest (installed by 'make test')"
	@echo ""
	@echo "Optional (documentation):"
	@echo "  - doxygen"
	@echo "  - sphinx-build"
	@echo "  - pip packages: sphinx, breathe, myst-parser (installed by 'make docs' into .venv)"
	@echo ""
	@echo "Optional (static analysis):"
	@echo "  - cppcheck"
	@echo ""
	@echo "Debian/Ubuntu quick install (base):"
	@echo "  sudo apt update && sudo apt install -y build-essential cmake python3 python3-venv"
	@echo ""
	@echo "Debian/Ubuntu optional extras:"
	@echo "  sudo apt install -y doxygen sphinx-doc cppcheck"

help:
	@echo "This is a simple Makefile wrapper around CMake. Available targets:"
	@echo "  all       - incremental build (default configuration)"
	@echo "  debug     - Debug build (-g -O0, for GDB)"
	@echo "  release   - Release build (-O3, for production)"
	@echo "  sanitize  - Debug + AddressSanitizer + UBSan"
	@echo "  reldbg    - RelWithDebInfo (-O2 -g, for profiling)"
	@echo "  clean     - remove build artifacts"
	@echo "  rebuild   - clean + rebuild"
	@echo "  test      - run pytest integration tests"
	@echo "  docs      - generate Sphinx + Doxygen documentation"
	@echo "  doxygen   - generate Doxygen documentation only"
	@echo "  cppcheck  - run cppcheck static analysis"
	@echo "  cppcheck-deep - run exhaustive cppcheck static analysis"
	@echo "  venv      - create .venv and install Python dependencies"
	@echo "  deps      - print required/optional dependencies"
	@echo "  help      - show this help"
