#ifndef ADEFINES_H
#define ADEFINES_H

#define SPECIAL_MENU 0

#if (defined XPERIA_GO || defined XPERIA_SOLA)
#define HAVE_EXTERNAL_SDCARD
#endif

#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#define HAVE_EXTERNAL_SDCARD
#endif

/* extendendcommands.c */
#define BOARD_UMS_LUNFILE	"/sys/devices/virtual/android_usb/android0/f_mass_storage/lun/file"
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#define BOARD_VOLD_SDCARD_VOLUME_INT	"/dev/block/vold/179:64"
#define BOARD_VOLD_SDCARD_VOLUME_EXT	"/dev/block/vold/179:65"
#else
#define BOARD_VOLD_SDCARD_VOLUME_INT	"/dev/block/vold/179:14"
#define BOARD_VOLD_SDCARD_VOLUME_EXT	"/dev/block/vold/179:97"
#endif

#define VIBRATOR_TIMEOUT_FILE "/sys/class/timed_output/vibrator/enable"
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#define VIBRATOR_TIME_MS	300
#else
#define VIBRATOR_TIME_MS	10
#endif

#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#define BATTERY_LEVEL_FILE	"/sys/devices/qpnp-charger-*/power_supply/battery/capacity"
#define BATTERY_STATUS_CHARGING_FILE_USB	"/sys/devices/msm_dwc3/power_supply/usb/online"
#define BATTERY_STATUS_CHARGING_FILE_AC	"/sys/devices/msm_dwc3/power_supply/usb/online"
#else
#define BATTERY_LEVEL_FILE	"/sys/devices/platform/ab8500-i2c.0/ab8500-fg.0/power_supply/ab8500_fg/capacity"
#define BATTERY_STATUS_CHARGING_FILE_USB	"/sys/devices/platform/ab8500-i2c.0/ab8500-charger.0/power_supply/ab8500_usb/online"
#define BATTERY_STATUS_CHARGING_FILE_AC	"/sys/devices/platform/ab8500-i2c.0/ab8500-charger.0/power_supply/ab8500_ac/online"
#endif

/* comment to disable the installation animation */
#define CWM_INST_ANIM

/* ui.c */
#define USB_STATE_FILE "/sys/class/android_usb/android0/state"

#if (defined XPERIA_GO || defined XPERIA_SOLA)
#define RED_LED_FILE	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0040/leds/red/brightness"
#define GREEN_LED_FILE	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0040/leds/green/brightness"
#define BUTTON_BACKLIGHT	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0040/leds/button-backlight/brightness"
#endif
#ifdef XPERIA_P
#define RED_LED_FILE	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0036/leds/red/brightness"
#define GREEN_LED_FILE	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0036/leds/green/brightness"
#define BUTTON_BACKLIGHT	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0036/leds/button-backlight/brightness"
#endif
#ifdef XPERIA_U
#define RED_LED_FILE	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0040/leds/pwr-red/brightness"
#define GREEN_LED_FILE	"/sys/devices/platform/nmk-i2c.2/i2c-2/2-0040/leds/pwr-green/brightness"
#endif
#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#define RED_LED_FILE	"/sys/class/leds/led:rgb_red/brightness"
#define GREEN_LED_FILE	"/sys/class/leds/led:rgb_green/brightness"
#endif

#if defined(XPERIA_Z1C) || defined(XPERIA_Z1)
#define KERNEL_TIME_SYSFS	"/sys/devices/qpnp-rtc-*/rtc/rtc0/time"
#else
#define KERNEL_TIME_SYSFS	"/sys/devices/platform/ab8500-i2c.0/ab8500-rtc.0/rtc/rtc0/time"
#endif

#endif
