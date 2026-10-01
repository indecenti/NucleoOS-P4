Object = require 'libraries.classic.classic'
Timer = require 'libraries.timer.timer'
Input = require 'libraries.boipushy.Input'
moonshine = require 'libraries.moonshine'
require 'utils'
require 'globals'

function love.load(arg)
    local file_list = {}
    recursive_enumerate('objects', file_list)
    require_files(file_list)

	file_list = {}
	recursive_enumerate('rooms', file_list)
	require_files(file_list)

	current_room = nil

	input = Input()
	bind_inputs()

	timer = Timer()

	go_to_room('Stage')

	effect = moonshine(moonshine.effects.crt)
	effect.crt.distortionFactor = {1.06, 1.065}
end

function love.update(dt)
	if input:pressed('quit') then
		love.event.quit()
	end
	timer:update(dt)
	current_room:update(dt)
end

function love.draw()
	effect(function()
		current_room:draw()
	end)
	
end

function love.run()
	if love.math then
		love.math.setRandomSeed(os.time())
	end

	if love.load then love.load(arg) end

	-- We don't want the first frame's dt to include time taken by love.load.
	if love.timer then love.timer.step() end

	local dt = 0

	-- Main loop time.
	while true do
		-- Process events.
		if love.event then
			love.event.pump()
			for name, a,b,c,d,e,f in love.event.poll() do
				if name == "quit" then
					if not love.quit or not love.quit() then
						return a
					end
				end
				love.handlers[name](a,b,c,d,e,f)
			end
		end

		-- Update dt, as we'll be passing it to update
		if love.timer then
			love.timer.step()
			dt = dt + love.timer.getDelta()
		end

        local FPS = 60
		-- Call update and draw
		while love.update and dt > 1/FPS do
            love.update(dt) 
            dt = dt - 1/FPS
        end -- will pass 0 if love.timer is disabled

		if love.graphics and love.graphics.isActive() then
			love.graphics.clear(love.graphics.getBackgroundColor())
			love.graphics.origin()
			if love.draw then love.draw() end
			love.graphics.present()
		end

		if love.timer then love.timer.sleep(0.001) end
	end

end

function recursive_enumerate(path, file_list)
	local lfs = love.filesystem
	local filesTable = lfs.getDirectoryItems(path)
	for i,v in ipairs(filesTable) do
		local file = path.."/"..v
        local fileInfo = lfs.getInfo(file)
        if fileInfo.type == 'file' then
            table.insert(file_list, file)
        elseif fileInfo.type == 'directory' then
            recursive_enumerate(file, file_list)
        end
	end
end

function require_files(files)
    for _, file in ipairs(files) do
        local file = file:sub(1, -5)
        require(file)
    end
end

function go_to_room(room_type, ...)
    current_room = _G[room_type](...)
end

function bind_inputs()
	input:bind('escape', 'quit')
	input:bind('left', 'move_left')
	input:bind('right', 'move_right')
	input:bind('up', 'move_up')
	input:bind('down', 'move_down')
end