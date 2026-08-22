# Show PC deployment: public access + autostart

This covers the parts of the setup that live outside the app itself: making
the remote controller reachable from a phone/iPad without needing to be on
the same network as the show PC, and getting everything running
automatically when the show PC boots.

## Why a public tunnel, not just the LAN

Convention center networks are frequently locked down in ways that break
plain LAN access between devices on the same Wi-Fi (client isolation is
common, specifically to stop one attendee's device from reaching another's)
-- and bringing your own router/AP to create a private LAN may not be
usable or permitted on the show floor. A Cloudflare Tunnel sidesteps this
entirely: the show PC makes an *outbound* connection to Cloudflare (almost
always allowed, even on locked-down guest networks), and the control device
just opens a normal HTTPS URL over its own internet connection (venue
Wi-Fi or cellular) -- neither device needs to reach the other directly, so
network topology at the venue stops mattering.

The tradeoff: this depends on the venue actually having usable outbound
internet. The travel router is the fallback if that turns out not to be
true on-site.

## Current setup

- **Public URL:** `https://gsxnetworkpuzzle.com` (domain registered on
  Namecheap, DNS hosted on Cloudflare, free plan)
- **Tunnel name:** `gsx-remote-controller`
- **Tunnel ID:** `39103a87-015e-4755-9714-8158f1eb55c2`
- **cloudflared config (used by the service):** `C:\ProgramData\Cloudflare\cloudflared\config.yml`
  (points the `gsxnetworkpuzzle.com` hostname at `http://127.0.0.1:8080`,
  i.e. the remote-controller server -- `127.0.0.1` specifically, not
  `localhost`; see the gotcha below)
- **Credentials file (service copy):** `C:\ProgramData\Cloudflare\cloudflared\39103a87-015e-4755-9714-8158f1eb55c2.json`
  (keep this private -- it's what authorizes the tunnel; if it ever leaks,
  delete the tunnel via the Cloudflare dashboard to revoke it)
- A second copy of both files lives under `C:\Users\Spaceship\.cloudflared\`
  from initial setup/manual testing -- harmless to leave, just not what the
  service actually reads.
- **cloudflared install:** `C:\Program Files (x86)\cloudflared\cloudflared.exe`
  (installed via `winget install --id Cloudflare.cloudflared -e`)
- Only the bare domain is routed (no `www`) -- that was a deliberate choice,
  not an oversight.

Runs as a **Windows service** (auto-starts at boot, and the Service Control
Manager can restart it automatically if it crashes -- more resilient than
launching it from a startup script, which has no self-healing if the
process dies mid-show).

### The two non-obvious gotchas that cost real time getting this working

**1. `cloudflared service install` does not support `--config`, and its
default config auto-discovery does not reliably find a config placed in
either the interactive user's `~/.cloudflared/` or (despite some
documentation implying otherwise) `C:\ProgramData\Cloudflare\cloudflared\`
when running under the `LocalSystem` account it installs as.** Passing
`--config` before `service install` is silently ignored -- its own
`--help` only documents an optional `[TOKEN]` argument (for
Cloudflare-dashboard-managed tunnels, not our locally-created named
tunnel). The service would start, show `RUNNING`, and never actually
connect to Cloudflare's edge (0 network connections, requests to the
public URL returned a `530` -- "no active connector") with no error
surfaced anywhere obvious.

**The fix:** point the service's actual launch command at the config file
directly, by editing its registry `ImagePath` (this is more reliable than
`sc.exe config Cloudflared binPath= "..."`, which is the "normal" way to
do this -- that command kept silently failing to apply, almost certainly
PowerShell mangling the nested quotes on their way to the native `sc.exe`
argument parser):

```powershell
# Run as Administrator
Set-ItemProperty -Path "HKLM:\SYSTEM\CurrentControlSet\Services\Cloudflared" -Name "ImagePath" `
  -Value '"C:\Program Files (x86)\cloudflared\cloudflared.exe" --config "C:\ProgramData\Cloudflare\cloudflared\config.yml" tunnel run'
Get-Process cloudflared | Stop-Process -Force
Start-Sleep -Seconds 2
Start-Service -Name Cloudflared
sc.exe qc Cloudflared   # BINARY_PATH_NAME should now show the full command with --config
```

**2. Use `127.0.0.1`, not `localhost`, in `config.yml`'s `service:` line.**
`localhost` can resolve differently (IPv4 vs IPv6 loopback) under the
`LocalSystem` account's network context than under an interactive user
session -- worked fine testing manually as a normal user, returned `502
Bad Gateway` (tunnel connected fine, couldn't reach the origin) once
running as the actual service. Switching to the literal `127.0.0.1`
resolved it.

### Recreating this from scratch (e.g. new PC, or the credentials leaked)

1. `cloudflared tunnel login` -- opens a browser, authorize against the
   Cloudflare account, select the zone (`gsxnetworkpuzzle.com`).
2. `cloudflared tunnel create <name>` -- writes a new credentials JSON to
   `~/.cloudflared/`.
3. `cloudflared tunnel route dns <name> gsxnetworkpuzzle.com` -- if this
   fails with "record already exists," delete the conflicting DNS record
   for the bare domain in the Cloudflare dashboard first (a parking-page
   record commonly gets imported automatically when a domain is first
   added to Cloudflare).
4. Create `C:\ProgramData\Cloudflare\cloudflared\` and place `config.yml`
   there (see the current one for the exact format -- `tunnel:`/
   `credentials-file:` pointing at the new tunnel's ID/JSON, `ingress`
   rule mapping `gsxnetworkpuzzle.com` → `http://127.0.0.1:8080` --
   **`127.0.0.1`, not `localhost`**), plus a copy of the credentials JSON
   in that same folder.
5. `cloudflared service install` (as Administrator) -- installs the
   service shell; don't expect it to actually work yet.
6. Apply the registry `ImagePath` fix from gotcha #1 above, then restart
   the service and confirm with `sc.exe qc Cloudflared` before trusting it.
7. Verify end-to-end: `Invoke-WebRequest https://gsxnetworkpuzzle.com`
   should return `200`, not `530` (dead tunnel) or `502` (tunnel up,
   origin unreachable).

## Autostart: `START_CISCO_NETWORK_PUZZLE.bat`

[`../START_CISCO_NETWORK_PUZZLE.bat`](../START_CISCO_NETWORK_PUZZLE.bat)
(project root; this is what's wired into Task Scheduler) launches, in order:
1. [`../run_server_watchdog.bat`](../run_server_watchdog.bat), in its own window.
2. A 3-second wait for the server to bind its ports.
3. [`../run_td_watchdog.bat`](../run_td_watchdog.bat), in its own window.

It does **not** launch the Cloudflare tunnel -- that's the Windows service
above, which starts independently at boot. Only add a tunnel line back into
this script if the service is ever uninstalled; running both at once means
two redundant tunnel processes.

### Why watchdog wrappers instead of launching node/TD directly

Neither the Node server nor TouchDesigner self-heals if it crashes mid-show
-- unlike the Cloudflare tunnel, which has Windows Service Control Manager
recovery. `run_server_watchdog.bat` and `run_td_watchdog.bat` each loop
`start /wait` around their respective process: if it ever exits for any
reason, the watchdog waits 5 seconds and relaunches it automatically,
logging each attempt with a timestamp so a crash loop is visible rather
than silent.

**This means closing the app itself doesn't stop it** -- the watchdog just
brings it back. To actually shut something down (e.g. end-of-night
teardown), close the **watchdog's own console window** first (titled "GSX
Remote Controller (watchdog)" / "TouchDesigner (watchdog)"), *then* close
the app. Closing them in the other order just makes the app pop back open.

### Wiring it to Task Scheduler

Create a task pointing at `START_CISCO_NETWORK_PUZZLE.bat` with trigger
**"At log on"** -- not the generic "At startup" trigger. A plain
system-startup trigger runs in Session 0, which has no visible desktop, so
none of these windows would actually appear. Also check **"Run only when
user is logged on"** in the task's General tab, for the same reason.

**Confirmed working** on a real reboot: the Task Scheduler task fired, the
server bound port 8080, TD launched with the project loaded, and TD's
heartbeat reached the server (remote page showed Connected/Idle) -- all
without any manual intervention.
