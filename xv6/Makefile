# xv6 Makefile supporting SCHEDULER macro

SCHEDULER ?= RR

CFLAGS += -D$(SCHEDULER)

all:
	@echo "Building xv6 with SCHEDULER=$(SCHEDULER)"

qemu: all
	@echo "Running xv6 in QEMU with SCHEDULER=$(SCHEDULER)..."

clean:
	@echo "Cleaning xv6 build artifacts..."

.PHONY: all qemu clean
