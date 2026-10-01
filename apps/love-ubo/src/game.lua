require 'lib/middleclass'
require 'player'
require 'ubo'
require 'grid'
require 'caption'

local lw = love.window
local lg = love.graphics

Stateful= require 'lib/stateful'

Game = class('Game')
Game:include(Stateful)

local InteruptState = Game:addState('Interupt')


function InteruptState:enteredState()
	self.menu:setLines({"RESUME GAME","MAIN MENU","QUIT"})
	self.menu:setMenuMode(true)
end

function InteruptState:update(dt)
	
end

function InteruptState:draw()
	self.grid:draw()
	self.menu:draw()
end

function InteruptState:doMenuAction()
	local choice = self.menu.selected
	if choice == 1 then
		self:popState('Interupt')
	elseif choice == 2 then
		self:gotoState('Menu')
	elseif choice == 3 then
		love.event.push("quit")
	end
end

function InteruptState:keypressed(key)
	if key == "escape" then
		love.event.push("quit")
	end
	self.menu:keypressed(key)
	self:doMenuAction()
end

function InteruptState:mousepressed(x,y,b)
	self.menu:mousepressed(x,y,b)
	self:doMenuAction()
end

local SelectVideoState = Game:addState('Video')

function SelectVideoState:enteredState()
	local w,h,fs,_,_ = lw.getMode()
	
	local modes = lw.getFullscreenModes()
	table.sort(modes, function(a, b) return a.width*a.height < b.width*b.height end)   -- sort from smallest to largest
	self.videoModes={}
	local lines={}
	local il=1
	local cm=1
	for _,md in ipairs(modes) do
		--if  lw.checkMode(md.width,md.height, fs ) then
			self.videoModes[il] = { width = md.width, height = md.height, fs=fs}
			lines[il]=string.format('%dx%d',md.width,md.height)
			if md.width == w and md.height == h then
				cm = il
			end
			il=il+1
		--end
	end
	lines[il]="RETURN TO MAIN MENU"
	self.videoMenu:setLines(lines)
	self.videoMenu:setMenuMode(true)
	self.videoMenu.active=cm
end

function SelectVideoState:update(dt)
	self.menuGrid:update(dt)
end

function SelectVideoState:draw()
	self.menuGrid:draw()
	self.videoMenu:draw()
end

function SelectVideoState:doMenuAction(key)
	local choice = self.videoMenu.selected
	if choice > #self.videoModes then
		self:popState('Video')
	elseif choice > 0 then
		local vm = self.videoModes[choice]
		if lw.setMode( vm.width, vm.height, vm.fs ) then
			self.grid = Grid(20)
			self.menuGrid = Grid(0)
			self.menuGrid:startMain()
			self.menu:center()
			self:popState('Video')
		else
			self.videoMenu.selected=0
		end
	end
end

function SelectVideoState:keypressed(key)
	if key == "escape" then
		self:popState('Video')
	end
	self.videoMenu:keypressed(key)
	self:doMenuAction()
end

function SelectVideoState:mousepressed(x,y,b)
	self.videoMenu:mousepressed(x,y,b)
	self:doMenuAction()
end

local MenuState = Game:addState('Menu')
function MenuState:enteredState()
	local _,_,fs,_,_ = lw.getMode()
	local l2 = "FULLSCREEN MODE"
	if fs then
		l2 = "WINDOW MODE"
	end
	self.menuGrid:startMain()
	self.menu:setLines({"START GAME",l2,"SCREEN RESOLUTION","QUIT"})
	self.menu:setMenuMode(true)
end

function MenuState:update(dt)
	self.menuGrid:update(dt)
end

function MenuState:draw()
	self.menuGrid:draw()
	self.menu:draw()
end

function MenuState:doMenuAction()
	local choice = self.menu.selected
	if choice == 1 then
		self.level=1
		self.grid:startLevel(self.level)
		self:gotoState(nil)
	elseif choice == 2 then
		local isfs = lw.getFullscreen()
		if lw.setFullscreen(not isfs) then
			self:gotoState('Menu')
		end
	elseif choice == 3 then
		self.menu.selected=0
		self:pushState('Video')
	elseif choice == 4 then
		love.event.push("quit")
	end
end

function MenuState:keypressed(key)
	if key == "escape" then
		love.event.push("quit")
	end
	self.menu:keypressed(key)
	self:doMenuAction()
end

function MenuState:mousepressed(x,y,b)
	self.menu:mousepressed(x,y,b)
	self:doMenuAction()
end

local MainState = Game:addState('Main')

function MainState:enteredState()
	self.level=1
	self.grid:startLevel(self.level)
end

function MainState:update(dt)
end

function MainState:draw()
end

function MainState:keypressed(key)
end

local TransitionState = Game:addState('Transition')

function TransitionState:enteredState()
	self.delay = Game.TRANSITION_DELAY
end

function TransitionState:update(dt)
	self.grid:update(dt)
	self.delay = self.delay - dt
	if self.delay < 0 then
		self.grid:startLevel(self.level)
		self:popState('Transition')
	end
end

function TransitionState:draw()
	self.grid:draw()
	self.caption:draw()
end

function TransitionState:keypressed(key)
	if key == "escape" then
		self.grid:startLevel(self.level)
		self:popState('Transition')
	end
end

function Game:initialize()
	self.grid = Grid(20)
	
	self.caption=Caption(Game.font)
	self.menu=Caption(Game.menuFont)
	self.videoMenu=Caption(Game.font)
	self.level=1
	self.grid:startLevel(self.level)
	
	self.menuGrid = Grid(0)
	self:gotoState('Menu')
end


function Game:draw()
	self.grid:draw()
end

function Game:update(dt)
	self.grid:update(dt)
	if self.grid:isFinished() then
		if self.grid:isSuccess() then
			self.level = self.level + 1
			self.caption:setLines({"LEVEL COMPLETED","GO TO NEXT LEVEL"})
		else
			self.caption:setLines({"LEVEL FAILED",self.grid.event,"RESTART LEVEL"})
		end
		self:pushState('Transition')
	end
end

function Game:keypressed(key)
	if key == "escape" then
		self:pushState('Interupt')
	end
end

function Game:mousepressed(x,y,b)
end

function Game.static:load(gsize)
	Game.static.font  = lg.newFont(18)
	Game.static.menuFont  = lg.newFont(24)
	Game.static.color ={1,1,1}
	Game.static.TRANSITION_DELAY=3
	Grid:load(gsize)	
end