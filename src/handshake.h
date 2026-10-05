#ifndef HANDSHAKE_H
#define HANDSHAKE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "Limelight.h"

typedef struct {
    char address[64];
    char client_cert_path[256];
    char client_key_path[256];
    char server_cert_hash_path[256];
    char unique_id[64];
    char host_uuid[40];          // the HOST's <uniqueid> (not ours); "" until known
    char rtsp_session_url[256];
    char server_app_version[32]; // e.g. "7.1.431.0" from /serverinfo

    // Apollo / Vibepollo host extensions, read from the HTTPS /serverinfo.
    // Plain Sunshine sends none of these, so everything below stays zero and
    // the client behaves exactly as before.
    int  is_apollo;        // host sent <VirtualDisplayCapable> (Apollo family)
    int  vd_capable;       // host can create a virtual display for us
    int  has_perm;         // <Permission> was present (this client's mask)
    unsigned int perm;     // Apollo PERM bitmask for THIS paired client
    // Last non-200 status_message the host sent, e.g. Vibepollo's
    // "Permission denied: this device lacks the "Launch" permission...".
    // Shown on the error screen instead of a generic failure.
    char last_status[192];
} handshake_info_t;

// Apollo PERM bits (Apollo src/crypto.h; Vibepollo keeps the same layout).
// New clients get only LIST|VIEW, so a freshly paired PS3 can see apps but
// cannot launch one or send controller input until the host grants it.
#define HV_PERM_INPUT_CONTROLLER 0x00000100u
#define HV_PERM_INPUT_MOUSE      0x00000800u
#define HV_PERM_INPUT_KBD        0x00001000u
#define HV_PERM_LIST             0x01000000u
#define HV_PERM_VIEW             0x02000000u
#define HV_PERM_LAUNCH           0x04000000u

#define MAX_APP_ENTRIES 32

typedef struct {
    int id;
    char name[64];
    char uuid[40];   // Apollo/Vibepollo <UUID>; empty on plain Sunshine
} ps3_app_entry_t;

typedef struct {
    ps3_app_entry_t apps[MAX_APP_ENTRIES];
    int count;
} ps3_app_list_t;

int hv_init(handshake_info_t *info, const char *address);
int hv_get_server_info(handshake_info_t *info);
int hv_get_first_appid(handshake_info_t *info);
int hv_get_app_list(handshake_info_t *info, ps3_app_list_t *list);
int hv_is_paired(handshake_info_t *info);
// otp_passphrase: NULL for normal pairing (client shows a PIN, user types it
// into the host).  Non-NULL for Apollo/Vibepollo OTP pairing, where the HOST
// issues the PIN and a passphrase and the client proves it knows both.
int hv_pair(handshake_info_t *info, const char *pin, const char *otp_passphrase);
// app_uuid may be NULL/empty (plain Sunshine).  virtual_display asks an
// Apollo-family host for a virtual display at the stream mode.
int hv_launch(handshake_info_t *info, int app_id, const char *app_uuid,
              int virtual_display, const char *rikey, int rikeyid);
// Ask the host to quit the running app / end the session (/cancel).
int hv_quit_app(handshake_info_t *info);
// Same, for the main menu (shorter timeouts, does not abort on the menu state).
int hv_quit_app_background(handshake_info_t *info);

// Host identity: key the pinned certificate on the host's <uniqueid> from
// plain-HTTP /serverinfo, migrating a pairing saved under the old IP-keyed name.
int hv_probe_host_uuid(const char *address, char *out, size_t out_size);
int hv_bind_host_identity(handshake_info_t *info);
// What is running on the host right now (0 = nothing); needs a paired client.
int hv_get_current_game(handshake_info_t *info, int *game_id);
int hv_app_name_for_id(handshake_info_t *info, int app_id, char *out, size_t out_size);
// Human-readable list of permissions this client is missing for streaming,
// or 0 if nothing is missing / the host does not use permissions.
int hv_missing_permissions(const handshake_info_t *info, char *out, size_t out_size);

// Internal helpers (could be exposed if needed)
int hv_generate_credentials(handshake_info_t *info);

#endif
