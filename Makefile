PRESET ?= strict
.PHONY: all configure test release deb install clean lint
all: configure
	cmake --build --preset $(PRESET)
configure:
	cmake --preset $(PRESET)
test: all
	ctest --preset $(PRESET)
release:
	$(MAKE) PRESET=release test
deb:
	python3 tools/build-debian.py
install: all
	cmake --install build/$(PRESET) --prefix "$(HOME)/.local"
clean:
	cmake --build build/$(PRESET) --target clean

lint: all
	cmake --build build/$(PRESET) --target lint
	shellcheck tools/record-radio tests/fixtures/generate.sh
