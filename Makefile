PROJECT_NAME="logloader"

all:
	@cmake -Bbuild -S. -DCMAKE_BUILD_TYPE=Release; cmake --build build -j$(nproc)
	@size build/${PROJECT_NAME}

debug:
	@cmake -Bbuild -S. -DCMAKE_BUILD_TYPE=Debug; cmake --build build -j$(nproc)
	@size build/${PROJECT_NAME}

# Separate from the build on purpose: a `make` that rewrites your sources cannot
# run from a read-only checkout and makes the build non-reproducible.
format:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@astyle --quiet --options=astylerc "tests/*.cpp,*.hpp"

check-format:
	@{ astyle --dry-run --formatted --options=astylerc src/*.cpp,*.hpp; astyle --dry-run --formatted --options=astylerc "tests/*.cpp,*.hpp"; } | grep Formatted \
		&& { echo "run 'make format'"; exit 1; } || echo "formatting OK"

install:
	@bash install.sh

clean:
	@rm -rf build
	@echo "All build artifacts removed"

.PHONY: all debug format check-format install clean
