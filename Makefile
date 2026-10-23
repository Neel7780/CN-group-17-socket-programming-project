# Top-level Makefile for IT305 Socket Programming Project
# Group 17 (Lab Groups 021 & 034)

SUBDIRS = Part1 Part2_Case1 Part2_Case2 Part2_Case2_Enhanced
PACKAGE_NAME = FinalSubmission_Group_021_034

all:
	@for dir in $(SUBDIRS); do \
		echo "==> Building $$dir..."; \
		$(MAKE) -C $$dir || exit 1; \
	done

clean:
	@for dir in $(SUBDIRS); do \
		echo "==> Cleaning $$dir..."; \
		$(MAKE) -C $$dir clean || true; \
	done
	rm -rf $(PACKAGE_NAME) $(PACKAGE_NAME).tar.gz $(PACKAGE_NAME).zip

test:
	@echo "==> Running Part 1 tests..."
	@$(MAKE) -C Part1 test
	@echo "==> Running Part 2 Case 1 fault tests..."
	@$(MAKE) -C Part2_Case1 test

verify-faults:
	@echo "==> Running end-to-end multi-case fault recovery test suite..."
	@bash scripts/verify_fault_recovery.sh

package: clean
	rm -rf $(PACKAGE_NAME) $(PACKAGE_NAME).tar.gz $(PACKAGE_NAME).zip
	mkdir -p $(PACKAGE_NAME)
	cp Makefile $(PACKAGE_NAME)/
	cp FinalReport_Group_021_034.pdf $(PACKAGE_NAME)/
	cp README.md $(PACKAGE_NAME)/
	cp -r Part1 Part2_Case1 Part2_Case2 Part2_Case2_Enhanced Results scripts $(PACKAGE_NAME)/
	find $(PACKAGE_NAME)/Part* \( -name "*.o" -o -name "*.d" -o -name "server" -o -name "client" -o -name "*.csv" \) -delete
	chmod +x $(PACKAGE_NAME)/scripts/*.sh
	tar -czf $(PACKAGE_NAME).tar.gz $(PACKAGE_NAME)
	zip -r -q $(PACKAGE_NAME).zip $(PACKAGE_NAME)
	rm -rf $(PACKAGE_NAME)
	@echo "Successfully generated submission archives:"
	@echo "  - $(PACKAGE_NAME).zip"
	@echo "  - $(PACKAGE_NAME).tar.gz"

.PHONY: all clean test verify-faults package
