# tests/host â€” PC-side unit tests and libFuzzer fuzzers for the firmware code that parses untrusted
# input (media files that reach the SD over the LAN, web API query strings and paths, auth headers, OTA manifests). Linux/WSL with
# clang >= 14 (libFuzzer) and gcc-multilib (the 32-bit builds: the P4 is RV32, and pointer/size_t
# overflows only show up at 32 bits). No ESP-IDF needed.
#
#   make                              unit tests, 64- and 32-bit, ASan + UBSan
#   make fuzz                         build every fuzzer, 64- and 32-bit
#   make fuzz-run F=avi A=x86 S=60    run one fuzzer for S seconds (seeds: corpus/F, finds: out/F)
#   make ci                           unit tests + every fuzzer for CI_SECS seconds on both arches
#   make clean
#
# From Windows:  wsl make -C /mnt/d/NucleoV2/tests/host ci

ROOT     := ../..
VP       := $(ROOT)/components/nv_vplayer
WEB      := $(ROOT)/components/nv_web
AUTH     := $(ROOT)/components/nv_auth
OTA      := $(ROOT)/components/nv_ota
MQTT     := $(ROOT)/components/nv_mqtt
STORE    := $(ROOT)/components/nv_appstore
WASM     := $(ROOT)/components/nv_wasm
CC       := clang
CXX      := clang++
SAN      := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
WARN     := -Wall -Wextra -Wno-unused-parameter
INC      := -I$(VP) -I$(WEB) -I$(AUTH)/include -I$(OTA)/include -I$(MQTT) -I$(STORE)/include -I$(WASM) -Iinclude -Iunit
CFLAGS   := -O1 -g $(SAN) $(WARN) $(INC) -D_GNU_SOURCE
CXXFLAGS := $(CFLAGS) -std=c++17
ARCHES   := x64 x86
m_x64    := -m64
m_x86    := -m32
CI_SECS  ?= 30
FUZZERS  := avi mp4 mpeg1 web auth ota ha pkg netpol
UNITS    := avi mp4 web auth ota ha pkg netpol wasiro anima

# firmware code under test (+ host stubs) per target
SRC_avi   := $(VP)/vp_avi.c
SRC_mp4   := $(VP)/vp_mp4.c
SRC_web   := $(WEB)/nv_web_util.cpp stub/nv_usb_stub.c
SRC_mpeg1 := stub/pl_mpeg_impl.c
SRC_auth  := $(AUTH)/nv_auth_core.cpp
SRC_ota   := $(OTA)/nv_ota_manifest.cpp
SRC_ha    := $(MQTT)/nv_ha_proto.c
SRC_pkg   := $(STORE)/nv_store_pkg.cpp
SRC_netpol := $(WASM)/nv_net_policy.c $(MQTT)/nv_mqtt_topic.c
# ANIMA: the whole engine (minus the Wi-Fi shim) over offline stand-ins for ESP-IDF (anima/stub,
# anima/anima_stubs.c) and a vendored cJSON; the SD root is ./anima_sd while the test runs.
ANIMA    := $(ROOT)/components/nv_anima
SRC_anima := $(filter-out $(ANIMA)/nucleo_setup_shim.c,$(wildcard $(ANIMA)/*.c)) anima/anima_stubs.c anima/third_party/cJSON.c
FLAGS_anima := -DANIMA_HOST -DNUCLEO_SD_MOUNT='"anima_sd"' -Ianima/stub -Ianima/third_party -I$(ANIMA) -I$(ANIMA)/include \
               -I$(ROOT)/components/nv_kernel/include \
               -Wno-sign-compare -Wno-misleading-indentation -Wno-format-truncation -Wno-unused-function
# pl_mpeg loads uint32_t from unaligned addresses and left-shifts negative coefficients on purpose;
# both are well defined on the P4's RV32 (and the player is HW-verified). Keep those two checks off so
# the fuzzer reaches memory bugs.
FLAGS_mpeg1 := -fno-sanitize=alignment -fno-sanitize=shift-base   # no commas: they split $(call) args
# headers the firmware sources and harnesses include (a change rebuilds everything)
HDRS := $(wildcard $(ROOT)/components/nv_anima/*.h $(ROOT)/components/nv_anima/include/*.h $(VP)/*.h $(WEB)/*.h $(AUTH)/include/*.h $(OTA)/include/nv_ota_manifest.h $(MQTT)/nv_ha_proto.h $(STORE)/include/nv_store_pkg.h $(WASM)/nv_net_policy.h $(WASM)/nv_wasi_ro.h $(MQTT)/nv_mqtt_topic.h unit/*.h)

# Fuzzer working dirs (seed copy + finds + artifacts). 32-bit programs can't stat() a WSL /mnt/<drive>
# path (64-bit inode numbers -> EOVERFLOW), so on WSL they live under ~/.cache instead of here.
WORK ?= $(if $(filter /mnt/%,$(CURDIR)),$(HOME)/.cache/nucleov2-host,$(CURDIR))

.PHONY: all unit fuzz fuzz-run ci clean
all: unit

# $(1)=arch $(2)=output $(3)=sources $(4)=extra flags. C with $(CC), C++ with $(CXX), link with $(CXX).
define build_bin
	@mkdir -p $(2).obj
	@set -e; objs=""; for s in $(3); do o=$(2).obj/$$(basename $$s).o; \
	  case $$s in *.c) $(CC) $(m_$(1)) $(CFLAGS) $(4) -c $$s -o $$o ;; \
	              *)   $(CXX) $(m_$(1)) $(CXXFLAGS) $(4) -c $$s -o $$o ;; esac; \
	  objs="$$objs $$o"; done; \
	  $(CXX) $(m_$(1)) $(CXXFLAGS) $(4) $$objs -o $(2)
endef

define arch_target
build/$(1)/unit_$(2): unit/test_$(2).cpp $(SRC_$(2)) $(HDRS) Makefile
	$$(call build_bin,$(1),$$@,unit/test_$(2).cpp $(SRC_$(2)),$(FLAGS_$(2)))
endef
define arch_fuzzer
build/$(1)/fuzz_$(2): fuzz/fuzz_$(2).cpp $(SRC_$(2)) $(HDRS) Makefile
	$$(call build_bin,$(1),$$@,fuzz/fuzz_$(2).cpp $(SRC_$(2)),-fsanitize=fuzzer $(FLAGS_$(2)))
endef
$(foreach a,$(ARCHES),$(foreach u,$(UNITS),$(eval $(call arch_target,$(a),$(u)))))
$(foreach a,$(ARCHES),$(foreach f,$(FUZZERS),$(eval $(call arch_fuzzer,$(a),$(f)))))

unit: $(foreach a,$(ARCHES),$(addprefix build/$(a)/unit_,$(UNITS)))
	@set -e; for a in $(ARCHES); do for u in $(UNITS); do build/$$a/unit_$$u; done; done
	@echo "UNIT OK"

fuzz: $(foreach a,$(ARCHES),$(addprefix build/$(a)/fuzz_,$(FUZZERS)))

# One fuzzer run: $(1)=target $(2)=arch $(3)=seconds. The committed seeds (corpus/<t>) are copied into
# $(WORK)/seeds; $(WORK)/out/<t> keeps what the fuzzer learned across runs (never committed).
define fuzz_one
	mkdir -p $(WORK)/out/$(1) $(WORK)/seeds && rm -rf $(WORK)/seeds/$(1)-$(2) && cp -r corpus/$(1) $(WORK)/seeds/$(1)-$(2); \
	build/$(2)/fuzz_$(1) -max_total_time=$(3) -timeout=10 -rss_limit_mb=2048 -print_final_stats=1 \
	    -artifact_prefix=$(WORK)/out/$(1)/$(2)- $(WORK)/out/$(1) $(WORK)/seeds/$(1)-$(2) >$(WORK)/out/$(1)/$(2).log 2>&1 || true; \
	grep -E "^#[0-9]+ +DONE|stat::number_of_executed_units|SUMMARY" $(WORK)/out/$(1)/$(2).log || true; \
	if ls $(WORK)/out/$(1)/$(2)-crash-* $(WORK)/out/$(1)/$(2)-leak-* $(WORK)/out/$(1)/$(2)-timeout-* \
	      $(WORK)/out/$(1)/$(2)-oom-* >/dev/null 2>&1; then \
	    echo "FUZZ FAIL: $(1) ($(2)) -- $(WORK)/out/$(1)/$(2).log"; exit 1; fi
endef

F ?= avi
A ?= x64
S ?= 60
fuzz-run: build/$(A)/fuzz_$(F)
	@$(call fuzz_one,$(F),$(A),$(S))

# CI: unit tests, then every fuzzer on both arches; any crash / leak / timeout / oom artifact fails.
ci: unit fuzz
	@set -e; for a in $(ARCHES); do for f in $(FUZZERS); do \
	    echo "== fuzz $$f ($$a, $(CI_SECS) s)"; $(call fuzz_one,$$f,$$a,$(CI_SECS)); done; done
	@echo "CI OK"

clean:
	rm -rf build $(WORK)/out $(WORK)/seeds
