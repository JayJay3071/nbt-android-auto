QCC = qcc
CFLAGS = -V4.4.2,gcc_ntox86 -O2 -Wall -Wextra

.PHONY: all test
all: build/hello_nbt

build/hello_nbt: src/hello_nbt.c
	mkdir -p build
	$(QCC) $(CFLAGS) src/hello_nbt.c -o build/hello_nbt

# Run only in the development VM, never on the vehicle without approval.
test: all
	./build/hello_nbt
