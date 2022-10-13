all: npshell.cpp
	g++ npshell.cpp -o npshell
.PHONY: clean
clean:
	rm -rf *.exe *.out *.o *.txt npshell
