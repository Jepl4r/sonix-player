#!/bin/sh

INTERFACE=wlan0

# stop already exist process
killall udhcpc > /dev/null
killall wpa_supplicant > /dev/null

ifconfig $INTERFACE down

/usr/bin/wlbt-wifi down

exit 0

