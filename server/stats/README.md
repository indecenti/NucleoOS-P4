# Opt-in statistics: store install counter + daily telemetry

Both run only after the owner says yes (setup wizard, or Settings > Security > "Anonymous
statistics"; nv_config `tl_consent`: 0 never asked, 1 yes, 2 no). Public notice: the store's
`privacy.html` (`server/appstore/privacy.html`). Nothing identifies a device: no id, and nginx logs
no IP address for these requests.

## Store install counter ("Most downloaded")

- Device: after a successful store install/update, `nv_appstore` sends `GET
  https://nucleoos.indexhub.it/stats/i/<id>` (install) or `/stats/u/<id>` (update). Nothing else.
- Server (Oracle box, `nucleoos.indexhub.it`, conf in `G:\nucleos_sites\deploy\nucleoos.conf`):
  nginx answers 204 and appends `time status path` to `/var/log/nginx/nvstore/hits` (own log
  format, no IP). `nvstore-stats` (hourly timer) sums it into `/stats/downloads.json`.
- Export: `server/appstore/export_static.py` reads `downloads.json` and writes each app's install
  count into the catalog (`downloads`). If the server can't be reached the previous numbers stay.

## Daily telemetry (`components/nv_telemetry`)

- Device: at most once per calendar day, after the clock synced, at a random minute within 15
  of it being due: `GET https://nucleoos.indexhub.it/t/1?v=&l=&r=&m=&n=&rb=&cr=&ota=&a=&si=&su=&sx=&hw=&hm=`
  (fields in `nvtele-stats.py`). `m`/`n` = first report this month / ever: monthly active and new
  devices are counted without any device id. App ids only for system and store apps (the store
  leaves `/sdcard/apps/<id>/origin`).
- Server: nginx 204 + log format `nvtele` (time, status, query) to `/var/log/nginx/nvstore/tele`;
  `/etc/logrotate.d/nvtele` deletes raw lines after 90 days; `nvtele-stats` (same hourly timer)
  keeps daily totals in `/var/lib/nvtele/days/` (never served).
- Report: `NV_STATS_KEY=<ssh key> python tools/telemetry_report.py` writes a private HTML page to
  `%USERPROFILE%\.nucleo	elemetryeport.html` (outside every repository).

Install on the server (see the pubblica-sito-statico skill for the load rules):

    scp nvstore-stats nvstore-stats.service nvstore-stats.timer opc@80.225.84.44:/tmp/
    sudo install -m 755 /tmp/nvstore-stats /usr/local/bin/
    sudo install -m 644 /tmp/nvstore-stats.{service,timer} /etc/systemd/system/
    sudo mkdir -p /var/log/nginx/nvstore /var/www/nucleoos/stats && sudo restorecon -R /var/log/nginx/nvstore /var/www/nucleoos/stats
    sudo systemctl daemon-reload && sudo systemctl enable --now nvstore-stats.timer
    # telemetry
    sudo install -m 755 /tmp/nvtele-stats.py /usr/local/bin/nvtele-stats
    sudo install -m 644 /tmp/nvtele.logrotate /etc/logrotate.d/nvtele
    sudo mkdir -p /var/lib/nvtele && sudo chmod 755 /var/lib/nvtele
