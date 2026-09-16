#ifndef HEADSET_H
#define HEADSET_H

#include <stdbool.h>

// The buttons on the headphone cable.
//
// On the R3 Pro II no ordinary keyboard driver reads them: a dedicated module,
// sa_earpods_adc.ko, samples SAR ADC channel 2 every 10 ms and turns three
// voltage windows into three keys. From its load script
// (module_driver/sa_earpods_adc.sh):
//
//     0 - 10 mV     hook (play/pause)
//     50 - 250 mV   volume up
//     350 - 550 mV  volume down
//
// The module does the rest itself, counting clicks on the centre button the way
// the Apple remote it is named after does: one = KEY_PLAYPAUSE (164), two =
// KEY_NEXTSONG (163), three or more = KEY_PREVIOUSSONG (165). Held down it also
// emits KEY_FASTFORWARD and KEY_REWIND, which neither the stock firmware nor
// this one acts on.
//
// The module starts OFF: `gi_earpods_adc_sw` lives in .bss and is zero until
// something writes "on" to its sysfs attribute. The stock binary does exactly
// that from its "In-line remote" setting (key `line_control` in /data/menu_cfg):
//
//     find /sys/devices/platform/earpods_adc/earpods_adc -name earpods_adc_sw
//     echo on > <that file>
//
// The search here walks the directory rather than shelling out; the file and
// the word written into it are the same.
//
// The module does not read the ADC at all with an empty jack: before each pass
// it asks sa_sound_switch whether `headset` is plugged in. So on does not mean
// "always draws power", it means "draws power with headphones connected".

// Reads the configuration ([system], key headset_controls) and applies what it
// finds. Call once at startup, after config_init.
void headset_init(void);

bool headset_controls_enabled(void);
void headset_set_controls_enabled(bool enabled); // saves and applies at once

// True when the module's sysfs file exists: on a device without the module
// loaded (or on the desktop build) the option has nothing to switch on.
bool headset_supported(void);

#endif /* HEADSET_H */
