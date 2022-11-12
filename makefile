all: np_simple.cpp
	g++ -g np_simple.cpp -o np_simple
.PHONY: clean
clean:
	rm -rf *.exe *.out *.o *.txt np_simple
