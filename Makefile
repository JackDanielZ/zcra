BUILD_DIR=build
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
