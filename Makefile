# Project Name
TARGET = versio_reverse

# Sources
CPP_SOURCES = src/main.cpp

# Library Locations
LIBDAISY_DIR = lib/libDaisy

# Core location, and generic Makefile.
SYSTEM_FILES_DIR = $(LIBDAISY_DIR)/core
include $(SYSTEM_FILES_DIR)/Makefile

# Host-side unit tests for the hardware independent parts
.PHONY: test
test:
	$(MAKE) -C tests
