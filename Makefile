# Keep root and t/ commands on the same container test targets.
.DEFAULT_GOAL := help
.PHONY: help build test test-all test-basic test-queue test-config test-memory test-performance test-native test-runtime test-valgrind test-verify test-sanitize test-pvs test-compat test-version test-nginx-version dev-test rebuild shell clean

help build test test-all test-basic test-queue test-config test-memory test-performance test-native test-runtime test-valgrind test-verify test-sanitize test-pvs test-compat test-version shell clean:
	$(MAKE) -C t $@

test-nginx-version: test-version

dev-test: test-basic

rebuild:
	$(MAKE) -C t clean
	$(MAKE) -C t build
