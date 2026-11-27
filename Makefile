# Builds nginx with this module in a container and runs t/test.sh against it,
# then the tests in t/*.t, with nginx-tests' Test::Nginx.
#   make test                          podman, nginx 1.30.6
#   make test ENGINE=docker NGINX_VERSION=1.29.8
#   make check IMAGE=...               test an existing image, no build
#   make check PROVE='-v t/health_check_config.t'   prove's arguments
# Each run has a container of its own, so runs may go side by side.

ENGINE         ?= podman
NGINX_VERSION  ?= 1.30.6
ALPINE_VERSION ?= 3.24.4
IMAGE          ?= localhost/ngx-health-check-test:$(NGINX_VERSION)
PROVE          ?= -j8 t

.PHONY: test check image clean

# check only once the image is built, also with make -j
test: image
	@$(MAKE) --no-print-directory check

check:
	ENGINE=$(ENGINE) IMAGE=$(IMAGE) ./t/test.sh
	$(ENGINE) run --rm --init --user nginx -w /tests --entrypoint prove \
	  $(IMAGE) -I /nginx-tests/lib $(PROVE)

image:
	$(ENGINE) build -t $(IMAGE) -f t/Containerfile \
	  --build-arg NGINX_VERSION=$(NGINX_VERSION) \
	  --build-arg ALPINE_VERSION=$(ALPINE_VERSION) .

clean:
	-$(ENGINE) rmi $(IMAGE)
