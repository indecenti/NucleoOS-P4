-- aspect is a module that manages the display mode, resolution and aspect ratio

local aspect = {}

local utils = require("modules.utils")

local WINDOW_WIDTH = 1280
local WINDOW_HEIGHT = 720
local GAME_WIDTH = 640
local GAME_HEIGHT = 360
local BAR_COLOR = {0,0,0}

local fullScreen = false
local windowWidth
local windowHeight
local scale
local bars = {}
local gameX
local gameY

function aspect.init()
	if fullScreen then
		local _, _, flags = love.window.getMode()
		local width, height = love.window.getDesktopDimensions(flags.display)
		windowWidth = width
		windowHeight = height
	else
		windowWidth = WINDOW_WIDTH
		windowHeight = WINDOW_HEIGHT
	end

	local gameAspect = GAME_WIDTH / GAME_HEIGHT
	local windowAspect = windowWidth / windowHeight
	
	if gameAspect > windowAspect then
		-- game is wider than window; scale to use full width, use horizontal letterboxing
		scale = windowWidth / GAME_WIDTH
		local scaledGameHeight = GAME_HEIGHT * scale
		local barHeight = math.ceil((windowHeight - scaledGameHeight) / 2)
		gameX = 0
		gameY = barHeight

		table.insert(bars,{
			x = 0,
			y = 0,
			width = windowWidth,
			height = barHeight
		})

		table.insert(bars,{
			x = 0,
			y = windowHeight - barHeight,
			width = windowWidth,
			height = barHeight
		})
	elseif windowAspect > gameAspect then
		-- window is wider than game; scale to use full height, use vertical letterboxing
		scale = windowHeight / GAME_HEIGHT
		local scaledGameWidth = GAME_WIDTH * scale
		local barWidth = math.ceil((windowWidth - scaledGameWidth) / 2)
		gameX = barWidth
		gameY = 0

		table.insert(bars,{
			x = 0,
			y = 0,
			width = barWidth,
			height = windowHeight
		})

		table.insert(bars,{
			x = windowWidth-barWidth,
			y = 0,
			width = barWidth,
			height = windowHeight
		})
	else
		-- scale to full width and height, no letterboxing
		scale = windowWidth / GAME_WIDTH
		gameX = 0
		gameY = 0
	end
	
    -- for now, not setting mode again, leaving it at default window dimensions set in conf.lua to avoid flickering at startup
	--love.window.setMode(WINDOW_WIDTH,WINDOW_HEIGHT,{fullscreen=fullScreen,fullscreentype="desktop"})
end

function aspect.apply()
	love.graphics.push()
	love.graphics.translate(gameX,gameY)
	love.graphics.scale(scale)
end

function aspect.letterbox()
	love.graphics.pop()
	love.graphics.push()
	love.graphics.setColor(BAR_COLOR[1],BAR_COLOR[2],BAR_COLOR[3])

	for i = 1, #bars do
		love.graphics.rectangle("fill",bars[i].x,bars[i].y,bars[i].width,bars[i].height)
	end
    
	love.graphics.pop()
end

function aspect.getGameWidth()
	return GAME_WIDTH
end

function aspect.getGameHeight()
	return GAME_HEIGHT
end

function aspect.toggleFullScreen()
	fullScreen = not(fullScreen)
	aspect.init()
end

function aspect.getScale()
	return scale
end

return aspect