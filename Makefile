all:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@cmake -Bbuild -H.; cmake --build build -j$(nproc)
	@size build/logloader

install:
	@cmake -Bbuild -H.
	cmake --build build -j$(nproc)
	@sudo cmake --install build
	@mkdir -p ${HOME}/logloader/logs
	@cp install.config.toml ${HOME}/logloader/config.toml

clean:
	@rm -rf build
	@echo "All build artifacts removed"

.PHONY: all install clean
