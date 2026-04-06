.PHONY: build flash monitor clean fullclean menuconfig

build:
	pio run

flash:
	pio run -t upload

monitor:
	pio device monitor

flash-monitor: flash monitor

clean:
	pio run -t clean

fullclean:
	rm -rf .pio

menuconfig:
	pio run -t menuconfig
