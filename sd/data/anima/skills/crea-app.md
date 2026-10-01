---
name: crea-app
description: build, run and fix graphical apps for NucleoOS in Lua (the Lua App engine)
triggers: crea un'app, creami un'app, fai un'app, scrivi un'app, nuova app, un gioco, crea un gioco, app lua, applicazione, debug, correggi l'app, make an app, build an app, write an app, create a game, lua app
offline: Per creare app serve un modello (cloud o un server Ollama da 7B in su): configuralo in Impostazioni web > IA.
---
NucleoOS runs graphical Lua 5.4 apps in the Lua App engine. A script needs no package: write
~/lua/<name>.lua (or ~/lua/<name>/main.lua with modules) and the "Lua App" tile lists and runs it.
Screen 1024x600, colours 0xRRGGBB, immediate mode: nv.draw() redraws the whole screen.

Workflow (one ACT per reply): 1) ACT write ~/lua/<name>.lua with the full app; 2) check syntax:
ACT sh lua -e "assert(loadfile('/lua/<name>.lua')) print('syntax ok')" (the terminal lua sees
/sdcard/home as / and has no gfx/ui: compile only, never run the app there); 3) fix with ACT edit; 4) ACT sh launch luaapp opens the Lua App launcher (the script is listed there); ask the user to
tap it, then ACT sh dmesg | tail -n 40 shows any Lua error/traceback to fix.
Keep apps small (< 150 lines), Italian UI text unless asked otherwise.

Life: main.lua runs once; then nv.init(), nv.update(dt), nv.draw(), nv.tap(x,y), nv.touch(ev)
(ev.type down/move/up, ev.x, ev.y), nv.key(ev) (ev.key "a" "enter" "left"..., ev.down, ev.text),
nv.back() (return true to stay). nv.redraw() asks a redraw; nv.continuous(true) for games
(draw every frame); nv.exit(). Storage: nv.load(key, default), nv.save(key, value).
Sound: nv.sound(name). Time: nv.time() (s). Language: nv.it (true when the UI is Italian).

gfx: clear(c) rect(x,y,w,h,c[,radius]) frame(x,y,w,h,c[,thick,radius]) circle(x,y,r,c)
ring(x,y,r,c[,t]) arc(x,y,r,a0,a1,c[,t]) line(x0,y0,x1,y1,c[,t]) tri(...) poly({x,y,...},c)
text(x,y,s[,size,c,align]) -> width (sizes 12 14 16 20 24 32 48 72; align left/center/right)
text_width(s,size) alpha(a) clip(x,y,w,h) push() pop() translate(x,y) scale(s) rgb(r,g,b).

ui (call inside nv.draw): ui.clear() ui.header(title,{back=true}) -> back_tapped
ui.button(x,y,w,h,label,{style="primary"|"flat"|"outline"|"danger"}) -> tapped
ui.toggle(x,y,on,label) -> on  ui.slider(x,y,w,v,min,max) -> v,changed
ui.list(id,x,y,w,h,items) -> index tapped (items "text" or {title=,sub=,right=})
ui.tabs(x,y,w,h,labels,sel) -> sel  ui.keys(x,y,w,h,rows) -> label (rows {{"7","8"},{"0:2"}})
ui.label(x,y,text[,size,color,align]) ui.paragraph(x,y,w,text) ui.panel(x,y,w,h)
ui.progress(x,y,w,h,frac) ui.icon(name,cx,cy,size) ui.prompt(title,initial,fn(text))
ui.grid(x,y,w,h,cols,rows) -> cell(c,r) -> x,y,w,h (take the 4 values first: local x,y,w,h = cell(1,1))
ui.W ui.H ui.S(v) ui.theme.{bg,panel,fg,dim,accent,ok,warn,err} ui.font.{small,body,big,title,huge,giant}

Minimal app (copy and grow):
local n = nv.load("n", 0)
function nv.draw()
  ui.clear()
  ui.header(nv.it and "Contatore" or "Counter")
  ui.label(ui.W / 2, 200, tostring(n), ui.font.giant, ui.theme.fg, "center")
  if ui.button(412, 400, 200, 80, "+1", {style = "primary"}) then n = n + 1; nv.save("n", n) end
end

Errors stop the app with the message and traceback on screen. Never invent APIs not listed here;
if unsure, read the docs with ACT sh cat or keep to gfx/ui/nv above.
