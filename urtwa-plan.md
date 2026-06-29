# urtwa RTL8821AU Plan

## Scope

`urtwa` is a fresh OpenBSD USB driver effort for Realtek RTL8821AU/RTL8811AU.
The goal is to avoid inheriting `urtwx` RTL8821CU lifecycle, scan, TX, and
state-machine assumptions.

Structural USB attach patterns may borrow from existing OpenBSD drivers such as
`urtwn` or `rsu`, but chip behavior must be taken from Linux `rtw88` only:

- `rtw8821au.c`
- `rtw8821a.c`
- `rtw88xxa.c`
- `usb.c`
- `fw.c`
- `mac.c`
- `phy.c`
- `rx.c`
- `tx.c`

## Guiding rules

- Do not copy register programming sequences from `urtwx`, `urtwn`, or `rsu`.
- Do not graft the `urtwx` async state machine or forced-scan behavior into
  `urtwa`.
- Keep the first milestones narrow and testable.
- Prefer a direct mapping from Linux subsystem to OpenBSD subsystem.

## Initial device scope

Start with the TP-Link RTL8821AU test devices already on hand:

- `2357:011e`
- `2357:011f`
- `2357:0120`

Linux maps all of these to `rtw8821a_hw_spec`.

Firmware target:

- `rtw8821a_fw.bin`

## File layout

Initial clean driver files:

- `sys/dev/usb/if_urtwa.c`

Expected follow-on files as the driver grows:

- `sys/dev/usb/if_urtwareg.h`
- `sys/dev/usb/if_urtwatbl.h`
- optional split-out power/table files if that keeps `if_urtwa.c` readable

## Milestones

### 1. Attach

Goal:

- match the RTL8821AU USB IDs
- attach cleanly as a new driver
- discover interface/endpoints
- establish a driver-private `softc`

Linux reference points:

- `rtw8821au.c`
- `rtw_usb_probe()` in `usb.c`

OpenBSD constraints:

- okay to borrow USB attach skeleton structure from `urtwn`/`rsu`
- do not borrow chip init, scan, or TX sequencing

Status:

- done
- endpoint discovery/open is working on the TP-Link test dongles

### 2. Firmware load

Goal:

- power-on enough of the device to load firmware
- implement the legacy 8051 firmware download path used by RTL8821A
- confirm firmware is running
- read efuse/ROM identity data

Linux reference points:

- `rtw88xxa_power_on()`
- `rtw88xxau_init_power_on()`
- `rtw_download_firmware()`
- `rtw88xxa_read_efuse()`

Status:

- done
- exact Linux 8821A USB power tables are in place
- legacy 8051 firmware download is running
- efuse dump/logical map unpack is running
- real MAC address is printed at attach

### 3. Passive scan / RX

Goal:

- fixed-channel receive
- stable beacon/probe-response RX
- correct RX descriptor parsing and PHY-status extraction
- enough channel control to passively hear nearby APs

Linux reference points:

- `rtw88xxa_set_channel()`
- `rtw_rx_query_rx_desc()`
- `rtw8821a_phy_calibration()`
- `rtw8821a` MAC/AGC/BB/RF/power-track tables

Status:

- in progress
- `urtwa0` now attaches as a real `ifnet`/`net80211` interface
- Linux-shaped queue/runtime MAC init is in place on `ifconfig up`
- Linux `8821A` MAC/AGC/BB/RF-A tables are now imported locally
- old-`88xxa` HSSI/SIPI RF read/write helpers are in place
- first 2.4 GHz fixed-channel PHY/channel bring-up path is in place
- RX bulk-transfer submission is now wired in
- first-pass Realtek RX descriptor splitting/parsing is now wired in
- first-pass management-frame logging is now wired in
- real RX completions and nonstop beacon reception are now confirmed
- passive RX works well enough to move on to management TX / active scan
- passive background channel rotation is now working; `ifconfig urtwa0 up`
  followed by `ifconfig urtwa0 scan` can populate results from multiple
  channels without an explicit join target
- RSSI/PHY-status decode is no longer stuck at `0%`, but it still runs hot on
  some distant APs and needs later tuning
- fixed-channel tuning is now good enough that `chan 6` / `chan 11` runs are
  dominated by beacons from the selected channel
- next step is the first management-TX path for active scan / probe requests

### 4. Active scan / TX

Goal:

- active scan probe-request TX on-air
- probe responses back to the driver
- management TX descriptor path validated end-to-end

Linux reference points:

- `rtw_tx_pkt_info_update()`
- `rtw_tx_fill_tx_desc()`
- `fill_txdesc_checksum`
- USB write-port handling in `usb.c`

Immediate implementation target:

- consume net80211 management traffic from `ic_mgtq`
- build Linux-shaped `8821A` management TX descriptors
- submit management TX on the correct USB bulk-out pipe
- log first probe-request TX attempts before worrying about auth/assoc

Status:

- done
- management TX is proven on-air through directed join:
  - probe requests are transmitted from `b0:19:21:53:e3:0a`
  - probe responses are received back from the AP
- active rotating probe scan is now working:
  - hardware retunes before per-channel probe emission
  - `send_mgmt probe request` / `TX probe request` are visible in `dmesg`
  - on-air captures show probe requests on the tuned channel and probe
    responses back from nearby APs
- `ifconfig urtwa0 scan` still dumps the node cache, but after `ifconfig up`
  the background net80211 scan now populates results from multiple channels

### 5. Association

Goal:

- open authentication
- association request/response
- then WPA later

Linux reference points:

- station lifecycle around `NO_LINK` -> `AUTH` -> `ASSOC` -> `RUN`
- firmware/media-status programming
- candidate BSSID / port programming

Status:

- open association is now working
- `ifconfig urtwa0` reaches `active` against the open AP `booya`
- repeated `ifconfig down/up` no longer panic
- probe request / response, auth, and assoc are all proven on-air
- station bring-up is now entering `RUN` cleanly enough to move on to data

### 6. Data path

Goal:

- transmit normal data frames from `if_snd`
- receive normal data frames well enough for open-network DHCP
- confirm first IP traffic before attempting WPA or broader cleanup

Immediate implementation target:

- keep the working management path unchanged
- add a minimal normal-data TX path in `if_start()`
- use `ieee80211_encap()` and Linux-shaped `8821A` txdesc build
- validate DHCP discover/request on-air and in `dmesg`

Status:

- normal `if_snd` TX is now wired in
- outbound data TX is proven on-air
- inbound unicast RX is proven
- open-network DHCP now works end-to-end
- real IP traffic is confirmed, including ping and later unicast traffic
- the current remaining work is polish, not basic data-path bring-up

### 7. WPA2-PSK / RSN

Goal:

- complete the 4-way handshake
- install CCMP keys through net80211
- carry encrypted DHCP and normal IP traffic

Status:

- WPA2-PSK now works on 2.4 GHz against `liberator`
- the full 4-way handshake is proven on-air
- `set_key cipher=ccmp` succeeds for pairwise and group keys
- post-handshake frames are now actually protected on-air
- DHCP over WPA now succeeds
- real encrypted IP traffic is confirmed
- the remaining WPA work is cleanup/polish, not first-pass functionality

## Non-goals for the first pass

- WPA before open association works
- broad device-ID coverage before the test dongles work
- merging with `urtwx` before `urtwa` is functionally proven

## Immediate next step

Clean up and broaden the now-working STA path:

- remove the remaining auth-time register-write path that can still trigger
  `not in process context` warnings
- broaden WPA/Open validation across more APs and bands
- decide whether QoS/WME TX shape or hardware CAM programming needs more work
  for interoperability, not first bring-up

## Deferred Issues

These are known and intentionally deferred so they do not distract from the
RX/TX bring-up path:

- RSSI is not computed correctly yet.
  - Early runs left reported RSSI at `0%`; that part is fixed.
  - The current `8821A` CCK fallback path is good enough to expose useful
    percentages, but some distant APs still pin at `100%`.
  - This likely needs more tuning of the CCK/Jaguar PHY-status heuristic
    rather than a larger lifecycle change.
  - This is no longer a functional blocker, but it should be cleaned up before
    final upstream-quality submission.
- QoS/WME transmit shape is not yet fully Linux-like.
  - Open association, DHCP, and real IP traffic already work.
  - WPA2-PSK also works, but the driver still needs broader
    interoperability validation before deciding whether QoS data or hardware
    key/CAM programming needs more work before the driver is considered
    polished.

- The auth-time pre-link register-write experiment is not needed for the
  working WPA/open path.
  - Earlier versions used it to try to prime MAC/BSSID/net-type state before
    auth.
  - In practice it only introduced `usbd_do_request: not in process context`
    and `ctl reg 0x0610 failed: INVAL` warnings.
  - The working path now relies on the later link-state task/H2C handling
    instead.

- Firmware load during early autoconf is not robust.
  - Resolved for the current bring-up path.
  - `urtwa` now keeps power-on/efuse attach working even if
    `/etc/firmware` is not yet available, defers firmware download until the
    first interface bring-up, and has now passed a real boot-time validation
    run.

- Unplug/detach teardown regression appears resolved.
  - Recent unplug/replug validation no longer shows the old
    `usbd_free_xfer: xfer ... not free` failure.
  - Keep an eye on detach ordering as the driver grows, but this is no longer
    an active blocker.
