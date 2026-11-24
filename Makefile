# Builds nginx with this module in a container and runs t/test.sh against it.
#   make test                          podman, nginx 1.30.6
#   make test ENGINE=docker NGINX_VERSION=1.29.8
#   make check IMAGE=...               test an existing image, no build

ENGINE         ?= podman
NGINX_VERSION  ?= 1.30.6
ALPINE_VERSION ?= 3.24.4
IMAGE          ?= localhost/ngx-health-check-test:$(NGINX_VERSION)
CONTAINER      ?= ngx-health-check-test
PORT           ?= 18082

.PHONY: test check image clean

test: image check

check:
	-@$(ENGINE) rm -f $(CONTAINER) >/dev/null 2>&1
	$(ENGINE) run -d --name $(CONTAINER) -p 127.0.0.1:$(PORT):8082 $(IMAGE) >/dev/null
	@BASE=http://127.0.0.1:$(PORT) CONTAINER=$(CONTAINER) ENGINE=$(ENGINE) \
	  ./t/test.sh; rc=$$?; \
	  if [ $$rc -ne 0 ]; then $(ENGINE) logs $(CONTAINER) 2>&1 | tail -n 40; fi; \
	  $(ENGINE) rm -f $(CONTAINER) >/dev/null; exit $$rc

image:
	$(ENGINE) build -t $(IMAGE) -f t/Containerfile \
	  --build-arg NGINX_VERSION=$(NGINX_VERSION) \
	  --build-arg ALPINE_VERSION=$(ALPINE_VERSION) .

clean:
	-$(ENGINE) rm -f $(CONTAINER)
	-$(ENGINE) rmi $(IMAGE)
