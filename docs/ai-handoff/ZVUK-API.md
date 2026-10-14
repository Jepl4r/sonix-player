# Zvuk (zvuk.com, ex-SberZvuk) — API notes for Sonix Player

Written 2026-10-14 before implementing the Zvuk service (branch r131-zvuk), updated after it
(sign-in by card file only, diagnostics, mock).
zvuk.com itself is NOT reachable from the build sandbox (egress proxy refuses it), so
everything below comes from open-source clients that are maintained against the live
service, read in full. Nothing was verified against a real account: see "Uncertain".

## Sources (read, not just skimmed)

| Source | Version / date | What it gave |
|---|---|---|
| `zvuk-music` (PyPI, MIT, https://github.com/trudenboy/zvuk-music) | 0.6.3 | the full GraphQL query texts (`graphql/queries/*.graphql`, `mutations/*.graphql`), Tiny REST endpoints, headers, error mapping (401/403/404/418/429, HTML anti-bot page), quality enums, anonymous-token call |
| Music Assistant provider `trudenboy/ma-provider-zvuk-music` | 1.8.11, 2026-10-07 | how a live integration streams *today*: one GraphQL `getStream` for all qualities; `flac` field = progressive **FLAC-in-MP4, no DRM**; MP3 320/128 fallback; anti-bot blocks `/api/tiny/profile` and direct stream links at times → verify token through GraphQL; "personal" playlists = fixed synthesis ids |
| `oshokin/zvuk-grabber` (Go) | 2026-10-09 HEAD | REST `/api/tiny/*` needs a `Referer` (anti-bot answers **418** without it; the request URL itself is accepted as Referer); auth also works as cookie `auth=<token>`; `/api/v2/tiny/profile` → `$.result.profile.token`, `$.result.subscription{title, expiration(ms)}`; FLAC download may be native FLAC **or** FLAC-in-MP4 (`ftyp`), remuxed by copying the `dfLa` metadata + `mdat` frames; browser login = phone + SMS behind Sber/Zvuk OAuth with stealth Chrome |
| music-assistant.io/music-providers/zvuk | page | "Zvuk has no sign-in for outside apps, so you copy a token from your browser": sign in at zvuk.com, open zvuk.com/api/tiny/profile, copy `"token"`; it expires periodically |
| lucida-module-zvuk README (abandoned) | — | token is 32 hex chars; FLAC there needed a decryptor (that was `flacdrm`) |

## Authentication

- Header `X-Auth-Token: <token>` on GraphQL and Tiny calls (zvuk-music). zvuk-grabber sends the
  same token as cookie `auth=<token>`. Sonix sends **both**.
- The token is the user's web-session token: 32 lowercase hex characters in practice.
  How a user gets it: sign in at zvuk.com in any browser → open `https://zvuk.com/api/tiny/profile`
  → copy the value of `"token"`. It expires "periodically" (no figure published).
- **No headless login exists in any open client.** Phone + SMS goes through an OAuth web flow
  protected by anti-bot JavaScript (zvuk-grabber needs a real Chrome with stealth patches and human-like
  mouse moves). No device/QR flow, no refresh token.
- **Sonix (owner's decision, ZVUK-DECISION.md): the token comes ONLY from a file on the card**,
  `zvuk_token.txt` at the card's root (generic mechanism `cardimport.c`, also used for `subsonic.txt`).
  No on-screen keyboard entry, no QR code, no SonixLink paste page. Format below.
- Anonymous token: `GET /api/tiny/profile` without auth returns a fresh anonymous token
  (`result.is_anonymous = true`): mid (128 kbps) only, no collection, no likes. Not offered by Sonix
  (nothing to browse without a collection), only detected and refused at sign-in.

## Endpoints used

Base `https://zvuk.com` (`[zvuk] base_url` in device_config.ini, so the emulator can use the mock).
Common headers (zvuk-music): `Accept: application/json`, `Origin: https://zvuk.com`,
`Referer: https://zvuk.com/` (GraphQL) / the request URL itself (Tiny), a browser User-Agent.

### Tiny REST (`/api/tiny/...`, JSON `{"result": {...}}`)
- `GET /api/v2/tiny/profile` → `result.profile{id,name,token,is_anonymous...}`, `result.subscription{title,expiration}`
  (v1 `/api/tiny/profile` → `result{...profile fields, token, is_anonymous}`). Often blocked by anti-bot → fall back to GraphQL.
- `GET /api/tiny/track/stream?id=<id>&quality=flac|high|mid` → `result.stream` (direct URL, no DRM). Used only as a fallback.
- (not used) `/api/tiny/lyrics?track_id=`, `/api/tiny/grid/content?name=editorial_playlist`, `/api/tiny/releases`, `/api/tiny/playlists`, `/api/tiny/labels`.

### GraphQL `POST /api/v1/graphql` (`{"query", "operationName", "variables"}` → `{"data", "errors"}`)
- `getStream($ids)` → `mediaContents[].stream{expire, expireDelta, flac, flacdrm, high, mid}`. `flac` = FLAC-in-MP4 (or native FLAC), `flacdrm` = Widevine-wrapped (unusable), `high` = MP3 320, `mid` = MP3 128. URLs are signed CDN links, valid `expireDelta` seconds.
- `getTracks($ids)` → `id title duration explicit hasFlac position artists{id title} release{id title date image{src}}`.
- `userTracks($orderBy:dateAdded, $orderDirection:desc)` → `collection.tracks[]{id}` — liked tracks, ids only.
- `userCollection` → `collection{artists,releases,playlists,synthesisPlaylists,tracks...}[]{id}`.
- `getReleases($ids)` → release with `tracks{...}`, `artists`, `date`, `image{src}`, `type`.
- `getArtists($ids, withReleases, withPopTracks, releasesLimit, tracksLimit...)`.
- `getPlaylists($ids)` → `title description image{src} tracks{id} userId` (all track ids at once).
- `search($query, $limit, $tracks/$artists/$releases/$playlists...)` → `search{tracks|artists|releases|playlists{page{total,next,cursor}, items[]}}`.
- Mutations `addItemToCollection($id,$type)` / `removeItemFromCollection($id,$type)`; type `track|release|artist|playlist`.
- "For you": playlists with fixed low ids `3,4,6,11,12,13,14,15` (synthesis/personal), per account, read with `getPlaylists` (MA 1.8.x).

## Formats
| quality | what | needs |
|---|---|---|
| flac | FLAC 16/24-bit, usually **FLAC in non-fragmented MP4** (ftyp, moov, mdat), sometimes native FLAC | subscription |
| high | MP3 320 | subscription per zvuk-music; "all accounts" per MA — uncertain |
| mid | MP3 128 | always |
Not encrypted (`flac`, `high`, `mid`); `flacdrm` is DRM and ignored. Sonix unwraps FLAC-in-MP4 into a plain `.flac` while it downloads (mp4flac.c, as for Yandex), so it plays while downloading when `moov` comes before `mdat`; when it does not, the frames are parked aside and the file is assembled at the end (plays once complete).

## Errors, limits
- 401/403 → token invalid/expired (sign in again). GraphQL may also answer 200 with `errors[].message` (e.g. "Unauthorized").
- 418, or an HTML page ("bot activity"), → ServicePipe anti-bot. For Tiny calls the cure is the `Referer` header; for the rest: back off.
- 429 with optional `Retry-After` → rate limit. No published numbers; zvuk-music's optional throttler defaults to 5 req/s; zvuk-grabber warns that parallel downloads "can trip rate limits". Sonix: ≥ 250 ms between requests, 15/60/120 s back-off after a refusal, one download at a time.
- 404 → gone.

## Not found / not implemented
- **"Моя волна"/personal endless stream**: no open client uses one; the web app's "волна" is not documented anywhere public. Not implemented; "Для вас" (synthesis playlists) stands in.
- Listen/scrobble events: none in the open clients (only `listeningHistory` query). Not sent.

## Uncertain without a real account (owner must check, log lines in the report)
1. Whether `getStream` answers `flac` for the account, and whether the FLAC is MP4 with moov first.
2. Whether `/api/v2/tiny/profile` answers from the device (anti-bot), and its exact subscription fields.
3. Whether the browser User-Agent is required (`[zvuk] user_agent` can override).
4. Whether the synthesis playlist ids 3..15 exist on the account.
5. How long a token lives.


## The token file (for the 4pda post)

`zvuk_token.txt` in the ROOT of the microSD card (not in Music/). Plain text, UTF-8; a BOM,
Windows line ends and spaces around are fine; `zvuk_token.txt.txt` (Windows hiding the extension)
is accepted too; files over 4 KB are refused.

    a1b2c3d4e5f60718293a4b5c6d7e8f90

The first non-empty line is the token (32 hex characters as copied from the `"token"` value of
https://zvuk.com/api/tiny/profile while signed in in a browser). Also understood on that line:
`token=...`, the value in quotes, or the whole profile JSON pasted in one line.

Looked for at start-up, when a card is mounted, and every time the Zvuk page opens
("Проверить карту ещё раз" button too). Outcomes:

| outcome | file | message |
|---|---|---|
| profile answered (or GraphQL when the profile is blocked) | deleted; token saved to `/usr/data/zvuk_session.ini` (tmp+fsync+rename, 0600) | «Звук: вошли как …» |
| no network / no answer / 429 / 5xx | kept, retried at the next occasion | «Нет сети: zvuk_token.txt остаётся на карте…» (silent at boot) |
| 401/403, GraphQL "unauthorized", or an anonymous (guest) token | renamed `zvuk_token.txt.bad` | «Токен Звука не принят» / guest-token sentence |
| 418 or an HTML page (ServicePipe) | kept | «Звук заблокировал запрос» |
| empty, oversize, nothing token-like | renamed `.bad` | «… пустой или слишком большой: переименован в .bad» / «В zvuk_token.txt нет токена» |

Expired in use (401/403 on any call) → signed out, «Токен Звука истёк — положите новый
zvuk_token.txt на карту»; tracks saved on the card (Music/Zvuk/…) keep playing. Factory reset deletes
`/usr/data/zvuk_session.ini`.

## subsonic.txt (same mechanism)

`subsonic.txt` in the card's root, three lines — server address, login, password:

    https://music.example.org
    alice
    secret

or `key = value` lines in any order (`server`/`url`, `user`/`login`/`username`, `password`/`pass`).
A server without `http(s)://` gets `http://`. Accepted → signed in as on the sign-in page (session in
device_config.ini), file deleted; wrong login/password → `subsonic.txt.bad` and the server's own
reason; no network → kept.

## Diagnostics in the log (what the owner's first device log answers)

On token import, on the first request of every session and in Developer options →
«Звук: проверка соединения» (profile, collection, getStream on a liked track or `[zvuk] check_track`,
then the first 16 bytes of the file):

    zvuk: trying the token from the card (32 characters), base https://zvuk.com
    zvuk: diag GET /api/tiny/profile -> HTTP 200, content-type "application/json; ...", 200 bytes, JSON
    zvuk: diag POST /api/v1/graphql getStream -> HTTP 200, content-type "...", 316 bytes, JSON
    zvuk: stream 61318527: asked flac, got flac (flac yes, flacdrm no, high yes, mid yes), host cdn..., expires in 600 s, expire ...
    zvuk: diag file at cdn... -> 25123456 bytes, starts 00 00 00 20 66 74 79 70 ... = MP4 (FLAC in MP4)
    cardimport: zvuk: zvuk_token.txt (1 line): accepted

Blocked looks like:

    zvuk: diag GET /api/tiny/profile -> HTTP 418, content-type "text/html", 142 bytes, NOT JSON
    zvuk: diag first 200 bytes: <!DOCTYPE html>...
    zvuk: diag BLOCKED by the anti-bot filter (ServicePipe): the firmware's request did not pass
    zvuk: the profile is blocked, trying the token through GraphQL

Every getStream logs "asked X, got Y" with which of flac/flacdrm/high/mid came.

## Mock

`tools/zvuk_mock.py` (fixtures: `--make-fixtures DIR`, ffmpeg): GraphQL ops used by Sonix, Tiny
profile v1, CDN with Range; tokens for subscription / free / anonymous / anti-bot; modes ok, 418,
html, 429, 500, 401, expire (first address of a track already expired), drop (CDN cuts once half
way), slow. Tracks: 1001 FLAC-in-MP4 moov first, 1002 moov last, 1003 native FLAC, 1004 flacdrm
only, 1005 MP3 only, 1009 no file, 2000–2299 liked filler.
