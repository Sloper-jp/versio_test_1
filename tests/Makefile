CXX ?= c++
CXXFLAGS = -std=c++14 -O1 -Wall -Wextra -Werror -I../src

build/test_runner: test_main.cpp ../src/*.h
	mkdir -p build
	$(CXX) $(CXXFLAGS) test_main.cpp -o $@
	./$@

.PHONY: clean
clean:
	rm -rf build
