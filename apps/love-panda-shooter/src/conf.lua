function love.conf(t)
	t.version = "11.5"
	t.window.title = "Panda Shooter!"
	t.window.icon = "assets/panda.png"
	t.window.width = 800
	t.window.height = 600
	t.window.fullscreen = false
	t.modules.joystick = false
	t.modules.physics = false
end
