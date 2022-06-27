CFLAGS = -g -O0 -Wall -std=gnu99
BIN_PATH ?= bin

all : kcp bin/kcp_basic bin/kcp_optional

$(BIN_PATH) :
	mkdir $(BIN_PATH)

kcp/ikcp.c :
	git submodule update --init

bin/kcp_basic: src/kcp_basic.c kcp/ikcp.c | $(BIN_PATH)
	$(CC) $(CFLAGS) -o $@ $^ -Ikcp 

bin/kcp_optional: src/kcp_optional.c kcp/ikcp.c  | $(BIN_PATH)
	$(CC) $(CFLAGS) -o $@ $^ -Ikcp 

clean:
	rm -rf $(BIN_PATH)
