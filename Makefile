BUILD_DIR := build

.PHONY: all sync setup build test install

# 4. Run jobs 0 → 3 in sequence
all: sync build test install

# 0. Fetch origin and rebase on top of main
sync:
	git fetch origin
	git rebase origin/main

# Meson configuration — wipes a stale build dir and reconfigures from scratch
setup:
	meson setup --wipe $(BUILD_DIR)

# 1. Build the app — auto-configures if the build dir is absent or invalid
build:
	@meson introspect --projectinfo $(BUILD_DIR) >/dev/null 2>&1 \
		|| meson setup --wipe $(BUILD_DIR)
	meson compile -C $(BUILD_DIR)

# 2. Run the tests
test:
	meson test -C $(BUILD_DIR)

# 3. Install the app
install:
	meson install -C $(BUILD_DIR)

push:
	git push fork --force-with-lease
