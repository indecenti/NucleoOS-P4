# sd/ — contenuto gestito della microSD

Mirror dei file che NucleoOS si aspetta di trovare sulla card. Si sincronizza sulla
SD con `tools\sync-sd.ps1 -Drive X:` (copia additiva: non tocca ciò che il sistema
scrive da solo — `apps/`, `nucleos/settings.nvb`, foto, note).

Layout sulla card (creato dal sistema o da questo mirror):

```
/sdcard
├── apps/                  # app WASM installate (gestite dal sistema, NON nel mirror)
├── nucleos/               # dir di sistema
│   ├── settings.nvb       # backup NVS automatico (gestito dal sistema)
│   └── drivers/
│       └── windows/       # driver PC per Second Screen USB (questo mirror)
├── notes.txt              # app Notes
└── Photos/ DCIM/          # app Gallery
```

I driver sono anche scaricabili dal web console (`http://nucleov2.local`) via
File > `/sdcard/nucleos/drivers/`.
