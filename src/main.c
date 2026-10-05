#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include <stddef.h>
#include <unistd.h>

#include <net/net.h>
#include <net/netctl.h>
#include <net/socket.h>
#include <netinet/in.h>
#include <sys/process.h>
#include <sysmodule/sysmodule.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <tiny3d.h>
#include <lv2/systime.h>

#include "audio.h"
#include "connection.h"
#include "handshake.h"
#include "net_logger.h"
#include "random.h"
#include "ui.h"
#include "video.h"
#include "input.h"
#include "moonlight_discovery.h"

SYS_PROCESS_PARAM(1001, 0x100000)

// PS3 watchdog'u besleme - şimdilik devre dışı (crash yapıyor olabilir)
// static void pump_sysutil() { sysUtilCheckCallback(); }

// ---------------------------------------------------------------------------
// libnet memory pool  --  OFF BY DEFAULT, and it should stay that way
//
// This was built on the theory that PSL1GHT's 128 KB pool was the bitrate cap.
// That theory was then TESTED ON HARDWARE in the JellyFin-PS3 client and
// FALSIFIED.  Same movie, same 25 Mbps:
//
//   stock 128 KB pool : net= median 25.0 Mbps, ring empty in  8% of heartbeats
//   4 MB pool         : net= median 12.0 Mbps, ring empty in 55% of heartbeats
//
// Throughput HALVED, and getsockopt(SO_RCVBUF) went from working to failing
// outright on the same line of code -- a pool that was merely too large would
// not break getsockopt, so the ...Ex path leaves libnet in a worse state than
// the stock init does.  A build that had been flawless at 25 Mbps started
// stuttering.
//
// It is kept only so the remaining question can be answered cheaply: is the
// damage the SIZE, or the netInitializeNetworkEx() path itself?  Putting 128 in
// the file below asks for the stock size through the Ex path and isolates that.
//
// Size in KB in /dev_hdd0/tmp/moonlight_netpool.txt.  Absent or 0 = stock
// netInitialize(), which is exactly what shipped before any of this.
//
// PSL1GHT's netInitialize() hands libnet a fixed 128 KB pool (LIBNET_MEMORY_SIZE
// in ppu/sprx/libnet/init.c) that every socket on the console shares: the video
// RTP socket, the audio RTP socket, the ENet control channel and the TCP
// RTSP/HTTP sockets.  Socket receive buffers are charged against that pool, so
// SO_RCVBUF on the video socket could never be pushed past ~64 KB no matter what
// was requested -- roughly 50 ms of headroom at 10 Mbps, and only 20 ms at
// 25 Mbps.  Once the receive thread falls behind by more than that the kernel
// drops UDP packets, and the stream degrades no matter how fast the link is.
//
// Ask libnet for a much larger pool up front.  The accepted size is firmware
// dependent and undocumented, so try descending sizes and fall back to
// PSL1GHT's stock initialiser if every one of them is refused.
// Read by SdpGenerator.c when building the Sunshine SDP offer.
volatile int ps3_request_intra_refresh = 1;

// Read by VideoStream.c when binding the video socket.  KB; 0 = size it from
// the frame burst, which is the default and lands at 256K-1M depending on mode.
//
// This was very nearly set to 128 KB on the reasoning that Movian -- a mature
// PS3 player -- deliberately asks for only 128 KB here while asking 192 KB on
// desktop.  That was MEASURED ON HARDWARE in the JellyFin-PS3 client and the
// borrowed constant lost: 512 KB beat 128 KB.  Borrowing a constant from
// another program is reasoning about a different workload, not measurement of
// this one, and it has now been wrong once.
//
// The knob stays because A/B-ing it costs seconds over FTP:
// /dev_hdd0/tmp/moonlight_rcvbuf.txt
volatile int ps3_video_rcvbuf_kb = 0;

// Apollo/Vibepollo OTP pairing credentials; see the pairing step below.
#define OTP_FILE_PATH "/dev_hdd0/tmp/moonlight_otp.txt"

static void *g_net_pool = NULL;
static u32 g_net_pool_size = 0;

#define NETPOOL_FILE "/dev_hdd0/tmp/moonlight_netpool.txt"

static u32 net_pool_kb_setting(void) {
  FILE *f = fopen(NETPOOL_FILE, "r");
  if (!f) return 0;               // absent = stock, the default
  int kb = 0;
  if (fscanf(f, "%d", &kb) != 1 || kb < 0) kb = 0;
  fclose(f);
  return (u32)kb;
}

static int net_init_pool(void) {
  u32 kb = net_pool_kb_setting();

  if (kb == 0) {
    return netInitialize();       // stock 128 KB -- the measured-good path
  }

  netInitParam params;
  g_net_pool = memalign(64, kb * 1024);
  if (g_net_pool) {
    memset(&params, 0, sizeof(params));
    params.memory = (u32)((u64)g_net_pool);
    params.memory_size = kb * 1024;
    params.flags = 0;

    if (netInitializeNetworkEx(&params) == 0) {
      g_net_pool_size = kb * 1024;
      return 0;
    }
    free(g_net_pool);
    g_net_pool = NULL;
  }

  return netInitialize();
}

// One throwaway socket at startup, so libnet's health is a line at the TOP of
// the log instead of something only visible once a stream is already failing.
// If set/get disagree, or get fails, the pool is in a bad state -- which is the
// signature the Jellyfin 4 MB build produced.
static void net_probe(void) {
  int s = socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) { NLOG("net: probe socket() failed"); return; }

  int want = 128 * 1024, got = -1;
  socklen_t len = sizeof(got);
  int sr = setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&want, sizeof(want));
  int gr = getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)&got, &len);
  NLOG("net: probe set=%d get=%d rcvbuf=%d%s", sr, gr, got,
       (gr != 0) ? "  <-- getsockopt FAILING, libnet is in a bad state" : "");
  netClose(s);
}

static void net_free_pool(void) {
  if (g_net_pool) {
    // We initialised libnet ourselves, so netDeinitialize()'s internal free()
    // does not cover this allocation.
    netFinalizeNetwork();
    free(g_net_pool);
    g_net_pool = NULL;
    g_net_pool_size = 0;
  } else {
    netDeinitialize();
  }
}

// The session being streamed, so the PS-button quit can end it on the host.
// Set only while a launched session is live.
static handshake_info_t *g_active_hinfo = NULL;
static int g_host_quit_sent = 0;

static void sysutil_exit_callback(u64 status, u64 param, void *usrdata) {
  (void)param;
  (void)usrdata;
  // The XMB is in front: the stream is not, so the pad must not keep buzzing.
  if (status == SYSUTIL_MENU_OPEN) ps3input_set_rumble_paused(1);
  if (status == SYSUTIL_MENU_CLOSE) ps3input_set_rumble_paused(0);
  if (status == SYSUTIL_EXIT_GAME) {
    NLOG("SYSUTIL_EXIT_GAME received. Exiting Moonlight PS3...");
    // Quit the app on the host FIRST.  After this event the system gives the
    // app only moments before killing it, so the normal end-of-stream path
    // never ran -- the log stopped at "Control stream connection failed".
    if (g_active_hinfo && !g_host_quit_sent && ui_get_quit_on_exit()) {
      g_host_quit_sent = 1;
      hv_quit_app(g_active_hinfo);
    }
    ps3input_rumble_stop();
    usleep(40000); // let the input thread deliver the zero before the system kills us
    LiInterruptConnection();
    ui_stop();
  }
}

// ---------------------------------------------------------------------------
// Host identity helpers.  A saved host is recognised by the host's own
// <uniqueid>, so a changed IP or a renamed PC does not cost a re-pair.
// ---------------------------------------------------------------------------

// A saved host stopped answering at its address.  Scan the LAN and look for its
// uniqueid at a different one; if found, re-point the saved host there.
static int relocate_saved_host(int idx) {
  const ui_saved_host_t *h = ui_get_saved_host(idx);
  if (!h || !h->uuid[0]) return 0;
  char want[40];
  snprintf(want, sizeof(want), "%s", h->uuid);
  char old_addr[16];
  snprintf(old_addr, sizeof(old_addr), "%s", h->address);

  NLOG("Host %s did not answer; scanning for uniqueid %s...", old_addr, want);
  mld_host_t found[MLD_MAX_HOSTS];
  int n = mld_scan(found, MLD_MAX_HOSTS, 0);
  for (int i = 0; i < n; i++) {
    if (strcmp(found[i].address, old_addr) == 0) continue;
    char uuid[40];
    if (hv_probe_host_uuid(found[i].address, uuid, sizeof(uuid)) != 0) continue;
    if (strcmp(uuid, want) != 0) continue;
    NLOG("Host moved: %s -> %s (%s)", old_addr, found[i].address, found[i].name);
    ui_set_host_address(idx, found[i].address, found[i].name);
    ui_save_settings();
    return 1;
  }
  NLOG("Host with uniqueid %s not found on the LAN", want);
  return 0;
}

// Menu-side view of the selected host: is anything running on it?  Refreshed
// from the idle loop while the main menu is up.  The host's /serverinfo
// <currentgame> is the truth, so it also survives an app restart.
static handshake_info_t menu_info;
static int menu_info_ready = 0;
static int menu_host_idx = -1;
static int menu_game = 0;
static char menu_game_name[64] = "";
static u64 menu_next_probe_us = 0;
static int menu_prev_state = -1;

static void menu_status_clear(void) {
  menu_game = 0;
  menu_game_name[0] = '\0';
  ui_set_host_status(0, "", 0);
}

static void menu_probe_tick(void) {
  int state = ui_get_state();
  int entered = (menu_prev_state != UI_STATE_IP_ENTRY && state == UI_STATE_IP_ENTRY);
  menu_prev_state = state;
  if (state != UI_STATE_IP_ENTRY) return;
  if (entered) menu_next_probe_us = 0;

  int quit_wanted = ui_take_quit_request();
  int idx = ui_get_selected_host_index();
  const ui_saved_host_t *sh = ui_get_saved_host(idx);
  // Only a paired host will answer over HTTPS.
  if (!sh || !sh->paired) {
    if (menu_info_ready) { menu_info_ready = 0; menu_status_clear(); }
    return;
  }

  if (!menu_info_ready || idx != menu_host_idx || strcmp(sh->address, menu_info.address) != 0) {
    menu_status_clear();
    if (hv_init(&menu_info, sh->address) != 0) return;
    hv_bind_host_identity(&menu_info);
    menu_host_idx = idx;
    menu_info_ready = 1;
    menu_next_probe_us = 0;
  }

  if (quit_wanted) {
    hv_quit_app_background(&menu_info);
    menu_status_clear();
    menu_next_probe_us = 0;
  }

  u64 now = sysGetSystemTime();
  if (now < menu_next_probe_us) return;

  int game = 0;
  if (hv_get_current_game(&menu_info, &game) == 0) {
    if (game != menu_game) {
      menu_game_name[0] = '\0';
      if (game) hv_app_name_for_id(&menu_info, game, menu_game_name, sizeof(menu_game_name));
      menu_game = game;
    }
    ui_set_host_status(game, menu_game_name, menu_info.is_apollo);
    menu_next_probe_us = now + 8ULL * 1000000ULL;
  } else {
    menu_status_clear();
    menu_next_probe_us = now + 20ULL * 1000000ULL;
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  // Ağı başlat
  // Ağı başlat
  sysModuleLoad(SYSMODULE_NET);
  net_init_pool();
  netCtlInit();

  // Register sysutil callback to handle game exit
  sysUtilRegisterCallback(0, sysutil_exit_callback, NULL);

  // Logger başlat
  net_logger_init();

  if (g_net_pool_size != 0) {
    NLOG("net: libnet pool = %u KB via netInitializeNetworkEx (EXPERIMENTAL - "
         "this halved throughput on the Jellyfin client; delete %s to revert)",
         g_net_pool_size / 1024, NETPOOL_FILE);
  } else {
    NLOG("net: libnet pool = stock 128 KB (netInitialize)");
  }
  net_probe();

  {
    FILE *f = fopen("/dev_hdd0/tmp/moonlight_rcvbuf.txt", "r");
    if (f) {
      int kb = 0;
      if (fscanf(f, "%d", &kb) == 1 && kb > 0) ps3_video_rcvbuf_kb = kb;
      fclose(f);
    }
    NLOG("net: video rcvbuf override = %d KB (%s)", ps3_video_rcvbuf_kb,
         ps3_video_rcvbuf_kb ? "from moonlight_rcvbuf.txt"
                             : "none, sized from the frame burst");
  }

  // Çözünürlük Tespiti
  videoState vstate;
  videoResolution vres;
  videoGetState(VIDEO_PRIMARY, 0, &vstate);
  videoGetResolution(vstate.displayMode.resolution, &vres);

  int width = vres.width;
  int height = vres.height;

  // Eğer çözünürlük tanımsızsa varsayılan 720p kullan
  if (width == 0 || height == 0) {
    width = 1280;
    height = 720;
  }

  NLOG("Detected Resolution: %dx%d", width, height);

  // Report the refresh rate the video output is actually running at.  This is
  // what decides whether a given stream frame rate paces evenly: 30 fps is a
  // clean 2:2 on a 60 Hz output but uneven on 50 Hz, and 50 fps is 1:1 on 50 Hz
  // but an uneven 6:5 on 60 Hz.
  //
  // READ ONLY.  Nothing here reconfigures the display: on this hardware a bad
  // videoConfigure() blanks the panel, and the revert can deadlock behind a
  // flip() that never returns.
  {
    u16 rr = vstate.displayMode.refreshRates;
    NLOG("output: refresh bits=0x%02x%s%s%s%s -> stream fps that paces 1:1: %s",
         rr,
         (rr & VIDEO_REFRESH_59_94HZ) ? " 59.94" : "",
         (rr & VIDEO_REFRESH_50HZ)    ? " 50"    : "",
         (rr & VIDEO_REFRESH_60HZ)    ? " 60"    : "",
         (rr & VIDEO_REFRESH_30HZ)    ? " 30"    : "",
         (rr & VIDEO_REFRESH_50HZ)    ? "50" : "60 (or 30 at a clean 2:2)");
  }

  ui_init(width, height);
  ps3input_start();
  NLOG("Moonlight PS3 UI Initialized");
  
  while (ui_is_running()) {
    if (ui_get_state() == UI_STATE_DISCOVERY) {
      NLOG("Discovery: searching for Sunshine via mDNS...");
      mld_host_t hosts[MLD_MAX_HOSTS];
      int found = mld_scan(hosts, MLD_MAX_HOSTS, 0);
      if (found < 0) found = 0;

      if (found == 0) {
        NLOG("Discovery: no hosts found, falling back to manual entry.");
        ui_set_state(UI_STATE_IP_ENTRY);
        ui_open_osk();
        continue;
      }

      NLOG("Discovery: found %d host(s).", found);

      // A saved host that came back under a different address: follow it by its
      // uniqueid instead of leaving the old address to fail.
      for (int i = 0; i < found; i++) {
        int known = 0;
        for (int k = 0; k < ui_get_saved_host_count(); k++)
          if (strcmp(ui_get_saved_host(k)->address, hosts[i].address) == 0) known = 1;
        if (known) continue;
        char uuid[40];
        if (hv_probe_host_uuid(hosts[i].address, uuid, sizeof(uuid)) != 0) continue;
        int moved = ui_find_host_by_uuid(uuid);
        if (moved >= 0) {
          NLOG("Discovery: saved host %d moved %s -> %s", moved,
               ui_get_saved_host(moved)->address, hosts[i].address);
          ui_set_host_address(moved, hosts[i].address, hosts[i].name);
          ui_save_settings();
        }
      }
      ui_set_discovered_hosts(hosts, found);

      while (ui_is_running() && ui_get_state() == UI_STATE_DISCOVERY && !ui_is_host_selected()) {
        sysUtilCheckCallback();
        usleep(20000);
      }

      if (!ui_is_running() || ui_get_state() != UI_STATE_DISCOVERY) continue;

      if (ui_wants_manual_entry()) {
        ui_reset_host_selection();
        ui_set_state(UI_STATE_IP_ENTRY);
        ui_open_osk();
        continue;
      }

      char chosen_ip[64]; char chosen_name[64];
      if (ui_get_selected_host_ip(chosen_ip, sizeof(chosen_ip)) &&
          ui_get_selected_host_name(chosen_name, sizeof(chosen_name))) {
        char chosen_uuid[40] = "";
        hv_probe_host_uuid(chosen_ip, chosen_uuid, sizeof(chosen_uuid));
        int host_idx = ui_find_host_by_uuid(chosen_uuid);
        if (host_idx >= 0) {
          ui_set_host_address(host_idx, chosen_ip, chosen_name);
        } else {
          host_idx = ui_upsert_saved_host(chosen_name, chosen_ip);
          ui_set_host_uuid(host_idx, chosen_uuid);
        }
        ui_select_host(host_idx);
        ui_save_settings();
        char logmsg[96];
        snprintf(logmsg, sizeof(logmsg), "Selected host: %s (%s)", chosen_name, chosen_ip);
        ui_push_log(logmsg);
      }
      ui_reset_host_selection();
      ui_set_state(UI_STATE_IP_ENTRY);
      continue;
    }

    if (ui_get_state() == UI_STATE_PAIRING) {
      const char *pcIp = ui_get_target_ip();
      NLOG("Attempting to connect to: %s", pcIp);
      ui_set_error_detail("");

      ui_set_pairing_pin("");

      // Handshake
      NLOG("H: Initializing Handshake...");
      handshake_info_t hinfo = {0};
      if (hv_init(&hinfo, pcIp) != 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        NLOG("H: hv_init failed!");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }

      // Key the pairing on the host's uniqueid (migrating an address-keyed one).
      // If the host does not answer and we know its uniqueid, it may have moved.
      {
        int sel = ui_get_selected_host_index();
        if (hv_bind_host_identity(&hinfo) != 0) {
          const ui_saved_host_t *sh0 = ui_get_saved_host(sel);
          if (sh0 && sh0->uuid[0] && ui_get_state() == UI_STATE_PAIRING && relocate_saved_host(sel)) {
            pcIp = ui_get_target_ip();
            if (hv_init(&hinfo, pcIp) == 0) hv_bind_host_identity(&hinfo);
          }
        }
        const ui_saved_host_t *sh1 = ui_get_saved_host(sel);
        if (hinfo.host_uuid[0] && sh1 && strcmp(sh1->uuid, hinfo.host_uuid) != 0) {
          ui_set_host_uuid(sel, hinfo.host_uuid);
          ui_save_settings();
        }
      }

      uint32_t random_value;
      char pin[5];
      if (ps3_random_u32(&random_value) != 0) {
        NLOG("H: Secure random number generation failed.");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }
      snprintf(pin, sizeof(pin), "%04u", (unsigned int)(random_value % 10000));
      int paired = hv_is_paired(&hinfo);
      if (ui_get_state() == UI_STATE_IP_ENTRY) continue;

      // Apollo/Vibepollo OTP pairing.  The host's web UI issues a PIN and a
      // passphrase; put both in /dev_hdd0/tmp/moonlight_otp.txt as
      //   <4-digit PIN> <passphrase>
      // (FTP it over, like the other knobs in /dev_hdd0/tmp).  Consumed and
      // deleted after a successful pairing.  Absent = normal PIN pairing.
      char otp_pass[129] = "";
      int use_otp = 0;
      if (!paired) {
        FILE *of = fopen(OTP_FILE_PATH, "r");
        if (of) {
          char otp_pin[16] = "";
          if (fscanf(of, "%15s %128s", otp_pin, otp_pass) == 2 && strlen(otp_pin) == 4) {
            memcpy(pin, otp_pin, 5);
            use_otp = 1;
            NLOG("H: OTP pairing requested (moonlight_otp.txt), host PIN %s", pin);
          } else {
            NLOG("H: moonlight_otp.txt ignored: expected '<4-digit PIN> <passphrase>'");
            otp_pass[0] = '\0';
          }
          fclose(of);
        }
      }
      
      if (!paired) {
          char pin_log[96];
          if (use_otp) {
            snprintf(pin_log, sizeof(pin_log), "OTP pairing with host PIN %s...", pin);
          } else {
            ui_set_pairing_pin(pin);
            snprintf(pin_log, sizeof(pin_log), "PIN %s: enter it in the host's web UI > PIN (waits 10 min)", pin);
          }
          ui_push_log(pin_log);
          NLOG("H: Attempting Pairing (Check Sunshine for PIN %s)...", pin);
          if (hv_pair(&hinfo, pin, use_otp ? otp_pass : NULL) != 0) {
            memset(otp_pass, 0, sizeof(otp_pass));
            ui_set_pairing_pin("");
            ui_set_error_detail(hinfo.last_status);
            if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
            // If user cancelled, they are already at UI_STATE_IP_ENTRY.
            // Only set UI_STATE_ERROR if it wasn't a deliberate cancel.
            if (ui_get_state() == UI_STATE_PAIRING) {
                NLOG("H: Pairing process failed or timed out.");
                ui_set_state(UI_STATE_ERROR);
            }
            continue;
          }
          ui_push_log("H: Pairing succeeded!");
          if (use_otp) remove(OTP_FILE_PATH);
          memset(otp_pass, 0, sizeof(otp_pass));
          // A newly paired client has a permission mask now; read it.
          hv_is_paired(&hinfo);
          ui_set_host_paired(ui_get_selected_host_index(), 1);
          ui_save_settings();
      } else {
          NLOG("H: Already paired with server. Skipping PIN entry.");
      }

      ui_set_pairing_pin("");

      if (ui_get_state() == UI_STATE_IP_ENTRY) continue; // Safety check

      NLOG("H: Fetching server info...");
      if (hv_get_server_info(&hinfo) != 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        strncpy(hinfo.server_app_version, "7.1.431.0", sizeof(hinfo.server_app_version) - 1);
      }

      if (ui_get_state() == UI_STATE_IP_ENTRY) continue;

      NLOG("H: Fetching app list...");
      ps3_app_list_t app_list = {0};
      if (hv_get_app_list(&hinfo, &app_list) != 0 || app_list.count == 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        NLOG("H: Failed to fetch app list or no apps found.");
        ui_set_error_detail(hinfo.last_status);
        ui_set_state(UI_STATE_ERROR);
        continue;
      }

      // Transition to App Selection state
      ui_set_app_list(&app_list);
      ui_reset_app_selection();
      ui_set_state(UI_STATE_APPLIST);
      NLOG("H: Waiting for user to select an app from UI (found %d apps)...", app_list.count);

      // Wait for user confirmation or cancellation
      while (ui_is_running() && ui_get_state() == UI_STATE_APPLIST && !ui_is_app_selected()) {
        sysUtilCheckCallback();
        usleep(20000);
      }

      if (!ui_is_running() || ui_get_state() != UI_STATE_APPLIST) {
        NLOG("H: App selection cancelled or aborted.");
        continue;
      }

      int app_id = ui_get_selected_app_id();
      const char *app_name = ui_get_selected_app_name();
      const char *app_uuid = ui_get_selected_app_uuid();

      // Vibepollo/Apollo give new clients LIST|VIEW only.  Say so up front
      // rather than failing the launch with no explanation.  Not a hard stop:
      // the host is the authority, and its own message is shown if it refuses.
      {
        char missing[96];
        if (hv_missing_permissions(&hinfo, missing, sizeof(missing)) > 0) {
          char msg[160];
          snprintf(msg, sizeof(msg), "Host denies: %s. Grant in Web UI > Client Management.", missing);
          ui_push_log(msg);
          NLOG("H: %s (perm mask 0x%08x)", msg, hinfo.perm);
        }
      }
      NLOG("H: User selected App '%s' (ID: %d)", app_name, app_id);

      unsigned char rikey_bin[16];
      char rikey_hex[33];
      if (ps3_random_bytes(rikey_bin, sizeof(rikey_bin)) != 0 ||
          ps3_random_u32(&random_value) != 0) {
        NLOG("H: Failed to generate secure session keys.");
        ui_set_state(UI_STATE_ERROR);
        continue;
      }
      for (int i = 0; i < 16; i++) sprintf(rikey_hex + (i * 2), "%02X", rikey_bin[i]);
      rikey_hex[32] = '\0';
      int rikeyid = (int)(random_value % 1000000);

      NLOG("H: Launching App '%s' (ID %d)...", app_name, app_id);
      if (hv_launch(&hinfo, app_id, app_uuid, ui_get_virtual_display(),
                    rikey_hex, rikeyid) != 0) {
        if (ui_get_state() == UI_STATE_IP_ENTRY) continue;
        NLOG("H: hv_launch failed.");
        ui_set_error_detail(hinfo.last_status);
        ui_set_state(UI_STATE_ERROR);
        continue;
      }

      // If we reach here, launch was successful!
      g_active_hinfo = &hinfo;
      g_host_quit_sent = 0;
      ui_set_host_last_app(ui_get_selected_host_index(), app_id);
      ui_save_settings();

      // Setup Stream
      STREAM_CONFIGURATION streamConfig;
      LiInitializeStreamConfiguration(&streamConfig);
      streamConfig.width = ui_get_stream_width();
      streamConfig.height = ui_get_stream_height();
      streamConfig.fps = ui_get_fps(); // Dynamic FPS from UI selection
      // The rate the host actually encodes at (59.94 by default): see
      // ui_ntsc_rate.  maxFPS stays the whole number the protocol expects.
      streamConfig.clientRefreshRateX100 = ui_get_refresh_x100();
      streamConfig.bitrate = ui_get_bitrate(); // Dynamic bitrate from UI selection
      // Payload bytes per RTP packet.  Every packet costs one recv() syscall on
      // the PPU, so at high bitrates it is the packet RATE, not the byte rate,
      // that saturates the receive thread.  1392 is Moonlight's standard
      // MTU-safe size and carries 36% more payload per syscall than 1024; 1024
      // is kept because it was this port's original default.  Selectable in
      // Stream Settings so the two can be compared on real hardware.
      streamConfig.packetSize = ui_get_packet_size();
      streamConfig.streamingRemotely = STREAM_CFG_LOCAL;
      streamConfig.audioConfiguration = AUDIO_CONFIGURATION_STEREO;
      streamConfig.supportedVideoFormats = VIDEO_FORMAT_H264;
      memcpy(streamConfig.remoteInputAesKey, rikey_bin, 16);

      ps3_request_intra_refresh = ui_get_intra_refresh();

      NLOG("stream cfg: %dx%d @%d fps (x100=%d), bitrate=%d kbps, packetSize=%d, "
           "output=%s, intraRefresh=%d",
           streamConfig.width, streamConfig.height, streamConfig.fps,
           streamConfig.clientRefreshRateX100,
           streamConfig.bitrate, streamConfig.packetSize,
           ui_get_pixel_format() ? "YUV420" : "ARGB32",
           ps3_request_intra_refresh);

      connection_callbacks_init();
      
      SERVER_INFORMATION server;
      LiInitializeServerInformation(&server);
      server.address = (char*)pcIp;
      server.serverInfoAppVersion = hinfo.server_app_version;
      server.serverInfoGfeVersion = "3.23.0.74";
      server.rtspSessionUrl = hinfo.rtsp_session_url;
      server.serverCodecModeSupport = SCM_H264;

      int ret = LiStartConnection(&server, &streamConfig, &connection_callbacks,
                                  &decoder_callbacks_ps3, &audio_callbacks_ps3,
                                  NULL, 0, NULL, 0);
      
      if (ret == 0) {
        NLOG("Connection Start initiated. Waiting for stream...");
        ui_set_state(UI_STATE_STREAMING);
        
        // Wait for connection to actually start or fail
        int timeout_wait = 0;
        while (ui_is_running() && !connection_is_connected() && connection_is_ready() && timeout_wait < 100) {
          sysUtilCheckCallback();
          usleep(100000);
          timeout_wait++;
        }

        if (ui_is_running() && connection_is_connected()) {
          NLOG("Connection fully established!");
          while (ui_is_running() && connection_is_ready() && ui_get_state() == UI_STATE_STREAMING) {
            sysUtilCheckCallback();
            
            // Check for emergency exit key combination (Select + Start + L3 + R3)
            ps3_pad_state_t pad;
            ps3input_get_data(&pad);
            int exit_combo = PLAY_FLAG | BACK_FLAG | LS_CLK_FLAG | RS_CLK_FLAG;
            if ((pad.buttons_down & exit_combo) == exit_combo) {
              NLOG("Emergency exit combo pressed! Returning to menu...");
              ui_set_state(UI_STATE_IP_ENTRY);
            }
            
            vdec_poll();
            usleep(1000); // 1000Hz poll: with DIRECT_SUBMIT, vdec_poll drains decoder output for RSX render
          }
        } else {
          NLOG("Connection failed to establish timely.");
          ui_set_state(UI_STATE_ERROR);
        }
        
        // Did the USER end it (exit combo / quit to XMB), or did the
        // connection drop?  Only the former quits the app on the host.
        int user_left = !ui_is_running() || ui_get_state() != UI_STATE_STREAMING;

        NLOG("Returning to Main Menu.");
        LiStopConnection();
        ps3input_rumble_stop();
        if (user_left && ui_get_quit_on_exit() && !g_host_quit_sent) {
          g_host_quit_sent = 1;
          hv_quit_app(&hinfo);
        } else if (!user_left) {
          NLOG("Stream ended by the connection; leaving the app running on the host for resume.");
        }
        g_active_hinfo = NULL;
        ui_set_state(UI_STATE_IP_ENTRY);
      } else {
        NLOG("LiStartConnection failed: %d", ret);
        g_active_hinfo = NULL;
        ui_set_state(UI_STATE_ERROR);
      }
    }
    
    menu_probe_tick();
    sysUtilCheckCallback();
    usleep(50000);
  }

  NLOG("Terminating Moonlight PS3 application...");
  LiStopConnection();
  ps3audio_stop();
  ps3video_stop();
  ps3input_stop();
  ui_shutdown();
  net_logger_shutdown();
  sysUtilUnregisterCallback(0);
  usleep(100000); // 100ms graceful drain for final network sockets
  netCtlTerm();
  net_free_pool();
  sysModuleUnload(SYSMODULE_NET);
  return 0;
}
