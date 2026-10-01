
Mobile = class('Mobile')

function Mobile:initialize(grid,speed,x,y)
	self.grid = grid
	self.speed = speed
	self.sx = 0
	self.sy = 0
	self:setGridCoord(x,y)
end

function Mobile:setGridCoord(x,y)
	self.x = x
	self.y = y
	local scrX,scrY = self.grid:getScreenCoord(x,y)
	self.act_x = scrX
	self.act_y = scrY
	self.grid_x = scrX
	self.grid_y = scrY
end

function Mobile:update(dt) 
	
	local deltaX = self.grid_x - self.act_x
	local deltaY = self.grid_y - self.act_y
	
	if  math.abs(deltaX) < 2 then
		self.sx=0
	end
	if  math.abs(deltaY) < 2 then
		self.sy=0
	end
	if self.sx == 0 then
		self.act_x = self.grid_x
		deltaX=0
	end
	if self.sy == 0 then
		self.act_y = self.grid_y
		deltaY=0
	end
	if self.sx ~=0 or self.sy ~= 0 then
		local u = self.speed*dt
		local dx = self.sx*u
		local dy = self.sy*u
		if math.abs(dx)>math.abs(deltaX) then
			dx = deltaX
		end
		if math.abs(dy)>math.abs(deltaY) then
			dy = deltaY
		end
		self.act_y = self.act_y + dy
		self.act_x = self.act_x + dx
	end
end

function Mobile:atdest() 
	if math.abs(self.act_x - self.grid_x) < 3 and math.abs(self.act_y - self.grid_y) < 3 then
		return true
	else
		return false
	end
end
