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
$syms = '61441,61448,61451,61452,61452,61453,61457,61459,61461,61465,61468,61473,61478,61479,61480,61502,61507,61512,61515,61516,61517,61521,61522,61523,61524,61543,61544,61550,61552,61553,61556,61559,61560,61561,61563,61587,61589,61636,61637,61639,61641,61664,61671,61674,61683,61724,61732,61787,61931,62016,62017,62018,62019,62020,62087,62099,62212,62189,62810,63426,63650'

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
Write-Host "=== all fonts generated ==="
