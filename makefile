TARGET = hakuya
CFLAGS = -O2 -std=c99 -pedantic-errors -Wall -Wextra -Wconversion \
         -Wsign-conversion -Wtype-limits -Wimplicit-fallthrough \
         -Wshadow -Wstrict-prototypes -Wmissing-prototypes
DEBUG_CFLAGS = $(CFLAGS) -O1 -g -fsanitize=address,undefined \
               -fno-sanitize-recover=undefined
LFLAGS = $(shell pkg-config --libs sdl3)
INCLUDES = -I src $(shell pkg-config --cflags sdl3)

BIN_DIR    = bin
SOURCE_DIR = src
BUILD_DIR  = build
SOURCES = $(shell find $(SOURCE_DIR) -name "*.c")
OBJECTS = $(patsubst $(SOURCE_DIR)/%.c, $(BUILD_DIR)/%.o, $(SOURCES))

$(BUILD_DIR)/%.o: $(SOURCE_DIR)/%.c
	@mkdir -p $(@D)
	@printf %s "Compiling $< into object code..."
	@$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@
	@echo successful!

$(BIN_DIR)/$(TARGET): $(OBJECTS)
	@printf %s "Linking together the final executable..."
	@mkdir -p $(@D)
	@$(CC) $(CFLAGS) $(LFLAGS) $^ -o $@ >/dev/null
	@echo successful!
	@echo $(BIN_DIR)/$(TARGET) was successfully created!

.PHONY: run test clean

run: $(BIN_DIR)/$(TARGET)
	./$(BIN_DIR)/$(TARGET)

test: $(BIN_DIR)/$(TARGET)
	./$(BIN_DIR)/$(TARGET) -b bios/SCPH1001.BIN -e exe/RenderRectangle16BPP.exe

clean:
	@echo Cleaning up build files and executables...
	@rm -rf $(BUILD_DIR) $(BIN_DIR)
