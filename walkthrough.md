# urtwx walkthrough

## Scope

`urtwx` is an OpenBSD USB `net80211` driver for the Realtek RTL8821CU
family.  The current implementation is intentionally focused on the 1x1
8821C USB path rather than a generic RTL88xx framework.

This driver now supports:

- station mode
- open, WPA2/RSN, and PMF/MFP
- 2.4 GHz and non-DFS 5 GHz
- 11a/11b/11g
- HT20/HT40
- VHT80
- on-air RX BA aggregation
- partial on-air TX aggregation

Notable limits today:

- no AP/hostap mode
- no monitor-mode transmit path
- no DFS channels
- no support yet for other RTL88xx USB parts
- TX aggregation is only partial
- USB recovery is good in normal churn/unplug tests, but watchdog/stall
  recovery is not fully characterized yet

## Chip model

RTL8821CU is a USB-attached 1x1 802.11ac part.  On Linux it lives in the
`rtw88` family, and this driver follows that model more closely than the
older `urtwn`/`88xxa` family.

At a high level the chip is split into:

- a USB transport front end
- an embedded WLAN CPU that runs downloaded firmware
- MAC state, DMA queue mapping, and H2C/C2H mailboxes
- BB/RF/channel-width logic
- a security engine with CAM entries for hardware keys

For OpenBSD, the practical consequence is that `urtwx` is not just “USB
Ethernet with radio registers.”  It has a real firmware lifecycle, an async
state machine, and a hardware security path that must stay consistent with
`net80211`.

## Files of interest

- Main driver: [if_urtwx.c](./sys/dev/usb/if_urtwx.c:1)
- Register definitions: [if_urtwxreg.h](./sys/dev/usb/if_urtwxreg.h:1)
- Remaining work: [todo](./todo:1)

## Attach and bring-up

Attach is intentionally lightweight.  [urtwx_attach()](./sys/dev/usb/if_urtwx.c:5157):

- discovers the bulk RX pipe, bulk TX pipes, and interrupt endpoint
- opens the USB pipes
- allocates RX transfers and one shared TX transfer/buffer
- advertises capabilities and channels to `net80211`
- installs `urtwx` hooks such as `ic_newstate`, key callbacks, and AMPDU RX
  callbacks

Attach does not load firmware.  That work was moved to
[urtwx_init()](./sys/dev/usb/if_urtwx.c:4403) so
autoconf no longer depends on `/etc/firmware`.

The first `ifconfig up` does:

1. [urtwx_fw_download()](./sys/dev/usb/if_urtwx.c:2152)
2. efuse/MAC read via [urtwx_read_efuse()](./sys/dev/usb/if_urtwx.c:2353)
3. [urtwx_power_on()](./sys/dev/usb/if_urtwx.c:1387)
4. runtime MAC/PHY/RX bring-up via [urtwx_mac_init_runtime()](./sys/dev/usb/if_urtwx.c:5109)
5. RX submission via [urtwx_rx_init()](./sys/dev/usb/if_urtwx.c:4357)
6. `ieee80211_begin_scan()`

Later `down/up` cycles reuse the loaded firmware and redo the runtime
bring-up only.

## USB transport model

The driver uses:

- one bulk RX pipe
- multiple bulk OUT pipes, chosen by hardware queue selector
- one shared TX transfer for normal asynchronous traffic
- one RX transfer list, currently `URTWX_RX_LIST_COUNT = 16`

Queue-to-pipe routing is done in
[urtwx_qsel_to_pipe()](./sys/dev/usb/if_urtwx.c:2441),
after queue mapping is programmed by
[urtwx_txdma_queue_mapping()](./sys/dev/usb/if_urtwx.c:4720).

USB-side RX aggregation is enabled in
[urtwx_usb_rx_agg_cfg()](./sys/dev/usb/if_urtwx.c:5089).
This is only a transport packing feature.  It is distinct from over-the-air
AMPDU aggregation.

## Firmware, H2C, and reserved pages

Firmware is loaded from `rtw8821c_fw.bin`.  The download flow follows the
usual Realtek pattern:

- power on the WLAN core
- reset the download platform state
- push IMEM/DMEM/EMEM sections
- validate firmware startup

Once runtime MAC is alive, `urtwx` initializes H2C in
[urtwx_init_h2c()](./sys/dev/usb/if_urtwx.c:4838)
and uses mailbox-style commands for:

- default-port selection
- media-status connect/disconnect state
- RA info
- reserved-page metadata

Reserved pages are important on this chip.  Before entering `RUN`,
[urtwx_fw_download_sta_rsvd_page()](./sys/dev/usb/if_urtwx.c:1271)
builds and downloads a small station reserved-page set containing:

- dummy page
- PS-Poll
- null frame
- QoS null frame when QoS is negotiated

This is one of the places where `urtwx` looks much more like a firmware
driver than a purely host-driven MAC.

## net80211 integration

The core integration points are:

- `if_start` -> [urtwx_start()](./sys/dev/usb/if_urtwx.c:4568)
- RX handoff -> [urtwx_rx_frame()](./sys/dev/usb/if_urtwx.c:4070)
- state transitions -> [urtwx_newstate()](./sys/dev/usb/if_urtwx.c:3365) and [urtwx_newstate_cb()](./sys/dev/usb/if_urtwx.c:3425)
- key management -> [urtwx_set_key()](./sys/dev/usb/if_urtwx.c:3556) and [urtwx_delete_key()](./sys/dev/usb/if_urtwx.c:3598)
- RX BA negotiation -> [urtwx_ampdu_rx_start()](./sys/dev/usb/if_urtwx.c:3390)

### State model

`net80211` drives scan/auth/assoc/run, but `urtwx` cannot do the hardware
work inline inside `ic_newstate`.  The driver therefore turns each state
change into an async command processed by `urtwx_newstate_cb()`.

That callback is responsible for:

- stopping scan dwell timers on non-scan states
- programming the chosen channel before AUTH/ASSOC/RUN
- programming the station hardware state on entry to `RUN`
- tearing down linked-state hardware on leave from `RUN`
- avoiding stale or duplicate queued state transitions

This async layer was one of the main sources of early bring-up bugs.  The
current implementation deliberately rejects stale callbacks and avoids
restarting the device underneath in-flight state transitions.

### TX path

[urtwx_start()](./sys/dev/usb/if_urtwx.c:4568)
services two queues:

- `ic_mgtq` for management traffic
- `if_snd` for ordinary data traffic in `RUN`

Management frames use a fixed basic-rate template.  Data frames go through
`ieee80211_encap()` and then
[urtwx_tx()](./sys/dev/usb/if_urtwx.c:1613),
which builds the Realtek USB TX descriptor, chooses the queue selector, and
submits the bulk transfer.

Ordinary data TX does support firmware-driven rate adaptation and partial
AMPDU signaling, but the host-side TX model is still simple and is one of
the reasons item 12 remains only partial.

### RX path

Bulk RX completions land in
[urtwx_rxeof()](./sys/dev/usb/if_urtwx.c:4257),
which parses the Realtek USB RX descriptor stream, handles C2H packets, and
passes data frames into [urtwx_rx_frame()](./sys/dev/usb/if_urtwx.c:4070).

`urtwx_rx_frame()` is where the driver:

- builds `ieee80211_rxinfo`
- attaches RSSI and rate information
- marks hardware-decrypted frames
- hands the frame to `net80211`

This path also contains the PHY-status decoding that fixed the original `0%`
RSSI problem.

## Channel, width, and rate handling

Channel setup is split into:

- parameter selection in [urtwx_get_chan_params()](./sys/dev/usb/if_urtwx.c:2798)
- MAC programming in [urtwx_phy_set_channel_mac()](./sys/dev/usb/if_urtwx.c:2976)
- BB programming in [urtwx_phy_set_channel_bb()](./sys/dev/usb/if_urtwx.c:2857)
- RF programming in [urtwx_phy_set_channel_rf()](./sys/dev/usb/if_urtwx.c:3028)
- top-level selection in [urtwx_set_chan()](./sys/dev/usb/if_urtwx.c:3146)

The current driver supports:

- 2.4 GHz HT20/HT40
- non-DFS 5 GHz HT20/HT40
- non-DFS 5 GHz VHT80

Runtime rate adaptation is mostly firmware-driven.  Firmware C2H RA reports
are decoded and reflected back into `net80211`, so `ifconfig` can now show
meaningful transitions between legacy, HT, and VHT rates.

## Security model

The current security split is:

- CCMP is offloaded through the RTL8821C security engine and CAM
- non-CCMP or software-forced crypto falls back to `net80211`
- PMF/MFP works through the normal `net80211` protected-management path

The key callbacks are asynchronous because CAM programming touches device
state rather than pure host memory.

Important detail: PMF support did not require a new hardware offload model.
The driver advertises `IEEE80211_C_MFP`, while BIP/IGTK stays on the
existing software path where appropriate.

## AMPDU / aggregation

### RX aggregation

RX BA is working and stable enough to keep enabled.  The major quirk is in
[urtwx_ampdu_rx_start()](./sys/dev/usb/if_urtwx.c:3390):

- `urtwx` accepts RX BA
- but clears `A-MSDU in A-MPDU`

Why:

- allowing A-MSDU inside RX BA caused severe reverse-direction throughput
  collapse and reorder-buffer instability on this USB design
- the working theory is that firmware/hardware splits A-MSDU subframes
  before `net80211` sees them, so multiple delivered MSDUs share a sequence
  number and collide in the software reorder path

With `URTWX_RX_LIST_COUNT = 16` and `A-MSDU in A-MPDU` disabled, reverse
multi-stream throughput is stable in roughly the `50 MB/s` range.

### TX aggregation

TX aggregation is only partial today.

What exists:

- TX BA negotiation
- descriptor aggregate hints in [urtwx_tx()](./sys/dev/usb/if_urtwx.c:1613)

What does not appear to be the main limit:

- USB-side TX batching
- deeper host-side async TX rings

Those experiments engaged correctly but did not materially improve
throughput, so further TX aggregation work probably needs to focus on
firmware/on-air behavior rather than more host-side USB packing.

## PMF/MFP

PMF is working for both optional and required AP policy.

From a behavior perspective:

- association succeeds with PMF optional
- association succeeds with PMF required
- unprotected deauthentication no longer disconnects the station

## Quirks and idiosyncrasies

- Firmware load is intentionally delayed until `ifconfig up`.
- The driver is very stateful.  Many early bugs were caused by scan/auth/
  assoc callbacks arriving after the hardware had already moved on.
- Reserved pages matter.  Without them, association and later runtime
  behavior are unreliable.
- The RTL8821C security engine is useful, but only if the driver and
  `net80211` agree on which frames are hardware-decrypted and which keys are
  offloaded.
- RX BA is stable only with `A-MSDU in A-MPDU` disabled.
- Throughput is good enough to use, but peak TX-side aggregation behavior
  is not fully characterized yet.

## Known issues and deferred work

- TX aggregation is still partial.  See item 12 in [todo](./todo:1).
- USB recovery is mostly good in practical testing, but watchdog-triggered
  timeout recovery and endpoint stall recovery still need deliberate testing.
- DFS channels are not supported yet.
- The code is still 8821C-specific in many places:
  - MAC/BB/RF init tables
  - PHY-status parsing
  - 1SS rate assumptions
- Other RTL88xx USB parts should be treated as follow-on chip backends, not
  just extra USB IDs.

## Practical reading order

If you are trying to understand the driver from the source, read it in this
order:

1. [urtwx_attach()](./sys/dev/usb/if_urtwx.c:5157)
2. [urtwx_init()](./sys/dev/usb/if_urtwx.c:4403)
3. [urtwx_newstate()](./sys/dev/usb/if_urtwx.c:3365) and [urtwx_newstate_cb()](./sys/dev/usb/if_urtwx.c:3425)
4. [urtwx_start()](./sys/dev/usb/if_urtwx.c:4568) and [urtwx_tx()](./sys/dev/usb/if_urtwx.c:1613)
5. [urtwx_rxeof()](./sys/dev/usb/if_urtwx.c:4257) and [urtwx_rx_frame()](./sys/dev/usb/if_urtwx.c:4070)
6. [urtwx_set_chan()](./sys/dev/usb/if_urtwx.c:3146)
7. [urtwx_set_key()](./sys/dev/usb/if_urtwx.c:3556) / [urtwx_delete_key()](./sys/dev/usb/if_urtwx.c:3598)

That covers most of the real design without getting lost in table imports and
register details too early.
