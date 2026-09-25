IMAGE ?= stm32-build
build:
	docker run --rm -v $(PWD):/workspace -w /workspace \
	--user $(id -u):$(id -g) $(IMAGE) \
	make -C firmware all