all:
	@astyle --quiet --options=astylerc src/*.cpp,*.hpp
	@cmake -Bbuild -H.; cmake --build build -j 12
	@size build/logloader

clean:
	@rm -rf build
	@echo "All build artifacts removed"

.PHONY: all clean
