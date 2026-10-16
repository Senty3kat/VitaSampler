# Хост-проверки Vita Sampler (без vitasdk): make -C host check | preview
CFLAGS = -std=gnu99 -Istub $(shell pkg-config --cflags freetype2) -g -O1 -DASSET_PREFIX=\"../assets/\" -Wall -Wextra -Wno-unused-parameter
check:
	gcc -fsyntax-only -Werror=implicit-function-declaration -Werror=int-conversion $(CFLAGS) ../main.c
preview: preview.c ../main.c
	gcc $(CFLAGS) -w preview.c -o preview.bin -lm -lpng $(shell pkg-config --libs freetype2) -Wl,--unresolved-symbols=ignore-all
	python3 shot.py
