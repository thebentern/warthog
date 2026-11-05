#
# Copyright 2026 Morse Micro
#

# Add a utility target that uses openocd to reprogram the example
# ELF with the platform's default openocd (or the custom OPENOCD_CFG
# file if it is defined)
OPENOCD_CFG ?= $(MMIOT_ROOT)/src/platforms/$(PLATFORM)/openocd.cfg

.PHONY: reprogram
reprogram: $(ELF_FILE)
	@echo "Programming $(ELF_FILE) to $(PLATFORM) with OpenOCD"
	openocd -c "set CONNECT_ASSERT_SRST 1" -f $(OPENOCD_CFG) -c "program $(ELF_FILE) verify reset exit"

# Add a utility target that uses openocd and the program-configstore.py
# script to reconfigure a platform with its default CONFIGFILE and OPENOCD_CFG
# (or the custom values for these if they are defined)
CONFIGFILE ?= ../../config.hjson

# Stamp file that tracks whether the pipenv venv is in sync with Pipfile.lock.
# Re-runs `pipenv sync` only when the lockfile changes.
PIPENV_STAMP := $(MMIOT_ROOT)/.pipenv-synced
$(PIPENV_STAMP): $(MMIOT_ROOT)/Pipfile.lock
	@echo "Syncing pipenv dependencies"
	@PIPENV_PIPFILE="$(MMIOT_ROOT)/Pipfile" pipenv sync >/dev/null
	@touch $@

.PHONY: reconfig
reconfig: $(CONFIGFILE) $(PIPENV_STAMP)
	@# This starts openocd, uses program-configstore.py, and then kills openocd by registering a trap on EXIT INT and TERM
	@echo "Starting OpenOCD"
	@openocd -f $(OPENOCD_CFG) & \
	OPENOCD_PID=$$!; \
	echo "OPENOCD_PID = $$OPENOCD_PID"; \
	trap 'echo "Stopping OpenOCD"; kill $$OPENOCD_PID 2>/dev/null || true; wait $$OPENOCD_PID 2>/dev/null || true' EXIT INT TERM; \
	echo "Writing device config store based on values in $<"; \
	PIPENV_PIPFILE="$(MMIOT_ROOT)/Pipfile" pipenv run $(MMIOT_ROOT)/tools/platform/program-configstore.py -H localhost -d write-json $<
