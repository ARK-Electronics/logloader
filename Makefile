all:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@cmake -Bbuild -H.; cmake --build build -j$(nproc)
	@size build/logloader

install: clean all
	@sudo cmake --install build
	@mkdir -p ${HOME}/logloader/logs
	@if [ -f install.config.toml ]; then \
		cp install.config.toml ${HOME}/logloader/config.toml; \
	else \
		cp config.toml ${HOME}/logloader/config.toml; \
	fi

clean:
	@rm -rf build
	@echo "All build artifacts removed"

.PHONY: all install clean
