require 'mobile'

local lg=love.graphics

Player = Mobile:subclass('Player')

Player.static.directions = {
	{0,-1},
	{0,1},
	{1,0},
	{-1,0}
}

Player.static.reverse = {2,1,4,3}

function Player:initialize(grid,x,y)
	Mobile.initialize(self,grid,60,x,y)
	
	self.dir=1
	self.move=false
	self.haveTail = false
	self.tailLen = 0
end

function Player:draw()
	lg.draw(Player.image,Player.quads[self.dir],self.act_x,self.act_y)
end

function Player:update(dt)
	Mobile.update(self,dt)
	if self:atdest() then
		if self.move then
			self.move = false
			if self.grid:placeTail(self.x,self.y) then
				self.haveTail = true
				self.tailLen = self.tailLen + 1
			elseif self.haveTail then
				self.haveTail = false
				self.tailLen = 0
				self.grid:setEvent(Grid.EVENT_CLOSE)
				return
			end
		end

		local newdir = 0
		
		if love.keyboard.isDown("up") then
			if self.y > 1 then
				newdir = 1
			end
		elseif love.keyboard.isDown("down") then
			if self.y < self.grid.hgrid then
				newdir = 2
			end
		elseif love.keyboard.isDown("left") then
			if self.x > 1 then
				newdir = 4
			end
		elseif love.keyboard.isDown("right") then
			if self.x < self.grid.wgrid then
				newdir=3
			end
		end

		if newdir ~= 0 and self.tailLen > 1 and Player.reverse[newdir] == self.dir then
				-- anti reverse 
				newdir = 0
		end
		
		if newdir ~= 0 then
			local deltaX,deltaY = Player.directions[newdir][1],Player.directions[newdir][2]
			self.x = self.x + deltaX
			self.y = self.y + deltaY
			self.grid_x = self.grid_x + deltaX * Grid.size
			self.grid_y = self.grid_y + deltaY * Grid.size
			if deltaX ~= 0 then
				self.sx = deltaX
			end
			if deltaY ~= 0 then
				self.sy = deltaY
			end
			self.dir = newdir
			self.move = true
		end
		if self.move and self.grid:isTail(self.x,self.y) then
				self.grid:setEvent(Grid.EVENT_SELF_HIT_TAIL)
				return
		end		
	end
end

function Player.static:load()

	local gsize = Grid.size
	local pid = love.image.newImageData(gsize*4,gsize)
	local pc={1,50/255,200/255}

	for x=0,pid:getWidth()-1 do
		for y=0,gsize-1 do
			pid:setPixel(x,y,0,0,0,0)
		end
	end
	local i = math.floor(gsize/4)
	local u = 0
	
	while 2*u <= gsize do
		for j = u,gsize-1-u do
			pid:setPixel(j,gsize-1-i,pc[1],pc[2],pc[3],1)
			pid:setPixel(j+gsize,i,pc[1],pc[2],pc[3],1)
			pid:setPixel(i+2*gsize,j,pc[1],pc[2],pc[3],1)
			pid:setPixel(4*gsize-1-i,j,pc[1],pc[2],pc[3],1)
		end
		i = i + 1
		u = u + 1
	end
	
	local img = lg.newImage(pid)
	Player.static.image = img
	Player.static.quads={}
	for i =1,4 do
		Player.static.quads[i] = lg.newQuad((i-1)*gsize,0,gsize,gsize,img:getWidth(),img:getHeight())
	end	
end