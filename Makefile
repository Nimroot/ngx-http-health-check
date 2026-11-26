# Builds nginx with this module in a container and runs t/test.sh against it.
#   make test                          podman, nginx 1.30.6
#   make test ENGINE=docker NGINX_VERSION=1.29.8
#   make check IMAGE=...               test an existing image, no build
# Each run has a container of its own, so runs may go side by side.

ENGINE         ?= podman
NGINX_VERSION  ?= 1.30.6
ALPINE_VERSION ?= 3.24.4
IMAGE          ?= localhost/ngx-health-check-test:$(NGINX_VERSION)

.PHONY: test check image clean

# check only once the image is built, also with make -j
test: image
	@$(MAKE) --no-print-directory check

check:
	ENGINE=$(ENGINE) IMAGE=$(IMAGE) ./t/test.sh

image:
	$(ENGINE) build -t $(IMAGE) -f t/Containerfile \
	  --build-arg NGINX_VERSION=$(NGINX_VERSION) \
	  --build-arg ALPINE_VERSION=$(ALPINE_VERSION) .

clean:
	-$(ENGINE) rmi $(IMAGE)
