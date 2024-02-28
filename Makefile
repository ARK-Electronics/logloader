all:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@cmake -Bbuild -H.; cmake --build build -j 12
	@size build/logloader

install: all
# 	@cmake -Bbuild -H.; cmake --build build -j 12
	@sudo cp build/logloader /usr/bin
	@mkdir -p ${HOME}/logloader/logs
	@cp install.config.toml ${HOME}/logloader/config.toml

clean:
	@rm -rf build
	@echo "All build artifacts removed"

.PHONY: all install clean
