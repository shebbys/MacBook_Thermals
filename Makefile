thermals: thermals.c
	clang -O2 -Wall -o thermals thermals.c -framework IOKit -framework CoreFoundation
install: thermals
	mkdir -p $(HOME)/bin && ln -sf $(CURDIR)/thermals $(HOME)/bin/thermals
clean:
	rm -f thermals
