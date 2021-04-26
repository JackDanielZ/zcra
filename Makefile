BUILD_DIR=build
PREFIX = /opt/mine

ifneq ($(V),1)
  Q := @
endif

default: $(BUILD_DIR)/bin/zcra

CFLAGS += -g -Wall -Wextra -Werror
LDFLAGS += -lutil

$(BUILD_DIR)/obj/%.o: src/%.c
	$(Q)echo Building object $@
	$(Q)mkdir -p $(@D)
	$(Q)$(TOOLSET)gcc -x c -c $< -o $@ $(CFLAGS)

$(BUILD_DIR)/bin/%:
	$(Q)echo Building binary $@
	$(Q)mkdir -p $(@D)
	$(Q)$(TOOLSET)gcc -o $@ $^ ${LDFLAGS}

$(BUILD_DIR)/bin/zcra: $(BUILD_DIR)/obj/zcra.o

install: $(BUILD_DIR)/bin/zcra
	mkdir -p $(PREFIX)/bin
	install -c build/bin/zcra $(PREFIX)/bin/

clean:
	rm -rf build/

