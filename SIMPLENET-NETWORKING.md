# SimpleNet networking architecture

SimpleNet has two separate backends. The normal Linux build includes both;
`SIMPLENET_WITH_NM=0` builds only standalone wpa_supplicant support.

The NetworkManager implementation was compared with the installed
NetworkManager/nmtui version, **1.52.1**:

- [nmtui activation and authentication lifecycle](https://github.com/NetworkManager/NetworkManager/blob/1.52.1/src/nmtui/nmtui-connect.c)
- [nmtui device and profile compatibility checks](https://github.com/NetworkManager/NetworkManager/blob/1.52.1/src/nmtui/nmt-connect-connection-list.c)
- [nmtui's internal secret-agent helper](https://github.com/NetworkManager/NetworkManager/blob/1.52.1/src/libnmc-base/nm-secret-agent-simple.c)

## NetworkManager backend

| Area | Implementation |
| --- | --- |
| Discovery | `NMClient`, `NMDeviceWifi`, and `NMAccessPoint` objects maintained by libnm's D-Bus event processing. |
| Saved profiles | Iterate `nm_client_get_connections()` and apply both `nm_device_connection_valid()` and `nm_access_point_connection_valid()`, as nmtui does. Profile names and rendered SSIDs are not identifiers. |
| Saved activation | `nm_client_activate_connection_async()` receives the original profile, device, and selected AP path. No replacement profile is created after a failure. |
| First connection | `nm_client_add_and_activate_connection_async()` receives a NULL template, device, and AP path, as in nmtui. NetworkManager completes and persists the settings. |
| Authentication | A short-lived `NMSecretAgentOld` subclass is registered for activation. It queues requests until the selected connection path is known, answers only that connection, supports interactive retries and cancellation, and is destroyed afterward. |
| Credential fields | PSK/SAE, indexed WEP keys, LEAP, and configured 802.1X credentials/hints follow the Wi-Fi field selection in nmtui's internal helper. The public libnm agent base is shared; the small curses prompt implementation belongs to SimpleNet. |
| Completion | Wait for active-connection and device state, including the device reason after a generic disconnect. Accepting the D-Bus request does not count as successful connection. |
| Cancellation | Esc dismisses the activation wait, as in nmtui; it does not claim to undo a request the daemon already accepted. Cancelling a secret prompt answers the pending request with cancellation. |
| Persistence and policy | NetworkManager owns profiles, stored secrets, DHCP, routes, security negotiation, autoconnection, and roaming. The UI never writes keyfiles or supplicant configuration in this backend. |
| Live state | Drain libnm events and rebuild the cached display each UI iteration. Preserve selection by AP identity; show the actual current AP after roaming. Retain the selected adapter name across removal/reappearance. |
| Scan failure | Keep cached networks when a scan is refused. Scan requests are asynchronous and separate from connection activation. |
| Backend ownership | A running or temporarily unreadable NetworkManager remains the selected backend. The combined build refuses standalone operations on an NM-managed adapter and rechecks ownership during use. |

The UI remains a Wi-Fi picker: it shows individual BSSIDs and one selected
adapter, whereas nmtui groups access points and exposes other connection types
and a full editor. `nmtui edit` remains the tool for enterprise enrollment,
hidden-network setup, full profile editing, and VPN configuration. These UI
differences do not introduce a second NetworkManager connection engine.

## Standalone backend

`simplenet -b wpa` uses the supplicant control protocol without NetworkManager.
The standalone-only build links ncurses and libc, not libnm or GLib. It requires
explicit `-b wpa` because that build cannot verify NetworkManager ownership.

It preserves SSID bytes, reuses compatible saved network IDs, and verifies the
activated ID rather than treating any connection to the same SSID as success.
Failed new configurations remove only the ID created by that attempt. Existing
same-SSID profiles are preserved. Previously enabled networks are restored after
`SELECT_NETWORK`; configuration is not saved if that restoration fails.

Truncated or timed-out control replies close the socket so that a late reply
cannot be interpreted as the answer to a subsequent mutation. `SAVE_CONFIG`
failure is reported as session-only persistence. Address assignment and routes
remain the responsibility of the standalone system's network service.

## Regressions covered

`make test-simplenet` includes control-socket fixtures and, with NetworkManager
enabled, a private D-Bus service driving the real libnm/curses executable through
a PTY. It covers:

- renamed saved profiles, non-UTF-8 SSIDs, incompatible band/security profiles,
  and saved connections absent from the device's available-connections list;
- missing credentials, a rejected password followed by a retry, secret-request
  cancellation, and authentication-agent cleanup;
- Esc during the initial activation request, without opening a password prompt;
- NULL new-profile templates and activation lasting beyond 35 seconds;
- refused scans retaining usable cached networks;
- selection surviving a stronger same-SSID AP appearing;
- connection status changing without a scan;
- radio disable/enable and adapter removal/reappearance;
- refusal to use the standalone backend on an NM-managed adapter or after
  losing the D-Bus connection used to check ownership;
- standalone saved-profile reuse, rollback, enabled-profile preservation,
  raw SSID encoding, exact active-network identity, and transport failures;
- secret-field selection for SAE, WEP, PEAP, and 802.1X hints.

These fixtures verify control flow and object handling. They do not simulate RF
conditions, an access point's authentication implementation, or a real DHCP
server. A live reconnection is a separate check because it changes the host's
active network connection.
