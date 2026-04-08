.PHONY: build flash monitor clean fullclean menuconfig web

web:
	cd web && bash build.sh

build: web
	pio run

flash:
	pio run -t upload

monitor:
	pio device monitor

flash-monitor: flash monitor

clean:
	pio run -t clean

fullclean:
	rm -rf .pio web/dist web/node_modules

menuconfig:
	pio run -t menuconfig
