# Generate extended Montserrat LVGL fonts (ASCII + Latin-1 accents + LVGL symbols).
# Drop-in named nv_font_<size>; replaces the ASCII-only built-in montserrat in the UI.
$ErrorActionPreference = 'Stop'
$env:Path += ";C:\Program Files\nodejs;$env:APPDATA\npm"

$base = 'D:\NucleoV2\managed_components\lvgl__lvgl\scripts\built_in_font'
$mont = Join-Path $base 'Montserrat-Medium.ttf'
$fa   = Join-Path $base 'FontAwesome5-Solid+Brands+Regular.woff'
$out  = 'D:\NucleoV2\components\nv_fonts'

# Text ranges: ASCII + Latin-1 Supplement (à é ñ ü ç ß …) + bullet.
$textRange = '0x20-0x7F,0xA0-0xFF,0x2022'
# Exact FontAwesome symbol code points used by LVGL's built-in fonts (LV_SYMBOL_*).
$syms = '61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650,61646'

foreach ($sz in 14,20,28) {
    $name = "nv_font_$sz"
    $dst  = Join-Path $out "$name.c"
    Write-Host "=== generating $name ==="
    lv_font_conv --bpp 4 --size $sz --no-compress --no-prefilter `
        --font $mont -r $textRange `
        --font $fa   -r $syms `
        --format lvgl --force-fast-kern-format `
        --lv-font-name $name -o $dst
    if (Test-Path $dst) { Write-Host "OK  $dst  ($((Get-Item $dst).Length) bytes)" }
    else { Write-Host "FAIL $name"; exit 1 }
}

# Terminal monospace (nv_font_mono_17): DejaVu Sans Mono, the stock Linux terminal face.
# ASCII + Latin-1 + œ Œ Ÿ „ ‰ ™ (the five UI languages) + typographic punctuation + arrows + a few
# math signs + box drawing / block elements (TUI tables) + the marks of WordGrinder's ruler.
$mono = 'C:\Windows\Fonts\DejaVuSansMono.ttf'
$monoRange = '0x20-0x7E,0xA0-0xFF,0x152-0x153,0x178,0x2013-0x2014,0x2018-0x201E,0x2022,0x2026,0x2030,0x2080-0x2089,0x20AC,0x2122,0x2190-0x2195,0x2212,0x221E,0x2260,0x2264-0x2265,0x2500-0x259F,0x25A0-0x25A1,0x25B2-0x25B3,0x25B6,0x25BC,0x25BE,0x25C0,0x25C7,0x25CF,0x25E4-0x25E5,0x25FA,0x25FF'
$dst = Join-Path $out 'nv_font_mono_17.c'
Write-Host "=== generating nv_font_mono_17 ==="
Push-Location $out
lv_font_conv --bpp 4 --size 17 --no-compress --no-prefilter `
    --font $mono -r $monoRange `
    --format lvgl --force-fast-kern-format `
    --lv-font-name nv_font_mono_17 -o nv_font_mono_17.c
Pop-Location
if (Test-Path $dst) { Write-Host "OK  $dst  ($((Get-Item $dst).Length) bytes)" }
else { Write-Host "FAIL nv_font_mono_17"; exit 1 }
# Fogli (the spreadsheet app): toolbar icons from FontAwesome + the Σ of DejaVu Sans Mono at 18 px,
# and € with typographic marks at 14 px, set behind nv_font_14 as its fallback (sheets_app.cpp).
$sheetIcons = '0xF002,0xF00C,0xF00D,0xF01E,0xF032,0xF033,0xF035,0xF036,0xF037,0xF038,0xF059,0xF067,0xF073,0xF078,0xF07C,0xF080,0xF0B0,0xF0C4,0xF0C5,0xF0C7,0xF0CB,0xF0CD,0xF0CE,0xF0D0,0xF0DB,0xF0E2,0xF0EA,0xF0EB,0xF11C,0xF12D,0xF142,0xF153,0xF15B,0xF160,0xF161,0xF1C3,0xF1D8,0xF1DA,0xF292,0xF295,0xF2ED,0xF4AD'
Push-Location $out
lv_font_conv --bpp 4 --size 18 --no-compress --no-prefilter `
    --font $fa -r $sheetIcons `
    --font $mono -r '0x3A3' `
    --format lvgl --force-fast-kern-format --lv-font-name nv_font_sheet_18 -o nv_font_sheet_18.c
lv_font_conv --bpp 4 --size 14 --no-compress --no-prefilter `
    --font $mont -r '0x20AC,0x2026,0x2013,0x2014,0x2018-0x201E,0x2212' `
    --font $mono -r '0x3A3,0x2260,0x2264-0x2265,0x221E' `
    --format lvgl --force-fast-kern-format --lv-font-name nv_font_sheet_14 -o nv_font_sheet_14.c
Pop-Location
Write-Host "=== all fonts generated ==="
