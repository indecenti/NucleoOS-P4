# The DMD menu simulator (see sim.cpp). Built with clang in WSL, like tests/host.
ROOT   := ../../..
LVGL   := $(ROOT)/managed_components/lvgl__lvgl
TP     := ../anima/third_party
B      := build
CC     := clang
CXX    := clang++
INC    := -I. -Istub -I$(LVGL) -I$(ROOT)/components/nv_dmd -I$(ROOT)/components/nv_dmd/include \
          -I$(ROOT)/components/nv_apps -I$(ROOT)/components/nv_ui/include -I$(ROOT)/components/nv_i18n/include \
          -I$(ROOT)/components/nv_fonts/include -I$(TP)
DEFS   := -DLV_CONF_INCLUDE_SIMPLE -DLV_LVGL_H_INCLUDE_SIMPLE
CFLAGS := -O1 -g $(INC) $(DEFS) -w
LV_SRC := $(shell find $(LVGL)/src -name '*.c')
LV_OBJ := $(patsubst $(LVGL)/%.c,$(B)/lvgl/%.o,$(LV_SRC))
APP_C  := $(ROOT)/components/nv_dmd/dmd_core.c $(ROOT)/components/nv_i18n/nv_i18n.c $(ROOT)/components/nv_apps/dmd_orb.c \
          $(wildcard $(ROOT)/components/nv_fonts/nv_font_*.c) $(TP)/cJSON.c $(TP)/miniz.c
APP_O  := $(patsubst %.c,$(B)/app/%.o,$(notdir $(APP_C)))

all: $(B)/sim

$(B)/lvgl/%.o: $(LVGL)/%.c lv_conf.h
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -c $< -o $@

define APPRULE
$(B)/app/$(notdir $(1:.c=.o)): $(1)
	@mkdir -p $(B)/app
	$(CC) $(CFLAGS) -c $$< -o $$@
endef
$(foreach f,$(APP_C),$(eval $(call APPRULE,$(f))))

$(B)/dmd_menu.o: $(ROOT)/components/nv_apps/dmd_menu.cpp $(ROOT)/components/nv_apps/dmd_menu.h
	@mkdir -p $(B)
	$(CXX) -std=gnu++2b $(CFLAGS) -DDMD_SIM_TRACE -DDMD_CACHE_DIR=\"sdcache\" -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -c $< -o $@

$(B)/dmd_comp.o: $(ROOT)/components/nv_apps/dmd_comp.cpp $(ROOT)/components/nv_apps/dmd_menu.h
	@mkdir -p $(B)
	$(CXX) -std=gnu++2b $(CFLAGS) -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -c $< -o $@

$(B)/sim.o: sim.cpp
	@mkdir -p $(B)
	$(CXX) -std=gnu++2b $(CFLAGS) -c $< -o $@

$(B)/sim: $(LV_OBJ) $(APP_O) $(B)/dmd_menu.o $(B)/dmd_comp.o $(B)/sim.o
	$(CXX) $^ -o $@ -lm

clean:
	rm -rf $(B)
.PHONY: all clean
