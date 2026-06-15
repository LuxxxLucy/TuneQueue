# TuneQueue
#
# Two products share one frontend-agnostic core (src/core, src/data, the audio
# and resolution parts of src/media): a Clay/raylib GUI and a text CLI.
# Recipes are quiet; ./build.sh provides the readable progress output.

CC      := cc
BUILD   := build
GUI_BIN := $(BUILD)/tune_queue_gui
CLI_BIN := $(BUILD)/tune_queue_cli

# raylib and mpv come from Homebrew
RAYLIB := $(shell brew --prefix raylib)
MPV    := $(shell brew --prefix mpv)

INCLUDES := -Isrc -Isrc/core -Isrc/data -Isrc/media -Isrc/gui -Isrc/cli \
            -Isrc/helper -Itest \
            -I3rd/clay -I3rd/cjson -I3rd/stb \
            -I$(RAYLIB)/include -I$(MPV)/include
CFLAGS := -std=c11 -Wall -Wextra -Wno-unused-parameter \
          -Wno-missing-field-initializers -MMD -MP $(INCLUDES)

FRAMEWORKS := -framework Cocoa -framework IOKit -framework CoreVideo \
              -framework CoreAudio -framework OpenGL -framework CoreFoundation
GUI_LIBS := -L$(RAYLIB)/lib -L$(MPV)/lib -lraylib -lmpv -lsqlite3 -lcurl $(FRAMEWORKS)
CORE_LIBS := -L$(MPV)/lib -lmpv -lsqlite3 -lcurl

# core: everything under src/ that is neither GUI- nor CLI-specific
ALL_SRCS  := $(wildcard src/*.c src/*/*.c)
GUI_SRCS  := src/gui/gui.c src/media/thumbs.c src/media/frames.c
CLI_SRCS  := $(wildcard src/cli/*.c)
CORE_SRCS := $(filter-out $(GUI_SRCS) $(CLI_SRCS),$(ALL_SRCS)) 3rd/cjson/cJSON.c

obj = $(1:%.c=$(BUILD)/%.o)
CORE_OBJS := $(call obj,$(CORE_SRCS))
GUI_OBJS  := $(call obj,$(GUI_SRCS))
CLI_OBJS  := $(call obj,$(CLI_SRCS))

# each test/<area>/<name>_test.c links against the core it drives
TEST_SRCS := $(wildcard test/*/*_test.c)
TEST_BINS := $(patsubst %_test.c,$(BUILD)/%,$(TEST_SRCS))

# single-file libraries downloaded into 3rd/ (untracked),
# each pinned to an upstream commit and verified by SHA256
RAW       := https://raw.githubusercontent.com
CLAY_REF  := e6cc36941ab2af5d81107617039d6f527a1c660b
STB_REF   := 31c1ad37456438565541f4919958214b6e762fb4
CJSON_REF := fb16e5cf358798aabb049655975cde8427101056
DEPS := 3rd/clay/clay.h 3rd/clay/clay_renderer_raylib.c \
        3rd/stb/stb_image.h 3rd/cjson/cJSON.c 3rd/cjson/cJSON.h

.PHONY: all gui cli run run-cli test deps clean
all: $(GUI_BIN) $(CLI_BIN)
gui: $(GUI_BIN)
cli: $(CLI_BIN)
deps: $(DEPS)

# launch straight from the shell so yt-dlp inherits the terminal's keychain
# access to Chrome cookies (a Finder/launchd launch cannot read them)
run: $(GUI_BIN)
	@$(GUI_BIN)
run-cli: $(CLI_BIN)
	@$(CLI_BIN)

# build and run every test in turn; each opens its own throwaway database, so
# none can touch the real queue.
test: $(TEST_BINS)
	@for t in $(TEST_BINS); do echo "── $$t"; "$$t" || exit 1; done

$(GUI_BIN): $(CORE_OBJS) $(GUI_OBJS)
	@$(CC) $^ $(GUI_LIBS) -o $@
$(CLI_BIN): $(CORE_OBJS) $(CLI_OBJS)
	@$(CC) $^ $(CORE_LIBS) -o $@

$(BUILD)/test/%: $(BUILD)/test/%_test.o $(CORE_OBJS)
	@mkdir -p $(@D)
	@$(CC) $^ $(CORE_LIBS) -o $@

$(BUILD)/%.o: %.c | $(DEPS)
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) -c $< -o $@

# download <url> <sha256>, verifying before moving into place
define fetch
@mkdir -p $(@D) && curl -fsSL $(1) -o $@.tmp \
	&& echo "$(2)  $@.tmp" | shasum -a 256 -c - >/dev/null \
	&& mv $@.tmp $@ || { rm -f $@.tmp; exit 1; }
endef

3rd/clay/clay.h:
	$(call fetch,$(RAW)/nicbarker/clay/$(CLAY_REF)/clay.h,54aef6d5f96b14e296bebb6db4a755543e98fd7e6c188411afb2f22a4b4f7728)
3rd/clay/clay_renderer_raylib.c:
	$(call fetch,$(RAW)/nicbarker/clay/$(CLAY_REF)/renderers/raylib/clay_renderer_raylib.c,1356d14886d5efc275d6695aa13e67a2839ae11b4859b30b53c57c209ec65c82)
3rd/stb/stb_image.h:
	$(call fetch,$(RAW)/nothings/stb/$(STB_REF)/stb_image.h,594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
3rd/cjson/cJSON.c:
	$(call fetch,$(RAW)/DaveGamble/cJSON/$(CJSON_REF)/cJSON.c,607e756460fa0de37d20a7a9181f2de29c97bfb7ce5a0e6c2f548243836cd852)
3rd/cjson/cJSON.h:
	$(call fetch,$(RAW)/DaveGamble/cJSON/$(CJSON_REF)/cJSON.h,25b0145150d500498e4d209cec69c18c42cf818bffcc54690be3b895a2a16dee)

clean:
	@rm -rf $(BUILD)

-include $(CORE_OBJS:.o=.d) $(GUI_OBJS:.o=.d) $(CLI_OBJS:.o=.d) \
         $(TEST_BINS:%=%_test.d)
