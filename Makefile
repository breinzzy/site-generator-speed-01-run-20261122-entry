CC = gcc
CFLAGS = -O3 -march=native -static -Wall -Wextra -Wno-unused-parameter -DNDEBUG
# NOTE: -march=native is fine for the local VM; for the grading container
# (also x86_64) it is still valid, but if the grader CPU differs, rebuild
# without -march=native. The checked-in binary is built WITHOUT it (see below).

sitegen: sitegen.c slugtab.h
	$(CC) -O3 -static -Wall -Wextra -Wno-unused-parameter -o $@ sitegen.c -lm

sitegen-native: sitegen.c slugtab.h
	$(CC) $(CFLAGS) -o $@ sitegen.c -lm

clean:
	rm -f sitegen sitegen-native

.PHONY: clean
