all: npshell.cpp
	g++ -g npshell.cpp -o npshell
.PHONY: clean
clean:
	rm -rf *.exe *.out *.o *.txt npshell
