#ifndef CARDIMPORT_H
#define CARDIMPORT_H

#include <stdbool.h>
#include <stddef.h>

// Signing in to a service from a file on the card.
//
// The player has no keyboard worth typing a token, a password or a server's
// address on, and for some services (Zvuk) there is no sign-in an outside
// application can do at all: what the user has is a string copied out of a
// browser. So the string travels the way files already do -- USB from a
// computer, or the Wi-Fi file transfer -- into a small text file at the root
// of the card, and the player picks it up:
//
//   zvuk_token.txt   the Zvuk web session's token, one line
//   subsonic.txt     server address, login, password, one per line
//
// Each service registers its file's name and a function that tries what the
// file says (cardimport_register). The files are looked for at start-up, when
// a card is mounted and when the service's page opens; reading one strips a
// UTF-8 BOM, carriage returns and the spaces at both ends of every line, and
// drops empty lines. "name.txt.txt" is accepted too -- what Windows makes of
// "name.txt" with its extensions hidden. A file over CARDIMPORT_MAX_BYTES is
// not a token and is not read.
//
// What happens to the file depends on how the attempt went, and is the same
// for every service:
//
//   CARDIMPORT_OK        the service kept the account (its own session file,
//                        in internal storage); the file is deleted, so the
//                        secret does not sit on a card that goes from hand to
//                        hand
//   CARDIMPORT_OFFLINE   no network, or no answer: the file stays and is tried
//                        again at the next occasion
//   CARDIMPORT_REFUSED   the service said no (401/403, wrong password): the
//                        file is renamed "<name>.bad", so it is not tried
//                        again at every start, and stays there to look at
//   CARDIMPORT_BLOCKED   an anti-bot filter answered instead of the service:
//                        the file stays (nothing is wrong with it)
//   CARDIMPORT_INVALID   nothing in the file looks like what it should hold:
//                        renamed "<name>.bad" as for a refusal
//
// The outcome goes to the listeners with a sentence fit to show; and to the
// log, with the file's name, never its contents.
//
// Thread contract: register at start-up (main thread, before the first
// check). The rest may be called from any thread and never blocks: the
// attempts run on a thread of their own, one at a time, started on the first
// check that finds a file -- a player with no such file never has it. The
// validate callbacks and the listener run on that thread.

#define CARDIMPORT_MAX_BYTES 4096
#define CARDIMPORT_MAX_LINES 8
#define CARDIMPORT_LINE_MAX 512

typedef enum {
	CARDIMPORT_OK = 0,
	CARDIMPORT_OFFLINE,
	CARDIMPORT_REFUSED,
	CARDIMPORT_BLOCKED,
	CARDIMPORT_INVALID,
} cardimport_result_t;

// Tries the file's non-empty lines (trimmed, at least one). Blocks on the
// network. `message` gets the sentence to show for any outcome (may be left
// empty for a default). Never logs the secret itself.
typedef cardimport_result_t (*cardimport_validate_fn)(const char lines[][CARDIMPORT_LINE_MAX], int count,
													  char *message, size_t message_size);

// `service` names it in the log and to the listener ("zvuk"); `file_name` is
// the file at the card's root ("zvuk_token.txt"). Both must live for ever.
void cardimport_register(const char *service, const char *file_name, cardimport_validate_fn validate);

// The card, as the storage code knows it (start-up, mount, unmount): every
// registered file is looked for on it. NULL or empty: no card.
void cardimport_set_root(const char *sd_root);

// Looks for one service's file now (its page was opened). Cheap when there is
// none: two stat() calls.
void cardimport_check(const char *service);

// Whether an attempt for `service` is queued or running, for a page that wants
// to say "signing in...".
bool cardimport_busy(const char *service);

// Listeners hear every outcome but an unasked-for OFFLINE (see the .c): the
// interface's toast (streaming.c), a page that shows the state of its file.
// Up to four; they run on the import thread.
typedef void (*cardimport_listener_t)(const char *service, cardimport_result_t result, const char *message);
void cardimport_add_listener(cardimport_listener_t listener);

// The last outcome for `service` this session and its sentence; false when
// there was none. Any thread.
bool cardimport_last(const char *service, cardimport_result_t *result, char *message, size_t size);

#endif /* CARDIMPORT_H */
