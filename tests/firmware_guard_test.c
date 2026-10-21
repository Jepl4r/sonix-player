#define HOST_BUILD 1
#define POWER_H /* Host firmware lookup does not use the LVGL power API. */
#include "../sonix-player/src/system/device/firmware.c"
#include <assert.h>
static const char *card_root;
static bool attached, exported;
const char *storage_sd_root(void) { return card_root; }
bool storage_card_attached(void) { return attached; }
bool usb_storage_active(void) { return exported; }
const char *config_get(const char *section, const char *key,
                       const char *fallback) {
  return "r1";
}
int main(int argc, char **argv) {
  assert(argc == 2);
  card_root = argv[1];
  char path[512];
  snprintf(path, sizeof(path), "%s/r1.upt", card_root);
  FILE *f = fopen(path, "wb");
  assert(f);
  fputs("fixture", f);
  fclose(f);
  assert(!firmware_update_file_find(path, sizeof(path)));
  attached = true;
  exported = true;
  assert(!firmware_update_file_find(path, sizeof(path)));
  exported = false;
  assert(firmware_update_file_find(path, sizeof(path)));
  return 0;
}
