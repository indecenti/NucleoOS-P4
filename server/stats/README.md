# App store install counter

Anonymous "most downloaded" numbers for the store (NucleoOS P4 1.1.133+).

- Device: after a successful store install/update, `nv_appstore` sends `GET
  https://nucleoos.indexhub.it/stats/i/<id>` (install) or `/stats/u/<id>` (update). Nothing else:
  no device id, no query string. Off switch: Settings > Security > "Anonymous store statistics"
  (nv_config `store_stats`).
- Server (Oracle box, `nucleoos.indexhub.it`, conf in `G:\nucleos_sites\deploy\nucleoos.conf`):
  nginx answers 204 and appends `time status path` to `/var/log/nginx/nvstore/hits` (own log
  format, no IP). `nvstore-stats` (hourly timer) sums it into `/stats/downloads.json`.
- Export: `server/appstore/export_static.py` reads `downloads.json` and writes each app's install
  count into the catalog (`downloads`). If the server can't be reached the previous numbers stay.

Install on the server (see the pubblica-sito-statico skill for the load rules):

    scp nvstore-stats nvstore-stats.service nvstore-stats.timer opc@80.225.84.44:/tmp/
    sudo install -m 755 /tmp/nvstore-stats /usr/local/bin/
    sudo install -m 644 /tmp/nvstore-stats.{service,timer} /etc/systemd/system/
    sudo mkdir -p /var/log/nginx/nvstore /var/www/nucleoos/stats && sudo restorecon -R /var/log/nginx/nvstore /var/www/nucleoos/stats
    sudo systemctl daemon-reload && sudo systemctl enable --now nvstore-stats.timer
