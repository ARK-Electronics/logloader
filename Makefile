all:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@cmake -Bbuild -H.; cmake --build build -j$(nproc)
	@size build/logloader

install: clean all
	@sudo cmake --install build

clean:
	@rm -rf build
	@echo "All build artifacts removed"

.PHONY: all install clean
