NATIVE_OUTPUT ?= $(CURDIR)/build/native-bin
.PHONY: build-native test
build-native:
	sh tools/build-native.sh "$(NATIVE_OUTPUT)"
test:
	HMJE_BOOTSTRAP_BIN="$(NATIVE_OUTPUT)" HMJE_FRONTEND="$(NATIVE_OUTPUT)/컴파일러" sh tools/test.sh
