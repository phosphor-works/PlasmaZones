# SPDX-FileCopyrightText: 2026 fuddlesworth
# SPDX-License-Identifier: GPL-3.0-or-later
#
# PlasmaZones: the tasks the CMake presets do not cover. Configure, build and
# test with the presets (CMakePresets.json, shared by every phosphor-works
# repository):
#
#   cmake --workflow --preset dev      configure, build and test (Debug, build/)
#   cmake --workflow --preset release  the same in build-release/
#   cmake --workflow --preset asan     with ASan and UBSan, in build-asan/
#
# Usage:
#   make              - Configure and build with the dev preset
#   make install      - Install build/ to the system (requires sudo)
#   make post-install - Refresh the KDE service cache after an install
#   make uninstall    - Uninstall from the system (requires sudo)
#   make run-editor   - Build and run the editor
#   make run-daemon   - Build and run the daemon
#   make format       - Format C++ and QML

# The dev preset's build directory.
BUILD_DIR := build
# Parallel jobs for the build. Empty: CMake's default.
JOBS ?=
JOBS_FLAG := $(if $(JOBS),-j$(JOBS))

# Auto-detect color support
ifneq ($(TERM),)
  ifneq ($(TERM),dumb)
    BLUE := $(shell printf '\033[1;34m')
    GREEN := $(shell printf '\033[1;32m')
    YELLOW := $(shell printf '\033[1;33m')
    RED := $(shell printf '\033[1;31m')
    NC := $(shell printf '\033[0m')
  endif
endif

.PHONY: all configure install post-install uninstall run-editor run-daemon help format format-cpp format-qml

# Default target: configure and build with the dev preset.
all: configure
	@echo "$(BLUE)>>> Building...$(NC)"
	@cmake --build --preset dev $(JOBS_FLAG)
	@echo "$(GREEN)>>> Build complete. Binaries in: $(BUILD_DIR)/bin/$(NC)"

configure:
	@cmake --preset dev

# Install to system (only installs files; packaging handles sycoca/service enable).
# Deliberately does NOT depend on a build: the documented invocation is
# `sudo make install`, and building under sudo leaves root-owned objects in
# build/ that break the user's next unprivileged make. Build first, then install.
install:
	@if [ ! -d $(BUILD_DIR) ]; then \
		echo "$(RED)>>> No build found. Run 'make' first, then 'sudo make install'.$(NC)"; \
		exit 1; \
	fi
	@echo "$(YELLOW)>>> Installing (may require sudo)...$(NC)"
	@cmake --install $(BUILD_DIR)
	@echo "$(GREEN)>>> Installation complete$(NC)"

# Optional: refresh KDE sycoca after local install (packaging does this for distro packages)
post-install:
	@echo "$(BLUE)>>> Refreshing KDE service cache...$(NC)"
	@$(SHELL) cmake/post-install-sycoca.sh
	@echo "$(GREEN)>>> Done. PlasmaZones should now appear in System Settings.$(NC)"

# Uninstall from system (uses full CMake uninstall: manifest + all known /usr paths)
uninstall:
	@if [ -f $(BUILD_DIR)/cmake_uninstall.cmake ]; then \
		echo "$(YELLOW)>>> Uninstalling (may require sudo)...$(NC)"; \
		cmake --build $(BUILD_DIR) --target uninstall; \
		echo "$(GREEN)>>> Uninstall complete$(NC)"; \
	elif [ -f $(BUILD_DIR)/install_manifest.txt ]; then \
		echo "$(YELLOW)>>> Uninstalling via manifest only (may require sudo)...$(NC)"; \
		xargs -r -d '\n' rm -vf < $(BUILD_DIR)/install_manifest.txt; \
		echo "$(GREEN)>>> Uninstall complete$(NC)"; \
	else \
		echo "$(RED)>>> No build found. Run 'make' and 'make install' first.$(NC)"; \
		exit 1; \
	fi

# Build and run the editor
run-editor: configure
	@cmake --build --preset dev --target plasmazones-editor $(JOBS_FLAG)
	@echo "$(BLUE)>>> Running editor...$(NC)"
	@$(BUILD_DIR)/bin/plasmazones-editor --new

# Build and run the daemon
run-daemon: configure
	@cmake --build --preset dev --target plasmazonesd $(JOBS_FLAG)
	@echo "$(BLUE)>>> Running daemon...$(NC)"
	@$(BUILD_DIR)/bin/plasmazonesd

# Format C++ (clang-format) and QML (qmlformat, if available)
format: format-cpp format-qml

# Format C++ only
format-cpp: configure
	@echo "$(BLUE)>>> Formatting C++...$(NC)"
	@cmake --build --preset dev --target clang-format
	@echo "$(GREEN)>>> C++ format complete$(NC)"

# Format QML only (requires qt6-tools: qmlformat)
format-qml: configure
	@echo "$(BLUE)>>> Formatting QML...$(NC)"
	@cmake --build --preset dev --target format-qml
	@echo "$(GREEN)>>> QML format complete$(NC)"

# Help
help:
	@echo "$(BLUE)PlasmaZones: the tasks the CMake presets do not cover$(NC)"
	@echo ""
	@echo "$(GREEN)Build with the presets:$(NC)"
	@echo "  cmake --workflow --preset dev      - Configure, build and test (Debug, build/)"
	@echo "  cmake --workflow --preset release  - The same in build-release/"
	@echo "  cmake --workflow --preset asan     - With ASan and UBSan, in build-asan/"
	@echo ""
	@echo "$(GREEN)Targets:$(NC)"
	@echo "  make              - Configure and build with the dev preset"
	@echo "  make run-editor   - Build and run the editor"
	@echo "  make run-daemon   - Build and run the daemon"
	@echo "  make install      - Install build/ to the system (may need sudo)"
	@echo "  make post-install - Refresh KDE sycoca after install (optional; packaging does this)"
	@echo "  make uninstall    - Uninstall from the system (may need sudo)"
	@echo "  make format       - Format C++ and QML (clang-format, qmlformat)"
	@echo "  make format-cpp   - Format C++ only"
	@echo "  make format-qml   - Format QML only (needs qt6-tools)"
	@echo "  make help         - Show this help"
	@echo ""
	@echo "$(GREEN)Variables:$(NC)"
	@echo "  JOBS=N            - Parallel build jobs (default: CMake's)"
