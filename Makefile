PROJ_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))

# Configuration of extension
EXT_NAME=events
EXT_CONFIG=${PROJ_DIR}extension_config.cmake

# This branch pins the released DuckDB ABI. DuckDB otherwise labels a local
# build as a development version, even when its checkout is on the release tag.
OVERRIDE_GIT_DESCRIBE ?= v2.0.0
export PYTHONUTF8 ?= 1

# Include the Makefile from extension-ci-tools
include extension-ci-tools/makefiles/duckdb_extension.Makefile

.PHONY: test_integration_release test_integration_debug test_integration_reldebug

test_release: test_integration_release
test_debug: test_integration_debug
test_reldebug: test_integration_reldebug

test_integration_release test_integration_debug test_integration_reldebug:
	@if [ "$(SKIP_TESTS)" != "1" ]; then \
		python3 test/scripts/test_events.py --duckdb build/$(patsubst test_integration_%,%,$@)/duckdb -v; \
	fi
