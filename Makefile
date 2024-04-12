flit: flit.c
	$(CC) flit.c -o flt -Wall -Wextra -O3 -pedantic -std=c99

clean:
	rm -f flt
