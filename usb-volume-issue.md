# USB-C DAC stuck at full volume: the dongle reports an impossible dB scale

Related: #12

## Summary

With some USB-C dongles, every volume index from 1 to 100 plays at full scale; only 0 is silent. The cause is in `usbaudio_apply_volume()` (`src/system/audio/usbaudio.c`): it trusts the dB scale (TLV) the dongle publishes for its volume control, and when that scale is wrong the software attenuation cancels itself out.

## Evidence

HiBy R1, Sonix 1.2.0, JSAUX USB-C to 3.5 mm adapter. Log with `log_to_sd` on:

```
usbaudio: card3 'Audio' at /sys/devices/platform/ahb2/13500000.otg_new/usb1/1-1/1-1:1.0; playback goes to plughw:3,0
usbaudio: volume goes to 'PCM Playback Volume' (0..255, 1 ch, -128.00..-127.00 dB)
```

The control claims 256 steps covering **-128 dB to -127 dB**: its top step is supposedly 127 dB below full scale. That is a descriptor (or kernel) error; the real range is almost certainly about -127..0 dB.

A full sweep from 59 % down to 0 % follows in the log, with no "did not take" line, and the audible level does not change.

## Why that pins the level at full scale

By default (`[usb] hardware_volume = 0`) the code:

1. asks `snd_tlv_convert_from_dB()` for the step nearest 0 dB. Nothing on this scale gets there, so it returns the top step, `value = 255`, which is the dongle's real maximum;
2. reads that step's level back off the same scale: `hw_db = -12700` (-127 dB);
3. gives the samples the difference: `rest = target - hw_db = target + 127 dB`;
4. clamps it: `swvolume_set_usb_gain(rest < 0 ? rest : 0, mute)`.

`target` runs from -94 dB (index 1) to 0 dB (index 100), so `rest` is positive at every index and becomes 0, which is unity gain. `swvolume_active()` then reports false, the samples go out untouched, and the control sits at its maximum. Only index 0 is silent, because mute is handled separately.

The "stuck control" check does not catch it either, because 255 really is the top of the range.

## Proposed fix

Two small changes in `src/system/audio/usbaudio.c`:

1. **Reject implausible dB scales** in `find_volume_control()`. A scale whose top is below -20 dB or above +24 dB, or that spans less than 10 dB, is treated as no scale at all. The existing no-scale path then holds the control at its top and puts the whole level on the samples.

   ```c
   if (volume_has_db && (db_max < -2000 || db_max > 2400 || db_max - db_min < 1000)) {
       fprintf(stderr, "usbaudio: '%s' reports an implausible dB scale (%.2f..%.2f dB); ignoring it\n", elem,
               db_min / 100.0, db_max / 100.0);
       volume_has_db = false;
   }
   ```

2. **Don't let the device's reported level reduce the software attenuation in the default mode.** Only `hardware_volume = 1` splits the level between the control and the samples; by default the samples take all of it.

   ```c
   long rest = hardware ? target - hw_db : target;
   ```

Either change alone fixes this dongle. Together, a different broken scale that the plausibility check misses still cannot turn the software volume off.

Trade-off: a dongle whose real maximum is below 0 dB comes out slightly quieter overall, which is the safe direction.

## Expected result

The log shows `usbaudio: 'PCM Playback Volume' reports an implausible dB scale (-128.00..-127.00 dB); ignoring it`, and the volume follows the same curve as the 3.5 mm jack: silent at 0, smooth up to 100.

## Not covered here

`audio_external_write()` (AirPlay, Bluetooth receiver, emulator) never applies the software volume, so those sources still play at full scale over USB-C.
