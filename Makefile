PROJECT_NAME="logloader"

all:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@cmake -Bbuild -H. -DDEBUG_BUILD=OFF; cmake --build build -j$(nproc)
	@size build/${PROJECT_NAME}

debug:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@cmake -Bbuild -H. -DDEBUG_BUILD=ON; cmake --build build -j$(nproc)
	@size build/${PROJECT_NAME}
	@echo "Debug build with logging enabled"

install:
	@bash install.sh

clean:
	@rm -rf build
	@echo "All build artifacts removed"

.PHONY: all debug install clean
