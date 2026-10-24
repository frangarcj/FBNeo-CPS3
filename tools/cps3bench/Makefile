LIBRETRO_INC := ../../src/burner/libretro/libretro-common/include

cps3bench: cps3bench.c
	$(CC) -O2 -Wall -I$(LIBRETRO_INC) -o $@ $<

clean:
	rm -f cps3bench
