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
- **cloudflared config:** `C:\Users\Spaceship\.cloudflared\config.yml`
  (points the `gsxnetworkpuzzle.com` hostname at `http://localhost:8080`,
  i.e. the remote-controller server)
- **Credentials file:** `C:\Users\Spaceship\.cloudflared\39103a87-015e-4755-9714-8158f1eb55c2.json`
  (keep this private -- it's what authorizes the tunnel; if it ever leaks,
  delete the tunnel via the Cloudflare dashboard to revoke it)
- **cloudflared install:** `C:\Program Files (x86)\cloudflared\cloudflared.exe`
  (installed via `winget install --id Cloudflare.cloudflared -e`)
- Only the bare domain is routed (no `www`) -- that was a deliberate choice,
  not an oversight.

Runs as a **Windows service** (auto-starts at boot, and the Service Control
Manager can restart it automatically if it crashes -- more resilient than
launching it from a startup script, which has no self-healing if the
process dies mid-show). Installed with:

```powershell
# Run as Administrator
& "C:\Program Files (x86)\cloudflared\cloudflared.exe" service install
```

To check it's running: `services.msc` → look for the cloudflared service,
confirm Startup Type is **Automatic**.

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
4. Write `config.yml` (see the current one for the exact format) pointing
   `tunnel:`/`credentials-file:` at the new tunnel's ID/JSON, with an
   `ingress` rule mapping `gsxnetworkpuzzle.com` → `http://localhost:8080`.
5. `cloudflared service install` (as Administrator).

## Autostart: `start_show.bat`

[`../start_show.bat`](../start_show.bat) (project root) launches, in order:
1. The remote-controller Node server (`node server.js`), in its own window.
2. A 3-second wait for it to bind its ports.
3. TouchDesigner, with `GSXFY27_NetworkPuzzle.toe` already loaded.

It does **not** launch the Cloudflare tunnel -- that's the Windows service
above, which starts independently at boot. Only re-add a tunnel line to
this script if the service is ever uninstalled; running both at once means
two redundant tunnel processes.

### Wiring it to Task Scheduler

Create a task pointing at `start_show.bat` with trigger **"At log on"** --
not the generic "At startup" trigger. A plain system-startup trigger runs
in Session 0, which has no visible desktop, so TouchDesigner and the
server's console window wouldn't actually appear. Also check **"Run only
when user is logged on"** in the task's General tab, for the same reason.

This hasn't been tested against a live run yet -- it was deliberately not
executed while an existing hand-configured TouchDesigner session was
already open (would have launched a conflicting duplicate instance against
the same file and UDP ports). Test on the next clean restart before
relying on it for the show.
