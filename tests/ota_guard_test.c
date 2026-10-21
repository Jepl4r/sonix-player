#include "../sonix-player/src/system/device/ota.c"
#include <assert.h>
static bool attached, exported, writable = true;
static int network_calls;
static const char *card_root;
const char *storage_sd_root(void) { return card_root; }
bool storage_card_attached(void) { return attached; }
bool storage_sd_writable(void) { return writable; }
bool usb_storage_active(void) { return exported; }
bool http_stream_open(http_stream_t *stream, const char *url,
                      int timeout_secs) {
  network_calls++;
  return false;
}
const char *http_last_error(void) { return "test"; }
int main(int argc, char **argv) {
  assert(argc == 2);
  card_root = argv[1];
  assert(do_download() == OTA_NO_CARD);
  assert(network_calls == 0);
  attached = true;
  exported = true;
  assert(do_download() == OTA_NO_CARD);
  assert(network_calls == 0);
  exported = false;
  writable = false;
  assert(do_download() == OTA_DOWNLOAD_FAILED);
  assert(network_calls == 0);
  return 0;
}
